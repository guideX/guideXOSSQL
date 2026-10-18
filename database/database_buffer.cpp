#include "database_buffer.h"

#include <new>
#include <type_traits>
#include <utility>

#include "portable/gx_allocator_hosted.h"
#include "portable/gx_page_codec.h"

namespace gxos {
namespace db {

namespace {

static_assert(std::is_nothrow_move_assignable<DatabasePage>::value,
              "published page views must be installable without allocation");

DbResult cacheStatus(GxStatus status, const char* operation) {
    switch (status) {
    case GxStatus::Ok:
        return DbResult::ok();
    case GxStatus::OutOfMemory:
        return DbResult::error(DbStatus::NoSpace, operation);
    case GxStatus::ResourceLimit:
    case GxStatus::Overflow:
        return DbResult::error(DbStatus::TransactionTooLarge, operation);
    case GxStatus::UnsupportedAlignment:
        return DbResult::error(DbStatus::Internal, operation);
    case GxStatus::InvalidArgument:
    case GxStatus::CorruptData:
        return DbResult::error(DbStatus::InvalidArgument, operation);
    }
    return DbResult::error(DbStatus::Internal, operation);
}

DbResult codecStatus(GxStatus status, const char* operation) {
    if (status == GxStatus::Ok) return DbResult::ok();
    if (status == GxStatus::InvalidArgument) {
        return DbResult::error(DbStatus::InvalidArgument, operation);
    }
    if (status == GxStatus::CorruptData) {
        return DbResult::error(DbStatus::CorruptPage, operation);
    }
    return cacheStatus(status, operation);
}

} // namespace

BufferManager::BufferManager(DatabaseFile& file, uint32_t capacity,
                             const GxAllocator* allocator)
    : _file(file), _capacity(capacity == 0 ? 1 : capacity), _pinned(0),
      _allocator(), _cache(), _slots(nullptr), _slotBytes(0),
      _slotsConstructed(0), _initialized(false),
      _useHostedAllocator(allocator == nullptr) {
    if (allocator) _allocator = *allocator;
}

BufferManager::~BufferManager() {
    if (_slots != nullptr) {
        for (uint32_t i = 0; i < _slotsConstructed; ++i) {
            _slots[i].~BufferSlot();
        }
        gxAllocatorFree(&_allocator, _slots);
        _slots = nullptr;
    }
    _slotsConstructed = 0;
    _slotBytes = 0;
    gxPageCacheDestroy(&_cache);
    _initialized = false;
}

DbResult BufferManager::init() {
    if (_initialized) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "buffer manager is already initialized");
    }
    if (_useHostedAllocator) {
        GxStatus hostedStatus = gxHostedAllocatorInit(
            &_allocator, static_cast<size_t>(-1));
        if (hostedStatus != GxStatus::Ok) {
            return cacheStatus(hostedStatus, "page-cache allocator setup failed");
        }
    } else if (_allocator.allocate == nullptr ||
               _allocator.deallocate == nullptr) {
        return DbResult::error(
            DbStatus::InvalidArgument,
            "page-cache allocator must provide allocate and deallocate callbacks");
    }
    if (_capacity > static_cast<size_t>(-1) / sizeof(BufferSlot)) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "buffer slot allocation size overflows");
    }
    const size_t slotBytes = static_cast<size_t>(_capacity) * sizeof(BufferSlot);
    const size_t pageSize = _file.pageSize();
    if (pageSize == 0 ||
        static_cast<size_t>(_capacity) > static_cast<size_t>(-1) / pageSize) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "page-cache image capacity overflows");
    }
    const size_t maximumImageBytes = static_cast<size_t>(_capacity) * pageSize;
    if (maximumImageBytes > static_cast<size_t>(-1) - pageSize) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "page-cache replacement capacity overflows");
    }
    const size_t descriptorBytes =
        static_cast<size_t>(_capacity) * sizeof(GxPageCacheEntry);
    const size_t transientImageBytes = maximumImageBytes + pageSize;
    if (transientImageBytes > static_cast<size_t>(-1) - descriptorBytes ||
        slotBytes > static_cast<size_t>(-1) - descriptorBytes -
                        transientImageBytes) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "page-cache total capacity overflows");
    }
    if (alignof(BufferSlot) > _allocator.maxAlignment) {
        return DbResult::error(DbStatus::Internal,
                               "page-cache allocator alignment is insufficient");
    }

    GxStatus status = gxPageCacheInit(&_cache, &_allocator, _capacity,
                                      _file.pageSize());
    if (status != GxStatus::Ok) {
        return cacheStatus(status, "page-cache entry allocation failed");
    }

    _slotBytes = slotBytes;
    void* allocation = nullptr;
    status = gxAllocatorAllocate(&_allocator, _slotBytes, alignof(BufferSlot),
                                 &allocation);
    if (status != GxStatus::Ok) {
        gxPageCacheDestroy(&_cache);
        _slotBytes = 0;
        return cacheStatus(status, "page-cache view allocation failed");
    }

    _slots = static_cast<BufferSlot*>(allocation);
    for (uint32_t i = 0; i < _capacity; ++i) {
        new (static_cast<void*>(&_slots[i])) BufferSlot();
        _slots[i].cacheIndex = i;
        ++_slotsConstructed;
    }
    _initialized = true;
    return DbResult::ok();
}

size_t BufferManager::cacheMemoryBytes() const {
    if (_slotBytes > static_cast<size_t>(-1) - _cache.stats.currentBytes) {
        return static_cast<size_t>(-1);
    }
    return _cache.stats.currentBytes + _slotBytes;
}

size_t BufferManager::cacheMemoryPeakBytes() const {
    if (_slotBytes > static_cast<size_t>(-1) - _cache.stats.peakBytes) {
        return static_cast<size_t>(-1);
    }
    return _cache.stats.peakBytes + _slotBytes;
}

DbResult BufferManager::allocateCandidate(uint8_t** outImage) {
    const GxStatus status = gxPageCacheAllocateImage(&_cache, outImage);
    return cacheStatus(status, "page-cache image allocation failed");
}

DbResult BufferManager::chooseTarget(size_t& outIndex) {
    outIndex = gxPageCacheEmptyIndex(&_cache);
    if (outIndex != GX_PAGE_CACHE_NO_INDEX) return DbResult::ok();
    outIndex = gxPageCacheVictimIndex(&_cache);
    if (outIndex == GX_PAGE_CACHE_NO_INDEX) {
        return DbResult::error(DbStatus::Internal,
                               "buffer pool exhausted: all slots pinned");
    }
    return DbResult::ok();
}

void BufferManager::syncSlot(BufferSlot& slot,
                             const GxPageCacheEntry& entry) {
    slot.pageId = entry.pageId;
    slot.dirty = entry.dirty;
    slot.pinCount = entry.pinCount;
    slot.lastUsed = entry.lastUsed;
}

void BufferManager::discardMaterialized(BufferSlot& slot) {
    if (!slot.materialized) return;
    slot.page = DatabasePage();
    slot.materialized = false;
}

DbResult BufferManager::materialize(size_t index) {
    if (index >= _capacity) {
        return DbResult::error(DbStatus::Internal,
                               "page-cache entry index is invalid");
    }
    BufferSlot& slot = _slots[index];
    GxPageCacheEntry& entry = _cache.entries[index];
    if (slot.materialized) return DbResult::ok();
    try {
        DbResult result = DatabasePage::parse(entry.image, _file.pageSize(),
                                              entry.pageId, slot.page);
        if (!result.isOk()) return result;
    } catch (const std::bad_alloc&) {
        return DbResult::error(DbStatus::NoSpace,
                               "page-view allocation failed");
    }
    slot.materialized = true;
    syncSlot(slot, entry);
    return DbResult::ok();
}

DbResult BufferManager::encodeSlot(size_t index) {
    if (index >= _capacity) {
        return DbResult::error(DbStatus::Internal,
                               "page-cache entry index is invalid");
    }
    BufferSlot& slot = _slots[index];
    GxPageCacheEntry& entry = _cache.entries[index];
    if (!slot.materialized || entry.pageId == 0 || !entry.image) {
        return DbResult::error(DbStatus::Internal,
                               "page-cache view is not materialized");
    }
    const GxStatus status = gxDatabasePageEncode(
        entry.image, _cache.pageSize, entry.pageId,
        static_cast<uint16_t>(slot.page.type), slot.page.flags,
        slot.page.generation,
        slot.page.payload.empty() ? nullptr : slot.page.payload.data(),
        slot.page.payload.size());
    if (status != GxStatus::Ok) {
        return codecStatus(status, "page-cache image encoding failed");
    }
    slot.page.pageId = entry.pageId;
    slot.page.payloadSize = static_cast<uint32_t>(slot.page.payload.size());
    return DbResult::ok();
}

DbResult BufferManager::flushVictim(size_t index) {
    if (index >= _capacity) {
        return DbResult::error(DbStatus::Internal,
                               "page-cache victim index is invalid");
    }
    GxPageCacheEntry& entry = _cache.entries[index];
    if (!entry.dirty) return DbResult::ok();
    DbResult result = _file.writePageImage(entry.pageId, entry.image,
                                           _cache.pageSize);
    if (!result.isOk()) return result;
    gxPageCacheClearDirty(&_cache, &entry);
    syncSlot(_slots[index], entry);
    return DbResult::ok();
}

DbResult BufferManager::pinPage(uint64_t pageId, BufferSlot*& outSlot) {
    outSlot = nullptr;
    if (!_initialized) {
        return DbResult::error(DbStatus::NotOpen,
                               "buffer manager is not initialized");
    }
    if (pageId == kHeaderPageId) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "page 0 is the header page");
    }
    if (_pinned == UINT32_MAX) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "buffer pin count is exhausted");
    }

    size_t index = GX_PAGE_CACHE_NO_INDEX;
    GxPageCacheEntry* existing = gxPageCacheLookup(&_cache, pageId, &index);
    if (existing != nullptr) {
        const bool wasMaterialized = _slots[index].materialized;
        DbResult result = materialize(index);
        if (!result.isOk()) return result;
        const GxStatus pinStatus = gxPageCachePin(&_cache, existing);
        if (pinStatus != GxStatus::Ok) {
            if (!wasMaterialized && existing->pinCount == 0) {
                discardMaterialized(_slots[index]);
            }
            return cacheStatus(pinStatus, "page-cache pin failed");
        }
        ++_pinned;
        syncSlot(_slots[index], *existing);
        outSlot = &_slots[index];
        return DbResult::ok();
    }

    DbResult result = chooseTarget(index);
    if (!result.isOk()) return result;

    uint8_t* candidate = nullptr;
    result = allocateCandidate(&candidate);
    if (!result.isOk()) return result;
    result = _file.readPageImage(pageId, candidate, _cache.pageSize);
    if (!result.isOk()) {
        gxPageCacheFreeImage(&_cache, candidate);
        return result;
    }

    DatabasePage page;
    try {
        result = DatabasePage::parse(candidate, _cache.pageSize, pageId, page);
    } catch (const std::bad_alloc&) {
        result = DbResult::error(DbStatus::NoSpace,
                                 "page-view allocation failed");
    }
    if (!result.isOk()) {
        gxPageCacheFreeImage(&_cache, candidate);
        return result;
    }

    GxPageCacheEntry& target = _cache.entries[index];
    if (target.pageId != 0 && target.dirty) {
        result = flushVictim(index);
        if (!result.isOk()) {
            gxPageCacheFreeImage(&_cache, candidate);
            return result;
        }
    }
    const GxStatus publishStatus = gxPageCachePublish(
        &_cache, index, pageId, candidate, false, true);
    if (publishStatus != GxStatus::Ok) {
        gxPageCacheFreeImage(&_cache, candidate);
        return cacheStatus(publishStatus, "page-cache publication failed");
    }

    BufferSlot& slot = _slots[index];
    discardMaterialized(slot);
    slot.page = std::move(page);
    slot.materialized = true;
    slot.cacheIndex = static_cast<uint32_t>(index);
    ++_pinned;
    syncSlot(slot, _cache.entries[index]);
    outSlot = &slot;
    return DbResult::ok();
}

DbResult BufferManager::pinAllocatedPage(uint64_t pageId,
                                         BufferSlot*& outSlot) {
    outSlot = nullptr;
    if (!_initialized) {
        return DbResult::error(DbStatus::NotOpen,
                               "buffer manager is not initialized");
    }
    if (pageId == kHeaderPageId) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "page 0 is the header page");
    }
    if (pageId >= _file.pageCount()) {
        return DbResult::error(DbStatus::OutOfBounds,
                               "allocated page id is outside the database");
    }
    if (_pinned == UINT32_MAX) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "buffer pin count is exhausted");
    }

    size_t index = GX_PAGE_CACHE_NO_INDEX;
    GxPageCacheEntry* existing = gxPageCacheLookup(&_cache, pageId, &index);
    if (existing != nullptr) {
        const bool wasMaterialized = _slots[index].materialized;
        DbResult result = materialize(index);
        if (!result.isOk()) return result;
        const GxStatus pinStatus = gxPageCachePin(&_cache, existing);
        if (pinStatus != GxStatus::Ok) {
            if (!wasMaterialized && existing->pinCount == 0) {
                discardMaterialized(_slots[index]);
            }
            return cacheStatus(pinStatus, "page-cache pin failed");
        }
        const GxStatus dirtyStatus = gxPageCacheMarkDirty(&_cache, existing);
        if (dirtyStatus != GxStatus::Ok) {
            (void)gxPageCacheUnpin(&_cache, existing);
            if (existing->pinCount == 0) discardMaterialized(_slots[index]);
            syncSlot(_slots[index], *existing);
            return cacheStatus(dirtyStatus, "page-cache dirty mark failed");
        }
        ++_pinned;
        syncSlot(_slots[index], *existing);
        outSlot = &_slots[index];
        return DbResult::ok();
    }

    DbResult result = chooseTarget(index);
    if (!result.isOk()) return result;
    uint8_t* candidate = nullptr;
    result = allocateCandidate(&candidate);
    if (!result.isOk()) return result;
    const GxStatus encodeStatus = gxDatabasePageEncode(
        candidate, _cache.pageSize, pageId,
        static_cast<uint16_t>(PageType::Data), 0, 0, nullptr, 0);
    if (encodeStatus != GxStatus::Ok) {
        gxPageCacheFreeImage(&_cache, candidate);
        return codecStatus(encodeStatus, "allocated page encoding failed");
    }

    GxPageCacheEntry& target = _cache.entries[index];
    if (target.pageId != 0 && target.dirty) {
        result = flushVictim(index);
        if (!result.isOk()) {
            gxPageCacheFreeImage(&_cache, candidate);
            return result;
        }
    }
    const GxStatus publishStatus = gxPageCachePublish(
        &_cache, index, pageId, candidate, true, true);
    if (publishStatus != GxStatus::Ok) {
        gxPageCacheFreeImage(&_cache, candidate);
        return cacheStatus(publishStatus, "page-cache publication failed");
    }

    BufferSlot& slot = _slots[index];
    discardMaterialized(slot);
    slot.page.pageId = pageId;
    slot.page.type = PageType::Data;
    slot.page.flags = 0;
    slot.page.payloadSize = 0;
    slot.page.generation = 0;
    slot.materialized = true;
    slot.cacheIndex = static_cast<uint32_t>(index);
    ++_pinned;
    syncSlot(slot, _cache.entries[index]);
    outSlot = &slot;
    return DbResult::ok();
}

DbResult BufferManager::installPage(uint64_t pageId,
                                    const DatabasePage& page) {
    if (!_initialized) {
        return DbResult::error(DbStatus::NotOpen,
                               "buffer manager is not initialized");
    }
    if (pageId == kHeaderPageId) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "page 0 is the header page");
    }
    if (pageId >= _file.pageCount()) {
        return DbResult::error(DbStatus::OutOfBounds,
                               "committed page id is outside the database");
    }

    size_t index = GX_PAGE_CACHE_NO_INDEX;
    GxPageCacheEntry* existing = gxPageCacheLookup(&_cache, pageId, &index);
    if (existing != nullptr) {
        DatabasePage viewCopy;
        const bool replaceView = existing->pinCount != 0;
        if (replaceView) {
            try {
                viewCopy = page;
            } catch (const std::bad_alloc&) {
                return DbResult::error(DbStatus::NoSpace,
                                       "page-view copy allocation failed");
            }
        }
        const GxStatus touchStatus = gxPageCacheTouch(&_cache, existing);
        if (touchStatus != GxStatus::Ok) {
            return cacheStatus(touchStatus, "page-cache LRU clock exhausted");
        }
        const GxStatus encodeStatus = gxDatabasePageEncode(
            existing->image, _cache.pageSize, pageId,
            static_cast<uint16_t>(page.type), page.flags, page.generation,
            page.payload.empty() ? nullptr : page.payload.data(),
            page.payload.size());
        if (encodeStatus != GxStatus::Ok) {
            return codecStatus(encodeStatus, "committed page encoding failed");
        }
        const GxStatus dirtyStatus = gxPageCacheMarkDirty(&_cache, existing);
        if (dirtyStatus != GxStatus::Ok) {
            return cacheStatus(dirtyStatus, "page-cache dirty mark failed");
        }
        if (replaceView) {
            viewCopy.pageId = pageId;
            _slots[index].page = std::move(viewCopy);
            _slots[index].materialized = true;
        }
        syncSlot(_slots[index], *existing);
        return DbResult::ok();
    }

    DbResult result = chooseTarget(index);
    if (!result.isOk()) return result;
    uint8_t* candidate = nullptr;
    result = allocateCandidate(&candidate);
    if (!result.isOk()) return result;
    const GxStatus encodeStatus = gxDatabasePageEncode(
        candidate, _cache.pageSize, pageId,
        static_cast<uint16_t>(page.type), page.flags, page.generation,
        page.payload.empty() ? nullptr : page.payload.data(),
        page.payload.size());
    if (encodeStatus != GxStatus::Ok) {
        gxPageCacheFreeImage(&_cache, candidate);
        return codecStatus(encodeStatus, "committed page encoding failed");
    }

    GxPageCacheEntry& target = _cache.entries[index];
    if (target.pageId != 0 && target.dirty) {
        result = flushVictim(index);
        if (!result.isOk()) {
            gxPageCacheFreeImage(&_cache, candidate);
            return result;
        }
    }
    const GxStatus publishStatus = gxPageCachePublish(
        &_cache, index, pageId, candidate, true, false);
    if (publishStatus != GxStatus::Ok) {
        gxPageCacheFreeImage(&_cache, candidate);
        return cacheStatus(publishStatus, "page-cache publication failed");
    }
    BufferSlot& slot = _slots[index];
    discardMaterialized(slot);
    slot.cacheIndex = static_cast<uint32_t>(index);
    syncSlot(slot, _cache.entries[index]);
    return DbResult::ok();
}

DbResult BufferManager::markDirty(BufferSlot& slot) {
    if (!_initialized || slot.cacheIndex >= _capacity ||
        &_slots[slot.cacheIndex] != &slot || !slot.materialized ||
        slot.pinCount == 0) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "page-cache slot is not pinned");
    }
    GxPageCacheEntry& entry = _cache.entries[slot.cacheIndex];
    if (entry.pageId == 0 || entry.pageId != slot.pageId) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "page-cache slot identity is stale");
    }
    DbResult result = encodeSlot(slot.cacheIndex);
    if (!result.isOk()) return result;
    const GxStatus status = gxPageCacheMarkDirty(&_cache, &entry);
    if (status != GxStatus::Ok) {
        return cacheStatus(status, "page-cache dirty mark failed");
    }
    syncSlot(slot, entry);
    return DbResult::ok();
}

DbResult BufferManager::unpin(BufferSlot& slot) {
    if (!_initialized || slot.cacheIndex >= _capacity ||
        &_slots[slot.cacheIndex] != &slot) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "page-cache slot is invalid");
    }
    GxPageCacheEntry& entry = _cache.entries[slot.cacheIndex];
    if (entry.pageId == 0 || entry.pageId != slot.pageId ||
        entry.pinCount == 0) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "page-cache slot is not pinned");
    }
    if (entry.dirty && slot.materialized) {
        DbResult result = encodeSlot(slot.cacheIndex);
        if (!result.isOk()) return result;
    }
    const GxStatus status = gxPageCacheUnpin(&_cache, &entry);
    if (status != GxStatus::Ok) {
        return cacheStatus(status, "page-cache unpin failed");
    }
    if (_pinned != 0) --_pinned;
    if (entry.pinCount == 0) discardMaterialized(slot);
    syncSlot(slot, entry);
    return DbResult::ok();
}

DbResult BufferManager::flush() {
    if (!_initialized) {
        return DbResult::error(DbStatus::NotOpen,
                               "buffer manager is not initialized");
    }
    for (uint32_t i = 0; i < _capacity; ++i) {
        GxPageCacheEntry& entry = _cache.entries[i];
        if (!entry.dirty) continue;
        if (_slots[i].materialized) {
            DbResult encoded = encodeSlot(i);
            if (!encoded.isOk()) return encoded;
        }
        DbResult result = _file.writePageImage(entry.pageId, entry.image,
                                               _cache.pageSize);
        if (!result.isOk()) return result;
        gxPageCacheClearDirty(&_cache, &entry);
        syncSlot(_slots[i], entry);
    }
    return _file.flush();
}

} // namespace db
} // namespace gxos
