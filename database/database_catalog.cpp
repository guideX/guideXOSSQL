#include "database_catalog.h"

#include <cstring>

#include "database_endian.h"
#include "database_format.h"
#include "database_utf8.h"

namespace gxos {
namespace db {

namespace {

// Reads a little-endian field from `bytes` at `offset`, bounds-checked against
// `length`. Returns false when the field would overrun the buffer.
template <typename T>
bool readField(const uint8_t* bytes, size_t length, size_t& offset, T& out) {
    if (offset + sizeof(T) > length) {
        return false;
    }
    if (sizeof(T) == 1) {
        out = static_cast<T>(bytes[offset]);
    } else if (sizeof(T) == 2) {
        out = static_cast<T>(loadLe16(bytes + offset));
    } else if (sizeof(T) == 4) {
        out = static_cast<T>(loadLe32(bytes + offset));
    } else {
        out = static_cast<T>(loadLe64(bytes + offset));
    }
    offset += sizeof(T);
    return true;
}

std::string makeReservedIndexName(IndexKind kind, uint32_t indexId) {
    std::string prefix;
    switch (kind) {
    case IndexKind::PrimaryKey: prefix = "$PK_"; break;
    case IndexKind::Unique: prefix = "$UQ_"; break;
    default: prefix = "$IX_"; break;
    }
    return prefix + std::to_string(static_cast<unsigned long long>(indexId));
}

} // namespace

Catalog::Catalog() : _tables(), _indexes(), _nextTableId(1), _nextIndexId(1),
                     _version(kCatalogVersion) {}

bool Catalog::validateName(const std::string& name, uint32_t maxBytes) const {
    if (name.empty() || name.size() > maxBytes) {
        return false;
    }
    return isValidUtf8(reinterpret_cast<const uint8_t*>(name.data()), name.size());
}

bool Catalog::tableNameExists(const std::string& name) const {
    for (size_t i = 0; i < _tables.size(); ++i) {
        if (_tables[i].name == name) {
            return true;
        }
    }
    return false;
}

bool Catalog::indexNameExists(const std::string& name) const {
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].name == name) {
            return true;
        }
    }
    return false;
}

bool Catalog::indexOnColumnExists(uint32_t tableId, uint32_t columnOrdinal) const {
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].tableId == tableId &&
            _indexes[i].columnOrdinal == columnOrdinal) {
            return true;
        }
    }
    return false;
}

std::vector<Catalog::IndexRecord> Catalog::indexesForTable(uint32_t tableId) const {
    std::vector<IndexRecord> out;
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].tableId == tableId) {
            out.push_back(_indexes[i]);
        }
    }
    return out;
}

uint32_t Catalog::recordSizeForDefinition(const TableDefinition& def) const {
    uint32_t size = 40 + static_cast<uint32_t>(def.name.size());
    for (size_t i = 0; i < def.columns.size(); ++i) {
        size += 16 + static_cast<uint32_t>(def.columns[i].name.size());
    }
    return size;
}

DbResult Catalog::addIndex(const IndexDefinition& def, uint32_t pageSize,
                           uint32_t& outIndexId) {
    if (!def.name.empty() && !validateName(def.name, kMaxIndexNameBytes)) {
        return DbResult::error(DbStatus::InvalidArgument, "invalid index name");
    }
    if (!def.name.empty() && indexNameExists(def.name)) {
        return DbResult::error(DbStatus::AlreadyExists, "index already exists");
    }
    const TableRecord* table = findTable(def.tableId);
    if (table == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "index table does not exist");
    }
    if (def.columnOrdinal >= table->columns.size()) {
        return DbResult::error(DbStatus::InvalidArgument, "index column does not exist");
    }
    if (!isIndexableType(table->columns[def.columnOrdinal].type)) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "column type is not indexable");
    }
    if (indexOnColumnExists(def.tableId, def.columnOrdinal)) {
        return DbResult::error(DbStatus::AlreadyExists,
                               "an index already exists on this column");
    }
    if (_indexes.size() >= kMaxIndexes) {
        return DbResult::error(DbStatus::NoSpace,
                               "catalog index count exceeds policy maximum");
    }
    uint32_t perTable = 0;
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].tableId == def.tableId) {
            ++perTable;
        }
    }
    if (perTable >= kMaxIndexesPerTable) {
        return DbResult::error(DbStatus::NoSpace,
                               "per-table index count exceeds policy maximum");
    }

    IndexRecord rec;
    rec.indexId = _nextIndexId++;
    rec.name = def.name;
    if (rec.name.empty()) {
        rec.name = makeReservedIndexName(def.kind, rec.indexId);
    }
    if (indexNameExists(rec.name)) {
        return DbResult::error(DbStatus::Internal, "reserved index name collision");
    }
    rec.tableId = def.tableId;
    rec.columnOrdinal = def.columnOrdinal;
    rec.rootPageId = 0;
    rec.flags = 0;
    if (indexKindIsUnique(def.kind)) {
        rec.flags |= 0x0001u;
    }
    if (def.kind == IndexKind::PrimaryKey) {
        rec.flags |= 0x0002u;
    }
    rec.formatVersion = 1;
    rec.entryCount = 0;

    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t contCap = capacity - kCatalogContHeaderSize;
    const uint32_t recordSize = 1 + 36 + static_cast<uint32_t>(rec.name.size());
    if (recordSize > contCap) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "index record too large for the database page size");
    }

    _indexes.push_back(rec);
    _version = kCatalogVersionV3;
    outIndexId = rec.indexId;
    return DbResult::ok();
}

void Catalog::setIndexRoot(uint32_t indexId, uint64_t rootPageId) {
    IndexRecord* rec = findIndexMutable(indexId);
    if (rec != nullptr) {
        rec->rootPageId = rootPageId;
    }
}

void Catalog::adjustIndexEntryCount(uint32_t indexId, int64_t delta) {
    IndexRecord* rec = findIndexMutable(indexId);
    if (rec == nullptr) {
        return;
    }
    if (delta < 0) {
        const uint64_t decrease = static_cast<uint64_t>(-delta);
        rec->entryCount = decrease > rec->entryCount ? 0 : rec->entryCount - decrease;
    } else {
        rec->entryCount += static_cast<uint64_t>(delta);
    }
}

void Catalog::setIndexEntryCount(uint32_t indexId, uint64_t count) {
    IndexRecord* rec = findIndexMutable(indexId);
    if (rec != nullptr) {
        rec->entryCount = count;
    }
}

DbResult Catalog::addTable(const TableDefinition& def, uint32_t pageSize,
                           uint32_t& outTableId) {
    if (def.name.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "table name must not be empty");
    }
    if (!validateName(def.name, kMaxTableNameBytes)) {
        return DbResult::error(DbStatus::InvalidArgument, "invalid table name");
    }
    if (tableNameExists(def.name)) {
        return DbResult::error(DbStatus::AlreadyExists, "table already exists");
    }
    if (def.columns.size() > kMaxColumnsPerTable) {
        return DbResult::error(DbStatus::InvalidArgument, "column count exceeds policy maximum");
    }
    if (_tables.size() >= kMaxTables) {
        return DbResult::error(DbStatus::NoSpace, "catalog table count exceeds policy maximum");
    }

    // Validate columns and detect duplicate names.
    for (size_t i = 0; i < def.columns.size(); ++i) {
        const ColumnDefinition& col = def.columns[i];
        if (!validateName(col.name, kMaxColumnNameBytes)) {
            return DbResult::error(DbStatus::InvalidArgument, "invalid column name");
        }
        if (!isConcreteDbType(col.type)) {
            return DbResult::error(DbStatus::InvalidArgument, "unsupported column type");
        }
        for (size_t j = 0; j < i; ++j) {
            if (def.columns[j].name == col.name) {
                return DbResult::error(DbStatus::AlreadyExists, "duplicate column name");
            }
        }
    }

    // SQL6: one single-column PRIMARY KEY at most, and its column must be
    // NOT NULL. Constraint ordinals must reference real indexable columns.
    // These are all validated before the table record is appended, so a failed
    // constraint can never leave a half-created table.
    bool sawPrimaryKey = false;
    for (size_t i = 0; i < def.indexes.size(); ++i) {
        const IndexDefinition& idx = def.indexes[i];
        if (idx.columnOrdinal >= def.columns.size()) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "constraint references an unknown column");
        }
        if (!isIndexableType(def.columns[idx.columnOrdinal].type)) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "constraint column type is not indexable");
        }
        if (idx.kind == IndexKind::PrimaryKey) {
            if (sawPrimaryKey) {
                return DbResult::error(DbStatus::InvalidArgument,
                                       "a table may have at most one PRIMARY KEY");
            }
            sawPrimaryKey = true;
            if (def.columns[idx.columnOrdinal].nullable) {
                return DbResult::error(DbStatus::InvalidArgument,
                                       "PRIMARY KEY column must be NOT NULL");
            }
        }
    }
    if (_indexes.size() + def.indexes.size() > kMaxIndexes) {
        return DbResult::error(DbStatus::NoSpace,
                               "catalog index count exceeds policy maximum");
    }
    if (def.indexes.size() > kMaxIndexesPerTable) {
        return DbResult::error(DbStatus::NoSpace,
                               "per-table index count exceeds policy maximum");
    }

    // The serialized record must fit in a catalog continuation page.
    const bool willBeV3 =
        _version == kCatalogVersionV3 || !_indexes.empty() || !def.indexes.empty();
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t contCap = capacity - kCatalogContHeaderSize;
    const uint32_t recordSize = recordSizeForDefinition(def) + (willBeV3 ? 1u : 0u);
    if (recordSize > contCap) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "schema too large for the database page size");
    }

    TableRecord rec;
    rec.tableId = _nextTableId++;
    rec.schemaVersion = 1;
    rec.name = def.name;
    rec.firstHeapPageId = 0;
    rec.heapPageCount = 0;
    rec.rowCount = 0;
    for (size_t i = 0; i < def.columns.size(); ++i) {
        ColumnDefinition col = def.columns[i];
        col.ordinal = static_cast<uint32_t>(i);
        rec.columns.push_back(col);
    }

    outTableId = rec.tableId;
    const size_t indexesBefore = _indexes.size();
    const uint32_t nextIndexBefore = _nextIndexId;
    _tables.push_back(rec);

    // Append constraint-backed indexes after the table exists so addIndex can
    // resolve the table and column type. On any failure undo the partial table
    // record so the in-memory catalog is never left inconsistent.
    for (size_t i = 0; i < def.indexes.size(); ++i) {
        IndexDefinition idx = def.indexes[i];
        idx.tableId = rec.tableId;
        uint32_t indexId = 0;
        DbResult result = addIndex(idx, pageSize, indexId);
        if (!result.isOk()) {
            _indexes.resize(indexesBefore);
            _nextIndexId = nextIndexBefore;
            _tables.pop_back();
            --_nextTableId;
            return result;
        }
    }
    if (willBeV3) {
        _version = kCatalogVersionV3;
    }
    return DbResult::ok();
}

const Catalog::TableRecord* Catalog::findTable(const std::string& name) const {
    for (size_t i = 0; i < _tables.size(); ++i) {
        if (_tables[i].name == name) {
            return &_tables[i];
        }
    }
    return nullptr;
}

const Catalog::TableRecord* Catalog::findTable(uint32_t tableId) const {
    for (size_t i = 0; i < _tables.size(); ++i) {
        if (_tables[i].tableId == tableId) {
            return &_tables[i];
        }
    }
    return nullptr;
}

Catalog::TableRecord* Catalog::findTableMutable(uint32_t tableId) {
    for (size_t i = 0; i < _tables.size(); ++i) {
        if (_tables[i].tableId == tableId) {
            return &_tables[i];
        }
    }
    return nullptr;
}

const Catalog::IndexRecord* Catalog::findIndex(const std::string& name) const {
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].name == name) {
            return &_indexes[i];
        }
    }
    return nullptr;
}

const Catalog::IndexRecord* Catalog::findIndex(uint32_t indexId) const {
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].indexId == indexId) {
            return &_indexes[i];
        }
    }
    return nullptr;
}

Catalog::IndexRecord* Catalog::findIndexMutable(uint32_t indexId) {
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].indexId == indexId) {
            return &_indexes[i];
        }
    }
    return nullptr;
}

DbResult Catalog::parseColumnRecord(const uint8_t* bytes, size_t length, size_t& offset,
                                    ColumnDefinition& out) {
    uint32_t ordinal = 0;
    uint16_t type = 0;
    uint16_t nullable = 0;
    uint32_t flags = 0;
    uint32_t nameLength = 0;

    if (!readField(bytes, length, offset, ordinal)) {
        return DbResult::error(DbStatus::CorruptPage, "column record truncated (ordinal)");
    }
    if (!readField(bytes, length, offset, type)) {
        return DbResult::error(DbStatus::CorruptPage, "column record truncated (type)");
    }
    if (!readField(bytes, length, offset, nullable)) {
        return DbResult::error(DbStatus::CorruptPage, "column record truncated (nullable)");
    }
    if (!readField(bytes, length, offset, flags)) {
        return DbResult::error(DbStatus::CorruptPage, "column record truncated (flags)");
    }
    if (!readField(bytes, length, offset, nameLength)) {
        return DbResult::error(DbStatus::CorruptPage, "column record truncated (name length)");
    }
    if (nameLength > kMaxColumnNameBytes) {
        return DbResult::error(DbStatus::CorruptPage, "column name length exceeds policy maximum");
    }
    if (offset + nameLength > length) {
        return DbResult::error(DbStatus::CorruptPage, "column name extends beyond record");
    }
    if (nameLength == 0) {
        return DbResult::error(DbStatus::CorruptPage, "column name is empty");
    }
    if (!isValidUtf8(bytes + offset, nameLength)) {
        return DbResult::error(DbStatus::CorruptPage, "column name is not valid UTF-8");
    }
    if (!isConcreteDbType(static_cast<DbType>(type))) {
        return DbResult::error(DbStatus::CorruptPage, "unsupported column type id");
    }
    if (nullable > 1) {
        return DbResult::error(DbStatus::CorruptPage, "invalid nullable flag");
    }

    out.ordinal = ordinal;
    out.type = static_cast<DbType>(type);
    out.nullable = nullable != 0;
    out.name.assign(reinterpret_cast<const char*>(bytes + offset), nameLength);
    offset += nameLength;
    return DbResult::ok();
}

DbResult Catalog::parseTableRecord(const uint8_t* bytes, size_t length, size_t& offset,
                                   TableRecord& out) {
    uint32_t tableId = 0;
    uint32_t schemaVersion = 0;
    uint32_t columnCount = 0;
    uint64_t firstHeapPageId = 0;
    uint32_t heapPageCount = 0;
    uint64_t rowCount = 0;
    uint32_t flags = 0;
    uint32_t nameLength = 0;

    if (!readField(bytes, length, offset, tableId)) {
        return DbResult::error(DbStatus::CorruptPage, "table record truncated (id)");
    }
    if (!readField(bytes, length, offset, schemaVersion)) {
        return DbResult::error(DbStatus::CorruptPage, "table record truncated (schema version)");
    }
    if (!readField(bytes, length, offset, columnCount)) {
        return DbResult::error(DbStatus::CorruptPage, "table record truncated (column count)");
    }
    if (!readField(bytes, length, offset, firstHeapPageId)) {
        return DbResult::error(DbStatus::CorruptPage, "table record truncated (first heap page)");
    }
    if (!readField(bytes, length, offset, heapPageCount)) {
        return DbResult::error(DbStatus::CorruptPage, "table record truncated (heap page count)");
    }
    if (!readField(bytes, length, offset, rowCount)) {
        return DbResult::error(DbStatus::CorruptPage, "table record truncated (row count)");
    }
    if (!readField(bytes, length, offset, flags)) {
        return DbResult::error(DbStatus::CorruptPage, "table record truncated (flags)");
    }
    if (!readField(bytes, length, offset, nameLength)) {
        return DbResult::error(DbStatus::CorruptPage, "table record truncated (name length)");
    }

    if (tableId == 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid table id");
    }
    if (schemaVersion == 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid schema version");
    }
    if (columnCount > kMaxColumnsPerTable) {
        return DbResult::error(DbStatus::CorruptPage, "column count exceeds policy maximum");
    }
    if (nameLength == 0 || nameLength > kMaxTableNameBytes) {
        return DbResult::error(DbStatus::CorruptPage, "table name length out of range");
    }
    if (offset + nameLength > length) {
        return DbResult::error(DbStatus::CorruptPage, "table name extends beyond record");
    }
    if (!isValidUtf8(bytes + offset, nameLength)) {
        return DbResult::error(DbStatus::CorruptPage, "table name is not valid UTF-8");
    }
    if (heapPageCount == 0 && firstHeapPageId != 0) {
        return DbResult::error(DbStatus::CorruptPage, "first heap page set without heap pages");
    }

    out.tableId = tableId;
    out.schemaVersion = schemaVersion;
    out.name.assign(reinterpret_cast<const char*>(bytes + offset), nameLength);
    offset += nameLength;
    out.firstHeapPageId = firstHeapPageId;
    out.heapPageCount = heapPageCount;
    out.rowCount = rowCount;
    out.columns.clear();

    for (uint32_t i = 0; i < columnCount; ++i) {
        ColumnDefinition col;
        DbResult result = parseColumnRecord(bytes, length, offset, col);
        if (!result.isOk()) {
            return result;
        }
        out.columns.push_back(col);
    }

    // Ordinals must be exactly 0..columnCount-1.
    std::vector<bool> seen(columnCount, false);
    for (size_t i = 0; i < out.columns.size(); ++i) {
        uint32_t ord = out.columns[i].ordinal;
        if (ord >= columnCount || seen[ord]) {
            return DbResult::error(DbStatus::CorruptPage, "invalid or duplicate column ordinal");
        }
        seen[ord] = true;
    }
    // Column names must be unique within the table.
    for (size_t i = 0; i < out.columns.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (out.columns[j].name == out.columns[i].name) {
                return DbResult::error(DbStatus::CorruptPage, "duplicate column name");
            }
        }
    }
    return DbResult::ok();
}

DbResult Catalog::parseIndexRecord(const uint8_t* bytes, size_t length, size_t& offset,
                                   IndexRecord& out) {
    uint32_t indexId = 0;
    uint32_t tableId = 0;
    uint32_t columnOrdinal = 0;
    uint64_t rootPageId = 0;
    uint16_t flags = 0;
    uint16_t formatVersion = 0;
    uint64_t entryCount = 0;
    uint32_t nameLength = 0;

    if (!readField(bytes, length, offset, indexId) ||
        !readField(bytes, length, offset, tableId) ||
        !readField(bytes, length, offset, columnOrdinal) ||
        !readField(bytes, length, offset, rootPageId) ||
        !readField(bytes, length, offset, flags) ||
        !readField(bytes, length, offset, formatVersion) ||
        !readField(bytes, length, offset, entryCount) ||
        !readField(bytes, length, offset, nameLength)) {
        return DbResult::error(DbStatus::CorruptPage, "index record truncated");
    }
    if (indexId == 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid index id");
    }
    if (tableId == 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid index table id");
    }
    if ((flags & ~0x0003u) != 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid index flags");
    }
    if (formatVersion == 0 || formatVersion > 1) {
        return DbResult::error(DbStatus::CorruptPage, "unsupported index format version");
    }
    if (nameLength == 0 || nameLength > kMaxIndexNameBytes) {
        return DbResult::error(DbStatus::CorruptPage, "index name length out of range");
    }
    if (offset + nameLength > length) {
        return DbResult::error(DbStatus::CorruptPage, "index name extends beyond record");
    }
    if (!isValidUtf8(bytes + offset, nameLength)) {
        return DbResult::error(DbStatus::CorruptPage, "index name is not valid UTF-8");
    }

    out.indexId = indexId;
    out.tableId = tableId;
    out.columnOrdinal = columnOrdinal;
    out.rootPageId = rootPageId;
    out.flags = flags;
    out.formatVersion = formatVersion;
    out.entryCount = entryCount;
    out.name.assign(reinterpret_cast<const char*>(bytes + offset), nameLength);
    offset += nameLength;
    return DbResult::ok();
}

void Catalog::serializeColumnRecord(const ColumnDefinition& col,
                                    std::vector<uint8_t>& out) const {
    const size_t base = out.size();
    out.resize(base + 16);
    storeLe32(out.data() + base + 0, col.ordinal);
    storeLe16(out.data() + base + 4, static_cast<uint16_t>(col.type));
    storeLe16(out.data() + base + 6, col.nullable ? 1u : 0u);
    storeLe32(out.data() + base + 8, 0); // flags
    storeLe32(out.data() + base + 12, static_cast<uint32_t>(col.name.size()));
    out.insert(out.end(), col.name.begin(), col.name.end());
}

void Catalog::serializeTableRecord(const TableRecord& rec, std::vector<uint8_t>& out) const {
    const size_t base = out.size();
    out.resize(base + 40);
    storeLe32(out.data() + base + 0, rec.tableId);
    storeLe32(out.data() + base + 4, rec.schemaVersion);
    storeLe32(out.data() + base + 8, static_cast<uint32_t>(rec.columns.size()));
    storeLe64(out.data() + base + 12, rec.firstHeapPageId);
    storeLe32(out.data() + base + 20, rec.heapPageCount);
    storeLe64(out.data() + base + 24, rec.rowCount);
    storeLe32(out.data() + base + 32, 0); // flags
    storeLe32(out.data() + base + 36, static_cast<uint32_t>(rec.name.size()));
    out.insert(out.end(), rec.name.begin(), rec.name.end());
    for (size_t i = 0; i < rec.columns.size(); ++i) {
        serializeColumnRecord(rec.columns[i], out);
    }
}

void Catalog::serializeIndexRecord(const IndexRecord& rec,
                                   std::vector<uint8_t>& out) const {
    const size_t base = out.size();
    out.resize(base + 36);
    storeLe32(out.data() + base + 0, rec.indexId);
    storeLe32(out.data() + base + 4, rec.tableId);
    storeLe32(out.data() + base + 8, rec.columnOrdinal);
    storeLe64(out.data() + base + 12, rec.rootPageId);
    storeLe16(out.data() + base + 20, rec.flags);
    storeLe16(out.data() + base + 22, rec.formatVersion);
    storeLe64(out.data() + base + 24, rec.entryCount);
    storeLe32(out.data() + base + 32, static_cast<uint32_t>(rec.name.size()));
    out.insert(out.end(), rec.name.begin(), rec.name.end());
}

DbResult Catalog::load(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize,
                       uint64_t pageCount) {
    _tables.clear();
    _indexes.clear();
    _nextTableId = 1;
    _nextIndexId = 1;
    _version = kCatalogVersion;
    _continuationPageIds.clear();

    if (pageSize < kPageHeaderSize + kCatalogRootHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "page too small for catalog header");
    }

    DatabasePage root;
    DbResult result = pages.readPage(rootPageId, root);
    if (!result.isOk()) {
        return result;
    }

    const std::vector<uint8_t>& payload = root.payload;
    if (payload.size() < kCatalogPayloadSize) {
        return DbResult::error(DbStatus::CorruptPage, "catalog payload too small");
    }
    if (loadLe32(payload.data() + 0) != kCatalogMagic) {
        return DbResult::error(DbStatus::CorruptPage, "catalog signature mismatch");
    }
    const uint16_t version = loadLe16(payload.data() + 4);
    if (version == kCatalogVersionLegacy) {
        // SQL1 database: empty catalog. nextTableId starts at 1.
        _version = kCatalogVersionLegacy;
        return DbResult::ok();
    }
    if (version == kCatalogVersion) {
        return loadV2(pages, payload, rootPageId, pageSize, pageCount);
    }
    if (version == kCatalogVersionV3) {
        return loadV3(pages, payload, rootPageId, pageSize, pageCount);
    }
    return DbResult::error(DbStatus::UnsupportedVersion,
                           "unsupported catalog version " +
                               std::to_string(static_cast<unsigned>(version)));
}

DbResult Catalog::loadV2(PageAccess& pages, const std::vector<uint8_t>& payload,
                         uint64_t rootPageId, uint32_t pageSize, uint64_t pageCount) {
    (void)rootPageId;
    (void)pageSize;
    if (payload.size() < kCatalogRootHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "catalog root header truncated");
    }

    const uint32_t tableCount = loadLe32(payload.data() + 8);
    _nextTableId = loadLe32(payload.data() + 12);
    const uint32_t continuationCount = loadLe32(payload.data() + 16);
    const uint64_t firstContinuation = loadLe32(payload.data() + 20);
    const uint32_t rootRecordCount = loadLe32(payload.data() + 24);

    if (tableCount > kMaxTables) {
        return DbResult::error(DbStatus::CorruptPage, "table count exceeds policy maximum");
    }
    if (_nextTableId == 0) {
        _nextTableId = 1;
    }
    if (continuationCount > pageCount) {
        return DbResult::error(DbStatus::CorruptPage, "continuation count exceeds page count");
    }
    if (continuationCount == 0 && firstContinuation != 0) {
        return DbResult::error(DbStatus::CorruptPage, "continuation pointer without continuations");
    }
    if (rootRecordCount > tableCount) {
        return DbResult::error(DbStatus::CorruptPage, "root record count exceeds table count");
    }

    // Parse the root page's table records.
    size_t offset = kCatalogRootHeaderSize;
    for (uint32_t i = 0; i < rootRecordCount; ++i) {
        TableRecord rec;
        DbResult result = parseTableRecord(payload.data(), payload.size(), offset, rec);
        if (!result.isOk()) {
            return result;
        }
        _tables.push_back(rec);
    }

    // Follow the continuation chain.
    std::vector<bool> visited(pageCount, false);
    uint64_t expectedNext = firstContinuation;
    for (uint32_t i = 0; i < continuationCount; ++i) {
        if (expectedNext == 0 || expectedNext >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "invalid catalog continuation page");
        }
        if (visited[expectedNext]) {
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation cycle");
        }
        visited[expectedNext] = true;
        _continuationPageIds.push_back(expectedNext);

        DatabasePage cont;
        DbResult result = pages.readPage(expectedNext, cont);
        if (!result.isOk()) {
            return result;
        }
        const std::vector<uint8_t>& cpayload = cont.payload;
        if (cpayload.size() < kCatalogContHeaderSize) {
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation header truncated");
        }
        if (loadLe32(cpayload.data() + 0) != kCatalogMagic) {
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation signature mismatch");
        }
        if (loadLe16(cpayload.data() + 4) != kCatalogVersion) {
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation version mismatch");
        }
        const uint64_t next = loadLe64(cpayload.data() + 8);
        const uint32_t recordCount = loadLe32(cpayload.data() + 16);
        if (recordCount > kMaxTables) {
            return DbResult::error(DbStatus::CorruptPage, "continuation record count too large");
        }

        size_t coffset = kCatalogContHeaderSize;
        for (uint32_t j = 0; j < recordCount; ++j) {
            TableRecord rec;
            DbResult result = parseTableRecord(cpayload.data(), cpayload.size(), coffset, rec);
            if (!result.isOk()) {
                return result;
            }
            _tables.push_back(rec);
        }
        expectedNext = next;
    }

    if (expectedNext != 0) {
        return DbResult::error(DbStatus::CorruptPage, "catalog continuation chain length mismatch");
    }

    if (_tables.size() != tableCount) {
        return DbResult::error(DbStatus::CorruptPage, "catalog record count mismatch");
    }

    // Cross-record validation: unique table ids and names.
    for (size_t i = 0; i < _tables.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (_tables[j].tableId == _tables[i].tableId) {
                return DbResult::error(DbStatus::CorruptPage, "duplicate table id");
            }
            if (_tables[j].name == _tables[i].name) {
                return DbResult::error(DbStatus::CorruptPage, "duplicate table name");
            }
        }
    }
    _version = kCatalogVersion;
    return DbResult::ok();
}

DbResult Catalog::loadV3(PageAccess& pages, const std::vector<uint8_t>& payload,
                         uint64_t rootPageId, uint32_t pageSize, uint64_t pageCount) {
    (void)rootPageId;
    (void)pageSize;
    if (payload.size() < kCatalogV3RootHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "catalog v3 root header truncated");
    }
    const uint32_t tableCount = loadLe32(payload.data() + 8);
    _nextTableId = loadLe32(payload.data() + 12);
    const uint32_t continuationCount = loadLe32(payload.data() + 16);
    const uint64_t firstContinuation = loadLe32(payload.data() + 20);
    const uint32_t rootRecordCount = loadLe32(payload.data() + 24);
    const uint32_t indexCount = loadLe32(payload.data() + 28);
    _nextIndexId = loadLe32(payload.data() + 32);

    if (tableCount > kMaxTables) {
        return DbResult::error(DbStatus::CorruptPage, "table count exceeds policy maximum");
    }
    if (indexCount > kMaxIndexes) {
        return DbResult::error(DbStatus::CorruptPage, "index count exceeds policy maximum");
    }
    if (_nextTableId == 0) {
        _nextTableId = 1;
    }
    if (_nextIndexId == 0) {
        _nextIndexId = 1;
    }
    if (continuationCount > pageCount) {
        return DbResult::error(DbStatus::CorruptPage, "continuation count exceeds page count");
    }
    if (continuationCount == 0 && firstContinuation != 0) {
        return DbResult::error(DbStatus::CorruptPage, "continuation pointer without continuations");
    }
    if (rootRecordCount > tableCount + indexCount) {
        return DbResult::error(DbStatus::CorruptPage, "root record count exceeds total records");
    }

    // Parse the root page's type-tagged records.
    size_t offset = kCatalogV3RootHeaderSize;
    for (uint32_t i = 0; i < rootRecordCount; ++i) {
        if (offset >= payload.size()) {
            return DbResult::error(DbStatus::CorruptPage, "catalog v3 record type truncated");
        }
        const uint8_t type = payload[offset++];
        if (type == kCatalogRecordTable) {
            TableRecord rec;
            DbResult result = parseTableRecord(payload.data(), payload.size(), offset, rec);
            if (!result.isOk()) {
                return result;
            }
            _tables.push_back(rec);
        } else if (type == kCatalogRecordIndex) {
            IndexRecord rec;
            DbResult result = parseIndexRecord(payload.data(), payload.size(), offset, rec);
            if (!result.isOk()) {
                return result;
            }
            _indexes.push_back(rec);
        } else {
            return DbResult::error(DbStatus::CorruptPage, "unknown catalog record type");
        }
    }

    // Follow the continuation chain.
    std::vector<bool> visited(pageCount, false);
    uint64_t expectedNext = firstContinuation;
    for (uint32_t i = 0; i < continuationCount; ++i) {
        if (expectedNext == 0 || expectedNext >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "invalid catalog continuation page");
        }
        if (visited[expectedNext]) {
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation cycle");
        }
        visited[expectedNext] = true;
        _continuationPageIds.push_back(expectedNext);

        DatabasePage cont;
        DbResult result = pages.readPage(expectedNext, cont);
        if (!result.isOk()) {
            return result;
        }
        const std::vector<uint8_t>& cpayload = cont.payload;
        if (cpayload.size() < kCatalogV3ContHeaderSize) {
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation header truncated");
        }
        if (loadLe32(cpayload.data() + 0) != kCatalogMagic) {
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation signature mismatch");
        }
        if (loadLe16(cpayload.data() + 4) != kCatalogVersionV3) {
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation version mismatch");
        }
        const uint64_t next = loadLe64(cpayload.data() + 8);
        const uint32_t recordCount = loadLe32(cpayload.data() + 16);
        if (recordCount > kMaxTables + kMaxIndexes) {
            return DbResult::error(DbStatus::CorruptPage, "continuation record count too large");
        }

        size_t coffset = kCatalogV3ContHeaderSize;
        for (uint32_t j = 0; j < recordCount; ++j) {
            if (coffset >= cpayload.size()) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "catalog v3 record type truncated");
            }
            const uint8_t type = cpayload[coffset++];
            if (type == kCatalogRecordTable) {
                TableRecord rec;
                DbResult result =
                    parseTableRecord(cpayload.data(), cpayload.size(), coffset, rec);
                if (!result.isOk()) {
                    return result;
                }
                _tables.push_back(rec);
            } else if (type == kCatalogRecordIndex) {
                IndexRecord rec;
                DbResult result =
                    parseIndexRecord(cpayload.data(), cpayload.size(), coffset, rec);
                if (!result.isOk()) {
                    return result;
                }
                _indexes.push_back(rec);
            } else {
                return DbResult::error(DbStatus::CorruptPage, "unknown catalog record type");
            }
        }
        expectedNext = next;
    }

    if (expectedNext != 0) {
        return DbResult::error(DbStatus::CorruptPage,
                               "catalog continuation chain length mismatch");
    }
    if (_tables.size() != tableCount) {
        return DbResult::error(DbStatus::CorruptPage, "catalog table count mismatch");
    }
    if (_indexes.size() != indexCount) {
        return DbResult::error(DbStatus::CorruptPage, "catalog index count mismatch");
    }

    // Cross-record validation: unique table ids/names, unique index ids/names,
    // and index references that resolve to a real indexable column.
    for (size_t i = 0; i < _tables.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (_tables[j].tableId == _tables[i].tableId) {
                return DbResult::error(DbStatus::CorruptPage, "duplicate table id");
            }
            if (_tables[j].name == _tables[i].name) {
                return DbResult::error(DbStatus::CorruptPage, "duplicate table name");
            }
        }
    }
    for (size_t i = 0; i < _indexes.size(); ++i) {
        const IndexRecord& idx = _indexes[i];
        const TableRecord* table = findTable(idx.tableId);
        if (table == nullptr) {
            return DbResult::error(DbStatus::CorruptPage, "index references unknown table");
        }
        if (idx.columnOrdinal >= table->columns.size()) {
            return DbResult::error(DbStatus::CorruptPage, "index references unknown column");
        }
        if (!isIndexableType(table->columns[idx.columnOrdinal].type)) {
            return DbResult::error(DbStatus::CorruptPage, "index column type is not indexable");
        }
        if (idx.rootPageId >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "index root page out of range");
        }
        for (size_t j = 0; j < i; ++j) {
            if (_indexes[j].indexId == idx.indexId) {
                return DbResult::error(DbStatus::CorruptPage, "duplicate index id");
            }
            if (_indexes[j].name == idx.name) {
                return DbResult::error(DbStatus::CorruptPage, "duplicate index name");
            }
        }
    }
    _version = kCatalogVersionV3;
    return DbResult::ok();
}

DbResult Catalog::save(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize) {
    const bool useV3 = _version == kCatalogVersionV3 || !_indexes.empty();
    if (useV3) {
        return saveV3(pages, rootPageId, pageSize);
    }
    return saveV2(pages, rootPageId, pageSize);
}

DbResult Catalog::saveV2(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize) {
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t rootCap = capacity - kCatalogRootHeaderSize;
    const uint32_t contCap = capacity - kCatalogContHeaderSize;

    // Serialize all table records.
    std::vector<std::vector<uint8_t>> records;
    records.reserve(_tables.size());
    for (size_t i = 0; i < _tables.size(); ++i) {
        std::vector<uint8_t> bytes;
        serializeTableRecord(_tables[i], bytes);
        records.push_back(std::move(bytes));
    }

    std::vector<std::vector<uint8_t>> pagePayloads;
    std::vector<uint32_t> pageRecordCounts;

    std::vector<uint8_t> current(rootCap, 0);
    storeLe32(current.data() + 0, kCatalogMagic);
    storeLe16(current.data() + 4, kCatalogVersion);
    storeLe16(current.data() + 6, 0);
    storeLe32(current.data() + 8, 0);
    storeLe32(current.data() + 12, 0);
    storeLe32(current.data() + 16, 0);
    storeLe32(current.data() + 20, 0);
    storeLe32(current.data() + 24, 0);
    storeLe32(current.data() + 28, 0);
    size_t currentOffset = kCatalogRootHeaderSize;
    uint32_t currentCap = rootCap;
    uint32_t currentRecords = 0;

    for (size_t i = 0; i < records.size(); ++i) {
        const std::vector<uint8_t>& rec = records[i];
        if (currentOffset + rec.size() <= currentCap) {
            std::memcpy(current.data() + currentOffset, rec.data(), rec.size());
            currentOffset += rec.size();
            ++currentRecords;
            continue;
        }
        pagePayloads.push_back(current);
        pageRecordCounts.push_back(currentRecords);

        if (rec.size() > contCap - kCatalogContHeaderSize) {
            return DbResult::error(DbStatus::Internal,
                                   "table record exceeds catalog page capacity");
        }
        current.assign(contCap, 0);
        storeLe32(current.data() + 0, kCatalogMagic);
        storeLe16(current.data() + 4, kCatalogVersion);
        storeLe16(current.data() + 6, 0);
        storeLe64(current.data() + 8, 0);
        storeLe32(current.data() + 16, 0);
        storeLe32(current.data() + 20, 0);
        currentOffset = kCatalogContHeaderSize;
        currentCap = contCap;
        currentRecords = 0;

        std::memcpy(current.data() + currentOffset, rec.data(), rec.size());
        currentOffset += rec.size();
        ++currentRecords;
    }
    pagePayloads.push_back(current);
    pageRecordCounts.push_back(currentRecords);

    const uint32_t pageTotal = static_cast<uint32_t>(pagePayloads.size());

    std::vector<uint64_t> contIds;
    contIds.reserve(pageTotal > 0 ? pageTotal - 1 : 0);
    for (uint32_t i = 1; i < pageTotal; ++i) {
        if (i - 1 < _continuationPageIds.size()) {
            contIds.push_back(_continuationPageIds[i - 1]);
        } else {
            uint64_t newId = 0;
            DbResult result = pages.allocatePage(PageType::Catalog, newId);
            if (!result.isOk()) {
                return result;
            }
            contIds.push_back(newId);
        }
    }

    {
        std::vector<uint8_t>& rootPayload = pagePayloads[0];
        storeLe32(rootPayload.data() + 8, static_cast<uint32_t>(_tables.size()));
        storeLe32(rootPayload.data() + 12, _nextTableId);
        storeLe32(rootPayload.data() + 16, static_cast<uint32_t>(contIds.size()));
        storeLe32(rootPayload.data() + 20,
                  contIds.empty() ? 0u : static_cast<uint32_t>(contIds[0]));
        storeLe32(rootPayload.data() + 24, pageRecordCounts[0]);
    }

    for (uint32_t i = 0; i < contIds.size(); ++i) {
        std::vector<uint8_t>& cp = pagePayloads[i + 1];
        const uint64_t next = (i + 1 < contIds.size()) ? contIds[i + 1] : 0;
        storeLe64(cp.data() + 8, next);
        storeLe32(cp.data() + 16, pageRecordCounts[i + 1]);
    }

    {
        DatabasePage rootPage;
        DbResult result = pages.readPage(rootPageId, rootPage);
        if (!result.isOk()) {
            return result;
        }
        rootPage.pageId = rootPageId;
        rootPage.payload = pagePayloads[0];
        rootPage.payloadSize = static_cast<uint32_t>(pagePayloads[0].size());
        rootPage.type = PageType::Catalog;
        result = pages.writePage(rootPage);
        if (!result.isOk()) {
            return result;
        }
    }

    for (uint32_t i = 0; i < contIds.size(); ++i) {
        DatabasePage contPage;
        DbResult result = pages.readPage(contIds[i], contPage);
        if (!result.isOk()) {
            return result;
        }
        contPage.pageId = contIds[i];
        contPage.payload = pagePayloads[i + 1];
        contPage.payloadSize = static_cast<uint32_t>(pagePayloads[i + 1].size());
        contPage.type = PageType::Catalog;
        result = pages.writePage(contPage);
        if (!result.isOk()) {
            return result;
        }
    }

    _continuationPageIds = contIds;
    _version = kCatalogVersion;
    return DbResult::ok();
}

DbResult Catalog::saveV3(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize) {
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t rootCap = capacity - kCatalogV3RootHeaderSize;
    const uint32_t contCap = capacity - kCatalogV3ContHeaderSize;

    // Serialize type-tagged records: table records first, then index records.
    std::vector<std::vector<uint8_t>> records;
    records.reserve(_tables.size() + _indexes.size());
    for (size_t i = 0; i < _tables.size(); ++i) {
        std::vector<uint8_t> bytes;
        bytes.push_back(kCatalogRecordTable);
        serializeTableRecord(_tables[i], bytes);
        records.push_back(std::move(bytes));
    }
    for (size_t i = 0; i < _indexes.size(); ++i) {
        std::vector<uint8_t> bytes;
        bytes.push_back(kCatalogRecordIndex);
        serializeIndexRecord(_indexes[i], bytes);
        records.push_back(std::move(bytes));
    }

    std::vector<std::vector<uint8_t>> pagePayloads;
    std::vector<uint32_t> pageRecordCounts;

    std::vector<uint8_t> current(rootCap, 0);
    storeLe32(current.data() + 0, kCatalogMagic);
    storeLe16(current.data() + 4, kCatalogVersionV3);
    storeLe16(current.data() + 6, 0);
    storeLe32(current.data() + 8, 0);  // tableCount
    storeLe32(current.data() + 12, 0); // nextTableId
    storeLe32(current.data() + 16, 0); // continuationCount
    storeLe32(current.data() + 20, 0); // firstContinuation
    storeLe32(current.data() + 24, 0); // rootRecordCount
    storeLe32(current.data() + 28, 0); // indexCount
    storeLe32(current.data() + 32, 0); // nextIndexId
    storeLe32(current.data() + 36, 0); // reserved
    size_t currentOffset = kCatalogV3RootHeaderSize;
    uint32_t currentCap = rootCap;
    uint32_t currentRecords = 0;

    for (size_t i = 0; i < records.size(); ++i) {
        const std::vector<uint8_t>& rec = records[i];
        if (currentOffset + rec.size() <= currentCap) {
            std::memcpy(current.data() + currentOffset, rec.data(), rec.size());
            currentOffset += rec.size();
            ++currentRecords;
            continue;
        }
        pagePayloads.push_back(current);
        pageRecordCounts.push_back(currentRecords);

        if (rec.size() > contCap - kCatalogV3ContHeaderSize) {
            return DbResult::error(DbStatus::Internal,
                                   "catalog record exceeds page capacity");
        }
        current.assign(contCap, 0);
        storeLe32(current.data() + 0, kCatalogMagic);
        storeLe16(current.data() + 4, kCatalogVersionV3);
        storeLe16(current.data() + 6, 0);
        storeLe64(current.data() + 8, 0);
        storeLe32(current.data() + 16, 0);
        storeLe32(current.data() + 20, 0);
        currentOffset = kCatalogV3ContHeaderSize;
        currentCap = contCap;
        currentRecords = 0;

        std::memcpy(current.data() + currentOffset, rec.data(), rec.size());
        currentOffset += rec.size();
        ++currentRecords;
    }
    pagePayloads.push_back(current);
    pageRecordCounts.push_back(currentRecords);

    const uint32_t pageTotal = static_cast<uint32_t>(pagePayloads.size());

    std::vector<uint64_t> contIds;
    contIds.reserve(pageTotal > 0 ? pageTotal - 1 : 0);
    for (uint32_t i = 1; i < pageTotal; ++i) {
        if (i - 1 < _continuationPageIds.size()) {
            contIds.push_back(_continuationPageIds[i - 1]);
        } else {
            uint64_t newId = 0;
            DbResult result = pages.allocatePage(PageType::Catalog, newId);
            if (!result.isOk()) {
                return result;
            }
            contIds.push_back(newId);
        }
    }

    {
        std::vector<uint8_t>& rootPayload = pagePayloads[0];
        storeLe32(rootPayload.data() + 8, static_cast<uint32_t>(_tables.size()));
        storeLe32(rootPayload.data() + 12, _nextTableId);
        storeLe32(rootPayload.data() + 16, static_cast<uint32_t>(contIds.size()));
        storeLe32(rootPayload.data() + 20,
                  contIds.empty() ? 0u : static_cast<uint32_t>(contIds[0]));
        storeLe32(rootPayload.data() + 24, pageRecordCounts[0]);
        storeLe32(rootPayload.data() + 28, static_cast<uint32_t>(_indexes.size()));
        storeLe32(rootPayload.data() + 32, _nextIndexId);
    }

    for (uint32_t i = 0; i < contIds.size(); ++i) {
        std::vector<uint8_t>& cp = pagePayloads[i + 1];
        const uint64_t next = (i + 1 < contIds.size()) ? contIds[i + 1] : 0;
        storeLe64(cp.data() + 8, next);
        storeLe32(cp.data() + 16, pageRecordCounts[i + 1]);
    }

    {
        DatabasePage rootPage;
        DbResult result = pages.readPage(rootPageId, rootPage);
        if (!result.isOk()) {
            return result;
        }
        rootPage.pageId = rootPageId;
        rootPage.payload = pagePayloads[0];
        rootPage.payloadSize = static_cast<uint32_t>(pagePayloads[0].size());
        rootPage.type = PageType::Catalog;
        result = pages.writePage(rootPage);
        if (!result.isOk()) {
            return result;
        }
    }

    for (uint32_t i = 0; i < contIds.size(); ++i) {
        DatabasePage contPage;
        DbResult result = pages.readPage(contIds[i], contPage);
        if (!result.isOk()) {
            return result;
        }
        contPage.pageId = contIds[i];
        contPage.payload = pagePayloads[i + 1];
        contPage.payloadSize = static_cast<uint32_t>(pagePayloads[i + 1].size());
        contPage.type = PageType::Catalog;
        result = pages.writePage(contPage);
        if (!result.isOk()) {
            return result;
        }
    }

    _continuationPageIds = contIds;
    _version = kCatalogVersionV3;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
