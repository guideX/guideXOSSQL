#include "gx_index_lookup.h"

#include "database_format.h"

namespace {

const size_t kPhysicalLocatorBytes = 12;

enum class ProviderPhase {
    Descent,
    LeafChain
};

struct ProviderBridge {
    const GxIndexPageProvider* source;
    GxIndexPageView borrowedView;
    uint64_t borrowedPageId;
    const uint8_t* logicalKeyBytes;
    size_t logicalKeyLength;
    const uint8_t* physicalLowerBound;
    size_t physicalLowerBoundLength;
    const uint8_t* previousPhysicalKey;
    size_t previousPhysicalKeyCapacity;
    const uint64_t* visitedPathPageIds;
    size_t visitedPathPageIdCapacity;
    const GxIndexLookupResult* outResult;
    bool active;
    bool invalidView;
    bool releaseMismatch;
    ProviderPhase phase;
};

bool rangesOverlap(const void* left, size_t leftLength,
                   const void* right, size_t rightLength) {
    if (left == nullptr || right == nullptr || leftLength == 0 ||
        rightLength == 0) {
        return false;
    }
    const uintptr_t leftStart = reinterpret_cast<uintptr_t>(left);
    const uintptr_t rightStart = reinterpret_cast<uintptr_t>(right);
    const uintptr_t maximum = static_cast<uintptr_t>(-1);
    if (leftLength > static_cast<size_t>(maximum - leftStart) ||
        rightLength > static_cast<size_t>(maximum - rightStart)) {
        return true;
    }
    const uintptr_t leftEnd = leftStart + static_cast<uintptr_t>(leftLength);
    const uintptr_t rightEnd = rightStart + static_cast<uintptr_t>(rightLength);
    return leftStart < rightEnd && rightStart < leftEnd;
}

bool spansOverlap(const void* left, size_t leftLength,
                  const void* right, size_t rightLength) {
    return rangesOverlap(left, leftLength, right, rightLength);
}

void copyBytes(uint8_t* destination, const uint8_t* source, size_t length) {
    for (size_t i = 0; i < length; ++i) destination[i] = source[i];
}

bool sameView(const GxIndexPageView& left, const GxIndexPageView& right) {
    return left.bytes == right.bytes && left.length == right.length &&
        left.pageId == right.pageId && left.pageType == right.pageType &&
        left.token == right.token;
}

GxIndexPageProviderStatus acquirePage(
    void* opaque, uint64_t pageId, GxIndexPageView* outView) {
    ProviderBridge* bridge = static_cast<ProviderBridge*>(opaque);
    if (bridge == nullptr || bridge->source == nullptr || outView == nullptr ||
        bridge->active) {
        return GxIndexPageProviderStatus::Error;
    }

    *outView = GxIndexPageView();
    GxIndexPageView sourceView = {};
    const GxIndexPageProviderStatus status = bridge->source->acquire(
        bridge->source->context, pageId, &sourceView);
    if (status != GxIndexPageProviderStatus::Ok) return status;

    bridge->active = true;
    bridge->borrowedView = sourceView;
    bridge->borrowedPageId = pageId;

    const bool aliasesCallerStorage =
        rangesOverlap(sourceView.bytes, sourceView.length,
                      bridge->logicalKeyBytes, bridge->logicalKeyLength) ||
        rangesOverlap(sourceView.bytes, sourceView.length,
                      bridge->physicalLowerBound,
                      bridge->physicalLowerBoundLength) ||
        rangesOverlap(sourceView.bytes, sourceView.length,
                      bridge->previousPhysicalKey,
                      bridge->previousPhysicalKeyCapacity) ||
        rangesOverlap(sourceView.bytes, sourceView.length,
                      bridge->visitedPathPageIds,
                      bridge->visitedPathPageIdCapacity * sizeof(uint64_t)) ||
        rangesOverlap(sourceView.bytes, sourceView.length,
                      bridge->outResult, sizeof(*bridge->outResult));
    const bool invalidLeafView = bridge->phase == ProviderPhase::LeafChain &&
        (sourceView.pageId != pageId || sourceView.pageType !=
         static_cast<uint16_t>(gxos::db::PageType::IndexLeaf));
    if (aliasesCallerStorage || invalidLeafView) {
        bridge->source->release(bridge->source->context, pageId, &sourceView);
        bridge->active = false;
        bridge->invalidView = true;
        return GxIndexPageProviderStatus::Error;
    }

    *outView = sourceView;
    return GxIndexPageProviderStatus::Ok;
}

void releasePage(void* opaque, uint64_t pageId,
                 const GxIndexPageView* view) {
    ProviderBridge* bridge = static_cast<ProviderBridge*>(opaque);
    if (bridge == nullptr || bridge->source == nullptr) return;
    if (!bridge->active || view == nullptr || pageId != bridge->borrowedPageId ||
        !sameView(*view, bridge->borrowedView)) {
        bridge->releaseMismatch = true;
        return;
    }
    bridge->source->release(bridge->source->context, bridge->borrowedPageId,
                            &bridge->borrowedView);
    bridge->active = false;
}

void releaseOutstanding(ProviderBridge& bridge) {
    if (!bridge.active) return;
    bridge.source->release(bridge.source->context, bridge.borrowedPageId,
                           &bridge.borrowedView);
    bridge.active = false;
    bridge.releaseMismatch = true;
}

struct VisitorBridge {
    GxIndexLookupMatchVisitorFn visitor;
    void* context;
};

GxIndexLeafVisitControl visitMatch(
    void* opaque, uint64_t, const GxIndexLeafEntryView* entry) {
    VisitorBridge* bridge = static_cast<VisitorBridge*>(opaque);
    if (bridge == nullptr || bridge->visitor == nullptr || entry == nullptr) {
        return GxIndexLeafVisitControl::Error;
    }
    const GxIndexLookupVisitControl control = bridge->visitor(
        bridge->context, entry->locatorPageId, entry->locatorSlot);
    if (control == GxIndexLookupVisitControl::Continue) {
        return GxIndexLeafVisitControl::Continue;
    }
    if (control == GxIndexLookupVisitControl::Stop) {
        return GxIndexLeafVisitControl::Stop;
    }
    return GxIndexLeafVisitControl::Error;
}

GxIndexLookupStatus mapDescentStatus(
    GxIndexTreeDescentStatus status, bool invalidView) {
    if (invalidView) return GxIndexLookupStatus::CorruptPage;
    switch (status) {
    case GxIndexTreeDescentStatus::Success:
        return GxIndexLookupStatus::Complete;
    case GxIndexTreeDescentStatus::InvalidArgument:
        return GxIndexLookupStatus::InvalidArgument;
    case GxIndexTreeDescentStatus::InvalidPageId:
        return GxIndexLookupStatus::InvalidPageId;
    case GxIndexTreeDescentStatus::MissingPage:
        return GxIndexLookupStatus::MissingPage;
    case GxIndexTreeDescentStatus::ProviderFailure:
        return GxIndexLookupStatus::ProviderFailure;
    case GxIndexTreeDescentStatus::CorruptPage:
        return GxIndexLookupStatus::CorruptPage;
    case GxIndexTreeDescentStatus::Cycle:
        return GxIndexLookupStatus::Cycle;
    case GxIndexTreeDescentStatus::TraversalLimit:
        return GxIndexLookupStatus::TraversalLimit;
    }
    return GxIndexLookupStatus::CorruptPage;
}

GxIndexLookupStatus mapLeafStatus(
    GxIndexLeafTraversalStatus status, bool invalidView) {
    if (invalidView) return GxIndexLookupStatus::CorruptPage;
    switch (status) {
    case GxIndexLeafTraversalStatus::EndOfChain:
    case GxIndexLeafTraversalStatus::KeyBoundary:
        return GxIndexLookupStatus::Complete;
    case GxIndexLeafTraversalStatus::VisitorStopped:
        return GxIndexLookupStatus::VisitorStopped;
    case GxIndexLeafTraversalStatus::InvalidArgument:
        return GxIndexLookupStatus::InvalidArgument;
    case GxIndexLeafTraversalStatus::InvalidPageId:
        return GxIndexLookupStatus::InvalidPageId;
    case GxIndexLeafTraversalStatus::MissingPage:
        return GxIndexLookupStatus::MissingPage;
    case GxIndexLeafTraversalStatus::ProviderFailure:
        return GxIndexLookupStatus::ProviderFailure;
    case GxIndexLeafTraversalStatus::CorruptPage:
        return GxIndexLookupStatus::CorruptPage;
    case GxIndexLeafTraversalStatus::TraversalLimit:
        return GxIndexLookupStatus::TraversalLimit;
    case GxIndexLeafTraversalStatus::VisitorFailure:
        return GxIndexLookupStatus::VisitorFailure;
    }
    return GxIndexLookupStatus::CorruptPage;
}

} // namespace

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
    GxIndexLookupResult* outResult) {
    if (provider == nullptr || provider->acquire == nullptr ||
        provider->release == nullptr || outResult == nullptr ||
        (logicalKeyBytes == nullptr && logicalKeyLength != 0) ||
        logicalKeyLength > static_cast<size_t>(-1) - kPhysicalLocatorBytes ||
        pageCount < 2 || maxDepth > gxos::db::kMaxIndexDepth ||
        visitedPathPageIds == nullptr ||
        visitedPathPageIdCapacity < static_cast<size_t>(maxDepth) + 1u ||
        visitedPathPageIdCapacity >
            static_cast<size_t>(-1) / sizeof(uint64_t) ||
        maxLeafPagesToVisit == 0 || physicalLowerBound == nullptr ||
        physicalLowerBoundCapacity < logicalKeyLength + kPhysicalLocatorBytes ||
        previousPhysicalKey == nullptr || previousPhysicalKeyCapacity == 0 ||
        visitor == nullptr) {
        return GxIndexLookupStatus::InvalidArgument;
    }

    const size_t physicalLowerBoundLength =
        logicalKeyLength + kPhysicalLocatorBytes;
    const size_t visitedPathBytes =
        visitedPathPageIdCapacity * sizeof(uint64_t);
    if (rangesOverlap(outResult, sizeof(*outResult), provider,
                      sizeof(*provider)) ||
        spansOverlap(logicalKeyBytes, logicalKeyLength,
                     physicalLowerBound, physicalLowerBoundLength) ||
        spansOverlap(logicalKeyBytes, logicalKeyLength,
                     previousPhysicalKey, previousPhysicalKeyCapacity) ||
        spansOverlap(logicalKeyBytes, logicalKeyLength,
                     visitedPathPageIds, visitedPathBytes) ||
        spansOverlap(logicalKeyBytes, logicalKeyLength,
                     outResult, sizeof(*outResult)) ||
        spansOverlap(physicalLowerBound, physicalLowerBoundLength,
                     previousPhysicalKey, previousPhysicalKeyCapacity) ||
        spansOverlap(physicalLowerBound, physicalLowerBoundLength,
                     visitedPathPageIds, visitedPathBytes) ||
        spansOverlap(physicalLowerBound, physicalLowerBoundLength,
                     outResult, sizeof(*outResult)) ||
        spansOverlap(previousPhysicalKey, previousPhysicalKeyCapacity,
                     visitedPathPageIds, visitedPathBytes) ||
        spansOverlap(previousPhysicalKey, previousPhysicalKeyCapacity,
                     outResult, sizeof(*outResult)) ||
        spansOverlap(visitedPathPageIds, visitedPathBytes,
                     outResult, sizeof(*outResult)) ||
        rangesOverlap(provider, sizeof(*provider), physicalLowerBound,
                      physicalLowerBoundLength) ||
        rangesOverlap(provider, sizeof(*provider), previousPhysicalKey,
                      previousPhysicalKeyCapacity) ||
        rangesOverlap(provider, sizeof(*provider), visitedPathPageIds,
                      visitedPathBytes)) {
        return GxIndexLookupStatus::InvalidArgument;
    }

    GxIndexLookupResult result = {};
    result.providerStatus = GxIndexPageProviderStatus::Ok;
    result.descentStatus = GxIndexTreeDescentStatus::InvalidArgument;
    result.descentError = GxIndexTreeDescentError::None;
    result.internalNodeError = GxIndexInternalNodeError::None;
    result.leafTraversalStatus = GxIndexLeafTraversalStatus::InvalidArgument;
    result.leafError = GxIndexLeafError::None;
    result.error = GxIndexLookupError::None;
    if (rootPageId == 0 || rootPageId >= pageCount) {
        result.descentStatus = GxIndexTreeDescentStatus::InvalidPageId;
        result.error = GxIndexLookupError::Descent;
        *outResult = result;
        return GxIndexLookupStatus::InvalidPageId;
    }

    copyBytes(physicalLowerBound, logicalKeyBytes, logicalKeyLength);
    for (size_t i = logicalKeyLength; i < physicalLowerBoundLength; ++i) {
        physicalLowerBound[i] = 0;
    }

    ProviderBridge providerBridge = {};
    providerBridge.source = provider;
    providerBridge.borrowedView = GxIndexPageView();
    providerBridge.borrowedPageId = 0;
    providerBridge.logicalKeyBytes = logicalKeyBytes;
    providerBridge.logicalKeyLength = logicalKeyLength;
    providerBridge.physicalLowerBound = physicalLowerBound;
    providerBridge.physicalLowerBoundLength = physicalLowerBoundLength;
    providerBridge.previousPhysicalKey = previousPhysicalKey;
    providerBridge.previousPhysicalKeyCapacity = previousPhysicalKeyCapacity;
    providerBridge.visitedPathPageIds = visitedPathPageIds;
    providerBridge.visitedPathPageIdCapacity = visitedPathPageIdCapacity;
    providerBridge.outResult = outResult;
    providerBridge.active = false;
    providerBridge.invalidView = false;
    providerBridge.releaseMismatch = false;
    providerBridge.phase = ProviderPhase::Descent;
    const GxIndexPageProvider bridgedProvider = {
        &providerBridge, acquirePage, releasePage
    };

    GxIndexTreeDescentResult descent = {};
    result.descentStatus = gxIndexTreeDescendToLeaf(
        &bridgedProvider, rootPageId, pageCount, expectedOwnerIndexId,
        physicalLowerBound, physicalLowerBoundLength, maxDepth,
        visitedPathPageIds, visitedPathPageIdCapacity, &descent);
    result.descentPagesVisited = descent.pagesVisited;
    result.leafPageId = descent.leafPageId;
    result.depth = descent.depth;
    result.descentError = descent.error;
    result.internalNodeError = descent.internalNodeError;
    result.providerStatus = descent.providerStatus;
    result.leafError = descent.leafError;

    if (providerBridge.active) releaseOutstanding(providerBridge);
    if (providerBridge.releaseMismatch) {
        result.error = GxIndexLookupError::ProviderView;
        *outResult = result;
        return GxIndexLookupStatus::CorruptPage;
    }
    if (result.descentStatus != GxIndexTreeDescentStatus::Success) {
        result.error = providerBridge.invalidView
            ? GxIndexLookupError::ProviderView : GxIndexLookupError::Descent;
        const GxIndexLookupStatus status = mapDescentStatus(
            result.descentStatus, providerBridge.invalidView);
        *outResult = result;
        return status;
    }

    providerBridge.phase = ProviderPhase::LeafChain;
    VisitorBridge visitorBridge = {visitor, visitorContext};
    GxIndexLeafTraversalResult traversal = {};
    result.leafTraversalStatus = gxIndexLeafLookupChain(
        &bridgedProvider, result.leafPageId, pageCount,
        expectedOwnerIndexId, maxLeafPagesToVisit, physicalLowerBound,
        physicalLowerBoundLength, previousPhysicalKey,
        previousPhysicalKeyCapacity, visitMatch, &visitorBridge, &traversal);
    if (providerBridge.active) releaseOutstanding(providerBridge);
    result.leafPagesVisited = traversal.pagesVisited;
    result.matchesVisited = traversal.matchesVisited;
    result.providerStatus = traversal.providerStatus;
    result.leafError = traversal.leafError;

    if (providerBridge.releaseMismatch) {
        result.error = GxIndexLookupError::ProviderView;
        *outResult = result;
        return GxIndexLookupStatus::CorruptPage;
    }
    result.error = providerBridge.invalidView
        ? GxIndexLookupError::ProviderView : GxIndexLookupError::LeafTraversal;
    const GxIndexLookupStatus status = mapLeafStatus(
        result.leafTraversalStatus, providerBridge.invalidView);
    if (status == GxIndexLookupStatus::Complete ||
        status == GxIndexLookupStatus::VisitorStopped) {
        result.error = GxIndexLookupError::None;
    }
    *outResult = result;
    return status;
}
