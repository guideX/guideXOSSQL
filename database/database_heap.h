#pragma once
// guideXOS SQL -- Phase SQL2
// HeapTable: heap-organized row storage with a bounded table scanner.
//
// A table is a chain of heap pages (PageType::Data):
//
//   firstHeapPage -> heap page -> heap page -> ... -> (next = 0)
//
// Each heap page payload:
//   [0..24)   heap header (magic, version, flags, nextPageId, slotCount, rowAreaEnd)
//   [24..rowAreaEnd)                           row area (rows packed forward)
//   [rowAreaEnd..capacity-8*slotCount)         free space
//   [capacity-8*slotCount..capacity)            slot directory (grows backward)
//
// A slot is {u32 rowOffset, u32 rowLength}; slot i lives at
// payload offset capacity - 8*(i+1).
//
// Row encoding (self-bounded):
//   u32 rowLength, u16 columnCount, u16 flags,
//   null bitmap [ceil(columnCount/8) bytes],
//   fixed-width data (non-null fixed columns, ordinal order),
//   variable-length table (u32 byte length per non-null Text/Blob, ordinal order),
//   variable payload (concatenated bytes).

#include <cstdint>
#include <memory>
#include <vector>

#include "database_relational.h"
#include "database_result.h"
#include "database_schema.h"
#include "database_types.h"

namespace gxos {
namespace db {

// Heap page payload field offsets (relative to the start of the payload).
const uint32_t kHeapMagicOffset = 0;
const uint32_t kHeapVersionOffset = 4;
const uint32_t kHeapFlagsOffset = 6;
const uint32_t kHeapNextPageOffset = 8;
const uint32_t kHeapSlotCountOffset = 16;
const uint32_t kHeapRowAreaEndOffset = 20;

class TableScan;

class Table {
public:
    Table(Database& db, uint32_t tableId);

    DbResult insert(const std::vector<DbValue>& values);
    DbResult scanStart(std::unique_ptr<TableScan>& out) const;

    uint32_t tableId() const { return _tableId; }
    const std::string& name() const;
    uint32_t columnCount() const;
    uint64_t rowCount() const;
    uint32_t heapPageCount() const;
    uint64_t firstHeapPageId() const;
    const std::vector<ColumnDefinition>& columns() const;

private:
    friend class TableScan;

    DbResult ensureLastHeapPage();
    DbResult updateStats(int64_t rowDelta, bool newPage, uint64_t firstPageId);
    void markCatalogDirty();

    Database& _db;
    uint32_t _tableId;
    mutable uint64_t _lastHeapPageId;
    mutable bool _lastResolved;
};

class TableScan {
public:
    explicit TableScan(const Table& table);

    // Returns true and fills `row` when another row is available. Returns
    // false at the end of the table or after an error; check status().
    bool next(std::vector<DbValue>& row);
    DbResult status() const { return _status; }

private:
    const Table& _table;
    DbResult _status;
    BufferSlot* _slot;
    uint64_t _currentPageId;
    uint64_t _nextPageId;
    uint32_t _slotIndex;
    uint32_t _slotCount;
    uint32_t _rowAreaEnd;
    uint32_t _pageSize;
    uint32_t _capacity;
    bool _started;
    bool _done;
    std::vector<uint64_t> _visited;
};

} // namespace db
} // namespace gxos
