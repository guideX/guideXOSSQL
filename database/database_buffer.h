#pragma once
// guideXOS SQL -- Phase SQL2
// BufferManager: a small bounded page cache between the relational layer and
// DatabaseFile.
//
//   Relational layer (Catalog / HeapTable)
//            |
//       BufferManager
//            |
//       DatabaseFile (page I/O, validation, durability)
//
// Properties:
//   * Bounded slot count (constructor capacity).
//   * Explicit dirty state; dirty pages are written before eviction.
//   * Deterministic LRU eviction (oldest last-used unpinned slot).
//   * Pinned slots are never evicted.
//   * flush() writes every dirty slot, then propagates to DatabaseFile::flush
//     (header-last ordering + OS durability barrier).
//   * Pages read through DatabaseFile::readPage, so CRC/structure validation
//     still applies to every page that enters the cache.
//   * No threads and no background flushing.

#include <stdint.h>

#include "database_file.h"
#include "database_page.h"
#include "database_result.h"
#include "portable/gx_page_cache.h"
#include "portable/gx_owned_page.h"

namespace gxos {
namespace db {

struct BufferSlot {
    uint64_t pageId;
    DatabasePage page;
    bool dirty;
    uint32_t pinCount;
    uint64_t lastUsed; // LRU clock
    uint32_t cacheIndex;
    bool materialized; // page bytes are valid only while the slot is pinned

    BufferSlot()
        : pageId(0), dirty(false), pinCount(0), lastUsed(0), cacheIndex(0),
          materialized(false) {}
};

class BufferManager {
public:
    // A null allocator selects the hosted allocator. A supplied allocator is
    // copied by value, while its context remains borrowed through destruction.
    BufferManager(DatabaseFile& file, uint32_t capacity,
                  const GxAllocator* allocator = nullptr);
    ~BufferManager();

    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;

    // Initializes bounded portable cache metadata and stable hosted page-view
    // slots. Allocation failures are returned before the manager is published.
    DbResult init();

    // Returns a pinned slot holding `pageId`, reading it from the file on a
    // cache miss. The caller MUST call unpin() exactly once. On failure the
    // slot is not pinned.
    DbResult pinPage(uint64_t pageId, BufferSlot*& outSlot);

    // Materializes the committed serialized image into an initialized
    // caller-owned page. The destination must use this database's page size;
    // its allocator context must outlive the owner, not this manager.
    DbResult readOwnedPage(uint64_t pageId, GxOwnedPage& out);

    // Pins a page that was already allocated and written (zeroed) by
    // DatabaseFile::allocatePage. The slot is marked dirty so the final
    // content is guaranteed to reach the file on flush/close.
    DbResult pinAllocatedPage(uint64_t pageId, BufferSlot*& outSlot);

    // SQL3: installs a complete committed page image into the cache without
    // reading it from the file. The slot is marked dirty so the image reaches
    // the file on the next flush. Used by transaction commit/recovery to
    // publish committed page images (including newly allocated pages) into the
    // committed cache. Never used for uncommitted data.
    DbResult installPage(uint64_t pageId, const DatabasePage& page);

    DbResult markDirty(BufferSlot& slot);
    DbResult unpin(BufferSlot& slot);

    // Writes all dirty slots, then flushes the underlying file (header +
    // durability barrier). Read-only databases are not written.
    DbResult flush();

    uint32_t capacity() const { return _capacity; }
    uint32_t residentCount() const { return static_cast<uint32_t>(_cache.resident); }
    uint32_t dirtyCount() const { return static_cast<uint32_t>(_cache.dirtyCount); }
    uint32_t pinnedCount() const { return _pinned; }
    const GxPageCacheStats& cacheStats() const { return _cache.stats; }
    // Borrowed only while this manager is alive. The context is the same
    // database-scoped allocator used for cache storage; callers may use it for
    // temporary page preparation that is destroyed before returning.
    const GxAllocator& scratchAllocator() const { return _allocator; }
    size_t cacheMemoryBytes() const;
    size_t cacheMemoryPeakBytes() const;

private:
    DbResult flushVictim(size_t index);
    DbResult materialize(size_t index);
    DbResult encodeSlot(size_t index);
    DbResult allocateCandidate(uint8_t** outImage);
    DbResult chooseTarget(size_t& outIndex);
    void discardMaterialized(BufferSlot& slot);
    void syncSlot(BufferSlot& slot, const GxPageCacheEntry& entry);

    DatabaseFile& _file;
    uint32_t _capacity;
    uint32_t _pinned;
    GxAllocator _allocator;
    GxPageCache _cache;
    BufferSlot* _slots;
    size_t _slotBytes;
    uint32_t _slotsConstructed;
    bool _initialized;
    bool _useHostedAllocator;
};

} // namespace db
} // namespace gxos
