#include "database_heap.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>

#include "database_endian.h"
#include "database_format.h"
#include "database_index.h"
#include "database_page.h"
#include "database_transaction.h"

namespace gxos {
namespace db {

namespace {

uint32_t fixedTypeSize(DbType type) {
    switch (type) {
    case DbType::Boolean: return 1;
    case DbType::Int32: return 4;
    case DbType::Int64: return 8;
    case DbType::Float64: return 8;
    default: return 0;
    }
}

bool isFixedType(DbType type) {
    return fixedTypeSize(type) != 0;
}

uint32_t variableSize(const DbValue& value) {
    if (value.type() == DbType::Text) {
        return static_cast<uint32_t>(value.textValue().size());
    }
    if (value.type() == DbType::Blob) {
        return static_cast<uint32_t>(value.blobValue().size());
    }
    return 0;
}

struct HeapPageInfo {
    uint64_t nextPageId;
    uint32_t slotCount;
    uint32_t rowAreaEnd;
};

DbResult readHeapPage(const DatabasePage& page, uint32_t pageSize, HeapPageInfo& out) {
    if (page.type != PageType::Data) {
        return DbResult::error(DbStatus::CorruptPage, "heap page has wrong page type");
    }
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const std::vector<uint8_t>& payload = page.payload;
    if (payload.size() != capacity) {
        return DbResult::error(DbStatus::CorruptPage, "heap page payload size mismatch");
    }
    if (payload.size() < kHeapHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "heap page header truncated");
    }
    if (loadLe32(payload.data() + kHeapMagicOffset) != kHeapMagic) {
        return DbResult::error(DbStatus::CorruptPage, "heap page signature mismatch");
    }
    if (loadLe16(payload.data() + kHeapVersionOffset) != kHeapVersion) {
        return DbResult::error(DbStatus::CorruptPage, "unsupported heap page version");
    }

    const uint32_t slotCount = loadLe32(payload.data() + kHeapSlotCountOffset);
    const uint32_t rowAreaEnd = loadLe32(payload.data() + kHeapRowAreaEndOffset);

    if (kHeapHeaderSize + 8u * slotCount > capacity) {
        return DbResult::error(DbStatus::CorruptPage, "impossible heap slot count");
    }
    if (rowAreaEnd < kHeapHeaderSize + 8u * slotCount || rowAreaEnd > capacity) {
        return DbResult::error(DbStatus::CorruptPage, "heap row area end out of range");
    }

    out.nextPageId = loadLe64(payload.data() + kHeapNextPageOffset);
    out.slotCount = slotCount;
    out.rowAreaEnd = rowAreaEnd;
    return DbResult::ok();
}

void writeHeapHeader(std::vector<uint8_t>& payload, uint64_t nextPageId, uint32_t slotCount,
                     uint32_t rowAreaEnd) {
    storeLe32(payload.data() + kHeapMagicOffset, kHeapMagic);
    storeLe16(payload.data() + kHeapVersionOffset, kHeapVersion);
    storeLe16(payload.data() + kHeapFlagsOffset, 0);
    storeLe64(payload.data() + kHeapNextPageOffset, nextPageId);
    storeLe32(payload.data() + kHeapSlotCountOffset, slotCount);
    storeLe32(payload.data() + kHeapRowAreaEndOffset, rowAreaEnd);
}

// Reads slot `index` from a heap page payload. `capacity` is the payload
// capacity; the slot directory grows backward from the end.
bool readSlot(const std::vector<uint8_t>& payload, uint32_t index, uint32_t capacity,
              uint32_t& outOffset, uint32_t& outLength) {
    if (8u * (index + 1) > capacity) {
        return false;
    }
    const uint32_t slotOffset = capacity - 8u * (index + 1);
    if (slotOffset < kHeapHeaderSize) {
        return false;
    }
    outOffset = loadLe32(payload.data() + slotOffset);
    outLength = loadLe32(payload.data() + slotOffset + 4);
    return true;
}

DbResult encodeRow(const std::vector<ColumnDefinition>& columns,
                   const std::vector<DbValue>& values, std::vector<uint8_t>& out) {
    const uint32_t columnCount = static_cast<uint32_t>(columns.size());
    const uint32_t bitmapSize = (columnCount + 7u) / 8u;

    uint32_t fixedSize = 0;
    uint32_t varCount = 0;
    uint64_t varBytes = 0;
    for (uint32_t i = 0; i < columnCount; ++i) {
        if (values[i].isNull()) {
            continue;
        }
        if (isFixedType(columns[i].type)) {
            fixedSize += fixedTypeSize(columns[i].type);
        } else {
            ++varCount;
            varBytes += variableSize(values[i]);
        }
    }

    const uint64_t rowLength = 8ull + bitmapSize + fixedSize + 4ull * varCount + varBytes;
    if (rowLength > kMaxRowBytes) {
        return DbResult::error(DbStatus::InvalidArgument, "encoded row exceeds maximum row size");
    }

    out.assign(static_cast<size_t>(rowLength), 0);
    storeLe32(out.data() + 0, static_cast<uint32_t>(rowLength));
    storeLe16(out.data() + 4, static_cast<uint16_t>(columnCount));
    storeLe16(out.data() + 6, 0); // flags

    for (uint32_t i = 0; i < columnCount; ++i) {
        if (values[i].isNull()) {
            out[8u + i / 8u] |= static_cast<uint8_t>(1u << (i % 8u));
        }
    }

    size_t fixedOffset = 8ull + bitmapSize;
    for (uint32_t i = 0; i < columnCount; ++i) {
        if (values[i].isNull() || !isFixedType(columns[i].type)) {
            continue;
        }
        const DbValue& v = values[i];
        switch (columns[i].type) {
        case DbType::Boolean:
            out[fixedOffset] = v.booleanValue() ? 1u : 0u;
            fixedOffset += 1;
            break;
        case DbType::Int32:
            storeLe32(out.data() + fixedOffset, static_cast<uint32_t>(v.int32Value()));
            fixedOffset += 4;
            break;
        case DbType::Int64:
            storeLe64(out.data() + fixedOffset, static_cast<uint64_t>(v.int64Value()));
            fixedOffset += 8;
            break;
        case DbType::Float64: {
            double d = v.float64Value();
            uint64_t bits = 0;
            std::memcpy(&bits, &d, sizeof(bits));
            storeLe64(out.data() + fixedOffset, bits);
            fixedOffset += 8;
            break;
        }
        default:
            return DbResult::error(DbStatus::Internal, "unexpected fixed type in encode");
        }
    }

    size_t varOffset = fixedOffset;
    for (uint32_t i = 0; i < columnCount; ++i) {
        if (values[i].isNull() || isFixedType(columns[i].type)) {
            continue;
        }
        const uint32_t len = variableSize(values[i]);
        storeLe32(out.data() + varOffset, len);
        varOffset += 4;
    }
    for (uint32_t i = 0; i < columnCount; ++i) {
        if (values[i].isNull() || isFixedType(columns[i].type)) {
            continue;
        }
        if (values[i].type() == DbType::Text) {
            const std::string& s = values[i].textValue();
            std::memcpy(out.data() + varOffset, s.data(), s.size());
            varOffset += s.size();
        } else {
            const std::vector<uint8_t>& b = values[i].blobValue();
            if (!b.empty()) {
                std::memcpy(out.data() + varOffset, b.data(), b.size());
                varOffset += b.size();
            }
        }
    }
    return DbResult::ok();
}

DbResult validateRowValues(const std::vector<ColumnDefinition>& cols,
                           const std::vector<DbValue>& values) {
    if (values.size() != cols.size()) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "value count does not match column count");
    }
    for (size_t i = 0; i < cols.size(); ++i) {
        if (values[i].isNull()) {
            if (!cols[i].nullable) {
                return DbResult::error(DbStatus::InvalidArgument,
                                       "NULL assigned to NOT NULL column");
            }
            continue;
        }
        if (values[i].type() != cols[i].type) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "value type does not match column type");
        }
        if (cols[i].type == DbType::Text && values[i].textValue().size() > kMaxTextBytes) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "text value exceeds maximum length");
        }
        if (cols[i].type == DbType::Blob && values[i].blobValue().size() > kMaxBlobBytes) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "blob value exceeds maximum length");
        }
    }
    return DbResult::ok();
}

// Rewrites `page` so it holds exactly `rows` in order, preserving the header's
// next-page link. The caller must have checked that the rows fit.
DbResult packHeapPage(DatabasePage& page,
                      const std::vector<std::vector<uint8_t> >& rows, uint64_t nextPageId,
                      uint32_t capacity) {
    page.payload.assign(capacity, 0);
    uint32_t offset = kHeapHeaderSize;
    for (size_t i = 0; i < rows.size(); ++i) {
        const uint32_t length = static_cast<uint32_t>(rows[i].size());
        const uint32_t slotOffset = capacity - 8u * (static_cast<uint32_t>(i) + 1u);
        if (offset > slotOffset || length > slotOffset - offset) {
            return DbResult::error(DbStatus::NoSpace, "heap page rows do not fit");
        }
        if (length > 0) {
            std::memcpy(page.payload.data() + offset, rows[i].data(), length);
        }
        storeLe32(page.payload.data() + slotOffset, offset);
        storeLe32(page.payload.data() + slotOffset + 4, length);
        offset += length;
    }
    writeHeapHeader(page.payload, nextPageId, static_cast<uint32_t>(rows.size()), offset);
    page.type = PageType::Data;
    page.payloadSize = capacity;
    return DbResult::ok();
}

DbResult decodeRow(const uint8_t* bytes, size_t length,
                   const std::vector<ColumnDefinition>& columns,
                   std::vector<DbValue>& out) {
    if (bytes == nullptr || length < 8) {
        return DbResult::error(DbStatus::CorruptPage, "row smaller than row header");
    }
    const uint32_t rowLength = loadLe32(bytes + 0);
    if (rowLength != length) {
        return DbResult::error(DbStatus::CorruptPage, "row length mismatch");
    }
    const uint16_t columnCount = loadLe16(bytes + 4);
    if (columnCount != columns.size()) {
        return DbResult::error(DbStatus::CorruptPage, "row column count mismatch");
    }
    const uint16_t flags = loadLe16(bytes + 6);
    if (flags != 0) {
        return DbResult::error(DbStatus::CorruptPage, "row flags are nonzero");
    }

    const uint32_t bitmapSize = (columnCount + 7u) / 8u;
    if (8ull + bitmapSize > rowLength) {
        return DbResult::error(DbStatus::CorruptPage, "row null bitmap overflows row");
    }

    uint32_t fixedSize = 0;
    uint32_t varCount = 0;
    for (uint32_t i = 0; i < columnCount; ++i) {
        const bool isNull = (bytes[8u + i / 8u] & (1u << (i % 8u))) != 0;
        if (isNull) {
            continue;
        }
        if (isFixedType(columns[i].type)) {
            fixedSize += fixedTypeSize(columns[i].type);
        } else {
            ++varCount;
        }
    }

    const uint64_t headerBytes = 8ull + bitmapSize + fixedSize + 4ull * varCount;
    if (headerBytes > rowLength) {
        return DbResult::error(DbStatus::CorruptPage, "row fixed/variable table overflows row");
    }

    uint32_t varLengths[kMaxColumnsPerTable];
    size_t varOffset = 8ull + bitmapSize + fixedSize;
    uint64_t varBytes = 0;
    for (uint32_t j = 0; j < varCount; ++j) {
        varLengths[j] = loadLe32(bytes + varOffset);
        varOffset += 4;
        varBytes += varLengths[j];
    }
    if (varOffset + varBytes > rowLength) {
        return DbResult::error(DbStatus::CorruptPage, "row variable payload overflows row");
    }

    out.clear();
    out.reserve(columnCount);
    size_t fixedOffset = 8ull + bitmapSize;
    uint32_t varIndex = 0;
    for (uint32_t i = 0; i < columnCount; ++i) {
        const bool isNull = (bytes[8u + i / 8u] & (1u << (i % 8u))) != 0;
        if (isNull) {
            out.push_back(DbValue::null());
            continue;
        }
        const DbType type = columns[i].type;
        if (isFixedType(type)) {
            switch (type) {
            case DbType::Boolean:
                out.push_back(DbValue::boolean(bytes[fixedOffset] != 0));
                fixedOffset += 1;
                break;
            case DbType::Int32:
                out.push_back(DbValue::int32(static_cast<int32_t>(loadLe32(bytes + fixedOffset))));
                fixedOffset += 4;
                break;
            case DbType::Int64:
                out.push_back(DbValue::int64(static_cast<int64_t>(loadLe64(bytes + fixedOffset))));
                fixedOffset += 8;
                break;
            case DbType::Float64: {
                const uint64_t bits = loadLe64(bytes + fixedOffset);
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                out.push_back(DbValue::float64(d));
                fixedOffset += 8;
                break;
            }
            default:
                return DbResult::error(DbStatus::CorruptPage, "unexpected fixed type in decode");
            }
        } else {
            const uint32_t len = varLengths[varIndex++];
            if (type == DbType::Text) {
                out.push_back(DbValue::text(
                    std::string(reinterpret_cast<const char*>(bytes + varOffset), len)));
            } else {
                out.push_back(DbValue::blob(bytes + varOffset, len));
            }
            varOffset += len;
        }
    }
    return DbResult::ok();
}

// ---- SQL6 index maintenance helpers ---------------------------------------

struct IndexTarget {
    Catalog::IndexRecord record;
    DbType type;
};

std::vector<IndexTarget> tableIndexTargets(Database& db, uint32_t tableId) {
    std::vector<IndexTarget> out;
    const Catalog::TableRecord* table = db.catalog().findTable(tableId);
    if (table == nullptr) {
        return out;
    }
    const std::vector<Catalog::IndexRecord>& indexes = db.catalog().indexes();
    for (size_t i = 0; i < indexes.size(); ++i) {
        if (indexes[i].tableId != tableId) {
            continue;
        }
        if (indexes[i].columnOrdinal >= table->columns.size()) {
            continue;
        }
        IndexTarget target;
        target.record = indexes[i];
        target.type = table->columns[indexes[i].columnOrdinal].type;
        out.push_back(target);
    }
    return out;
}

DbResult encodeKeyForColumn(Database& db, const Catalog::IndexRecord& rec, DbType type,
                            const std::vector<DbValue>& values,
                            std::vector<uint8_t>& outKey) {
    if (rec.columnOrdinal >= values.size()) {
        return DbResult::error(DbStatus::Internal, "index column out of range");
    }
    if (!encodeIndexKey(type, values[rec.columnOrdinal], outKey)) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "value is not indexable (non-finite Float64?)");
    }
    if (outKey.size() > maxIndexKeyBytes(db.pages().pageSize())) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "indexed value exceeds the maximum index key size");
    }
    return DbResult::ok();
}

DbResult encodeKeyFromRowBytes(Database& db, const Catalog::IndexRecord& rec, DbType type,
                               const std::vector<ColumnDefinition>& columns,
                               const std::vector<uint8_t>& rowBytes,
                               std::vector<uint8_t>& outKey) {
    std::vector<DbValue> values;
    DbResult result = decodeRow(rowBytes.data(), rowBytes.size(), columns, values);
    if (!result.isOk()) {
        return result;
    }
    return encodeKeyForColumn(db, rec, type, values, outKey);
}

// A surviving row whose physical locator and/or indexed values changed.
struct IndexChange {
    RowLocator oldLocator;
    RowLocator newLocator;
    std::vector<uint8_t> oldBytes;
    std::vector<uint8_t> newBytes;
    IndexChange() : oldLocator(), newLocator(), oldBytes(), newBytes() {}
};

// A row removed by the mutation.
struct IndexDeleted {
    RowLocator locator;
    std::vector<uint8_t> bytes;
    IndexDeleted() : locator(), bytes() {}
};

// Applies every index change for one mutation: removes stale entries first
// (so key swaps are legal), then inserts the new entries and enforces
// uniqueness against the post-removal state.
DbResult maintainIndexes(Database& db, uint32_t tableId,
                         const std::vector<ColumnDefinition>& columns,
                         const std::vector<IndexTarget>& targets,
                         const std::vector<IndexDeleted>& deleted,
                         const std::vector<IndexChange>& changes) {
    (void)tableId;
    for (size_t t = 0; t < targets.size(); ++t) {
        const Catalog::IndexRecord rec = targets[t].record;
        const DbType type = targets[t].type;
        const bool unique = rec.unique();

        struct Edit {
            std::vector<uint8_t> key;
            RowLocator locator;
        };
        std::vector<Edit> removals;
        std::vector<Edit> insertions;

        for (size_t i = 0; i < deleted.size(); ++i) {
            std::vector<uint8_t> key;
            DbResult r =
                encodeKeyFromRowBytes(db, rec, type, columns, deleted[i].bytes, key);
            if (!r.isOk()) {
                return r;
            }
            Edit e;
            e.key = key;
            e.locator = deleted[i].locator;
            removals.push_back(e);
        }
        for (size_t i = 0; i < changes.size(); ++i) {
            std::vector<uint8_t> oldKey;
            std::vector<uint8_t> newKey;
            DbResult r =
                encodeKeyFromRowBytes(db, rec, type, columns, changes[i].oldBytes, oldKey);
            if (!r.isOk()) {
                return r;
            }
            r = encodeKeyFromRowBytes(db, rec, type, columns, changes[i].newBytes, newKey);
            if (!r.isOk()) {
                return r;
            }
            const bool keySame =
                compareEncodedKeys(oldKey.data(), oldKey.size(), newKey.data(),
                                   newKey.size()) == 0;
            const bool locSame =
                changes[i].oldLocator.pageId == changes[i].newLocator.pageId &&
                changes[i].oldLocator.slot == changes[i].newLocator.slot;
            if (keySame && locSame) {
                continue;
            }
            Edit oldEdit;
            oldEdit.key = oldKey;
            oldEdit.locator = changes[i].oldLocator;
            removals.push_back(oldEdit);
            Edit newEdit;
            newEdit.key = newKey;
            newEdit.locator = changes[i].newLocator;
            insertions.push_back(newEdit);
        }

        if (removals.empty() && insertions.empty()) {
            continue;
        }

        IndexTree tree(db.pages(), type, rec.indexId);
        uint64_t root = rec.rootPageId;
        uint64_t removedCount = 0;
        for (size_t i = 0; i < removals.size(); ++i) {
            if (root == 0) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "index entry missing for removal");
            }
            uint64_t newRoot = root;
            bool removed = false;
            DbResult r = tree.remove(removals[i].key, removals[i].locator, root, newRoot,
                                     removed);
            if (!r.isOk()) {
                return r;
            }
            if (!removed) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "index entry missing for removal");
            }
            if (newRoot != root) {
                root = newRoot;
                db.catalog().setIndexRoot(rec.indexId, root);
            }
            ++removedCount;
        }
        uint64_t insertedCount = 0;
        for (size_t i = 0; i < insertions.size(); ++i) {
            if (root == 0) {
                DbResult r = tree.createEmpty(root);
                if (!r.isOk()) {
                    return r;
                }
                db.catalog().setIndexRoot(rec.indexId, root);
            }
            uint64_t newRoot = root;
            bool inserted = false;
            DbResult r = tree.insert(insertions[i].key, insertions[i].locator, root,
                                     unique, newRoot, inserted);
            if (!r.isOk()) {
                return r;
            }
            if (newRoot != root) {
                root = newRoot;
                db.catalog().setIndexRoot(rec.indexId, root);
            }
            if (inserted) {
                ++insertedCount;
            }
        }
        if (removedCount != 0) {
            db.catalog().adjustIndexEntryCount(rec.indexId,
                                               -static_cast<int64_t>(removedCount));
        }
        if (insertedCount != 0) {
            db.catalog().adjustIndexEntryCount(rec.indexId,
                                               static_cast<int64_t>(insertedCount));
        }
        db.markCatalogDirty();
    }
    return DbResult::ok();
}

// Compares two DbValues for equality. Returns 0 if equal.
int compareValues(const DbValue& a, const DbValue& b) {
    if (a.isNull() != b.isNull()) {
        return a.isNull() ? -1 : 1;
    }
    if (a.isNull()) {
        return 0;
    }
    if (a.type() != b.type()) {
        return a.type() < b.type() ? -1 : 1;
    }
    switch (a.type()) {
    case DbType::Boolean:
        return a.booleanValue() == b.booleanValue() ? 0 : (a.booleanValue() ? 1 : -1);
    case DbType::Int32:
        return a.int32Value() == b.int32Value() ? 0 : (a.int32Value() < b.int32Value() ? -1 : 1);
    case DbType::Int64:
        return a.int64Value() == b.int64Value() ? 0 : (a.int64Value() < b.int64Value() ? -1 : 1);
    case DbType::Float64:
        return a.float64Value() == b.float64Value() ? 0 : (a.float64Value() < b.float64Value() ? -1 : 1);
    case DbType::Text:
        return a.textValue().compare(b.textValue());
    case DbType::Blob:
        if (a.blobValue().size() != b.blobValue().size()) {
            return a.blobValue().size() < b.blobValue().size() ? -1 : 1;
        }
        return std::memcmp(a.blobValue().data(), b.blobValue().data(), a.blobValue().size());
    default:
        return 0;
    }
}

// ---- SQL8 foreign-key enforcement -------------------------------------------

// Returns the FK records where `tableId` is the child table.
std::vector<Catalog::ForeignKeyRecord> childForeignKeys(Database& db, uint32_t tableId) {
    return db.catalog().foreignKeysForChildTable(tableId);
}

// Returns the FK records where `tableId` is the parent table.
std::vector<Catalog::ForeignKeyRecord> parentForeignKeys(Database& db, uint32_t tableId) {
    return db.catalog().foreignKeysForParentTable(tableId);
}

// Compares two non-NULL values for equality using the same total order used by
// index keys. NULLs are considered equal to each other.
bool valuesEqual(const DbValue& a, const DbValue& b) {
    if (a.isNull() || b.isNull()) {
        return a.isNull() && b.isNull();
    }
    return compareValues(a, b) == 0;
}

// Returns the index of the mutation targeting `loc`, or -1 when the row is not
// part of the batch.
int findMutationIndex(const std::vector<RowMutation>& mutations,
                      const RowLocator& loc) {
    for (size_t i = 0; i < mutations.size(); ++i) {
        if (mutations[i].locator.pageId == loc.pageId &&
            mutations[i].locator.slot == loc.slot) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Checks child-side FK constraints for a row being inserted or updated.
// For each non-NULL FK value, the referenced parent row must exist in the
// statement's final logical state. When `batch` is provided (UPDATE/DELETE),
// self-referential parents that are inserted or moved within the same
// statement are taken into account, and parent rows that are deleted or moved
// away by the same statement are treated as absent.
DbResult enforceChildForeignKeys(Database& db, uint32_t tableId,
                                 const std::vector<DbValue>& values,
                                 const std::vector<RowMutation>* batch) {
    const std::vector<Catalog::ForeignKeyRecord> fks = childForeignKeys(db, tableId);
    for (size_t i = 0; i < fks.size(); ++i) {
        const Catalog::ForeignKeyRecord& fk = fks[i];
        if (fk.childColumnOrdinal >= values.size()) {
            continue;
        }
        const DbValue& childValue = values[fk.childColumnOrdinal];
        if (childValue.isNull()) {
            continue;
        }
        const Catalog::IndexRecord* parentRec = db.catalog().findIndex(fk.referencedIndexId);
        if (parentRec == nullptr) {
            return DbResult::error(DbStatus::Internal, "FK referenced index not found");
        }
        const Catalog::TableRecord* parentTable = db.catalog().findTable(parentRec->tableId);
        if (parentTable == nullptr) {
            return DbResult::error(DbStatus::Internal, "FK parent table not found");
        }
        std::vector<uint8_t> key;
        if (!encodeIndexKey(parentTable->columns[parentRec->columnOrdinal].type, childValue, key)) {
            return DbResult::error(DbStatus::Internal, "FK key encoding failed");
        }
        IndexTree tree(db.pages(), parentTable->columns[parentRec->columnOrdinal].type,
                       parentRec->indexId);
        std::vector<IndexEntry> entries;
        DbResult result = tree.lookupAll(key, parentRec->rootPageId, entries);
        if (!result.isOk()) {
            return result;
        }

        if (fk.parentTableId != tableId || batch == nullptr) {
            // Cross-table (or plain insert): the referenced parent must already
            // exist in the index.
            if (entries.empty()) {
                return DbResult::error(
                    DbStatus::ForeignKeyViolation,
                    "foreign key constraint violation: referenced parent row does not exist");
            }
            continue;
        }

        // Self-referential final-state check.
        const uint32_t parentCol = fk.parentColumnOrdinal;
        bool live = false;
        for (size_t e = 0; e < entries.size() && !live; ++e) {
            const int mi = findMutationIndex(*batch, entries[e].locator);
            if (mi < 0) {
                live = true; // parent row is untouched by this statement
                break;
            }
            const RowMutation& m = (*batch)[static_cast<size_t>(mi)];
            if (!m.deleted && parentCol < m.values.size() &&
                valuesEqual(m.values[parentCol], childValue)) {
                live = true;
            }
        }
        if (!live) {
            // A parent row may be provided by the same statement even though the
            // index has not been updated yet.
            for (size_t m = 0; m < batch->size(); ++m) {
                const RowMutation& mut = (*batch)[m];
                if (!mut.deleted && parentCol < mut.values.size() &&
                    valuesEqual(mut.values[parentCol], childValue)) {
                    live = true;
                    break;
                }
            }
        }
        if (!live) {
            return DbResult::error(
                DbStatus::ForeignKeyViolation,
                "foreign key constraint violation: referenced parent row does not exist");
        }
    }
    return DbResult::ok();
}

// Checks parent-side FK RESTRICT constraints against the statement's final
// logical state. A parent key that is removed (deleted or changed away) by the
// statement is rejected when a surviving child row still references it. For a
// self-referential FK, child rows that are themselves deleted or moved away by
// the same statement do not block the parent change.
DbResult enforceParentForeignKeys(Database& db, uint32_t tableId,
                                  const std::vector<RowMutation>& mutations,
                                  const std::vector<std::vector<DbValue> >& oldValues) {
    const std::vector<Catalog::ForeignKeyRecord> fks = parentForeignKeys(db, tableId);
    for (size_t f = 0; f < fks.size(); ++f) {
        const Catalog::ForeignKeyRecord& fk = fks[f];
        const uint32_t parentCol = fk.parentColumnOrdinal;
        const uint32_t childCol = fk.childColumnOrdinal;

        // Values still provided by the statement (non-deleted replacement rows).
        std::vector<DbValue> stillPresent;
        for (size_t i = 0; i < mutations.size(); ++i) {
            if (!mutations[i].deleted && parentCol < mutations[i].values.size()) {
                stillPresent.push_back(mutations[i].values[parentCol]);
            }
        }

        for (size_t i = 0; i < mutations.size(); ++i) {
            const RowMutation& mutation = mutations[i];
            if (parentCol >= oldValues[i].size()) {
                continue;
            }
            const DbValue& oldKey = oldValues[i][parentCol];
            if (oldKey.isNull()) {
                continue;
            }
            DbValue newKey = DbValue::null();
            if (!mutation.deleted && parentCol < mutation.values.size()) {
                newKey = mutation.values[parentCol];
            }
            if (!mutation.deleted && valuesEqual(oldKey, newKey)) {
                continue; // parent key unchanged
            }
            bool present = false;
            for (size_t s = 0; s < stillPresent.size(); ++s) {
                if (valuesEqual(stillPresent[s], oldKey)) {
                    present = true;
                    break;
                }
            }
            if (present) {
                continue; // key is re-established by the same statement
            }

            const Catalog::TableRecord* childTable = db.catalog().findTable(fk.childTableId);
            if (childTable == nullptr) {
                return DbResult::error(DbStatus::Internal, "FK child table not found");
            }
            const Catalog::IndexRecord* supportRec = db.catalog().findIndex(fk.supportIndexId);
            if (supportRec == nullptr) {
                return DbResult::error(DbStatus::Internal, "FK support index not found");
            }
            std::vector<uint8_t> key;
            if (!encodeIndexKey(childTable->columns[fk.childColumnOrdinal].type, oldKey, key)) {
                return DbResult::error(DbStatus::Internal, "FK key encoding failed");
            }
            IndexTree tree(db.pages(), childTable->columns[fk.childColumnOrdinal].type,
                           supportRec->indexId);
            std::vector<IndexEntry> entries;
            DbResult result = tree.lookupAll(key, supportRec->rootPageId, entries);
            if (!result.isOk()) {
                return result;
            }
            for (size_t e = 0; e < entries.size(); ++e) {
                if (fk.childTableId == tableId) {
                    const int mi = findMutationIndex(mutations, entries[e].locator);
                    if (mi < 0) {
                        return DbResult::error(
                            DbStatus::ForeignKeyViolation,
                            "foreign key constraint violation: parent row is referenced by child rows");
                    }
                    const RowMutation& child = mutations[static_cast<size_t>(mi)];
                    if (child.deleted) {
                        continue; // child row removed by this statement
                    }
                    if (childCol < child.values.size() &&
                        valuesEqual(child.values[childCol], oldKey)) {
                        return DbResult::error(
                            DbStatus::ForeignKeyViolation,
                            "foreign key constraint violation: parent row is referenced by child rows");
                    }
                    // child moved away from this key: resolved
                } else {
                    return DbResult::error(
                        DbStatus::ForeignKeyViolation,
                        "foreign key constraint violation: parent row is referenced by child rows");
                }
            }
        }
    }
    return DbResult::ok();
}

} // namespace

Table::Table(Database& db, uint32_t tableId)
    : _db(db), _tableId(tableId), _lastHeapPageId(0), _lastResolved(false),
      _resolvedEpoch(0), _skipForeignKeys(false) {}

const std::string& Table::name() const {
    static const std::string empty;
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    return rec ? rec->name : empty;
}

uint32_t Table::columnCount() const {
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    return rec ? static_cast<uint32_t>(rec->columns.size()) : 0;
}

uint64_t Table::rowCount() const {
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    return rec ? rec->rowCount : 0;
}

uint32_t Table::heapPageCount() const {
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    return rec ? rec->heapPageCount : 0;
}

uint64_t Table::firstHeapPageId() const {
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    return rec ? rec->firstHeapPageId : 0;
}

const std::vector<ColumnDefinition>& Table::columns() const {
    static const std::vector<ColumnDefinition> empty;
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    return rec ? rec->columns : empty;
}

void Table::markCatalogDirty() {
    _db.markCatalogDirty();
}

DbResult Table::updateStats(int64_t rowDelta, uint32_t newPageCount, uint64_t firstPageId) {
    Catalog::TableRecord* rec = _db.catalog().findTableMutable(_tableId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::Internal, "table record vanished from catalog");
    }
    if (rowDelta < 0) {
        const uint64_t decrease = static_cast<uint64_t>(-rowDelta);
        rec->rowCount = (decrease > rec->rowCount) ? 0 : (rec->rowCount - decrease);
    } else {
        rec->rowCount += static_cast<uint64_t>(rowDelta);
    }
    if (newPageCount != 0) {
        rec->heapPageCount += newPageCount;
        if (rec->firstHeapPageId == 0) {
            rec->firstHeapPageId = firstPageId;
        }
    }
    _db.markCatalogDirty();
    return DbResult::ok();
}

DbResult Table::ensureLastHeapPage() {
    if (_lastResolved && _resolvedEpoch == _db.dataEpoch()) {
        return DbResult::ok();
    }
    _lastResolved = true;
    _resolvedEpoch = _db.dataEpoch();
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::Internal, "table record not found");
    }
    if (rec->heapPageCount == 0 || rec->firstHeapPageId == 0) {
        _lastHeapPageId = 0;
        return DbResult::ok();
    }
    // Walk the chain to find the last page.
    uint64_t current = rec->firstHeapPageId;
    uint64_t pageCount = _db.pages().pageCount();
    std::vector<uint64_t> visited;
    while (current != 0) {
        if (current >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "heap page id outside allocated range");
        }
        for (size_t i = 0; i < visited.size(); ++i) {
            if (visited[i] == current) {
                return DbResult::error(DbStatus::CorruptPage, "heap page chain cycle");
            }
        }
        visited.push_back(current);
        if (visited.size() > pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "heap page chain cycle");
        }
        DatabasePage page;
        DbResult result = _db.pages().readPage(current, page);
        if (!result.isOk()) {
            return result;
        }
        if (page.type != PageType::Data) {
            return DbResult::error(DbStatus::CorruptPage, "heap page has wrong page type");
        }
        HeapPageInfo info;
        result = readHeapPage(page, _db.pages().pageSize(), info);
        if (!result.isOk()) {
            return result;
        }
        _lastHeapPageId = current;
        current = info.nextPageId;
    }
    return DbResult::ok();
}

DbResult Table::insert(const std::vector<DbValue>& values) {
    if (_db.isReadOnly()) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    // If an explicit transaction is active the insert participates in it;
    // otherwise wrap the single operation in an implicit transaction.
    if (_db.activeTransaction() != nullptr) {
        return insertInTransaction(values);
    }
    std::unique_ptr<Transaction> tx;
    DbResult begin = _db.beginTransaction(tx);
    if (!begin.isOk()) {
        return begin;
    }
    DbResult result = insertInTransaction(values);
    if (!result.isOk()) {
        tx->rollback();
        return result;
    }
    return tx->commit();
}

DbResult Table::insertInTransaction(const std::vector<DbValue>& values) {
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::Internal, "table record not found");
    }
    const std::vector<ColumnDefinition>& cols = rec->columns;

    DbResult validation = validateRowValues(cols, values);
    if (!validation.isOk()) {
        return validation;
    }

    std::vector<uint8_t> rowBytes;
    DbResult result = encodeRow(cols, values, rowBytes);
    if (!result.isOk()) {
        return result;
    }

    // SQL6: validate every indexed key and enforce UNIQUE / PRIMARY KEY before
    // touching the heap, so a constraint violation leaves no partial change.
    const std::vector<IndexTarget> targets = tableIndexTargets(_db, _tableId);
    std::vector<std::vector<uint8_t> > keys(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        result = encodeKeyForColumn(_db, targets[i].record, targets[i].type, values,
                                    keys[i]);
        if (!result.isOk()) {
            return result;
        }
        if (targets[i].record.unique() &&
            !encodedKeyIsNull(keys[i].data(), keys[i].size()) &&
            targets[i].record.rootPageId != 0) {
            IndexTree tree(_db.pages(), targets[i].type, targets[i].record.indexId);
            std::vector<IndexEntry> existing;
            result = tree.lookupAll(keys[i], targets[i].record.rootPageId, existing);
            if (!result.isOk()) {
                return result;
            }
            if (!existing.empty()) {
                return DbResult::error(DbStatus::AlreadyExists,
                                       "unique index constraint violation");
            }
        }
    }

    // SQL8: enforce child-side foreign key constraints.
    if (!_skipForeignKeys) {
        result = enforceChildForeignKeys(_db, _tableId, values, nullptr);
        if (!result.isOk()) {
            return result;
        }
    }

    RowLocator locator;
    uint32_t newPages = 0;
    uint64_t firstNewPage = 0;
    result = appendEncodedRowToTail(rowBytes, newPages, firstNewPage, &locator);
    if (!result.isOk()) {
        return result;
    }

    for (size_t i = 0; i < targets.size(); ++i) {
        IndexTree tree(_db.pages(), targets[i].type, targets[i].record.indexId);
        uint64_t root = targets[i].record.rootPageId;
        if (root == 0) {
            result = tree.createEmpty(root);
            if (!result.isOk()) {
                return result;
            }
            _db.catalog().setIndexRoot(targets[i].record.indexId, root);
        }
        uint64_t newRoot = root;
        bool inserted = false;
        result = tree.insert(keys[i], locator, root, targets[i].record.unique(),
                             newRoot, inserted);
        if (!result.isOk()) {
            return result;
        }
        if (newRoot != root) {
            _db.catalog().setIndexRoot(targets[i].record.indexId, newRoot);
        }
        if (inserted) {
            _db.catalog().adjustIndexEntryCount(targets[i].record.indexId, 1);
        }
    }
    if (!targets.empty()) {
        _db.markCatalogDirty();
    }

    DbResult statsResult = updateStats(1, newPages, firstNewPage);
    _db.refreshBufferDiagnostics();
    return statsResult;
}

DbResult Table::appendEncodedRowToTail(const std::vector<uint8_t>& rowBytes,
                                       uint32_t& newPagesOut, uint64_t& firstNewPageOut,
                                       RowLocator* outLocator) {
    const uint32_t pageSize = _db.pages().pageSize();
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t rowLength = static_cast<uint32_t>(rowBytes.size());
    if (rowLength + 8u > capacity) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "encoded row exceeds heap page capacity");
    }

    DbResult result = ensureLastHeapPage();
    if (!result.isOk()) {
        return result;
    }

    if (_lastHeapPageId != 0) {
        DatabasePage page;
        result = _db.pages().readPage(_lastHeapPageId, page);
        if (!result.isOk()) {
            return result;
        }
        if (page.type != PageType::Data) {
            return DbResult::error(DbStatus::CorruptPage, "heap page has wrong page type");
        }
        HeapPageInfo info;
        result = readHeapPage(page, pageSize, info);
        if (!result.isOk()) {
            return result;
        }
        if (info.rowAreaEnd + rowLength <= capacity - 8u * (info.slotCount + 1)) {
            std::vector<uint8_t>& payload = page.payload;
            std::memcpy(payload.data() + info.rowAreaEnd, rowBytes.data(), rowBytes.size());
            const uint32_t slotOffset = capacity - 8u * (info.slotCount + 1);
            storeLe32(payload.data() + slotOffset, info.rowAreaEnd);
            storeLe32(payload.data() + slotOffset + 4, rowLength);
            storeLe32(payload.data() + kHeapSlotCountOffset, info.slotCount + 1);
            storeLe32(payload.data() + kHeapRowAreaEndOffset, info.rowAreaEnd + rowLength);
            if (outLocator != nullptr) {
                *outLocator = RowLocator(_lastHeapPageId, info.slotCount);
            }
            return _db.pages().writePage(page);
        }
    }

    // Allocate a new heap page and link it into the chain.
    uint64_t newPageId = 0;
    result = _db.pages().allocatePage(PageType::Data, newPageId);
    if (!result.isOk()) {
        return result;
    }

    DatabasePage newPageImage;
    result = _db.pages().readPage(newPageId, newPageImage);
    if (!result.isOk()) {
        return result;
    }
    newPageImage.pageId = newPageId;
    newPageImage.type = PageType::Data;
    newPageImage.payload.assign(capacity, 0);
    writeHeapHeader(newPageImage.payload, 0, 0, kHeapHeaderSize);
    newPageImage.payloadSize = capacity;

    {
        std::vector<uint8_t>& payload = newPageImage.payload;
        std::memcpy(payload.data() + kHeapHeaderSize, rowBytes.data(), rowBytes.size());
        const uint32_t slotOffset = capacity - 8u;
        storeLe32(payload.data() + slotOffset, kHeapHeaderSize);
        storeLe32(payload.data() + slotOffset + 4, rowLength);
        storeLe32(payload.data() + kHeapSlotCountOffset, 1u);
        storeLe32(payload.data() + kHeapRowAreaEndOffset, kHeapHeaderSize + rowLength);
    }
    result = _db.pages().writePage(newPageImage);
    if (!result.isOk()) {
        return result;
    }

    if (_lastHeapPageId != 0) {
        DatabasePage oldPage;
        result = _db.pages().readPage(_lastHeapPageId, oldPage);
        if (!result.isOk()) {
            return result;
        }
        storeLe64(oldPage.payload.data() + kHeapNextPageOffset, newPageId);
        result = _db.pages().writePage(oldPage);
        if (!result.isOk()) {
            return result;
        }
    }

    if (newPagesOut == 0) {
        firstNewPageOut = newPageId;
    }
    ++newPagesOut;
    _lastHeapPageId = newPageId;
    if (outLocator != nullptr) {
        *outLocator = RowLocator(newPageId, 0);
    }
    return DbResult::ok();
}

DbResult Table::applyMutations(const std::vector<RowMutation>& mutations) {
    if (_db.isReadOnly()) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    if (mutations.empty()) {
        return DbResult::ok();
    }
    if (_db.activeTransaction() != nullptr) {
        return applyMutationsInTransaction(mutations);
    }
    std::unique_ptr<Transaction> tx;
    DbResult begin = _db.beginTransaction(tx);
    if (!begin.isOk()) {
        return begin;
    }
    DbResult result = applyMutationsInTransaction(mutations);
    if (!result.isOk()) {
        tx->rollback();
        return result;
    }
    return tx->commit();
}

DbResult Table::applyMutationsInTransaction(const std::vector<RowMutation>& mutations) {
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::Internal, "table record not found");
    }
    const std::vector<ColumnDefinition>& cols = rec->columns;
    const uint32_t pageSize = _db.pages().pageSize();
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);

    struct Entry {
        uint32_t slot;
        bool deleted;
        std::vector<uint8_t> encoded;
    };
    std::map<uint64_t, std::vector<Entry> > byPage;
    std::set<std::pair<uint64_t, uint32_t> > seen;
    int64_t rowDelta = 0;

    for (size_t i = 0; i < mutations.size(); ++i) {
        const RowMutation& mutation = mutations[i];
        if (!seen
                 .insert(std::make_pair(mutation.locator.pageId, mutation.locator.slot))
                 .second) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "duplicate row locator in mutation plan");
        }
        Entry entry;
        entry.slot = mutation.locator.slot;
        entry.deleted = mutation.deleted;
        if (mutation.deleted) {
            rowDelta -= 1;
        } else {
            DbResult validation = validateRowValues(cols, mutation.values);
            if (!validation.isOk()) {
                return validation;
            }
            DbResult encoded = encodeRow(cols, mutation.values, entry.encoded);
            if (!encoded.isOk()) {
                return encoded;
            }
            if (entry.encoded.size() + 8u > capacity) {
                return DbResult::error(DbStatus::InvalidArgument,
                                       "replacement row exceeds heap page capacity");
            }
        }
        byPage[mutation.locator.pageId].push_back(entry);
    }

    // SQL8: enforce foreign key constraints before applying mutations, against
    // the statement's final logical state. Child-side checks validate every
    // replacement row; parent-side checks consider all mutations together so a
    // self-referential statement that removes both sides in one batch succeeds.
    if (!_skipForeignKeys) {
        DbResult fkResult;
        std::vector<std::vector<DbValue> > oldValuesList(mutations.size());
        for (size_t i = 0; i < mutations.size(); ++i) {
            fkResult = fetchRow(mutations[i].locator, oldValuesList[i]);
            if (!fkResult.isOk()) {
                return fkResult;
            }
        }
        for (size_t i = 0; i < mutations.size(); ++i) {
            if (!mutations[i].deleted) {
                fkResult = enforceChildForeignKeys(_db, _tableId, mutations[i].values,
                                                   &mutations);
                if (!fkResult.isOk()) {
                    return fkResult;
                }
            }
        }
        fkResult = enforceParentForeignKeys(_db, _tableId, mutations, oldValuesList);
        if (!fkResult.isOk()) {
            return fkResult;
        }
    }

    // Walk the heap chain once to establish physical order, so relocated rows
    // preserve the relative order of the pages they came from.
    std::vector<uint64_t> chain;
    {
        uint64_t current = rec->firstHeapPageId;
        const uint64_t pageCount = _db.pages().pageCount();
        std::set<uint64_t> visited;
        while (current != 0) {
            if (current >= pageCount) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "heap page id outside allocated range");
            }
            if (!visited.insert(current).second) {
                return DbResult::error(DbStatus::CorruptPage, "heap page chain cycle");
            }
            chain.push_back(current);
            DatabasePage page;
            DbResult result = _db.pages().readPage(current, page);
            if (!result.isOk()) {
                return result;
            }
            if (page.type != PageType::Data) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "heap page has wrong page type");
            }
            HeapPageInfo info;
            result = readHeapPage(page, pageSize, info);
            if (!result.isOk()) {
                return result;
            }
            current = info.nextPageId;
        }
    }

    // Physical changes to report to the index layer. `changes` covers every
    // surviving row whose locator or indexed values changed, including rows
    // moved indirectly by page compaction, not only the rows explicitly
    // updated. `relocations` are the rows that no longer fit their page and are
    // appended to the tail; their new locator is known only after the append.
    std::vector<IndexChange> changes;
    std::vector<IndexDeleted> deletedRows;
    std::vector<IndexChange> relocations;
    std::set<uint64_t> processed;

    for (size_t ci = 0; ci < chain.size(); ++ci) {
        const uint64_t pageId = chain[ci];
        std::map<uint64_t, std::vector<Entry> >::iterator it = byPage.find(pageId);
        if (it == byPage.end()) {
            continue;
        }
        std::vector<Entry>& entries = it->second;
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.slot < b.slot; });

        DatabasePage page;
        DbResult result = _db.pages().readPage(pageId, page);
        if (!result.isOk()) {
            return result;
        }
        if (page.type != PageType::Data) {
            return DbResult::error(DbStatus::CorruptPage, "heap page has wrong page type");
        }
        HeapPageInfo info;
        result = readHeapPage(page, pageSize, info);
        if (!result.isOk()) {
            return result;
        }

        struct RowSlot {
            uint32_t oldSlot;
            std::vector<uint8_t> oldBytes;
            std::vector<uint8_t> newBytes;
        };
        std::vector<RowSlot> finalRows;
        size_t entryIndex = 0;
        for (uint32_t slot = 0; slot < info.slotCount; ++slot) {
            if (entryIndex < entries.size() && entries[entryIndex].slot < slot) {
                return DbResult::error(DbStatus::InvalidArgument,
                                       "mutation locator slot out of range");
            }
            const Entry* entry = nullptr;
            if (entryIndex < entries.size() && entries[entryIndex].slot == slot) {
                entry = &entries[entryIndex];
                ++entryIndex;
            }
            uint32_t offset = 0;
            uint32_t length = 0;
            if (!readSlot(page.payload, slot, capacity, offset, length)) {
                return DbResult::error(DbStatus::CorruptPage, "invalid heap slot");
            }
            if (offset < kHeapHeaderSize || length < 8u ||
                offset + length > info.rowAreaEnd) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "heap slot row out of range");
            }
            std::vector<uint8_t> oldBytes(page.payload.begin() + offset,
                                          page.payload.begin() + offset + length);
            if (entry != nullptr && entry->deleted) {
                IndexDeleted removed;
                removed.locator = RowLocator(pageId, slot);
                removed.bytes = oldBytes;
                deletedRows.push_back(removed);
                continue;
            }
            RowSlot rowSlot;
            rowSlot.oldSlot = slot;
            rowSlot.oldBytes = oldBytes;
            rowSlot.newBytes = (entry != nullptr) ? entry->encoded : oldBytes;
            finalRows.push_back(rowSlot);
        }
        if (entryIndex < entries.size()) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "mutation locator slot out of range");
        }

        // Keep the prefix that still fits the page; relocate the remainder to
        // the tail. Relocation is how a variable-width UPDATE can grow a row.
        uint32_t used = kHeapHeaderSize;
        size_t keep = 0;
        for (; keep < finalRows.size(); ++keep) {
            const uint32_t need = 8u + static_cast<uint32_t>(finalRows[keep].newBytes.size());
            if (used + need > capacity) {
                break;
            }
            used += need;
        }
        std::vector<std::vector<uint8_t> > kept;
        kept.reserve(keep);
        for (size_t i = 0; i < keep; ++i) {
            kept.push_back(finalRows[i].newBytes);
            const RowLocator oldLocator(pageId, finalRows[i].oldSlot);
            const RowLocator newLocator(pageId, static_cast<uint32_t>(i));
            if (oldLocator.slot != newLocator.slot ||
                finalRows[i].oldBytes != finalRows[i].newBytes) {
                IndexChange change;
                change.oldLocator = oldLocator;
                change.newLocator = newLocator;
                change.oldBytes = finalRows[i].oldBytes;
                change.newBytes = finalRows[i].newBytes;
                changes.push_back(change);
            }
        }
        for (size_t i = keep; i < finalRows.size(); ++i) {
            IndexChange relocation;
            relocation.oldLocator = RowLocator(pageId, finalRows[i].oldSlot);
            relocation.oldBytes = finalRows[i].oldBytes;
            relocation.newBytes = finalRows[i].newBytes;
            relocations.push_back(relocation);
        }

        result = packHeapPage(page, kept, info.nextPageId, capacity);
        if (!result.isOk()) {
            return result;
        }
        result = _db.pages().writePage(page);
        if (!result.isOk()) {
            return result;
        }
        processed.insert(pageId);
    }

    if (processed.size() != byPage.size()) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "mutation locator does not match a heap page");
    }

    // Append all relocated rows only after every affected page has been
    // rewritten, so a relocated row can never be rewritten twice.
    uint32_t newPages = 0;
    uint64_t firstNewPage = 0;
    for (size_t i = 0; i < relocations.size(); ++i) {
        RowLocator newLocator;
        DbResult result = appendEncodedRowToTail(relocations[i].newBytes, newPages,
                                                 firstNewPage, &newLocator);
        if (!result.isOk()) {
            return result;
        }
        IndexChange change;
        change.oldLocator = relocations[i].oldLocator;
        change.newLocator = newLocator;
        change.oldBytes = relocations[i].oldBytes;
        change.newBytes = relocations[i].newBytes;
        changes.push_back(change);
    }

    // Keep every table index consistent with the heap. This includes locator
    // remaps for rows that were moved only because a page was repacked.
    const std::vector<IndexTarget> targets = tableIndexTargets(_db, _tableId);
    if (!targets.empty() && (!deletedRows.empty() || !changes.empty())) {
        DbResult indexResult =
            maintainIndexes(_db, _tableId, cols, targets, deletedRows, changes);
        if (!indexResult.isOk()) {
            return indexResult;
        }
    }

    DbResult statsResult = updateStats(rowDelta, newPages, firstNewPage);
    _db.refreshBufferDiagnostics();
    return statsResult;
}

DbResult Table::alterAddColumn(const ColumnDefinition& newCol) {
    // Must be called with an active transaction so the statement savepoint can
    // restore the old schema and rows on any failure.
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::Internal, "table record not found");
    }
    if (!newCol.nullable && !newCol.hasDefault && rec->rowCount > 0) {
        return DbResult::error(
            DbStatus::InvalidArgument,
            "cannot add a NOT NULL column without a default to a non-empty table");
    }

    // 1. Scan all existing rows under the current (old) schema.
    std::vector<RowLocator> locators;
    std::vector<std::vector<DbValue> > rows;
    std::unique_ptr<TableScan> scan;
    DbResult result = scanStart(scan);
    if (!result.isOk()) {
        return result;
    }
    std::vector<DbValue> row;
    while (scan->next(row)) {
        locators.push_back(RowLocator(scan->currentPageId(), scan->currentSlotIndex()));
        rows.push_back(row);
    }
    if (!scan->status().isOk()) {
        return scan->status();
    }

    // 2. Remove every old row. FK enforcement is suspended: the logical rows are
    // unchanged, only their physical encoding/locators change.
    _skipForeignKeys = true;
    if (!locators.empty()) {
        std::vector<RowMutation> deletions;
        deletions.reserve(locators.size());
        for (size_t i = 0; i < locators.size(); ++i) {
            RowMutation del;
            del.locator = locators[i];
            del.deleted = true;
            deletions.push_back(del);
        }
        result = applyMutationsInTransaction(deletions);
        if (!result.isOk()) {
            _skipForeignKeys = false;
            return result;
        }
    }

    // 3. Append the column to the catalog (old rows are already gone).
    const DbValue fill = newCol.hasDefault ? newCol.defaultValue : DbValue::null();
    result = _db.catalog().addColumnToTable(_tableId, newCol, _db.pageSize());
    if (!result.isOk()) {
        _skipForeignKeys = false;
        return result;
    }
    markCatalogDirty();

    // 4. Re-insert every row with the appended value. Existing indexes are
    // maintained and receive the new physical locators.
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<DbValue> newValues = rows[i];
        newValues.push_back(fill);
        result = insertInTransaction(newValues);
        if (!result.isOk()) {
            _skipForeignKeys = false;
            return result;
        }
    }
    _skipForeignKeys = false;
    return DbResult::ok();
}

DbResult Table::fetchRow(const RowLocator& locator, std::vector<DbValue>& out) const {
    const Catalog::TableRecord* rec = _db.catalog().findTable(_tableId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::Internal, "table record not found");
    }
    const uint64_t pageCount = _db.pages().pageCount();
    if (locator.pageId == 0 || locator.pageId >= pageCount) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index locator page out of range");
    }
    DatabasePage page;
    DbResult result = _db.pages().readPage(locator.pageId, page);
    if (!result.isOk()) {
        return result;
    }
    if (page.type != PageType::Data) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index locator points at a non-heap page");
    }
    const uint32_t pageSize = _db.pages().pageSize();
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    HeapPageInfo info;
    result = readHeapPage(page, pageSize, info);
    if (!result.isOk()) {
        return result;
    }
    if (locator.slot >= info.slotCount) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index locator slot out of range");
    }
    uint32_t offset = 0;
    uint32_t length = 0;
    if (!readSlot(page.payload, locator.slot, capacity, offset, length)) {
        return DbResult::error(DbStatus::CorruptPage, "invalid heap slot");
    }
    if (offset < kHeapHeaderSize || length < 8u || offset + length > info.rowAreaEnd) {
        return DbResult::error(DbStatus::CorruptPage, "heap slot row out of range");
    }
    return decodeRow(page.payload.data() + offset, length, rec->columns, out);
}

DbResult Table::scanStart(std::unique_ptr<TableScan>& out) const {
    out.reset(new TableScan(*this));
    return DbResult::ok();
}

TableScan::TableScan(const Table& table)
    : _table(table), _status(DbResult::ok()), _page(),
      _currentPageId(0), _nextPageId(0), _slotIndex(0), _slotCount(0), _rowAreaEnd(0),
      _pageSize(table._db.pages().pageSize()),
      _capacity(DatabasePage::payloadCapacity(table._db.pages().pageSize())),
      _started(false), _done(false), _rowPageId(0), _rowSlot(0) {}

bool TableScan::next(std::vector<DbValue>& row) {
    if (!_status.isOk() || _done) {
        return false;
    }

    const Catalog::TableRecord* rec = _table._db.catalog().findTable(_table._tableId);
    if (rec == nullptr) {
        _status = DbResult::error(DbStatus::Internal, "table record not found");
        return false;
    }
    const std::vector<ColumnDefinition>& cols = rec->columns;

    while (true) {
        if (_currentPageId == 0) {
            if (_started) {
                _currentPageId = _nextPageId;
            } else {
                _started = true;
                _currentPageId = rec->firstHeapPageId;
            }
            if (_currentPageId == 0) {
                _done = true;
                return false;
            }
            const uint64_t pageCount = _table._db.pages().pageCount();
            if (_currentPageId >= pageCount) {
                _status = DbResult::error(DbStatus::CorruptPage,
                                          "heap page id outside allocated range");
                return false;
            }
            for (size_t i = 0; i < _visited.size(); ++i) {
                if (_visited[i] == _currentPageId) {
                    _status = DbResult::error(DbStatus::CorruptPage, "heap page chain cycle");
                    return false;
                }
            }
            if (_visited.size() >= pageCount) {
                _status = DbResult::error(DbStatus::CorruptPage, "heap page chain cycle");
                return false;
            }
            _visited.push_back(_currentPageId);

            DbResult result = _table._db.pages().readPage(_currentPageId, _page);
            if (!result.isOk()) {
                _status = result;
                return false;
            }
            if (_page.type != PageType::Data) {
                _status = DbResult::error(DbStatus::CorruptPage, "heap page has wrong page type");
                return false;
            }
            HeapPageInfo info;
            result = readHeapPage(_page, _pageSize, info);
            if (!result.isOk()) {
                _status = result;
                return false;
            }
            _nextPageId = info.nextPageId;
            _slotIndex = 0;
            _slotCount = info.slotCount;
            _rowAreaEnd = info.rowAreaEnd;

        }

        while (_slotIndex < _slotCount) {
            uint32_t offset = 0;
            uint32_t length = 0;
            if (!readSlot(_page.payload, _slotIndex, _capacity, offset, length)) {
                _status = DbResult::error(DbStatus::CorruptPage, "invalid heap slot");
                return false;
            }
            ++_slotIndex;
            if (offset < kHeapHeaderSize || length < 8u ||
                offset + length > _rowAreaEnd) {
                _status = DbResult::error(DbStatus::CorruptPage, "heap slot row out of range");
                return false;
            }
            std::vector<DbValue> values;
            DbResult result = decodeRow(_page.payload.data() + offset, length, cols, values);
            if (!result.isOk()) {
                _status = result;
                return false;
            }
            _rowPageId = _currentPageId;
            _rowSlot = _slotIndex - 1;
            row = values;
            return true;
        }

        // Current page exhausted; advance.
        _currentPageId = 0;

    }
}

} // namespace db
} // namespace gxos
