#ifndef GX_PORTABLE_PAGE_CACHE_H
#define GX_PORTABLE_PAGE_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"

#define GX_PAGE_CACHE_NO_INDEX ((size_t)-1)

// Plain-data cache metadata. The serialized page image is separately owned by
// the cache allocator. Entries are allocated once and are never relocated.
struct GxPageCacheEntry {
    uint64_t pageId;
    uint64_t lastUsed;
    uint32_t pinCount;
    uint8_t* image;
    bool dirty;
};

struct GxPageCacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t imageAllocations;
    uint64_t lookupComparisons;
    size_t currentBytes;
    size_t peakBytes;
};

struct GxPageCache {
    GxAllocator allocator;
    GxPageCacheEntry* entries;
    size_t capacity;
    size_t resident;
    size_t dirtyCount;
    uint32_t pageSize;
    uint64_t clock;
    GxPageCacheStats stats;
    bool initialized;
};

// The caller zero-initializes cache before its first init. Init allocates only
// the fixed descriptor table; page images are allocated on cache misses.
GxStatus gxPageCacheInit(GxPageCache* cache, const GxAllocator* allocator,
                         size_t capacity, uint32_t pageSize);
void gxPageCacheDestroy(GxPageCache* cache);

GxPageCacheEntry* gxPageCacheLookup(GxPageCache* cache, uint64_t pageId,
                                    size_t* outIndex);
size_t gxPageCacheEmptyIndex(const GxPageCache* cache);
size_t gxPageCacheVictimIndex(const GxPageCache* cache);

GxStatus gxPageCacheAllocateImage(GxPageCache* cache, uint8_t** outImage);
void gxPageCacheFreeImage(GxPageCache* cache, uint8_t* image);

// Publishes a complete image. Ownership transfers only on success. A pinned
// publication is used for a successful miss so no fallible step remains after
// replacing an eligible victim.
GxStatus gxPageCachePublish(GxPageCache* cache, size_t index, uint64_t pageId,
                            uint8_t* image, bool dirty, bool pin);
GxStatus gxPageCacheTouch(GxPageCache* cache, GxPageCacheEntry* entry);
GxStatus gxPageCachePin(GxPageCache* cache, GxPageCacheEntry* entry);
GxStatus gxPageCacheUnpin(GxPageCache* cache, GxPageCacheEntry* entry);
GxStatus gxPageCacheMarkDirty(GxPageCache* cache, GxPageCacheEntry* entry);
void gxPageCacheClearDirty(GxPageCache* cache, GxPageCacheEntry* entry);

#endif
