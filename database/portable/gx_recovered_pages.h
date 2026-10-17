#ifndef GX_PORTABLE_RECOVERED_PAGES_H
#define GX_PORTABLE_RECOVERED_PAGES_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"

enum { GX_RECOVERED_PAGES_SQL3_MAX_COUNT = 4096 };

// One allocator-owned, plain-data node. `image` contains a complete validated
// serialized database page, not a pointer into WAL scanner scratch storage.
// Nodes are never relocated; the only link is a non-owning list edge.
typedef struct GxRecoveredPage {
    uint64_t pageId;
    struct GxRecoveredPage* next;
} GxRecoveredPage;

typedef struct GxRecoveredPageCollection {
    GxAllocator allocator;
    GxRecoveredPage* head;
    GxRecoveredPage* tail;
    size_t count;
    size_t maximumCount;
    size_t pageSize;
    size_t retainedBytes;
    size_t maximumBytes;
    bool initialized;
} GxRecoveredPageCollection;

// Zero-initialize before first init. The SQL3 adapter uses the existing 4096
// page-per-transaction limit and the WAL byte limit for maximumBytes.
GxStatus gxRecoveredPagesInit(GxRecoveredPageCollection* collection,
                              const GxAllocator* allocator, size_t pageSize,
                              size_t maximumCount, size_t maximumBytes);

// Validates the complete serialized page, then copies it into an independent
// allocator-owned node. A duplicate page id replaces the prior image only
// after allocation and validation succeed; the replacement is placed at the
// tail so iteration follows the last-occurrence replay order.
GxStatus gxRecoveredPagesRetain(GxRecoveredPageCollection* collection,
                                uint64_t pageId, const uint8_t* image,
                                size_t imageSize);

// Move ownership into an empty or initialized destination, leaving source
// empty and safely destroyable. Copying a collection by value is unsupported.
GxStatus gxRecoveredPagesMove(GxRecoveredPageCollection* destination,
                              GxRecoveredPageCollection* source);

void gxRecoveredPagesClear(GxRecoveredPageCollection* collection);
void gxRecoveredPagesDestroy(GxRecoveredPageCollection* collection);
size_t gxRecoveredPagesCount(const GxRecoveredPageCollection* collection);
size_t gxRecoveredPagesBytes(const GxRecoveredPageCollection* collection);
const GxRecoveredPage* gxRecoveredPagesFirst(
    const GxRecoveredPageCollection* collection);
const GxRecoveredPage* gxRecoveredPageNext(const GxRecoveredPage* page);
const uint8_t* gxRecoveredPageImage(const GxRecoveredPage* page);

#endif
