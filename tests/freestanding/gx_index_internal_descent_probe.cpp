#include "database_format.h"
#include "gx_index_internal_node.h"
#include "gx_index_leaf_traversal.h"
#include "gx_index_tree_descent.h"

namespace {

void storeProbeLe32(uint8_t* bytes, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

void storeProbeLe64(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

uint32_t loadProbeLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

struct ProbePageProvider {
    const uint8_t* const* payloads;
    const uint64_t* pageIds;
    const uint16_t* pageTypes;
    const size_t* lengths;
    size_t pageTotal;
    uint64_t acquireCount;
    uint64_t releaseCount;
    uint64_t failOnAcquire;
    uint64_t overridePageId;
    const uint8_t* overrideBytes;
    size_t overrideLength;
    uint16_t overridePageType;
    bool hasOverride;
    bool active;
    uint8_t buffer[480];
};

GxIndexPageProviderStatus acquirePage(
    void* opaque, uint64_t pageId, GxIndexPageView* outView) {
    ProbePageProvider* provider = static_cast<ProbePageProvider*>(opaque);
    if (provider == nullptr || outView == nullptr || provider->active) {
        return GxIndexPageProviderStatus::Error;
    }
    ++provider->acquireCount;
    *outView = GxIndexPageView();
    if (provider->failOnAcquire != 0 &&
        provider->acquireCount == provider->failOnAcquire) {
        return GxIndexPageProviderStatus::Error;
    }
    const uint8_t* bytes = nullptr;
    size_t length = 0;
    uint16_t pageType = 0;
    if (provider->hasOverride && pageId == provider->overridePageId) {
        bytes = provider->overrideBytes;
        length = provider->overrideLength;
        pageType = provider->overridePageType;
    } else {
        for (size_t i = 0; i < provider->pageTotal; ++i) {
            if (provider->pageIds[i] == pageId) {
                bytes = provider->payloads[i];
                length = provider->lengths[i];
                pageType = provider->pageTypes[i];
                break;
            }
        }
    }
    if (bytes == nullptr) return GxIndexPageProviderStatus::MissingPage;
    if (length > sizeof(provider->buffer)) {
        return GxIndexPageProviderStatus::Error;
    }
    for (size_t i = 0; i < length; ++i) provider->buffer[i] = bytes[i];
    provider->active = true;
    outView->bytes = provider->buffer;
    outView->length = length;
    outView->pageId = pageId;
    outView->pageType = pageType;
    outView->token = provider;
    return GxIndexPageProviderStatus::Ok;
}

void releasePage(void* opaque, uint64_t pageId,
                 const GxIndexPageView* view) {
    ProbePageProvider* provider = static_cast<ProbePageProvider*>(opaque);
    if (provider == nullptr) return;
    if (!provider->active || view == nullptr || view->token != provider ||
        view->pageId != pageId || view->bytes != provider->buffer) {
        provider->active = false;
        return;
    }
    provider->active = false;
    ++provider->releaseCount;
    for (size_t i = 0; i < sizeof(provider->buffer); ++i) {
        provider->buffer[i] = 0xDDu;
    }
}

struct MatchState {
    uint64_t count;
    uint64_t firstPage;
    bool crossedPage;
    bool invalid;
    uint32_t expectedCount;
};

GxIndexLeafVisitControl visitDuplicate(
    void* opaque, uint64_t pageId, const GxIndexLeafEntryView* entry) {
    MatchState* state = static_cast<MatchState*>(opaque);
    const uint8_t expectedKey[] = {0x01u, 'm', '-', 'd', 'u', 'p'};
    if (state == nullptr || entry == nullptr ||
        entry->keyLength != sizeof(expectedKey) ||
        entry->locatorPageId != 1 ||
        entry->locatorSlot != 1000u + state->count) {
        if (state != nullptr) state->invalid = true;
        return GxIndexLeafVisitControl::Error;
    }
    for (size_t i = 0; i < sizeof(expectedKey); ++i) {
        if (entry->keyBytes[i] != expectedKey[i]) {
            state->invalid = true;
            return GxIndexLeafVisitControl::Error;
        }
    }
    if (state->count == 0) state->firstPage = pageId;
    else if (pageId != state->firstPage) state->crossedPage = true;
    ++state->count;
    return GxIndexLeafVisitControl::Continue;
}

} // namespace

extern "C" int gxIndexInternalDescentProbe(
    const uint8_t* const* payloads, const uint64_t* pageIds,
    const uint16_t* pageTypes, const size_t* lengths, size_t pageTotal,
    uint64_t pageCount, uint32_t ownerIndexId, uint64_t rootPageId,
    uint32_t expectedDuplicateCount) {
    if (payloads == nullptr || pageIds == nullptr || pageTypes == nullptr ||
        lengths == nullptr || pageTotal < 3 || pageCount <= pageTotal ||
        ownerIndexId == 0 || rootPageId == 0 || expectedDuplicateCount < 2) {
        return 1;
    }
    ProbePageProvider pageState = {};
    pageState.payloads = payloads;
    pageState.pageIds = pageIds;
    pageState.pageTypes = pageTypes;
    pageState.lengths = lengths;
    pageState.pageTotal = pageTotal;
    GxIndexPageProvider provider = {&pageState, acquirePage, releasePage};

    const uint8_t logicalKey[] = {0x01u, 'm', '-', 'd', 'u', 'p'};
    uint8_t physicalSearchKey[sizeof(logicalKey) + 12] = {};
    for (size_t i = 0; i < sizeof(logicalKey); ++i) {
        physicalSearchKey[i] = logicalKey[i];
    }
    uint64_t visited[gxos::db::kMaxIndexDepth + 1] = {};
    GxIndexTreeDescentResult descent = {};
    const GxIndexTreeDescentStatus descentStatus =
        gxIndexTreeDescendToLeaf(
            &provider, rootPageId, pageCount, ownerIndexId,
            physicalSearchKey, sizeof(physicalSearchKey),
            gxos::db::kMaxIndexDepth, visited,
            sizeof(visited) / sizeof(visited[0]), &descent);
    if (descentStatus != GxIndexTreeDescentStatus::Success ||
        descent.leafPageId == 0 || descent.depth < 2 ||
        descent.pagesVisited < 3 || pageState.active ||
        pageState.releaseCount != descent.pagesVisited) {
        return 2;
    }

    // Verify the same production reader rejects a child reference that points
    // beyond the page-store identifier domain.
    size_t rootIndex = pageTotal;
    for (size_t i = 0; i < pageTotal; ++i) {
        if (pageIds[i] == rootPageId) rootIndex = i;
    }
    if (rootIndex == pageTotal || lengths[rootIndex] != sizeof(pageState.buffer) ||
        pageTypes[rootIndex] != static_cast<uint16_t>(
            gxos::db::PageType::IndexInternal)) {
        return 3;
    }
    uint8_t malformedRoot[480] = {};
    for (size_t i = 0; i < sizeof(malformedRoot); ++i) {
        malformedRoot[i] = payloads[rootIndex][i];
    }
    const uint32_t childCount = loadProbeLe32(malformedRoot + 16);
    if (childCount < 2 || childCount > 22) return 4;
    const size_t firstSlot = sizeof(malformedRoot) - 8u;
    const uint32_t firstOffset = loadProbeLe32(malformedRoot + firstSlot);
    if (firstOffset > sizeof(malformedRoot) - 12u) return 5;
    storeProbeLe64(malformedRoot + firstOffset, pageCount);
    GxIndexInternalNodeCursor internalCursor = {};
    GxIndexInternalNodeError internalError = GxIndexInternalNodeError::None;
    if (gxIndexInternalNodeCursorInit(
            malformedRoot, sizeof(malformedRoot), pageCount, ownerIndexId,
            &internalCursor, &internalError) != GxStatus::CorruptData ||
        internalError != GxIndexInternalNodeError::ChildPageIdInvalid) {
        return 6;
    }

    pageState.acquireCount = 0;
    pageState.releaseCount = 0;
    pageState.failOnAcquire = 2;
    GxIndexTreeDescentResult failedProvider = {};
    if (gxIndexTreeDescendToLeaf(
            &provider, rootPageId, pageCount, ownerIndexId,
            physicalSearchKey, sizeof(physicalSearchKey),
            gxos::db::kMaxIndexDepth, visited,
            sizeof(visited) / sizeof(visited[0]), &failedProvider) !=
            GxIndexTreeDescentStatus::ProviderFailure ||
        pageState.releaseCount != 1 || pageState.active) {
        return 7;
    }
    pageState.failOnAcquire = 0;
    pageState.acquireCount = 0;
    pageState.releaseCount = 0;

    MatchState matches = {};
    matches.expectedCount = expectedDuplicateCount;
    uint8_t previousPhysicalKey[480] = {};
    GxIndexLeafTraversalResult traversal = {};
    const GxIndexLeafTraversalStatus chainStatus = gxIndexLeafLookupChain(
        &provider, descent.leafPageId, pageCount, ownerIndexId,
        pageCount - 1, physicalSearchKey, sizeof(physicalSearchKey),
        previousPhysicalKey, sizeof(previousPhysicalKey), visitDuplicate,
        &matches, &traversal);
    if ((chainStatus != GxIndexLeafTraversalStatus::KeyBoundary &&
         chainStatus != GxIndexLeafTraversalStatus::EndOfChain) ||
        matches.invalid || matches.count != expectedDuplicateCount ||
        !matches.crossedPage || traversal.matchesVisited != expectedDuplicateCount ||
        traversal.pagesVisited < 2 || pageState.active ||
        pageState.releaseCount != traversal.pagesVisited) {
        return 8;
    }
    return 0;
}
