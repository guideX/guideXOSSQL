#include "database_heap.h"

#include <cstring>

#include "database_endian.h"
#include "database_format.h"
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

} // namespace

Table::Table(Database& db, uint32_t tableId)
    : _db(db), _tableId(tableId), _lastHeapPageId(0), _lastResolved(false),
      _resolvedEpoch(0) {}

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

DbResult Table::updateStats(int64_t rowDelta, bool newPage, uint64_t firstPageId) {
    Catalog::TableRecord* rec = _db.catalog().findTableMutable(_tableId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::Internal, "table record vanished from catalog");
    }
    rec->rowCount += static_cast<uint64_t>(rowDelta);
    if (newPage) {
        rec->heapPageCount += 1;
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

    if (values.size() != cols.size()) {
        return DbResult::error(DbStatus::InvalidArgument, "value count does not match column count");
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
            return DbResult::error(DbStatus::InvalidArgument, "value type does not match column type");
        }
        if (cols[i].type == DbType::Text &&
            values[i].textValue().size() > kMaxTextBytes) {
            return DbResult::error(DbStatus::InvalidArgument, "text value exceeds maximum length");
        }
        if (cols[i].type == DbType::Blob &&
            values[i].blobValue().size() > kMaxBlobBytes) {
            return DbResult::error(DbStatus::InvalidArgument, "blob value exceeds maximum length");
        }
    }

    std::vector<uint8_t> rowBytes;
    DbResult result = encodeRow(cols, values, rowBytes);
    if (!result.isOk()) {
        return result;
    }

    const uint32_t pageSize = _db.pages().pageSize();
    const uint32_t capacity = DatabasePage::payloadCapacity(pageSize);
    const uint32_t rowLength = static_cast<uint32_t>(rowBytes.size());
    if (rowLength + 8u > capacity) {
        return DbResult::error(DbStatus::InvalidArgument, "encoded row exceeds heap page capacity");
    }

    result = ensureLastHeapPage();
    if (!result.isOk()) {
        return result;
    }

    bool newPage = false;
    uint64_t firstPageId = 0;

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
            // Append the row and its slot.
            std::vector<uint8_t>& payload = page.payload;
            std::memcpy(payload.data() + info.rowAreaEnd, rowBytes.data(), rowBytes.size());
            const uint32_t slotOffset = capacity - 8u * (info.slotCount + 1);
            storeLe32(payload.data() + slotOffset, info.rowAreaEnd);
            storeLe32(payload.data() + slotOffset + 4, rowLength);
            storeLe32(payload.data() + kHeapSlotCountOffset, info.slotCount + 1);
            storeLe32(payload.data() + kHeapRowAreaEndOffset, info.rowAreaEnd + rowLength);
            result = _db.pages().writePage(page);
            if (!result.isOk()) {
                return result;
            }
            DbResult statsResult = updateStats(1, false, 0);
            _db.refreshBufferDiagnostics();
            return statsResult;
        }
    }

    // Allocate a new heap page.
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
        storeLe32(payload.data() + kHeapRowAreaEndOffset,
                  kHeapHeaderSize + rowLength);
    }
    result = _db.pages().writePage(newPageImage);
    if (!result.isOk()) {
        return result;
    }

    // Link the new page into the chain.
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
    } else {
        firstPageId = newPageId;
    }

    _lastHeapPageId = newPageId;
    newPage = true;
    DbResult statsResult = updateStats(1, newPage, firstPageId);
    _db.refreshBufferDiagnostics();
    return statsResult;
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
      _started(false), _done(false) {}

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
            row = values;
            return true;
        }

        // Current page exhausted; advance.
        _currentPageId = 0;

    }
}

} // namespace db
} // namespace gxos
