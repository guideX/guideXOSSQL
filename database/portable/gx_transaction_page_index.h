#ifndef GX_PORTABLE_TRANSACTION_PAGE_INDEX_H
#define GX_PORTABLE_TRANSACTION_PAGE_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"

// A fixed-capacity page-ID-to-owner-index table. Page ID zero is reserved by
// the database format and represents an empty slot, so this table needs no
// tombstones: entries are removed only by clearing the complete transaction.
struct GxTransactionPageIndexSlot {
    uint64_t pageId;
    size_t ownerIndex;
};

struct GxTransactionPageIndexStats {
    uint64_t lookupHits;
    uint64_t lookupMisses;
    uint64_t lookupProbes;
    uint64_t insertionProbes;
    uint64_t collisions;
    size_t maximumProbeLength;
};

struct GxTransactionPageIndex {
    GxAllocator allocator;
    GxTransactionPageIndexSlot* slots;
    size_t capacity;
    size_t count;
    size_t maximumEntries;
    size_t allocationBytes;
    GxTransactionPageIndexStats stats;
    bool initialized;
};

// Returns the power-of-two slot capacity and allocation size for the requested
// maximum entry count. Capacity is at least twice the entry bound.
GxStatus gxTransactionPageIndexStorageSize(size_t maximumEntries,
                                           size_t* outCapacity,
                                           size_t* outBytes);
GxStatus gxTransactionPageIndexInit(GxTransactionPageIndex* index,
                                    const GxAllocator* allocator,
                                    size_t maximumEntries);
void gxTransactionPageIndexDestroy(GxTransactionPageIndex* index);

// A miss is GxStatus::Ok with outFound=false. Keys must be valid nonzero page
// IDs. Linear probing is bounded by capacity and never allocates after init.
GxStatus gxTransactionPageIndexLookup(GxTransactionPageIndex* index,
                                      uint64_t pageId, bool* outFound,
                                      size_t* outOwnerIndex);
GxStatus gxTransactionPageIndexInsert(GxTransactionPageIndex* index,
                                      uint64_t pageId, size_t ownerIndex);
void gxTransactionPageIndexClear(GxTransactionPageIndex* index);

// Clone copies the complete bounded table. Move transfers its allocation and
// destroys the destination first; both operations preserve owner indices.
GxStatus gxTransactionPageIndexClone(GxTransactionPageIndex* destination,
                                     const GxTransactionPageIndex* source);
GxStatus gxTransactionPageIndexMove(GxTransactionPageIndex* destination,
                                    GxTransactionPageIndex* source);

#endif
