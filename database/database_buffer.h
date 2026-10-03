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

#include <cstdint>

#include "database_file.h"
#include "database_page.h"
#include "database_result.h"

namespace gxos {
namespace db {

struct BufferSlot {
    uint64_t pageId;
    DatabasePage page;
    bool dirty;
    uint32_t pinCount;
    uint64_t lastUsed; // LRU clock

    BufferSlot()
        : pageId(0), dirty(false), pinCount(0), lastUsed(0) {}
};

class BufferManager {
public:
    BufferManager(DatabaseFile& file, uint32_t capacity);
    ~BufferManager();

    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;

    // Returns a pinned slot holding `pageId`, reading it from the file on a
    // cache miss. The caller MUST call unpin() exactly once. On failure the
    // slot is not pinned.
    DbResult pinPage(uint64_t pageId, BufferSlot*& outSlot);

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

    void markDirty(BufferSlot& slot);
    void unpin(BufferSlot& slot);

    // Writes all dirty slots, then flushes the underlying file (header +
    // durability barrier). Read-only databases are not written.
    DbResult flush();

    uint32_t capacity() const { return _capacity; }
    uint32_t residentCount() const { return _resident; }
    uint32_t dirtyCount() const { return _dirty; }
    uint32_t pinnedCount() const { return _pinned; }

private:
    BufferSlot* findSlot(uint64_t pageId);
    uint32_t findEmptySlot() const;
    DbResult loadIntoSlot(BufferSlot& slot, uint64_t pageId);
    DbResult evictOne();

    DatabaseFile& _file;
    uint32_t _capacity;
    uint32_t _resident;
    uint32_t _dirty;
    uint32_t _pinned;
    uint64_t _clock;
    BufferSlot* _slots;
};

} // namespace db
} // namespace gxos
