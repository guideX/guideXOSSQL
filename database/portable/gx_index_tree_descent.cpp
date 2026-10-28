#include "gx_index_tree_descent.h"

#include "database_format.h"
#include "gx_index_leaf_entry.h"

namespace {

const size_t kPhysicalLocatorSize = 12;

GxIndexTreeDescentStatus providerFailure(
    GxIndexPageProviderStatus status,
    GxIndexTreeDescentResult& result) {
    result.providerStatus = status;
    if (status == GxIndexPageProviderStatus::MissingPage) {
        return GxIndexTreeDescentStatus::MissingPage;
    }
    if (status == GxIndexPageProviderStatus::InvalidPageId) {
        return GxIndexTreeDescentStatus::InvalidPageId;
    }
    return GxIndexTreeDescentStatus::ProviderFailure;
}

bool wasVisited(const uint64_t* pageIds, size_t count, uint64_t pageId) {
    for (size_t i = 0; i < count; ++i) {
        if (pageIds[i] == pageId) return true;
    }
    return false;
}

} // namespace

GxIndexTreeDescentStatus gxIndexTreeDescendToLeaf(
    const GxIndexPageProvider* provider, uint64_t rootPageId,
    uint64_t pageCount, uint32_t expectedOwnerIndexId,
    const uint8_t* physicalSearchKey, size_t physicalSearchKeyLength,
    uint32_t maxDepth, uint64_t* visitedPageIds,
    size_t visitedPageIdCapacity, GxIndexTreeDescentResult* outResult) {
    if (provider == nullptr || provider->acquire == nullptr ||
        provider->release == nullptr || outResult == nullptr ||
        physicalSearchKey == nullptr ||
        physicalSearchKeyLength < kPhysicalLocatorSize || pageCount < 2 ||
        rootPageId == 0 || rootPageId >= pageCount ||
        maxDepth > gxos::db::kMaxIndexDepth || visitedPageIds == nullptr ||
        visitedPageIdCapacity < static_cast<size_t>(maxDepth) + 1u) {
        return GxIndexTreeDescentStatus::InvalidArgument;
    }

    GxIndexTreeDescentResult result = {};
    result.providerStatus = GxIndexPageProviderStatus::Ok;
    result.error = GxIndexTreeDescentError::None;
    result.internalNodeError = GxIndexInternalNodeError::None;
    result.leafError = GxIndexLeafError::None;
    size_t visitedCount = 0;
    uint64_t currentPageId = rootPageId;

    for (uint32_t depth = 0;; ++depth) {
        if (currentPageId == 0 || currentPageId >= pageCount) {
            result.error = GxIndexTreeDescentError::InternalNodeFormat;
            *outResult = result;
            return GxIndexTreeDescentStatus::CorruptPage;
        }
        if (wasVisited(visitedPageIds, visitedCount, currentPageId)) {
            result.error = GxIndexTreeDescentError::InternalNodeFormat;
            *outResult = result;
            return GxIndexTreeDescentStatus::Cycle;
        }
        if (visitedCount >= visitedPageIdCapacity) {
            *outResult = result;
            return GxIndexTreeDescentStatus::TraversalLimit;
        }
        visitedPageIds[visitedCount++] = currentPageId;

        GxIndexPageView view = {};
        const GxIndexPageProviderStatus acquireStatus =
            provider->acquire(provider->context, currentPageId, &view);
        if (acquireStatus != GxIndexPageProviderStatus::Ok) {
            const GxIndexTreeDescentStatus status =
                providerFailure(acquireStatus, result);
            *outResult = result;
            return status;
        }
        ++result.pagesVisited;
        result.depth = depth;

        GxIndexTreeDescentStatus status = GxIndexTreeDescentStatus::CorruptPage;
        uint64_t nextPageId = 0;
        bool descend = false;
        if (view.bytes == nullptr || view.length == 0) {
            result.error = GxIndexTreeDescentError::InvalidPageView;
        } else if (view.pageId != currentPageId) {
            result.error = GxIndexTreeDescentError::PageIdentityMismatch;
        } else if (view.pageType == static_cast<uint16_t>(
                       gxos::db::PageType::IndexLeaf)) {
            GxIndexLeafCursor leafCursor = {};
            GxIndexLeafError leafError = GxIndexLeafError::None;
            GxStatus leafStatus = gxIndexLeafCursorInit(
                view.bytes, view.length, &leafCursor, &leafError);
            if (leafStatus == GxStatus::Ok &&
                leafCursor.page.ownerIndexId != expectedOwnerIndexId) {
                result.error = GxIndexTreeDescentError::LeafOwnerMismatch;
                result.leafError = GxIndexLeafError::InvalidArgument;
            } else if (leafStatus == GxStatus::Ok) {
                leafStatus = gxIndexLeafValidate(view.bytes, view.length,
                                                 &leafError);
                if (leafStatus == GxStatus::Ok) {
                    result.leafPageId = currentPageId;
                    result.error = GxIndexTreeDescentError::None;
                    status = GxIndexTreeDescentStatus::Success;
                } else {
                    result.error = GxIndexTreeDescentError::LeafFormat;
                    result.leafError = leafError;
                }
            } else {
                result.error = GxIndexTreeDescentError::LeafFormat;
                result.leafError = leafError;
            }
        } else if (view.pageType == static_cast<uint16_t>(
                       gxos::db::PageType::IndexInternal)) {
            GxIndexInternalNodeError nodeError =
                GxIndexInternalNodeError::None;
            uint32_t childIndex = 0;
            const GxStatus nodeStatus = gxIndexInternalNodeSelectChild(
                view.bytes, view.length, pageCount, expectedOwnerIndexId,
                physicalSearchKey, physicalSearchKeyLength, &nextPageId,
                &childIndex, &nodeError);
            (void)childIndex;
            result.internalNodeError = nodeError;
            if (nodeStatus == GxStatus::Ok) {
                if (depth >= maxDepth) {
                    status = GxIndexTreeDescentStatus::TraversalLimit;
                } else if (nextPageId == currentPageId ||
                           wasVisited(visitedPageIds, visitedCount,
                                      nextPageId)) {
                    result.error = GxIndexTreeDescentError::InternalNodeFormat;
                    status = GxIndexTreeDescentStatus::Cycle;
                } else {
                    descend = true;
                }
            } else if (nodeStatus == GxStatus::InvalidArgument) {
                result.error = GxIndexTreeDescentError::InternalNodeFormat;
                status = GxIndexTreeDescentStatus::InvalidArgument;
            } else {
                result.error = GxIndexTreeDescentError::InternalNodeFormat;
            }
        } else {
            result.error = GxIndexTreeDescentError::InvalidPageType;
        }

        provider->release(provider->context, currentPageId, &view);
        if (status == GxIndexTreeDescentStatus::Success ||
            status == GxIndexTreeDescentStatus::TraversalLimit ||
            status == GxIndexTreeDescentStatus::Cycle ||
            status == GxIndexTreeDescentStatus::InvalidArgument || !descend) {
            *outResult = result;
            return status;
        }
        currentPageId = nextPageId;
    }
}
