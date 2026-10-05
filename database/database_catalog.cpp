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

Catalog::Catalog() : _tables(), _indexes(), _foreignKeys(), _nextTableId(1),
                     _nextIndexId(1), _nextForeignKeyId(1), _version(kCatalogVersion) {}

bool Catalog::validateName(const std::string& name, uint32_t maxBytes) const {
    if (name.empty() || name.size() > maxBytes) {
        return false;
    }
    // A leading '$' is reserved for system-generated names (constraint-backed
    // and foreign-key support indexes). User-defined objects may not use it.
    if (name[0] == '$') {
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

const Catalog::ForeignKeyRecord* Catalog::findForeignKey(uint32_t foreignKeyId) const {
    for (size_t i = 0; i < _foreignKeys.size(); ++i) {
        if (_foreignKeys[i].foreignKeyId == foreignKeyId) {
            return &_foreignKeys[i];
        }
    }
    return nullptr;
}

std::vector<Catalog::ForeignKeyRecord> Catalog::foreignKeysForChildTable(uint32_t tableId) const {
    std::vector<ForeignKeyRecord> out;
    for (size_t i = 0; i < _foreignKeys.size(); ++i) {
        if (_foreignKeys[i].childTableId == tableId) {
            out.push_back(_foreignKeys[i]);
        }
    }
    return out;
}

std::vector<Catalog::ForeignKeyRecord> Catalog::foreignKeysForParentTable(uint32_t tableId) const {
    std::vector<ForeignKeyRecord> out;
    for (size_t i = 0; i < _foreignKeys.size(); ++i) {
        if (_foreignKeys[i].parentTableId == tableId) {
            out.push_back(_foreignKeys[i]);
        }
    }
    return out;
}

uint32_t Catalog::recordSizeForDefinition(const TableDefinition& def, bool v4) const {
    uint32_t size = 40 + static_cast<uint32_t>(def.name.size());
    for (size_t i = 0; i < def.columns.size(); ++i) {
        size += 16 + static_cast<uint32_t>(def.columns[i].name.size());
        if (v4) {
            size += 1; // hasDefault flag
            if (def.columns[i].hasDefault) {
                size += 2; // default type + null flag
                const DbValue& v = def.columns[i].defaultValue;
                if (!v.isNull()) {
                    switch (v.type()) {
                    case DbType::Boolean:
                        size += 1;
                        break;
                    case DbType::Int32:
                        size += 4;
                        break;
                    case DbType::Int64:
                    case DbType::Float64:
                        size += 8;
                        break;
                    case DbType::Text:
                        size += 4 + static_cast<uint32_t>(v.textValue().size());
                        break;
                    case DbType::Blob:
                        size += 4 + static_cast<uint32_t>(v.blobValue().size());
                        break;
                    default:
                        break;
                    }
                }
            }
        }
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
    // SQL8: a system-owned FK support index is allowed to coexist with an
    // ordinary/user index on the same child column. User indexes remain
    // one-per-column.
    if (!def.systemOwned && indexOnColumnExists(def.tableId, def.columnOrdinal)) {
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
    if (_version != kCatalogVersionV4) {
        _version = kCatalogVersionV3;
    }
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
    bool hasDefault = false;
    for (size_t i = 0; i < def.columns.size(); ++i) {
        if (def.columns[i].hasDefault) {
            hasDefault = true;
            break;
        }
    }
    const bool willBeV4 = _version == kCatalogVersionV4 || hasDefault ||
                          !def.foreignKeys.empty();
    const bool willBeV3 = willBeV4 || _version == kCatalogVersionV3 ||
                          !_indexes.empty() || !def.indexes.empty();
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t contCap = capacity - kCatalogContHeaderSize;
    const uint32_t recordSize = recordSizeForDefinition(def, willBeV4) + (willBeV3 ? 1u : 0u);
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
    if (willBeV4) {
        _version = kCatalogVersionV4;
    } else if (willBeV3) {
        _version = kCatalogVersionV3;
    }

    // SQL8: append foreign-key constraints (and their system-owned support
    // indexes) as part of the same in-memory creation. On any failure undo the
    // table, its indexes and any FK records so no partial schema is published.
    if (!def.foreignKeys.empty()) {
        const size_t fksBefore = _foreignKeys.size();
        const uint32_t nextFkBefore = _nextForeignKeyId;
        const uint16_t versionBefore = _version;
        for (size_t i = 0; i < def.foreignKeys.size(); ++i) {
            ForeignKeyDefinition fk = def.foreignKeys[i];
            fk.childTableId = rec.tableId;
            if (fk.parentTableId == 0) {
                fk.parentTableId = rec.tableId; // self-reference
            }
            uint32_t fkId = 0;
            DbResult fkResult = addForeignKey(fk, pageSize, fkId);
            if (!fkResult.isOk()) {
                _foreignKeys.resize(fksBefore);
                _nextForeignKeyId = nextFkBefore;
                _indexes.resize(indexesBefore);
                _nextIndexId = nextIndexBefore;
                _tables.pop_back();
                --_nextTableId;
                _version = versionBefore;
                return fkResult;
            }
        }
    }
    return DbResult::ok();
}

DbResult Catalog::addForeignKey(const ForeignKeyDefinition& def, uint32_t pageSize,
                                uint32_t& outForeignKeyId) {
    const TableRecord* childTable = findTable(def.childTableId);
    if (childTable == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "foreign key child table does not exist");
    }
    const TableRecord* parentTable = findTable(def.parentTableId);
    if (parentTable == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "foreign key parent table does not exist");
    }
    if (def.childColumnOrdinal >= childTable->columns.size()) {
        return DbResult::error(DbStatus::InvalidArgument, "foreign key child column does not exist");
    }
    if (def.parentColumnOrdinal >= parentTable->columns.size()) {
        return DbResult::error(DbStatus::InvalidArgument, "foreign key parent column does not exist");
    }
    const DbType childType = childTable->columns[def.childColumnOrdinal].type;
    const DbType parentType = parentTable->columns[def.parentColumnOrdinal].type;
    if (childType != parentType) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "foreign key child and parent column types must match");
    }
    if (!isIndexableType(childType)) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "foreign key column type is not indexable");
    }

    // The referenced parent column must be backed by a PK or UNIQUE index.
    const IndexRecord* referenced = nullptr;
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].tableId == def.parentTableId &&
            _indexes[i].columnOrdinal == def.parentColumnOrdinal &&
            indexKindIsUnique(_indexes[i].kind())) {
            referenced = &_indexes[i];
            break;
        }
    }
    if (referenced == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "referenced parent column must have a PRIMARY KEY or UNIQUE constraint");
    }
    // Capture the id now: adding the support index below may reallocate
    // `_indexes` and invalidate `referenced`.
    const uint32_t referencedIndexId = referenced->indexId;

    // Count FKs on the child table.
    uint32_t fkCount = 0;
    for (size_t i = 0; i < _foreignKeys.size(); ++i) {
        if (_foreignKeys[i].childTableId == def.childTableId) {
            ++fkCount;
        }
    }
    if (fkCount >= kMaxForeignKeysPerTable) {
        return DbResult::error(DbStatus::NoSpace,
                               "foreign key count per table exceeds policy maximum");
    }
    if (_foreignKeys.size() >= kMaxForeignKeys) {
        return DbResult::error(DbStatus::NoSpace,
                               "catalog foreign key count exceeds policy maximum");
    }

    // Create the support index on the child column.
    IndexDefinition supportDef;
    supportDef.name = "";
    supportDef.tableId = def.childTableId;
    supportDef.columnOrdinal = def.childColumnOrdinal;
    supportDef.kind = IndexKind::Ordinary;
    supportDef.systemOwned = true;
    uint32_t supportIndexId = 0;
    DbResult result = addIndex(supportDef, pageSize, supportIndexId);
    if (!result.isOk()) {
        return result;
    }

    // Mark the support index as system-owned and record its FK owner.
    IndexRecord* supportRec = findIndexMutable(supportIndexId);
    if (supportRec != nullptr) {
        supportRec->flags |= 0x0004u;
    }

    ForeignKeyRecord rec;
    rec.foreignKeyId = _nextForeignKeyId++;
    rec.childTableId = def.childTableId;
    rec.childColumnOrdinal = def.childColumnOrdinal;
    rec.parentTableId = def.parentTableId;
    rec.parentColumnOrdinal = def.parentColumnOrdinal;
    rec.referencedIndexId = referencedIndexId;
    rec.supportIndexId = supportIndexId;
    rec.flags = 0;
    rec.formatVersion = 1;

    if (supportRec != nullptr) {
        supportRec->ownerForeignKeyId = rec.foreignKeyId;
    }

    _foreignKeys.push_back(rec);
    _version = kCatalogVersionV4;
    outForeignKeyId = rec.foreignKeyId;
    return DbResult::ok();
}

DbResult Catalog::removeTable(uint32_t tableId) {
    const TableRecord* table = findTable(tableId);
    if (table == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "table does not exist");
    }

    // Check for incoming FK dependencies from other tables.
    for (size_t i = 0; i < _foreignKeys.size(); ++i) {
        if (_foreignKeys[i].parentTableId == tableId &&
            _foreignKeys[i].childTableId != tableId) {
            return DbResult::error(DbStatus::DependencyExists,
                                   "table is referenced by a foreign key from another table");
        }
    }

    // Remove FK records where this table is the child (includes self-references).
    std::vector<ForeignKeyRecord> remainingFks;
    for (size_t i = 0; i < _foreignKeys.size(); ++i) {
        if (_foreignKeys[i].childTableId != tableId) {
            remainingFks.push_back(_foreignKeys[i]);
        }
    }
    _foreignKeys.swap(remainingFks);

    // Remove all indexes belonging to this table.
    std::vector<IndexRecord> remainingIndexes;
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].tableId != tableId) {
            remainingIndexes.push_back(_indexes[i]);
        }
    }
    _indexes.swap(remainingIndexes);

    // Remove the table record.
    std::vector<TableRecord> remainingTables;
    for (size_t i = 0; i < _tables.size(); ++i) {
        if (_tables[i].tableId != tableId) {
            remainingTables.push_back(_tables[i]);
        }
    }
    _tables.swap(remainingTables);

    if (_version == kCatalogVersionV4 && _foreignKeys.empty()) {
        // Can downgrade to v3 if no FKs remain and no system-owned indexes exist.
        bool hasSystemOwned = false;
        for (size_t i = 0; i < _indexes.size(); ++i) {
            if (_indexes[i].systemOwned()) {
                hasSystemOwned = true;
                break;
            }
        }
        if (!hasSystemOwned) {
            _version = kCatalogVersionV3;
        }
    }
    return DbResult::ok();
}

DbResult Catalog::removeIndex(uint32_t indexId) {
    const IndexRecord* rec = findIndex(indexId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "index does not exist");
    }
    if (rec->systemOwned() || rec->unique() || rec->primaryKey()) {
        return DbResult::error(DbStatus::DependencyExists,
                               "cannot drop a system-owned or constraint-backed index");
    }

    std::vector<IndexRecord> remaining;
    for (size_t i = 0; i < _indexes.size(); ++i) {
        if (_indexes[i].indexId != indexId) {
            remaining.push_back(_indexes[i]);
        }
    }
    _indexes.swap(remaining);
    return DbResult::ok();
}

DbResult Catalog::addColumnToTable(uint32_t tableId, const ColumnDefinition& col,
                                   uint32_t pageSize) {
    TableRecord* table = findTableMutable(tableId);
    if (table == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "table does not exist");
    }
    if (table->columns.size() >= kMaxColumnsPerTable) {
        return DbResult::error(DbStatus::NoSpace, "column count exceeds policy maximum");
    }
    if (!validateName(col.name, kMaxColumnNameBytes)) {
        return DbResult::error(DbStatus::InvalidArgument, "invalid column name");
    }
    if (!isConcreteDbType(col.type)) {
        return DbResult::error(DbStatus::InvalidArgument, "unsupported column type");
    }
    for (size_t i = 0; i < table->columns.size(); ++i) {
        if (table->columns[i].name == col.name) {
            return DbResult::error(DbStatus::AlreadyExists, "duplicate column name");
        }
    }

    ColumnDefinition newCol = col;
    newCol.ordinal = static_cast<uint32_t>(table->columns.size());

    // Validate default: NOT NULL + DEFAULT NULL is rejected.
    if (!newCol.nullable && newCol.hasDefault && newCol.defaultValue.isNull()) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "NOT NULL column cannot have DEFAULT NULL");
    }
    // Validate default type matches column type.
    if (newCol.hasDefault && !newCol.defaultValue.isNull()) {
        if (newCol.defaultValue.type() != newCol.type) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "default value type does not match column type");
        }
    }

    // Check serialized record size.
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t contCap = capacity - kCatalogContHeaderSize;
    TableRecord temp = *table;
    temp.columns.push_back(newCol);
    std::vector<uint8_t> bytes;
    serializeTableRecord(temp, bytes, true);
    if (bytes.size() + 1u > contCap) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "schema too large for the database page size");
    }

    table->columns.push_back(newCol);
    table->schemaVersion += 1;
    _version = kCatalogVersionV4;
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
                                    ColumnDefinition& out, bool hasDefaultMetadata) {
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

    out.hasDefault = false;
    out.defaultValue = DbValue::null();

    if (hasDefaultMetadata) {
        uint8_t hasDefault = 0;
        if (!readField(bytes, length, offset, hasDefault)) {
            return DbResult::error(DbStatus::CorruptPage, "column record truncated (default flag)");
        }
        if (hasDefault > 1) {
            return DbResult::error(DbStatus::CorruptPage, "invalid default flag");
        }
        if (hasDefault) {
            uint8_t defaultType = 0;
            uint8_t defaultNull = 0;
            if (!readField(bytes, length, offset, defaultType) ||
                !readField(bytes, length, offset, defaultNull)) {
                return DbResult::error(DbStatus::CorruptPage, "column record truncated (default type)");
            }
            if (defaultNull > 1) {
                return DbResult::error(DbStatus::CorruptPage, "invalid default null flag");
            }
            const DbType dt = static_cast<DbType>(defaultType);
            if (!defaultNull && !isConcreteDbType(dt)) {
                return DbResult::error(DbStatus::CorruptPage, "invalid default type id");
            }
            if (defaultNull) {
                out.hasDefault = true;
                out.defaultValue = DbValue::null();
            } else {
                switch (dt) {
                case DbType::Boolean: {
                    uint8_t v = 0;
                    if (!readField(bytes, length, offset, v)) {
                        return DbResult::error(DbStatus::CorruptPage, "default value truncated (boolean)");
                    }
                    if (v > 1) {
                        return DbResult::error(DbStatus::CorruptPage, "invalid boolean default value");
                    }
                    out.hasDefault = true;
                    out.defaultValue = DbValue::boolean(v != 0);
                    break;
                }
                case DbType::Int32: {
                    uint32_t v = 0;
                    if (!readField(bytes, length, offset, v)) {
                        return DbResult::error(DbStatus::CorruptPage, "default value truncated (int32)");
                    }
                    out.hasDefault = true;
                    out.defaultValue = DbValue::int32(static_cast<int32_t>(v));
                    break;
                }
                case DbType::Int64: {
                    uint64_t v = 0;
                    if (!readField(bytes, length, offset, v)) {
                        return DbResult::error(DbStatus::CorruptPage, "default value truncated (int64)");
                    }
                    out.hasDefault = true;
                    out.defaultValue = DbValue::int64(static_cast<int64_t>(v));
                    break;
                }
                case DbType::Float64: {
                    uint64_t v = 0;
                    if (!readField(bytes, length, offset, v)) {
                        return DbResult::error(DbStatus::CorruptPage, "default value truncated (float64)");
                    }
                    double d = 0.0;
                    std::memcpy(&d, &v, sizeof(d));
                    out.hasDefault = true;
                    out.defaultValue = DbValue::float64(d);
                    break;
                }
                case DbType::Text: {
                    uint32_t len = 0;
                    if (!readField(bytes, length, offset, len)) {
                        return DbResult::error(DbStatus::CorruptPage, "default value truncated (text length)");
                    }
                    if (len > kMaxTextBytes) {
                        return DbResult::error(DbStatus::CorruptPage, "default text value exceeds maximum length");
                    }
                    if (offset + len > length) {
                        return DbResult::error(DbStatus::CorruptPage, "default text value extends beyond record");
                    }
                    out.hasDefault = true;
                    out.defaultValue = DbValue::text(std::string(reinterpret_cast<const char*>(bytes + offset), len));
                    offset += len;
                    break;
                }
                case DbType::Blob: {
                    uint32_t len = 0;
                    if (!readField(bytes, length, offset, len)) {
                        return DbResult::error(DbStatus::CorruptPage, "default value truncated (blob length)");
                    }
                    if (len > kMaxBlobBytes) {
                        return DbResult::error(DbStatus::CorruptPage, "default blob value exceeds maximum length");
                    }
                    if (offset + len > length) {
                        return DbResult::error(DbStatus::CorruptPage, "default blob value extends beyond record");
                    }
                    out.hasDefault = true;
                    out.defaultValue = DbValue::blob(bytes + offset, len);
                    offset += len;
                    break;
                }
                default:
                    return DbResult::error(DbStatus::CorruptPage, "unsupported default value type");
                }
            }
        }
    }
    return DbResult::ok();
}

DbResult Catalog::parseTableRecord(const uint8_t* bytes, size_t length, size_t& offset,
                                   TableRecord& out, bool hasDefaultMetadata) {
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
        DbResult result = parseColumnRecord(bytes, length, offset, col, hasDefaultMetadata);
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
                                   IndexRecord& out, bool hasOwnershipMetadata) {
    uint32_t indexId = 0;
    uint32_t tableId = 0;
    uint32_t columnOrdinal = 0;
    uint64_t rootPageId = 0;
    uint16_t flags = 0;
    uint16_t formatVersion = 0;
    uint64_t entryCount = 0;
    uint32_t ownerForeignKeyId = 0;
    uint32_t nameLength = 0;

    if (!readField(bytes, length, offset, indexId) ||
        !readField(bytes, length, offset, tableId) ||
        !readField(bytes, length, offset, columnOrdinal) ||
        !readField(bytes, length, offset, rootPageId) ||
        !readField(bytes, length, offset, flags) ||
        !readField(bytes, length, offset, formatVersion) ||
        !readField(bytes, length, offset, entryCount)) {
        return DbResult::error(DbStatus::CorruptPage, "index record truncated");
    }
    if (hasOwnershipMetadata) {
        if (!readField(bytes, length, offset, ownerForeignKeyId)) {
            return DbResult::error(DbStatus::CorruptPage, "index record truncated (owner)");
        }
    }
    if (!readField(bytes, length, offset, nameLength)) {
        return DbResult::error(DbStatus::CorruptPage, "index record truncated (name length)");
    }
    if (indexId == 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid index id");
    }
    if (tableId == 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid index table id");
    }
    const uint16_t allowedFlags = hasOwnershipMetadata ? 0x0007u : 0x0003u;
    if ((flags & ~allowedFlags) != 0) {
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
    out.ownerForeignKeyId = ownerForeignKeyId;
    out.name.assign(reinterpret_cast<const char*>(bytes + offset), nameLength);
    offset += nameLength;
    return DbResult::ok();
}

void Catalog::serializeColumnRecord(const ColumnDefinition& col,
                                    std::vector<uint8_t>& out, bool v4) const {
    const size_t base = out.size();
    out.resize(base + 16);
    storeLe32(out.data() + base + 0, col.ordinal);
    storeLe16(out.data() + base + 4, static_cast<uint16_t>(col.type));
    storeLe16(out.data() + base + 6, col.nullable ? 1u : 0u);
    storeLe32(out.data() + base + 8, 0); // flags
    storeLe32(out.data() + base + 12, static_cast<uint32_t>(col.name.size()));
    out.insert(out.end(), col.name.begin(), col.name.end());

    // SQL8: serialize DEFAULT metadata only in the v4 catalog layout.
    if (!v4) {
        return;
    }
    out.push_back(col.hasDefault ? 1u : 0u);
    if (col.hasDefault) {
        // A NULL default is stored with the column's declared type so the
        // record remains self-describing.
        const DbType storedType =
            col.defaultValue.isNull() ? col.type : col.defaultValue.type();
        out.push_back(static_cast<uint8_t>(storedType));
        out.push_back(col.defaultValue.isNull() ? 1u : 0u);
        if (!col.defaultValue.isNull()) {
            switch (col.defaultValue.type()) {
            case DbType::Boolean:
                out.push_back(col.defaultValue.booleanValue() ? 1u : 0u);
                break;
            case DbType::Int32: {
                const int32_t v = col.defaultValue.int32Value();
                out.resize(out.size() + 4);
                storeLe32(out.data() + out.size() - 4, static_cast<uint32_t>(v));
                break;
            }
            case DbType::Int64: {
                const int64_t v = col.defaultValue.int64Value();
                out.resize(out.size() + 8);
                storeLe64(out.data() + out.size() - 8, static_cast<uint64_t>(v));
                break;
            }
            case DbType::Float64: {
                const double v = col.defaultValue.float64Value();
                out.resize(out.size() + 8);
                storeLe64(out.data() + out.size() - 8, *reinterpret_cast<const uint64_t*>(&v));
                break;
            }
            case DbType::Text: {
                const std::string& s = col.defaultValue.textValue();
                out.resize(out.size() + 4);
                storeLe32(out.data() + out.size() - 4, static_cast<uint32_t>(s.size()));
                out.insert(out.end(), s.begin(), s.end());
                break;
            }
            case DbType::Blob: {
                const std::vector<uint8_t>& b = col.defaultValue.blobValue();
                out.resize(out.size() + 4);
                storeLe32(out.data() + out.size() - 4, static_cast<uint32_t>(b.size()));
                out.insert(out.end(), b.begin(), b.end());
                break;
            }
            default:
                break;
            }
        }
    }
}

void Catalog::serializeTableRecord(const TableRecord& rec, std::vector<uint8_t>& out,
                                   bool v4) const {
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
        serializeColumnRecord(rec.columns[i], out, v4);
    }
}

void Catalog::serializeIndexRecord(const IndexRecord& rec,
                                   std::vector<uint8_t>& out, bool v4) const {
    const size_t base = out.size();
    const size_t headerSize = v4 ? 40u : 36u;
    out.resize(base + headerSize);
    storeLe32(out.data() + base + 0, rec.indexId);
    storeLe32(out.data() + base + 4, rec.tableId);
    storeLe32(out.data() + base + 8, rec.columnOrdinal);
    storeLe64(out.data() + base + 12, rec.rootPageId);
    storeLe16(out.data() + base + 20, rec.flags);
    storeLe16(out.data() + base + 22, rec.formatVersion);
    storeLe64(out.data() + base + 24, rec.entryCount);
    if (v4) {
        storeLe32(out.data() + base + 32, rec.ownerForeignKeyId);
    }
    storeLe32(out.data() + base + (v4 ? 36u : 32u),
              static_cast<uint32_t>(rec.name.size()));
    out.insert(out.end(), rec.name.begin(), rec.name.end());
}

void Catalog::serializeForeignKeyRecord(const ForeignKeyRecord& rec,
                                        std::vector<uint8_t>& out) const {
    const size_t base = out.size();
    out.resize(base + 36);
    storeLe32(out.data() + base + 0, rec.foreignKeyId);
    storeLe32(out.data() + base + 4, rec.childTableId);
    storeLe32(out.data() + base + 8, rec.childColumnOrdinal);
    storeLe32(out.data() + base + 12, rec.parentTableId);
    storeLe32(out.data() + base + 16, rec.parentColumnOrdinal);
    storeLe32(out.data() + base + 20, rec.referencedIndexId);
    storeLe32(out.data() + base + 24, rec.supportIndexId);
    storeLe16(out.data() + base + 28, rec.flags);
    storeLe16(out.data() + base + 30, rec.formatVersion);
    storeLe32(out.data() + base + 32, 0); // reserved
}

DbResult Catalog::parseForeignKeyRecord(const uint8_t* bytes, size_t length, size_t& offset,
                                        ForeignKeyRecord& out) {
    uint32_t foreignKeyId = 0;
    uint32_t childTableId = 0;
    uint32_t childColumnOrdinal = 0;
    uint32_t parentTableId = 0;
    uint32_t parentColumnOrdinal = 0;
    uint32_t referencedIndexId = 0;
    uint32_t supportIndexId = 0;
    uint16_t flags = 0;
    uint16_t formatVersion = 0;
    uint32_t reserved = 0;

    if (!readField(bytes, length, offset, foreignKeyId) ||
        !readField(bytes, length, offset, childTableId) ||
        !readField(bytes, length, offset, childColumnOrdinal) ||
        !readField(bytes, length, offset, parentTableId) ||
        !readField(bytes, length, offset, parentColumnOrdinal) ||
        !readField(bytes, length, offset, referencedIndexId) ||
        !readField(bytes, length, offset, supportIndexId) ||
        !readField(bytes, length, offset, flags) ||
        !readField(bytes, length, offset, formatVersion) ||
        !readField(bytes, length, offset, reserved)) {
        return DbResult::error(DbStatus::CorruptPage, "foreign key record truncated");
    }
    if (foreignKeyId == 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid foreign key id");
    }
    if (childTableId == 0 || parentTableId == 0) {
        return DbResult::error(DbStatus::CorruptPage, "invalid foreign key table id");
    }
    if (formatVersion == 0 || formatVersion > 1) {
        return DbResult::error(DbStatus::CorruptPage, "unsupported foreign key format version");
    }

    out.foreignKeyId = foreignKeyId;
    out.childTableId = childTableId;
    out.childColumnOrdinal = childColumnOrdinal;
    out.parentTableId = parentTableId;
    out.parentColumnOrdinal = parentColumnOrdinal;
    out.referencedIndexId = referencedIndexId;
    out.supportIndexId = supportIndexId;
    out.flags = flags;
    out.formatVersion = formatVersion;
    return DbResult::ok();
}

DbResult Catalog::load(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize,
                       uint64_t pageCount) {
    _tables.clear();
    _indexes.clear();
    _foreignKeys.clear();
    _nextTableId = 1;
    _nextIndexId = 1;
    _nextForeignKeyId = 1;
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
    if (version == kCatalogVersionV4) {
        return loadV4(pages, payload, rootPageId, pageSize, pageCount);
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
        DbResult result = parseTableRecord(payload.data(), payload.size(), offset, rec, false);
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
            DbResult result = parseTableRecord(cpayload.data(), cpayload.size(), coffset, rec, false);
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
            DbResult result = parseTableRecord(payload.data(), payload.size(), offset, rec, false);
            if (!result.isOk()) {
                return result;
            }
            _tables.push_back(rec);
        } else if (type == kCatalogRecordIndex) {
            IndexRecord rec;
            DbResult result = parseIndexRecord(payload.data(), payload.size(), offset, rec, false);
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
                    parseTableRecord(cpayload.data(), cpayload.size(), coffset, rec, false);
                if (!result.isOk()) {
                    return result;
                }
                _tables.push_back(rec);
            } else if (type == kCatalogRecordIndex) {
                IndexRecord rec;
                DbResult result =
                    parseIndexRecord(cpayload.data(), cpayload.size(), coffset, rec, false);
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
    const bool useV4 = _version == kCatalogVersionV4 || !_foreignKeys.empty();
    if (useV4) {
        return saveV4(pages, rootPageId, pageSize);
    }
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
        serializeTableRecord(_tables[i], bytes, false);
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
        serializeTableRecord(_tables[i], bytes, false);
        records.push_back(std::move(bytes));
    }
    for (size_t i = 0; i < _indexes.size(); ++i) {
        std::vector<uint8_t> bytes;
        bytes.push_back(kCatalogRecordIndex);
        serializeIndexRecord(_indexes[i], bytes, false);
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

DbResult Catalog::loadV4(PageAccess& pages, const std::vector<uint8_t>& payload,
                         uint64_t rootPageId, uint32_t pageSize, uint64_t pageCount) {
    (void)rootPageId;
    (void)pageSize;
    if (payload.size() < kCatalogV4RootHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "catalog v4 root header truncated");
    }
    const uint32_t tableCount = loadLe32(payload.data() + 8);
    _nextTableId = loadLe32(payload.data() + 12);
    const uint32_t continuationCount = loadLe32(payload.data() + 16);
    const uint64_t firstContinuation = loadLe32(payload.data() + 20);
    const uint32_t rootRecordCount = loadLe32(payload.data() + 24);
    const uint32_t indexCount = loadLe32(payload.data() + 28);
    _nextIndexId = loadLe32(payload.data() + 32);
    const uint32_t foreignKeyCount = loadLe32(payload.data() + 36);
    _nextForeignKeyId = loadLe32(payload.data() + 40);
    const uint32_t reserved = loadLe32(payload.data() + 44);

    if (tableCount > kMaxTables) {
        return DbResult::error(DbStatus::CorruptPage, "table count exceeds policy maximum");
    }
    if (indexCount > kMaxIndexes) {
        return DbResult::error(DbStatus::CorruptPage, "index count exceeds policy maximum");
    }
    if (foreignKeyCount > kMaxForeignKeys) {
        return DbResult::error(DbStatus::CorruptPage, "foreign key count exceeds policy maximum");
    }
    if (reserved != 0) {
        return DbResult::error(DbStatus::CorruptPage, "catalog v4 reserved field is nonzero");
    }
    if (_nextTableId == 0) _nextTableId = 1;
    if (_nextIndexId == 0) _nextIndexId = 1;
    if (_nextForeignKeyId == 0) _nextForeignKeyId = 1;
    if (continuationCount > pageCount) {
        return DbResult::error(DbStatus::CorruptPage, "continuation count exceeds page count");
    }
    if (continuationCount == 0 && firstContinuation != 0) {
        return DbResult::error(DbStatus::CorruptPage, "continuation pointer without continuations");
    }
    if (rootRecordCount > tableCount + indexCount + foreignKeyCount) {
        return DbResult::error(DbStatus::CorruptPage, "root record count exceeds total records");
    }

    size_t offset = kCatalogV4RootHeaderSize;
    for (uint32_t i = 0; i < rootRecordCount; ++i) {
        if (offset >= payload.size()) {
            return DbResult::error(DbStatus::CorruptPage, "catalog v4 record type truncated");
        }
        const uint8_t type = payload[offset++];
        if (type == kCatalogRecordTable) {
            TableRecord rec;
            DbResult result = parseTableRecord(payload.data(), payload.size(), offset, rec, true);
            if (!result.isOk()) return result;
            _tables.push_back(rec);
        } else if (type == kCatalogRecordIndex) {
            IndexRecord rec;
            DbResult result = parseIndexRecord(payload.data(), payload.size(), offset, rec, true);
            if (!result.isOk()) return result;
            _indexes.push_back(rec);
        } else if (type == kCatalogRecordForeignKey) {
            ForeignKeyRecord rec;
            DbResult result = parseForeignKeyRecord(payload.data(), payload.size(), offset, rec);
            if (!result.isOk()) return result;
            _foreignKeys.push_back(rec);
        } else {
            return DbResult::error(DbStatus::CorruptPage, "unknown catalog v4 record type");
        }
    }

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
        if (!result.isOk()) return result;
        const std::vector<uint8_t>& cpayload = cont.payload;
        if (cpayload.size() < kCatalogV4ContHeaderSize) {
            return DbResult::error(DbStatus::CorruptPage, "catalog v4 continuation header truncated");
        }
        if (loadLe32(cpayload.data() + 0) != kCatalogMagic) {
            return DbResult::error(DbStatus::CorruptPage, "catalog v4 continuation signature mismatch");
        }
        if (loadLe16(cpayload.data() + 4) != kCatalogVersionV4) {
            return DbResult::error(DbStatus::CorruptPage, "catalog v4 continuation version mismatch");
        }
        const uint64_t next = loadLe64(cpayload.data() + 8);
        const uint32_t recordCount = loadLe32(cpayload.data() + 16);
        if (recordCount > kMaxTables + kMaxIndexes + kMaxForeignKeys) {
            return DbResult::error(DbStatus::CorruptPage, "continuation record count too large");
        }

        size_t coffset = kCatalogV4ContHeaderSize;
        for (uint32_t j = 0; j < recordCount; ++j) {
            if (coffset >= cpayload.size()) {
                return DbResult::error(DbStatus::CorruptPage, "catalog v4 record type truncated");
            }
            const uint8_t type = cpayload[coffset++];
            if (type == kCatalogRecordTable) {
                TableRecord rec;
                DbResult result = parseTableRecord(cpayload.data(), cpayload.size(), coffset, rec, true);
                if (!result.isOk()) return result;
                _tables.push_back(rec);
            } else if (type == kCatalogRecordIndex) {
                IndexRecord rec;
                DbResult result = parseIndexRecord(cpayload.data(), cpayload.size(), coffset, rec, true);
                if (!result.isOk()) return result;
                _indexes.push_back(rec);
            } else if (type == kCatalogRecordForeignKey) {
                ForeignKeyRecord rec;
                DbResult result = parseForeignKeyRecord(cpayload.data(), cpayload.size(), coffset, rec);
                if (!result.isOk()) return result;
                _foreignKeys.push_back(rec);
            } else {
                return DbResult::error(DbStatus::CorruptPage, "unknown catalog v4 record type");
            }
        }
        expectedNext = next;
    }

    if (expectedNext != 0) {
        return DbResult::error(DbStatus::CorruptPage, "catalog continuation chain length mismatch");
    }
    if (_tables.size() != tableCount) {
        return DbResult::error(DbStatus::CorruptPage, "catalog v4 table count mismatch");
    }
    if (_indexes.size() != indexCount) {
        return DbResult::error(DbStatus::CorruptPage, "catalog v4 index count mismatch");
    }
    if (_foreignKeys.size() != foreignKeyCount) {
        return DbResult::error(DbStatus::CorruptPage, "catalog v4 foreign key count mismatch");
    }

    // Cross-record validation.
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
    for (size_t i = 0; i < _foreignKeys.size(); ++i) {
        const ForeignKeyRecord& fk = _foreignKeys[i];
        if (findTable(fk.childTableId) == nullptr) {
            return DbResult::error(DbStatus::CorruptPage, "foreign key references unknown child table");
        }
        if (findTable(fk.parentTableId) == nullptr) {
            return DbResult::error(DbStatus::CorruptPage, "foreign key references unknown parent table");
        }
        if (findIndex(fk.referencedIndexId) == nullptr) {
            return DbResult::error(DbStatus::CorruptPage, "foreign key references unknown index");
        }
        if (findIndex(fk.supportIndexId) == nullptr) {
            return DbResult::error(DbStatus::CorruptPage, "foreign key references unknown support index");
        }
        for (size_t j = 0; j < i; ++j) {
            if (_foreignKeys[j].foreignKeyId == fk.foreignKeyId) {
                return DbResult::error(DbStatus::CorruptPage, "duplicate foreign key id");
            }
        }
    }
    _version = kCatalogVersionV4;
    return DbResult::ok();
}

DbResult Catalog::saveV4(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize) {
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t rootCap = capacity - kCatalogV4RootHeaderSize;
    const uint32_t contCap = capacity - kCatalogV4ContHeaderSize;

    std::vector<std::vector<uint8_t>> records;
    records.reserve(_tables.size() + _indexes.size() + _foreignKeys.size());
    for (size_t i = 0; i < _tables.size(); ++i) {
        std::vector<uint8_t> bytes;
        bytes.push_back(kCatalogRecordTable);
        serializeTableRecord(_tables[i], bytes, true);
        records.push_back(std::move(bytes));
    }
    for (size_t i = 0; i < _indexes.size(); ++i) {
        std::vector<uint8_t> bytes;
        bytes.push_back(kCatalogRecordIndex);
        serializeIndexRecord(_indexes[i], bytes, true);
        records.push_back(std::move(bytes));
    }
    for (size_t i = 0; i < _foreignKeys.size(); ++i) {
        std::vector<uint8_t> bytes;
        bytes.push_back(kCatalogRecordForeignKey);
        serializeForeignKeyRecord(_foreignKeys[i], bytes);
        records.push_back(std::move(bytes));
    }

    std::vector<std::vector<uint8_t>> pagePayloads;
    std::vector<uint32_t> pageRecordCounts;

    std::vector<uint8_t> current(rootCap, 0);
    storeLe32(current.data() + 0, kCatalogMagic);
    storeLe16(current.data() + 4, kCatalogVersionV4);
    storeLe16(current.data() + 6, 0);
    storeLe32(current.data() + 8, 0);
    storeLe32(current.data() + 12, 0);
    storeLe32(current.data() + 16, 0);
    storeLe32(current.data() + 20, 0);
    storeLe32(current.data() + 24, 0);
    storeLe32(current.data() + 28, 0);
    storeLe32(current.data() + 32, 0);
    storeLe32(current.data() + 36, 0);
    storeLe32(current.data() + 40, 0);
    storeLe32(current.data() + 44, 0);
    size_t currentOffset = kCatalogV4RootHeaderSize;
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

        if (rec.size() > contCap - kCatalogV4ContHeaderSize) {
            return DbResult::error(DbStatus::Internal, "catalog v4 record exceeds page capacity");
        }
        current.assign(contCap, 0);
        storeLe32(current.data() + 0, kCatalogMagic);
        storeLe16(current.data() + 4, kCatalogVersionV4);
        storeLe16(current.data() + 6, 0);
        storeLe64(current.data() + 8, 0);
        storeLe32(current.data() + 16, 0);
        storeLe32(current.data() + 20, 0);
        currentOffset = kCatalogV4ContHeaderSize;
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
            if (!result.isOk()) return result;
            contIds.push_back(newId);
        }
    }

    {
        std::vector<uint8_t>& rootPayload = pagePayloads[0];
        storeLe32(rootPayload.data() + 8, static_cast<uint32_t>(_tables.size()));
        storeLe32(rootPayload.data() + 12, _nextTableId);
        storeLe32(rootPayload.data() + 16, static_cast<uint32_t>(contIds.size()));
        storeLe32(rootPayload.data() + 20, contIds.empty() ? 0u : static_cast<uint32_t>(contIds[0]));
        storeLe32(rootPayload.data() + 24, pageRecordCounts[0]);
        storeLe32(rootPayload.data() + 28, static_cast<uint32_t>(_indexes.size()));
        storeLe32(rootPayload.data() + 32, _nextIndexId);
        storeLe32(rootPayload.data() + 36, static_cast<uint32_t>(_foreignKeys.size()));
        storeLe32(rootPayload.data() + 40, _nextForeignKeyId);
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
        if (!result.isOk()) return result;
        rootPage.pageId = rootPageId;
        rootPage.payload = pagePayloads[0];
        rootPage.payloadSize = static_cast<uint32_t>(pagePayloads[0].size());
        rootPage.type = PageType::Catalog;
        result = pages.writePage(rootPage);
        if (!result.isOk()) return result;
    }

    for (uint32_t i = 0; i < contIds.size(); ++i) {
        DatabasePage contPage;
        DbResult result = pages.readPage(contIds[i], contPage);
        if (!result.isOk()) return result;
        contPage.pageId = contIds[i];
        contPage.payload = pagePayloads[i + 1];
        contPage.payloadSize = static_cast<uint32_t>(pagePayloads[i + 1].size());
        contPage.type = PageType::Catalog;
        result = pages.writePage(contPage);
        if (!result.isOk()) return result;
    }

    _continuationPageIds = contIds;
    _version = kCatalogVersionV4;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
