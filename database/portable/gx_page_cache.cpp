#include "gx_page_cache.h"

namespace {

void saturatingIncrement(uint64_t* value) {
    if (*value != UINT64_MAX) ++*value;
}

bool supportedPageSize(uint32_t pageSize) {
    return pageSize >= 512 && pageSize <= 65536 &&
           (pageSize & (pageSize - 1)) == 0;
}

void addCurrentBytes(GxPageCache* cache, size_t bytes) {
    if (bytes > static_cast<size_t>(-1) - cache->stats.currentBytes) {
        cache->stats.currentBytes = static_cast<size_t>(-1);
    } else {
        cache->stats.currentBytes += bytes;
    }
    if (cache->stats.currentBytes > cache->stats.peakBytes) {
        cache->stats.peakBytes = cache->stats.currentBytes;
    }
}

void subtractCurrentBytes(GxPageCache* cache, size_t bytes) {
    if (bytes > cache->stats.currentBytes) {
        cache->stats.currentBytes = 0;
    } else {
        cache->stats.currentBytes -= bytes;
    }
}

GxStatus touch(GxPageCache* cache, GxPageCacheEntry* entry) {
    if (cache->clock == UINT64_MAX) return GxStatus::Overflow;
    ++cache->clock;
    entry->lastUsed = cache->clock;
    return GxStatus::Ok;
}

} // namespace

GxStatus gxPageCacheInit(GxPageCache* cache, const GxAllocator* allocator,
                         size_t capacity, uint32_t pageSize) {
    if (!cache || !allocator || cache->initialized || capacity == 0 ||
        !supportedPageSize(pageSize)) {
        return GxStatus::InvalidArgument;
    }
    if (capacity > static_cast<size_t>(-1) / sizeof(GxPageCacheEntry)) {
        return GxStatus::Overflow;
    }
    if (capacity > static_cast<size_t>(-1) / pageSize) {
        return GxStatus::Overflow;
    }
    const size_t maximumImageBytes = capacity * pageSize;
    if (maximumImageBytes > static_cast<size_t>(-1) - pageSize) {
        return GxStatus::Overflow;
    }

    void* allocation = 0;
    const size_t descriptorBytes = capacity * sizeof(GxPageCacheEntry);
    if (descriptorBytes > static_cast<size_t>(-1) - maximumImageBytes) {
        return GxStatus::Overflow;
    }
    GxStatus status = gxAllocatorAllocate(allocator, descriptorBytes,
                                          alignof(GxPageCacheEntry),
                                          &allocation);
    if (status != GxStatus::Ok) return status;

    cache->allocator = *allocator;
    cache->entries = static_cast<GxPageCacheEntry*>(allocation);
    cache->capacity = capacity;
    cache->resident = 0;
    cache->dirtyCount = 0;
    cache->pageSize = pageSize;
    cache->clock = 0;
    cache->stats.hits = 0;
    cache->stats.misses = 0;
    cache->stats.evictions = 0;
    cache->stats.imageAllocations = 0;
    cache->stats.lookupComparisons = 0;
    cache->stats.currentBytes = descriptorBytes;
    cache->stats.peakBytes = descriptorBytes;
    cache->initialized = true;

    for (size_t i = 0; i < capacity; ++i) {
        GxPageCacheEntry& entry = cache->entries[i];
        entry.pageId = 0;
        entry.lastUsed = 0;
        entry.pinCount = 0;
        entry.image = 0;
        entry.dirty = false;
    }
    return GxStatus::Ok;
}

void gxPageCacheDestroy(GxPageCache* cache) {
    if (!cache || !cache->initialized) return;
    for (size_t i = 0; i < cache->capacity; ++i) {
        gxPageCacheFreeImage(cache, cache->entries[i].image);
        cache->entries[i].image = 0;
    }
    gxAllocatorFree(&cache->allocator, cache->entries);
    cache->entries = 0;
    cache->capacity = 0;
    cache->resident = 0;
    cache->dirtyCount = 0;
    cache->pageSize = 0;
    cache->clock = 0;
    cache->initialized = false;
    cache->stats.currentBytes = 0;
}

GxPageCacheEntry* gxPageCacheLookup(GxPageCache* cache, uint64_t pageId,
                                    size_t* outIndex) {
    if (outIndex) *outIndex = GX_PAGE_CACHE_NO_INDEX;
    if (!cache || !cache->initialized || pageId == 0) return 0;
    for (size_t i = 0; i < cache->capacity; ++i) {
        saturatingIncrement(&cache->stats.lookupComparisons);
        if (cache->entries[i].pageId != pageId) continue;
        saturatingIncrement(&cache->stats.hits);
        if (outIndex) *outIndex = i;
        return &cache->entries[i];
    }
    saturatingIncrement(&cache->stats.misses);
    return 0;
}

size_t gxPageCacheEmptyIndex(const GxPageCache* cache) {
    if (!cache || !cache->initialized) return GX_PAGE_CACHE_NO_INDEX;
    for (size_t i = 0; i < cache->capacity; ++i) {
        if (cache->entries[i].pageId == 0) return i;
    }
    return GX_PAGE_CACHE_NO_INDEX;
}

size_t gxPageCacheVictimIndex(const GxPageCache* cache) {
    if (!cache || !cache->initialized) return GX_PAGE_CACHE_NO_INDEX;
    size_t victim = GX_PAGE_CACHE_NO_INDEX;
    uint64_t oldest = 0;
    for (size_t i = 0; i < cache->capacity; ++i) {
        const GxPageCacheEntry& entry = cache->entries[i];
        if (entry.pageId == 0 || entry.pinCount != 0) continue;
        if (victim == GX_PAGE_CACHE_NO_INDEX || entry.lastUsed < oldest) {
            victim = i;
            oldest = entry.lastUsed;
        }
    }
    return victim;
}

GxStatus gxPageCacheAllocateImage(GxPageCache* cache, uint8_t** outImage) {
    if (outImage) *outImage = 0;
    if (!cache || !cache->initialized || !outImage) {
        return GxStatus::InvalidArgument;
    }
    void* allocation = 0;
    GxStatus status = gxAllocatorAllocate(&cache->allocator, cache->pageSize,
                                          alignof(uint8_t), &allocation);
    if (status != GxStatus::Ok) return status;
    *outImage = static_cast<uint8_t*>(allocation);
    saturatingIncrement(&cache->stats.imageAllocations);
    addCurrentBytes(cache, cache->pageSize);
    return GxStatus::Ok;
}

void gxPageCacheFreeImage(GxPageCache* cache, uint8_t* image) {
    if (!cache || !cache->initialized || !image) return;
    gxAllocatorFree(&cache->allocator, image);
    subtractCurrentBytes(cache, cache->pageSize);
}

GxStatus gxPageCachePublish(GxPageCache* cache, size_t index, uint64_t pageId,
                            uint8_t* image, bool dirty, bool pin) {
    if (!cache || !cache->initialized || index >= cache->capacity ||
        pageId == 0 || !image) {
        return GxStatus::InvalidArgument;
    }
    GxPageCacheEntry& entry = cache->entries[index];
    if (entry.pinCount != 0 ||
        (entry.pageId != 0 && entry.pageId != pageId && entry.dirty)) {
        return GxStatus::InvalidArgument;
    }
    if (!entry.pageId && cache->resident == static_cast<size_t>(-1)) {
        return GxStatus::Overflow;
    }
    if (cache->clock == UINT64_MAX) return GxStatus::Overflow;
    if (dirty && !entry.dirty &&
        cache->dirtyCount == static_cast<size_t>(-1)) {
        return GxStatus::Overflow;
    }

    const bool wasResident = entry.pageId != 0;
    const bool replacingDifferentPage = wasResident && entry.pageId != pageId;
    if (entry.dirty) {
        if (cache->dirtyCount != 0) --cache->dirtyCount;
    }

    uint8_t* priorImage = entry.image;
    entry.pageId = pageId;
    entry.lastUsed = 0;
    entry.pinCount = 0;
    entry.image = image;
    entry.dirty = dirty;
    if (!wasResident) ++cache->resident;
    if (replacingDifferentPage) saturatingIncrement(&cache->stats.evictions);
    if (dirty) ++cache->dirtyCount;
    ++cache->clock;
    entry.lastUsed = cache->clock;
    if (pin) entry.pinCount = 1;
    if (priorImage != image) gxPageCacheFreeImage(cache, priorImage);
    return GxStatus::Ok;
}

GxStatus gxPageCacheReplaceImage(GxPageCache* cache, size_t index,
                                 uint64_t pageId, uint8_t* image,
                                 bool dirty) {
    if (!cache || !cache->initialized || index >= cache->capacity ||
        pageId == 0 || !image) {
        return GxStatus::InvalidArgument;
    }
    GxPageCacheEntry& entry = cache->entries[index];
    if (entry.pageId != pageId || !entry.image || entry.image == image) {
        return GxStatus::InvalidArgument;
    }
    if (cache->clock == UINT64_MAX) return GxStatus::Overflow;
    if (dirty && !entry.dirty &&
        cache->dirtyCount == static_cast<size_t>(-1)) {
        return GxStatus::Overflow;
    }
    if (!dirty && entry.dirty && cache->dirtyCount == 0) {
        return GxStatus::InvalidArgument;
    }

    uint8_t* priorImage = entry.image;
    if (dirty && !entry.dirty) ++cache->dirtyCount;
    if (!dirty && entry.dirty) --cache->dirtyCount;
    entry.image = image;
    entry.dirty = dirty;
    entry.lastUsed = ++cache->clock;
    gxPageCacheFreeImage(cache, priorImage);
    return GxStatus::Ok;
}

GxStatus gxPageCachePin(GxPageCache* cache, GxPageCacheEntry* entry) {
    if (!cache || !cache->initialized || !entry || entry->pageId == 0 ||
        !entry->image) {
        return GxStatus::InvalidArgument;
    }
    if (entry->pinCount == UINT32_MAX) return GxStatus::ResourceLimit;
    GxStatus status = touch(cache, entry);
    if (status != GxStatus::Ok) return status;
    ++entry->pinCount;
    return GxStatus::Ok;
}

GxStatus gxPageCacheTouch(GxPageCache* cache, GxPageCacheEntry* entry) {
    if (!cache || !cache->initialized || !entry || entry->pageId == 0 ||
        !entry->image) {
        return GxStatus::InvalidArgument;
    }
    return touch(cache, entry);
}

GxStatus gxPageCacheUnpin(GxPageCache* cache, GxPageCacheEntry* entry) {
    if (!cache || !cache->initialized || !entry || entry->pageId == 0 ||
        entry->pinCount == 0) {
        return GxStatus::InvalidArgument;
    }
    --entry->pinCount;
    return GxStatus::Ok;
}

GxStatus gxPageCacheMarkDirty(GxPageCache* cache, GxPageCacheEntry* entry) {
    if (!cache || !cache->initialized || !entry || entry->pageId == 0 ||
        !entry->image) {
        return GxStatus::InvalidArgument;
    }
    if (!entry->dirty) {
        if (cache->dirtyCount == static_cast<size_t>(-1)) {
            return GxStatus::Overflow;
        }
        entry->dirty = true;
        ++cache->dirtyCount;
    }
    return GxStatus::Ok;
}

void gxPageCacheClearDirty(GxPageCache* cache, GxPageCacheEntry* entry) {
    if (!cache || !cache->initialized || !entry || !entry->dirty) return;
    entry->dirty = false;
    if (cache->dirtyCount != 0) --cache->dirtyCount;
}
