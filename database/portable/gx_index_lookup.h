#ifndef GX_PORTABLE_INDEX_LOOKUP_H
#define GX_PORTABLE_INDEX_LOOKUP_H

#include <stddef.h>
#include <stdint.h>

#include "gx_index_tree_descent.h"

enum class GxIndexLookupStatus {
    Complete = 0,
    VisitorStopped,
    InvalidArgument,
    InvalidPageId,
    MissingPage,
    ProviderFailure,
    CorruptPage,
    Cycle,
    TraversalLimit,
    VisitorFailure
};

enum class GxIndexLookupError {
    None = 0,
    ProviderView,
    Descent,
    LeafTraversal
};

enum class GxIndexLookupVisitControl {
    Continue = 0,
    Stop,
    Error
};

// Receives locator values, not page-owned bytes. The values are valid for the
// duration of the call and may be copied by the caller. Returning Stop ends
// successfully with VisitorStopped; returning Error reports VisitorFailure.
// The callback must not retain provider views, release a page, or re-enter
// this lookup operation.
typedef GxIndexLookupVisitControl (*GxIndexLookupMatchVisitorFn)(
    void* context, uint64_t locatorPageId, uint32_t locatorSlot);

struct GxIndexLookupResult {
    uint64_t leafPageId;
    uint64_t descentPagesVisited;
    uint64_t leafPagesVisited;
    uint64_t matchesVisited;
    uint32_t depth;
    GxIndexPageProviderStatus providerStatus;
    GxIndexTreeDescentStatus descentStatus;
    GxIndexTreeDescentError descentError;
    GxIndexInternalNodeError internalNodeError;
    GxIndexLeafTraversalStatus leafTraversalStatus;
    GxIndexLeafError leafError;
    GxIndexLookupError error;
};

// Performs one bounded exact logical-key lookup over an existing B+ tree.
// physicalLowerBound is caller-owned scratch with room for
// logicalKeyLength + 12 bytes; the operation copies the logical key and adds
// the established all-zero minimum row-locator suffix. previousPhysicalKey
// is the bounded leaf-chain comparator scratch required by
// gxIndexLeafLookupChain. visitedPathPageIds is the caller-owned descent
// cycle-detection span and must contain at least maxDepth + 1 elements.
//
// maxDepth is capped by kMaxIndexDepth. maxLeafPagesToVisit must be finite,
// nonzero, and is capped at pageCount - 1. The leaf-chain traversal reports
// TraversalLimit if a nonterminal link remains at that bound; callers that
// need earlier leaf-cycle diagnosis may enforce it in their provider. The
// hosted adapter uses its established GxPageVisitSet for this purpose.
//
// All scratch and output spans must be disjoint from one another, the logical
// key, and every acquired page view. Successful provider acquisitions are
// released exactly once before another page is acquired and before this call
// returns. A provider may reuse the same backing memory after release. No
// allocation, page mutation, hosted runtime, or borrowed result view is used.
//
// Complete includes the no-match case (matchesVisited == 0). A later error
// can follow earlier visitor calls; in that case the non-success status is
// authoritative and the delivered matches are only a partial result. Invalid
// arguments leave outResult unchanged. Other outcomes publish traversal
// counters and diagnostics.
GxIndexLookupStatus gxIndexLookupAll(
    const GxIndexPageProvider* provider,
    uint64_t rootPageId,
    uint64_t pageCount,
    uint32_t expectedOwnerIndexId,
    const uint8_t* logicalKeyBytes,
    size_t logicalKeyLength,
    uint32_t maxDepth,
    uint64_t* visitedPathPageIds,
    size_t visitedPathPageIdCapacity,
    uint64_t maxLeafPagesToVisit,
    uint8_t* physicalLowerBound,
    size_t physicalLowerBoundCapacity,
    uint8_t* previousPhysicalKey,
    size_t previousPhysicalKeyCapacity,
    GxIndexLookupMatchVisitorFn visitor,
    void* visitorContext,
    GxIndexLookupResult* outResult);

#endif
