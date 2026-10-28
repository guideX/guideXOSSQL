#include "database_format.h"
#include "gx_index_lookup.h"

namespace {

struct ProbePageProvider {
    const uint8_t* const* payloads;
    const uint64_t* pageIds;
    const uint16_t* pageTypes;
    const size_t* lengths;
    size_t pageTotal;
    uint64_t rootPageId;
    uint64_t acquireCount;
    uint64_t successfulAcquireCount;
    uint64_t releaseCount;
    uint64_t failOnAcquire;
    const uint64_t* visitorCount;
    bool failAfterVisitor;
    bool overrideRootType;
    bool active;
    uint64_t requestedPageId;
    uint8_t buffer[480];
};

struct MatchState {
    uint64_t count;
    uint32_t expectedCount;
    uint32_t stopAfter;
    bool invalid;
};

GxIndexPageProviderStatus acquirePage(
    void* opaque, uint64_t pageId, GxIndexPageView* outView) {
    ProbePageProvider* provider = static_cast<ProbePageProvider*>(opaque);
    if (provider == nullptr || outView == nullptr || provider->active) {
        return GxIndexPageProviderStatus::Error;
    }
    ++provider->acquireCount;
    *outView = GxIndexPageView();
    if ((provider->failOnAcquire != 0 &&
         provider->acquireCount == provider->failOnAcquire) ||
        (provider->failAfterVisitor && provider->visitorCount != nullptr &&
         *provider->visitorCount != 0)) {
        return GxIndexPageProviderStatus::Error;
    }

    size_t found = provider->pageTotal;
    for (size_t i = 0; i < provider->pageTotal; ++i) {
        if (provider->pageIds[i] == pageId) {
            found = i;
            break;
        }
    }
    if (found == provider->pageTotal) {
        return GxIndexPageProviderStatus::MissingPage;
    }
    if (provider->lengths[found] > sizeof(provider->buffer)) {
        return GxIndexPageProviderStatus::Error;
    }
    for (size_t i = 0; i < provider->lengths[found]; ++i) {
        provider->buffer[i] = provider->payloads[found][i];
    }
    provider->active = true;
    provider->requestedPageId = pageId;
    ++provider->successfulAcquireCount;
    outView->bytes = provider->buffer;
    outView->length = provider->lengths[found];
    outView->pageId = pageId;
    outView->pageType = provider->overrideRootType &&
        pageId == provider->rootPageId
        ? 99u : provider->pageTypes[found];
    outView->token = provider;
    return GxIndexPageProviderStatus::Ok;
}

void releasePage(void* opaque, uint64_t pageId,
                 const GxIndexPageView* view) {
    ProbePageProvider* provider = static_cast<ProbePageProvider*>(opaque);
    if (provider == nullptr) return;
    if (!provider->active || view == nullptr || view->token != provider ||
        pageId != provider->requestedPageId || view->bytes != provider->buffer ||
        view->pageId != pageId) {
        provider->active = false;
        return;
    }
    provider->active = false;
    ++provider->releaseCount;
    for (size_t i = 0; i < sizeof(provider->buffer); ++i) {
        provider->buffer[i] = 0xDDu;
    }
}

GxIndexLookupVisitControl visitMatch(
    void* opaque, uint64_t locatorPageId, uint32_t locatorSlot) {
    MatchState* state = static_cast<MatchState*>(opaque);
    if (state == nullptr || locatorPageId != 1 ||
        locatorSlot != 1000u + state->count) {
        if (state != nullptr) state->invalid = true;
        return GxIndexLookupVisitControl::Error;
    }
    ++state->count;
    if (state->stopAfter != 0 && state->count == state->stopAfter) {
        return GxIndexLookupVisitControl::Stop;
    }
    return GxIndexLookupVisitControl::Continue;
}

void resetProvider(ProbePageProvider& provider) {
    provider.acquireCount = 0;
    provider.successfulAcquireCount = 0;
    provider.releaseCount = 0;
    provider.failOnAcquire = 0;
    provider.visitorCount = nullptr;
    provider.failAfterVisitor = false;
    provider.overrideRootType = false;
    provider.active = false;
    provider.requestedPageId = 0;
}

GxIndexLookupStatus lookup(
    ProbePageProvider& state, uint64_t rootPageId, uint64_t pageCount,
    uint32_t ownerIndexId, MatchState& matches,
    GxIndexLookupResult& result) {
    GxIndexPageProvider provider = {&state, acquirePage, releasePage};
    state.rootPageId = rootPageId;
    const uint8_t logicalKey[] = {0x01u, 'm', '-', 'd', 'u', 'p'};
    uint8_t lowerBound[sizeof(logicalKey) + 12] = {};
    uint8_t previousPhysicalKey[480] = {};
    uint64_t visitedPath[gxos::db::kMaxIndexDepth + 1] = {};
    return gxIndexLookupAll(
        &provider, rootPageId, pageCount, ownerIndexId, logicalKey,
        sizeof(logicalKey), gxos::db::kMaxIndexDepth, visitedPath,
        sizeof(visitedPath) / sizeof(visitedPath[0]), pageCount - 1,
        lowerBound, sizeof(lowerBound), previousPhysicalKey,
        sizeof(previousPhysicalKey), visitMatch, &matches, &result);
}

} // namespace

extern "C" int gxIndexCompleteLookupProbe(
    const uint8_t* const* payloads, const uint64_t* pageIds,
    const uint16_t* pageTypes, const size_t* lengths, size_t pageTotal,
    uint64_t pageCount, uint32_t ownerIndexId, uint64_t rootPageId,
    uint32_t expectedDuplicateCount) {
    if (payloads == nullptr || pageIds == nullptr || pageTypes == nullptr ||
        lengths == nullptr || pageTotal < 3 || pageCount <= pageTotal ||
        ownerIndexId == 0 || rootPageId == 0 || expectedDuplicateCount < 2) {
        return 1;
    }

    ProbePageProvider provider = {};
    provider.payloads = payloads;
    provider.pageIds = pageIds;
    provider.pageTypes = pageTypes;
    provider.lengths = lengths;
    provider.pageTotal = pageTotal;
    provider.rootPageId = rootPageId;

    MatchState matches = {};
    matches.expectedCount = expectedDuplicateCount;
    GxIndexLookupResult result = {};
    if (lookup(provider, rootPageId, pageCount, ownerIndexId,
               matches, result) != GxIndexLookupStatus::Complete ||
        matches.invalid || matches.count != expectedDuplicateCount ||
        result.matchesVisited != expectedDuplicateCount ||
        result.depth < 2 || result.descentPagesVisited < 3 ||
        result.leafPagesVisited < 2 || provider.active ||
        provider.releaseCount != provider.successfulAcquireCount) {
        return 2;
    }

    resetProvider(provider);
    MatchState stopped = {};
    stopped.expectedCount = expectedDuplicateCount;
    stopped.stopAfter = 3;
    GxIndexLookupResult stoppedResult = {};
    if (lookup(provider, rootPageId, pageCount, ownerIndexId,
               stopped, stoppedResult) != GxIndexLookupStatus::VisitorStopped ||
        stopped.invalid || stopped.count != 3 ||
        stoppedResult.matchesVisited != 3 || provider.active ||
        provider.releaseCount != provider.successfulAcquireCount) {
        return 3;
    }

    resetProvider(provider);
    MatchState partial = {};
    partial.expectedCount = expectedDuplicateCount;
    provider.visitorCount = &partial.count;
    provider.failAfterVisitor = true;
    GxIndexLookupResult partialResult = {};
    if (lookup(provider, rootPageId, pageCount, ownerIndexId,
               partial, partialResult) != GxIndexLookupStatus::ProviderFailure ||
        partial.invalid || partial.count == 0 ||
        partialResult.matchesVisited == 0 || provider.active ||
        provider.releaseCount != provider.successfulAcquireCount) {
        return 4;
    }

    resetProvider(provider);
    MatchState invalidRootMatches = {};
    GxIndexLookupResult invalidRoot = {};
    if (lookup(provider, pageCount, pageCount, ownerIndexId,
               invalidRootMatches, invalidRoot) !=
            GxIndexLookupStatus::InvalidPageId ||
        provider.acquireCount != 0) {
        return 5;
    }

    resetProvider(provider);
    provider.overrideRootType = true;
    MatchState corruptMatches = {};
    GxIndexLookupResult corrupt = {};
    if (lookup(provider, rootPageId, pageCount, ownerIndexId,
               corruptMatches, corrupt) != GxIndexLookupStatus::CorruptPage ||
        provider.active || provider.releaseCount != 1) {
        return 6;
    }

    return 0;
}
