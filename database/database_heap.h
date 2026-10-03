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

// A planned row mutation: either a full replacement row or a deletion. The
// relational layer validates and applies a bounded set of these atomically.
struct RowMutation {
    RowLocator locator;
    bool deleted;
    std::vector<DbValue> values; // replacement row when !deleted

    RowMutation() : locator(), deleted(false), values() {}
};

class Table {
public:
    Table(Database& db, uint32_t tableId);

    DbResult insert(const std::vector<DbValue>& values);
    DbResult scanStart(std::unique_ptr<TableScan>& out) const;

    // Fetches the row at an exact physical locator. Returns CorruptPage when the
    // page/slot is invalid or the page is not a heap page; callers treat that as
    // index corruption rather than "row not found".
    DbResult fetchRow(const RowLocator& locator, std::vector<DbValue>& out) const;

    // Applies a bounded set of row replacements/deletions. Each target locator
    // must have come from a single prior scan. The whole set is applied inside
    // one transaction context: an implicit transaction when none is active, or
    // the caller's active transaction otherwise. Returns InvalidArgument on a
    // stale/duplicate locator or invalid replacement value; the caller is
    // responsible for statement-level rollback when an explicit transaction is
    // active.
    DbResult applyMutations(const std::vector<RowMutation>& mutations);

    uint32_t tableId() const { return _tableId; }
    const std::string& name() const;
    uint32_t columnCount() const;
    uint64_t rowCount() const;
    uint32_t heapPageCount() const;
    uint64_t firstHeapPageId() const;
    const std::vector<ColumnDefinition>& columns() const;

private:
    friend class TableScan;

    DbResult insertInTransaction(const std::vector<DbValue>& values);
    DbResult applyMutationsInTransaction(const std::vector<RowMutation>& mutations);
    DbResult appendEncodedRowToTail(const std::vector<uint8_t>& rowBytes,
                                    uint32_t& newPagesOut, uint64_t& firstNewPageOut,
                                    RowLocator* outLocator);
    DbResult ensureLastHeapPage();
    DbResult updateStats(int64_t rowDelta, uint32_t newPageCount, uint64_t firstPageId);
    void markCatalogDirty();

    Database& _db;
    uint32_t _tableId;
    mutable uint64_t _lastHeapPageId;
    mutable bool _lastResolved;
    mutable uint64_t _resolvedEpoch;
};

class TableScan {
public:
    explicit TableScan(const Table& table);

    // Returns true and fills `row` when another row is available. Returns
    // false at the end of the table or after an error; check status().
    bool next(std::vector<DbValue>& row);
    DbResult status() const { return _status; }

    // Locator of the row most recently returned by next(). Valid until the next
    // call to next() or after the underlying data changes.
    uint64_t currentPageId() const { return _rowPageId; }
    uint32_t currentSlotIndex() const { return _rowSlot; }

private:
    const Table& _table;
    DbResult _status;
    DatabasePage _page;
    uint64_t _currentPageId;
    uint64_t _nextPageId;
    uint32_t _slotIndex;
    uint32_t _slotCount;
    uint32_t _rowAreaEnd;
    uint32_t _pageSize;
    uint32_t _capacity;
    bool _started;
    bool _done;
    uint64_t _rowPageId;
    uint32_t _rowSlot;
    std::vector<uint64_t> _visited;
};

} // namespace db
} // namespace gxos
