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

} // namespace

Catalog::Catalog() : _nextTableId(1) {}

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

uint32_t Catalog::recordSizeForDefinition(const TableDefinition& def) const {
    uint32_t size = 40 + static_cast<uint32_t>(def.name.size());
    for (size_t i = 0; i < def.columns.size(); ++i) {
        size += 16 + static_cast<uint32_t>(def.columns[i].name.size());
    }
    return size;
}

DbResult Catalog::addTable(const TableDefinition& def, uint32_t pageSize, uint32_t& outTableId) {
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

    // The serialized record must fit in a catalog continuation page.
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t contCap = capacity - kCatalogContHeaderSize;
    if (recordSizeForDefinition(def) > contCap) {
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
    _tables.push_back(rec);
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

DbResult Catalog::load(BufferManager& buffer, uint64_t rootPageId, uint32_t pageSize,
                       uint64_t pageCount) {
    _tables.clear();
    _nextTableId = 1;
    _continuationPageIds.clear();

    if (pageSize < kPageHeaderSize + kCatalogRootHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "page too small for catalog header");
    }

    BufferSlot* root = nullptr;
    DbResult result = buffer.pinPage(rootPageId, root);
    if (!result.isOk()) {
        return result;
    }

    const std::vector<uint8_t>& payload = root->page.payload;
    if (payload.size() < kCatalogPayloadSize) {
        buffer.unpin(*root);
        return DbResult::error(DbStatus::CorruptPage, "catalog payload too small");
    }
    if (loadLe32(payload.data() + 0) != kCatalogMagic) {
        buffer.unpin(*root);
        return DbResult::error(DbStatus::CorruptPage, "catalog signature mismatch");
    }
    const uint16_t version = loadLe16(payload.data() + 4);
    if (version == kCatalogVersionLegacy) {
        // SQL1 database: empty catalog. nextTableId starts at 1.
        buffer.unpin(*root);
        return DbResult::ok();
    }
    if (version != kCatalogVersion) {
        buffer.unpin(*root);
        return DbResult::error(DbStatus::UnsupportedVersion,
                               "unsupported catalog version " +
                                   std::to_string(static_cast<unsigned>(version)));
    }
    if (payload.size() < kCatalogRootHeaderSize) {
        buffer.unpin(*root);
        return DbResult::error(DbStatus::CorruptPage, "catalog root header truncated");
    }

    const uint32_t tableCount = loadLe32(payload.data() + 8);
    _nextTableId = loadLe32(payload.data() + 12);
    const uint32_t continuationCount = loadLe32(payload.data() + 16);
    const uint64_t firstContinuation = loadLe64(payload.data() + 20);

    if (tableCount > kMaxTables) {
        buffer.unpin(*root);
        return DbResult::error(DbStatus::CorruptPage, "table count exceeds policy maximum");
    }
    if (_nextTableId == 0) {
        _nextTableId = 1;
    }
    if (continuationCount > pageCount) {
        buffer.unpin(*root);
        return DbResult::error(DbStatus::CorruptPage, "continuation count exceeds page count");
    }
    if (continuationCount == 0 && firstContinuation != 0) {
        buffer.unpin(*root);
        return DbResult::error(DbStatus::CorruptPage, "continuation pointer without continuations");
    }

    // Parse the root page's table records.
    size_t offset = kCatalogRootHeaderSize;
    for (uint32_t i = 0; i < tableCount; ++i) {
        TableRecord rec;
        result = parseTableRecord(payload.data(), payload.size(), offset, rec);
        if (!result.isOk()) {
            buffer.unpin(*root);
            return result;
        }
        _tables.push_back(rec);
    }

    // The root page may also hold records beyond tableCount only if the on-disk
    // tableCount was inconsistent; any leftover bytes are rejected.
    if (offset != payload.size()) {
        buffer.unpin(*root);
        return DbResult::error(DbStatus::CorruptPage, "trailing bytes in catalog root page");
    }

    buffer.unpin(*root);

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

        BufferSlot* cont = nullptr;
        result = buffer.pinPage(expectedNext, cont);
        if (!result.isOk()) {
            return result;
        }
        const std::vector<uint8_t>& cpayload = cont->page.payload;
        if (cpayload.size() < kCatalogContHeaderSize) {
            buffer.unpin(*cont);
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation header truncated");
        }
        if (loadLe32(cpayload.data() + 0) != kCatalogMagic) {
            buffer.unpin(*cont);
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation signature mismatch");
        }
        if (loadLe16(cpayload.data() + 4) != kCatalogVersion) {
            buffer.unpin(*cont);
            return DbResult::error(DbStatus::CorruptPage, "catalog continuation version mismatch");
        }
        const uint64_t next = loadLe64(cpayload.data() + 8);
        const uint32_t recordCount = loadLe32(cpayload.data() + 16);
        if (recordCount > kMaxTables) {
            buffer.unpin(*cont);
            return DbResult::error(DbStatus::CorruptPage, "continuation record count too large");
        }

        size_t coffset = kCatalogContHeaderSize;
        for (uint32_t j = 0; j < recordCount; ++j) {
            TableRecord rec;
            result = parseTableRecord(cpayload.data(), cpayload.size(), coffset, rec);
            if (!result.isOk()) {
                buffer.unpin(*cont);
                return result;
            }
            _tables.push_back(rec);
        }
        if (coffset != cpayload.size()) {
            buffer.unpin(*cont);
            return DbResult::error(DbStatus::CorruptPage,
                                   "trailing bytes in catalog continuation page");
        }
        expectedNext = next;
        buffer.unpin(*cont);
    }

    if (expectedNext != 0) {
        return DbResult::error(DbStatus::CorruptPage, "catalog continuation chain length mismatch");
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
    return DbResult::ok();
}

DbResult Catalog::save(BufferManager& buffer, DatabaseFile& file, uint64_t rootPageId,
                       uint32_t pageSize) {
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

    // Pack records into page payloads (root first, then continuations).
    std::vector<std::vector<uint8_t>> pagePayloads;
    std::vector<uint32_t> pageRecordCounts;

    std::vector<uint8_t> current(rootCap, 0);
    storeLe32(current.data() + 0, kCatalogMagic);
    storeLe16(current.data() + 4, kCatalogVersion);
    storeLe16(current.data() + 6, 0); // reserved
    storeLe32(current.data() + 8, 0); // tableCount (patched later)
    storeLe32(current.data() + 12, 0); // nextTableId (patched later)
    storeLe32(current.data() + 16, 0); // continuationCount (patched later)
    storeLe32(current.data() + 20, 0); // firstContinuation (patched later)
    storeLe32(current.data() + 24, 0); // reserved
    storeLe32(current.data() + 28, 0); // reserved
    size_t currentOffset = kCatalogRootHeaderSize;
    uint32_t currentCap = rootCap;
    uint32_t currentRecords = 0;
    bool currentIsRoot = true;

    for (size_t i = 0; i < records.size(); ++i) {
        const std::vector<uint8_t>& rec = records[i];
        if (currentOffset + rec.size() <= currentCap) {
            std::memcpy(current.data() + currentOffset, rec.data(), rec.size());
            currentOffset += rec.size();
            ++currentRecords;
            continue;
        }
        // Finalize the current page and start a new continuation page.
        pagePayloads.push_back(current);
        pageRecordCounts.push_back(currentRecords);

        if (rec.size() > contCap - kCatalogContHeaderSize) {
            return DbResult::error(DbStatus::Internal,
                                   "table record exceeds catalog page capacity");
        }
        current.assign(contCap, 0);
        storeLe32(current.data() + 0, kCatalogMagic);
        storeLe16(current.data() + 4, kCatalogVersion);
        storeLe16(current.data() + 6, 0); // reserved
        storeLe64(current.data() + 8, 0); // next continuation (patched later)
        storeLe32(current.data() + 16, 0); // record count (patched later)
        storeLe32(current.data() + 20, 0); // reserved
        currentOffset = kCatalogContHeaderSize;
        currentCap = contCap;
        currentRecords = 0;
        currentIsRoot = false;

        std::memcpy(current.data() + currentOffset, rec.data(), rec.size());
        currentOffset += rec.size();
        ++currentRecords;
    }
    pagePayloads.push_back(current);
    pageRecordCounts.push_back(currentRecords);
    (void)currentIsRoot;

    const uint32_t pageTotal = static_cast<uint32_t>(pagePayloads.size());

    // Assign continuation page ids: reuse existing pages, allocate new ones.
    std::vector<uint64_t> contIds;
    contIds.reserve(pageTotal > 0 ? pageTotal - 1 : 0);
    for (uint32_t i = 1; i < pageTotal; ++i) {
        if (i - 1 < _continuationPageIds.size()) {
            contIds.push_back(_continuationPageIds[i - 1]);
        } else {
            uint64_t newId = 0;
            DbResult result = file.allocatePage(PageType::Catalog, newId);
            if (!result.isOk()) {
                return result;
            }
            contIds.push_back(newId);
        }
    }

    // Patch the root header.
    {
        std::vector<uint8_t>& rootPayload = pagePayloads[0];
        storeLe32(rootPayload.data() + 8, static_cast<uint32_t>(_tables.size()));
        storeLe32(rootPayload.data() + 12, _nextTableId);
        storeLe32(rootPayload.data() + 16, static_cast<uint32_t>(contIds.size()));
        storeLe32(rootPayload.data() + 20,
                  contIds.empty() ? 0u : static_cast<uint32_t>(contIds[0]));
    }

    // Patch continuation headers (next pointer + record count).
    for (uint32_t i = 0; i < contIds.size(); ++i) {
        std::vector<uint8_t>& cp = pagePayloads[i + 1];
        const uint64_t next = (i + 1 < contIds.size()) ? contIds[i + 1] : 0;
        storeLe64(cp.data() + 8, next);
        storeLe32(cp.data() + 16, pageRecordCounts[i + 1]);
    }

    // Write the root page.
    {
        BufferSlot* slot = nullptr;
        DbResult result = buffer.pinPage(rootPageId, slot);
        if (!result.isOk()) {
            return result;
        }
        slot->page.payload = pagePayloads[0];
        slot->page.payloadSize = static_cast<uint32_t>(pagePayloads[0].size());
        slot->page.type = PageType::Catalog;
        buffer.markDirty(*slot);
        buffer.unpin(*slot);
    }

    // Write continuation pages.
    for (uint32_t i = 0; i < contIds.size(); ++i) {
        BufferSlot* slot = nullptr;
        DbResult result = buffer.pinPage(contIds[i], slot);
        if (!result.isOk()) {
            return result;
        }
        slot->page.payload = pagePayloads[i + 1];
        slot->page.payloadSize = static_cast<uint32_t>(pagePayloads[i + 1].size());
        slot->page.type = PageType::Catalog;
        buffer.markDirty(*slot);
        buffer.unpin(*slot);
    }

    _continuationPageIds = contIds;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
