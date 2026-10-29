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
    // Outer database-page identity and type supplied by the provider. Leaf
    // chain traversal only consumes the payload fields; root descent also
    // checks these values before interpreting the payload. Descent requires
    // the requested page id and actual persisted type (IndexLeaf=3 or
    // IndexInternal=4).
    uint64_t pageId;
    uint16_t pageType;
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

// Snapshot contract for complete index operations: the caller must keep the
// provider context alive for the entire operation, and rootPageId, pageCount,
// and every successful acquire must describe one logically consistent,
// read-only storage view. A snapshot token or matching page generation alone
// does not establish that guarantee; the underlying storage/provider must
// keep the view stable across all page acquisitions. A successful borrow is
// immutable and remains valid through release().

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

// Scans an ordered leaf chain starting at the leaf selected by
// physicalLowerBound. The logical lower and upper bounds compare complete
// encoded logical keys (the row-locator suffix is excluded). The seek probe
// must be lowerKeyBytes || 12 zero locator bytes when hasLower is true, or
// twelve zero bytes when no lower bound is present. A range can therefore
// begin at every duplicate for its lower key before the visitor applies an
// exclusive lower endpoint. A missing upper bound scans to the chain end.
// KeyBoundary means an upper endpoint stopped the scan; EndOfChain means the
// valid zero successor marker ended it. Both are normal completion statuses
// for this operation. Entry views are borrowed only for the visitor call.
GxIndexLeafTraversalStatus gxIndexLeafScanRangeChain(
    const GxIndexPageProvider* provider,
    uint64_t firstPageId,
    uint64_t pageCount,
    uint32_t expectedOwnerIndexId,
    uint64_t maxPagesToVisit,
    const uint8_t* physicalLowerBound,
    size_t physicalLowerBoundLength,
    bool hasLower,
    const uint8_t* lowerKeyBytes,
    size_t lowerKeyLength,
    bool lowerInclusive,
    bool hasUpper,
    const uint8_t* upperKeyBytes,
    size_t upperKeyLength,
    bool upperInclusive,
    uint8_t* previousPhysicalKey,
    size_t previousPhysicalKeyCapacity,
    GxIndexLeafMatchVisitorFn visitor,
    void* visitorContext,
    GxIndexLeafTraversalResult* outResult);

#endif
