#ifndef GX_PORTABLE_INDEX_LEAF_TRAVERSAL_H
#define GX_PORTABLE_INDEX_LEAF_TRAVERSAL_H

#include <stddef.h>
#include <stdint.h>

#include "gx_index_leaf_entry.h"

// Page access results are separate from leaf-format validation. A provider
// supplies bytes; the portable traversal validates and interprets them.
enum class GxIndexPageProviderStatus {
    Ok = 0,
    MissingPage,
    InvalidPageId,
    Error
};

// A borrowed payload span. `token` is opaque to the traversal and is returned
// unchanged to release(). A successful acquire creates exactly one view that
// must be released exactly once. A failed acquire creates no view.
struct GxIndexPageView {
    const uint8_t* bytes;
    size_t length;
    void* token;
};

typedef GxIndexPageProviderStatus (*GxIndexPageAcquireFn)(
    void* context, uint64_t pageId, GxIndexPageView* outView);
typedef void (*GxIndexPageReleaseFn)(
    void* context, uint64_t pageId, const GxIndexPageView* view);

struct GxIndexPageProvider {
    void* context;
    GxIndexPageAcquireFn acquire;
    GxIndexPageReleaseFn release;
};

enum class GxIndexLeafVisitControl {
    Continue = 0,
    Stop,
    Error
};

// The entry view is borrowed from the currently acquired page. A visitor must
// consume or copy it before returning and must not retain it, release the page,
// or re-enter this traversal.
typedef GxIndexLeafVisitControl (*GxIndexLeafMatchVisitorFn)(
    void* context, uint64_t pageId, const GxIndexLeafEntryView* entry);

enum class GxIndexLeafTraversalStatus {
    EndOfChain = 0,
    KeyBoundary,
    VisitorStopped,
    InvalidArgument,
    InvalidPageId,
    MissingPage,
    ProviderFailure,
    CorruptPage,
    TraversalLimit,
    VisitorFailure
};

struct GxIndexLeafTraversalResult {
    uint64_t pagesVisited;
    uint64_t matchesVisited;
    uint64_t lastPageId;
    GxIndexPageProviderStatus providerStatus;
    GxIndexLeafError leafError;
};

// Searches an existing linked leaf chain for all entries matching the logical
// key prefix of `physicalLowerBound`. The lower bound must be
// logicalKeyBytes || 12 zero bytes, which preserves every locator for that
// logical key. Each leaf uses gxIndexLeafLookup() to position the search, and
// gxIndexLeafCursorReadNext() to visit later duplicates.
//
// Page identifiers follow the database's established page-store contract:
// page 0 is reserved and valid page ids are [1, pageCount). maxPagesToVisit is
// finite and is capped at pageCount - 1, the maximum number of distinct
// ordinary pages. If a nonterminal link remains at the limit, the result is
// TraversalLimit; without caller-owned visited-id storage this does not claim
// that a particular cycle was diagnosed.
//
// The provider must keep a successful view's bytes stable until release() and
// may reuse the same address for any later acquire. The traversal releases
// each page before requesting its successor and never retains a borrowed entry
// after release. `previousPhysicalKey` is caller-owned scratch used to compare
// adjacent nonempty leaf boundaries; it must have room for one complete
// physical key (page payload size is a sufficient capacity). No allocation or
// page mutation occurs in this operation.
//
// The output and scratch spans must not overlap the lower-bound bytes. They
// must also be disjoint from each acquired provider view. The result contains
// counters only and never returns a borrowed entry view. On argument errors,
// outResult is left unchanged.
GxIndexLeafTraversalStatus gxIndexLeafLookupChain(
    const GxIndexPageProvider* provider,
    uint64_t firstPageId,
    uint64_t pageCount,
    uint32_t expectedOwnerIndexId,
    uint64_t maxPagesToVisit,
    const uint8_t* physicalLowerBound,
    size_t physicalLowerBoundLength,
    uint8_t* previousPhysicalKey,
    size_t previousPhysicalKeyCapacity,
    GxIndexLeafMatchVisitorFn visitor,
    void* visitorContext,
    GxIndexLeafTraversalResult* outResult);

#endif
