#ifndef GX_PORTABLE_INDEX_TREE_DESCENT_H
#define GX_PORTABLE_INDEX_TREE_DESCENT_H

#include <stddef.h>
#include <stdint.h>

#include "gx_index_internal_node.h"
#include "gx_index_leaf_traversal.h"

enum class GxIndexTreeDescentStatus {
    Success = 0,
    InvalidArgument,
    InvalidPageId,
    MissingPage,
    ProviderFailure,
    CorruptPage,
    Cycle,
    TraversalLimit
};

enum class GxIndexTreeDescentError {
    None = 0,
    PageIdentityMismatch,
    InvalidPageType,
    InvalidPageView,
    LeafFormat,
    LeafOwnerMismatch,
    InternalNodeFormat
};

struct GxIndexTreeDescentResult {
    uint64_t leafPageId;
    uint64_t pagesVisited;
    uint32_t depth;
    GxIndexPageProviderStatus providerStatus;
    GxIndexTreeDescentError error;
    GxIndexInternalNodeError internalNodeError;
    GxIndexLeafError leafError;
};

// Reads a root-to-leaf path using only caller-owned spans and the established
// page provider. pageCount bounds every page identifier. maxDepth is the
// maximum number of parent-to-child edges (0 permits a root leaf) and may not
// exceed kMaxIndexDepth; visitedPageIds must have maxDepth + 1 elements for
// exact path-cycle detection. Successful page borrows are always released
// before the next acquire, and no borrowed pointer is returned.
GxIndexTreeDescentStatus gxIndexTreeDescendToLeaf(
    const GxIndexPageProvider* provider, uint64_t rootPageId,
    uint64_t pageCount, uint32_t expectedOwnerIndexId,
    const uint8_t* physicalSearchKey, size_t physicalSearchKeyLength,
    uint32_t maxDepth, uint64_t* visitedPageIds,
    size_t visitedPageIdCapacity, GxIndexTreeDescentResult* outResult);

#endif
