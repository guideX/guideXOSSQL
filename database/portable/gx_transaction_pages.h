#ifndef GX_PORTABLE_TRANSACTION_PAGES_H
#define GX_PORTABLE_TRANSACTION_PAGES_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"

// One transaction-owned, serialized database page. The descriptor array is
// fixed for the lifetime of the set; image storage is separately allocated.
struct GxTransactionPageEntry {
    uint64_t pageId;
    uint8_t* image;
};

struct GxTransactionPageStats {
    uint64_t imageAllocations;
    uint64_t replacements;
    uint64_t allocationFailures;
    size_t currentBytes;
    size_t peakBytes;
};

struct GxTransactionPages {
    GxAllocator allocator;
    GxTransactionPageEntry* entries;
    size_t count;
    size_t capacity;
    size_t pageSize;
    size_t maximumOwnedBytes;
    GxTransactionPageStats stats;
    bool initialized;
};

// Zero-initialize before first use. Initialization allocates a fixed table
// sized for maximumPages; page images are allocated as pages are staged.
GxStatus gxTransactionPagesInit(GxTransactionPages* pages,
                                const GxAllocator* allocator,
                                size_t pageSize, size_t maximumPages);
void gxTransactionPagesDestroy(GxTransactionPages* pages);

// Append preserves first-stage order. The caller must ensure pageId is unique
// in its lookup index. Image bytes are encoded into newly owned storage before
// the entry is published.
GxStatus gxTransactionPagesAppendPage(GxTransactionPages* pages,
                                      uint64_t pageId, uint16_t pageType,
                                      uint16_t pageFlags,
                                      uint32_t pageGeneration,
                                      const uint8_t* payload,
                                      size_t payloadSize,
                                      size_t* outIndex);

// Replaces one staged page with a strong guarantee: the existing image remains
// owned and unchanged unless allocation and encoding of the replacement pass.
GxStatus gxTransactionPagesReplacePage(GxTransactionPages* pages,
                                       size_t index, uint64_t pageId,
                                       uint16_t pageType,
                                       uint16_t pageFlags,
                                       uint32_t pageGeneration,
                                       const uint8_t* payload,
                                       size_t payloadSize);

// Removes only the last appended entry. Used to undo publication when the
// hosted page-id lookup index could not allocate its map node.
GxStatus gxTransactionPagesPopLast(GxTransactionPages* pages,
                                   size_t expectedIndex);
void gxTransactionPagesClear(GxTransactionPages* pages);

const GxTransactionPageEntry* gxTransactionPagesAt(
    const GxTransactionPages* pages, size_t index);
size_t gxTransactionPagesCount(const GxTransactionPages* pages);

// Deep clone preserves order and leaves source untouched on failure. Move
// destroys destination first and transfers the fixed descriptor allocation.
GxStatus gxTransactionPagesClone(GxTransactionPages* destination,
                                 const GxTransactionPages* source);
GxStatus gxTransactionPagesMove(GxTransactionPages* destination,
                                GxTransactionPages* source);

#endif
