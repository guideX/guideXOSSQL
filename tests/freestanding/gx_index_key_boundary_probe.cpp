#include "gx_index_key.h"
#include "gx_index_leaf_entry.h"
#include "gx_index_leaf_traversal.h"
#include "gx_index_physical_key.h"

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

struct ProbePageProvider {
    const uint8_t* const* payloads;
    const uint64_t* pageIds;
    const size_t* lengths;
    size_t pageTotal;
    uint64_t acquireCount;
    uint64_t releaseCount;
    uint64_t failOnAcquire;
    uint64_t overridePageId;
    const uint8_t* overrideBytes;
    size_t overrideLength;
    bool hasOverride;
    bool active;
    uint8_t buffer[480];
};

GxIndexPageProviderStatus probeAcquirePage(
    void* opaque, uint64_t pageId, GxIndexPageView* outView) {
    ProbePageProvider* provider = static_cast<ProbePageProvider*>(opaque);
    if (provider == nullptr || outView == nullptr || provider->active) {
        return GxIndexPageProviderStatus::Error;
    }
    ++provider->acquireCount;
    outView->bytes = nullptr;
    outView->length = 0;
    outView->token = nullptr;
    if (provider->failOnAcquire != 0 &&
        provider->acquireCount == provider->failOnAcquire) {
        return GxIndexPageProviderStatus::Error;
    }

    const uint8_t* bytes = nullptr;
    size_t length = 0;
    if (provider->hasOverride && pageId == provider->overridePageId) {
        bytes = provider->overrideBytes;
        length = provider->overrideLength;
    } else {
        for (size_t i = 0; i < provider->pageTotal; ++i) {
            if (provider->pageIds[i] == pageId) {
                bytes = provider->payloads[i];
                length = provider->lengths[i];
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
    outView->token = provider;
    return GxIndexPageProviderStatus::Ok;
}

void probeReleasePage(void* opaque, uint64_t,
                      const GxIndexPageView* view) {
    ProbePageProvider* provider = static_cast<ProbePageProvider*>(opaque);
    if (provider == nullptr) return;
    if (!provider->active || view == nullptr || view->token != provider ||
        view->bytes != provider->buffer) {
        provider->active = false;
        return;
    }
    provider->active = false;
    ++provider->releaseCount;
    for (size_t i = 0; i < sizeof(provider->buffer); ++i) {
        provider->buffer[i] = 0xDDu;
    }
}

struct ProbeMatchState {
    uint64_t count;
    uint64_t firstPageId;
    bool sawAnotherPage;
    bool invalid;
};

GxIndexLeafVisitControl probeVisitMatch(
    void* opaque, uint64_t pageId, const GxIndexLeafEntryView* entry) {
    ProbeMatchState* state = static_cast<ProbeMatchState*>(opaque);
    const uint8_t expectedKey[] = {0x01u, 'm', 0, 'n'};
    if (state == nullptr || entry == nullptr || entry->keyLength != 4 ||
        entry->keyBytes[0] != expectedKey[0] ||
        entry->keyBytes[1] != expectedKey[1] ||
        entry->keyBytes[2] != expectedKey[2] ||
        entry->keyBytes[3] != expectedKey[3] ||
        entry->locatorPageId != 1 ||
        entry->locatorSlot != 100u + state->count) {
        if (state != nullptr) state->invalid = true;
        return GxIndexLeafVisitControl::Error;
    }
    if (state->count == 0) state->firstPageId = pageId;
    else if (pageId != state->firstPageId) state->sawAnotherPage = true;
    ++state->count;
    return GxIndexLeafVisitControl::Continue;
}

} // namespace

// Freestanding compile/link probe for the production portable index-key API.
// Keeping the encoded bytes on this stack frame makes the lifetime underlying
// the borrowed decode view explicit for the entire use of that view.
extern "C" int gxIndexKeyBoundaryProbe(
    const uint8_t* const* leafPayloads,
    const uint64_t* leafPageIds,
    const size_t* leafLengths,
    size_t leafCount,
    uint64_t firstPageId,
    uint64_t pageCount,
    uint32_t ownerIndexId) {
    const uint8_t textBytes[] = {'a', 0x00u, 'b'};
    uint8_t encodedBytes[sizeof(textBytes) + 1] = {};
    size_t encodedLength = 0;
    GxTextByteView text = {nullptr, 0};
    bool isNull = true;
    int comparison = 0;

    if (gxEncodeIndexTextKey(textBytes, sizeof(textBytes), encodedBytes,
                             sizeof(encodedBytes), &encodedLength) !=
        GxStatus::Ok) {
        return 1;
    }
    if (gxDecodeIndexTextKey(encodedBytes, encodedLength, &text, &isNull) !=
            GxStatus::Ok ||
        isNull || text.data != encodedBytes + 1 ||
        text.length != sizeof(textBytes)) {
        return 2;
    }
    if (gxIndexKeyIsNull(encodedBytes, encodedLength, &isNull) !=
            GxStatus::Ok ||
        isNull) {
        return 3;
    }
    if (gxCompareIndexKeys(text.data, text.length, textBytes,
                           sizeof(textBytes), &comparison) != GxStatus::Ok ||
        comparison != 0) {
        return 4;
    }

    encodedBytes[1] = 'z';
    if (text.data[0] != 'z' || text.data[1] != 0x00u ||
        text.data[2] != 'b') {
        return 5;
    }

    const uint8_t int32Minimum[] = {0x01u, 0x00u, 0x00u, 0x00u, 0x00u};
    int32_t decodedInt32 = 0;
    if (gxDecodeIndexInt32Key(int32Minimum, sizeof(int32Minimum),
                              &decodedInt32, &isNull) != GxStatus::Ok ||
        isNull || decodedInt32 != INT32_MIN) {
        return 6;
    }

    uint8_t encodedInt64[9] = {};
    size_t encodedInt64Length = 0;
    int64_t decodedInt64 = 0;
    if (gxEncodeIndexInt64Key(false, -1, encodedInt64,
                              sizeof(encodedInt64), &encodedInt64Length) !=
            GxStatus::Ok ||
        encodedInt64Length != sizeof(encodedInt64) ||
        gxDecodeIndexInt64Key(encodedInt64, encodedInt64Length,
                              &decodedInt64, &isNull) != GxStatus::Ok ||
        isNull || decodedInt64 != -1) {
        return 7;
    }

    uint8_t encodedInt32[5] = {};
    size_t encodedInt32Length = 0;
    if (gxEncodeIndexInt32Key(false, INT32_MAX, encodedInt32,
                              sizeof(encodedInt32), &encodedInt32Length) !=
            GxStatus::Ok ||
        encodedInt32Length != sizeof(encodedInt32) ||
        encodedInt32[0] != 0x01u || encodedInt32[1] != 0xFFu ||
        encodedInt32[2] != 0xFFu || encodedInt32[3] != 0xFFu ||
        encodedInt32[4] != 0xFFu) {
        return 8;
    }

    uint8_t encodedBoolean[2] = {};
    size_t encodedBooleanLength = 0;
    bool decodedBoolean = true;
    if (gxEncodeIndexBooleanKey(false, false, encodedBoolean,
                                sizeof(encodedBoolean),
                                &encodedBooleanLength) != GxStatus::Ok ||
        encodedBooleanLength != 2 || encodedBoolean[0] != 0x01u ||
        encodedBoolean[1] != 0x00u ||
        gxDecodeIndexBooleanKey(encodedBoolean, encodedBooleanLength,
                                &decodedBoolean, &isNull) != GxStatus::Ok ||
        isNull || decodedBoolean) {
        return 9;
    }
    if (gxEncodeIndexBooleanKey(false, true, encodedBoolean,
                                sizeof(encodedBoolean),
                                &encodedBooleanLength) != GxStatus::Ok ||
        encodedBooleanLength != 2 || encodedBoolean[0] != 0x01u ||
        encodedBoolean[1] != 0x01u ||
        gxDecodeIndexBooleanKey(encodedBoolean, encodedBooleanLength,
                                &decodedBoolean, &isNull) != GxStatus::Ok ||
        isNull || !decodedBoolean) {
        return 10;
    }
    if (gxEncodeIndexBooleanKey(true, false, encodedBoolean,
                                sizeof(encodedBoolean),
                                &encodedBooleanLength) != GxStatus::Ok ||
        encodedBooleanLength != 1 || encodedBoolean[0] != 0x00u ||
        gxDecodeIndexBooleanKey(encodedBoolean, encodedBooleanLength,
                                &decodedBoolean, &isNull) != GxStatus::Ok ||
        !isNull || decodedBoolean) {
        return 11;
    }

    const uint8_t nullKey[] = {0x00u};
    const uint8_t falseKey[] = {0x01u, 0x00u};
    if (gxCompareIndexPhysicalKeyViews(
            nullKey, sizeof(nullKey), true, 4, 2,
            falseKey, sizeof(falseKey), true, 4, 2,
            &comparison) != GxStatus::Ok || comparison != -1) {
        return 12;
    }

    // The stored full bound and a borrowed key-plus-locator view must compare
    // equal without a temporary combined-key buffer.
    const uint8_t storedPhysical[] = {
        0x01u, 'x', 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 4
    };
    if (gxCompareIndexPhysicalKeyViews(
            storedPhysical, sizeof(storedPhysical), false, 0, 0,
            storedPhysical, 2, true, 3, 4,
            &comparison) != GxStatus::Ok || comparison != 0) {
        return 13;
    }

    // Execute the production leaf cursor and validator against a caller-owned
    // payload with the existing little-endian header, entry and reverse slot.
    uint8_t leafBytes[480] = {};
    storeProbeLe32(leafBytes, 0x4C495847u);
    leafBytes[4] = 1;
    storeProbeLe32(leafBytes + 16, 1);
    storeProbeLe32(leafBytes + 20, 5);
    storeProbeLe32(leafBytes + 24, 2);
    leafBytes[28] = 0x01u;
    leafBytes[29] = 0x20u;
    storeProbeLe64(leafBytes + 30, 7);
    storeProbeLe32(leafBytes + 38, 4);
    storeProbeLe32(leafBytes + 472, 24);
    storeProbeLe32(leafBytes + 476, 18);

    GxIndexLeafCursor leafCursor = {};
    GxIndexLeafEntryView leafEntry = {};
    GxIndexLeafError leafError = GxIndexLeafError::None;
    bool hasLeafEntry = false;
    if (gxIndexLeafCursorInit(leafBytes, sizeof(leafBytes), &leafCursor,
                              &leafError) != GxStatus::Ok ||
        leafCursor.page.entryCount != 1 ||
        leafCursor.page.ownerIndexId != 5) {
        return 14;
    }
    if (gxIndexLeafCursorReadNext(&leafCursor, &leafEntry, &hasLeafEntry,
                                  &leafError) != GxStatus::Ok ||
        !hasLeafEntry || leafEntry.keyLength != 2 ||
        leafEntry.keyBytes[0] != 0x01u || leafEntry.keyBytes[1] != 0x20u ||
        leafEntry.locatorPageId != 7 || leafEntry.locatorSlot != 4) {
        return 15;
    }
    if (gxIndexLeafCursorReadNext(&leafCursor, &leafEntry, &hasLeafEntry,
                                  &leafError) != GxStatus::Ok || hasLeafEntry ||
        gxIndexLeafValidate(leafBytes, sizeof(leafBytes), &leafError) !=
            GxStatus::Ok) {
        return 16;
    }
    storeProbeLe32(leafBytes + 476, 0xFFFFFFFFu);
    if (gxIndexLeafCursorInit(leafBytes, sizeof(leafBytes), &leafCursor,
                              &leafError) != GxStatus::Ok ||
        gxIndexLeafCursorReadNext(&leafCursor, &leafEntry, &hasLeafEntry,
                                  &leafError) != GxStatus::CorruptData ||
        hasLeafEntry || leafError != GxIndexLeafError::EntryOutOfRange) {
        return 17;
    }

    // The production leaf lookup returns a physical-key match and a
    // continuation positioned after it, without allocating a combined key.
    storeProbeLe32(leafBytes + 472, 24);
    storeProbeLe32(leafBytes + 476, 18);
    const uint8_t exactPhysicalKey[] = {
        0x01u, 0x20u, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 4
    };
    GxIndexLeafLookupResult lookup = {};
    if (gxIndexLeafLookup(leafBytes, sizeof(leafBytes), exactPhysicalKey,
                          sizeof(exactPhysicalKey), &lookup, &leafError) !=
            GxStatus::Ok ||
        !lookup.hasEntry || !lookup.exactMatch || lookup.entryPosition != 0 ||
        lookup.entry.locatorPageId != 7 || lookup.entry.locatorSlot != 4) {
        return 18;
    }
    if (gxIndexLeafCursorReadNext(&lookup.continuation, &leafEntry,
                                  &hasLeafEntry, &leafError) != GxStatus::Ok ||
        hasLeafEntry) {
        return 19;
    }

    const uint8_t missingPhysicalKey[] = {
        0x01u, 0x30u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    if (gxIndexLeafLookup(leafBytes, sizeof(leafBytes), missingPhysicalKey,
                          sizeof(missingPhysicalKey), &lookup, &leafError) !=
            GxStatus::Ok ||
        lookup.hasEntry || lookup.exactMatch || lookup.entryPosition != 1 ||
        lookup.continuation.position != 1) {
        return 20;
    }

    storeProbeLe32(leafBytes + 476, 0xFFFFFFFFu);
    lookup.entryPosition = 99;
    lookup.hasEntry = true;
    if (gxIndexLeafLookup(leafBytes, sizeof(leafBytes), exactPhysicalKey,
                          sizeof(exactPhysicalKey), &lookup, &leafError) !=
            GxStatus::CorruptData ||
        leafError != GxIndexLeafError::EntryOutOfRange || lookup.hasEntry ||
        lookup.entryPosition != 0 || lookup.continuation.pageBytes != 0) {
        return 21;
    }

    if (leafPayloads == nullptr || leafPageIds == nullptr ||
        leafLengths == nullptr || leafCount < 3 || pageCount <= leafCount ||
        firstPageId == 0 || ownerIndexId == 0) {
        return 22;
    }
    ProbePageProvider pageProvider = {};
    pageProvider.payloads = leafPayloads;
    pageProvider.pageIds = leafPageIds;
    pageProvider.lengths = leafLengths;
    pageProvider.pageTotal = leafCount;
    GxIndexPageProvider provider = {
        &pageProvider, probeAcquirePage, probeReleasePage
    };
    uint8_t probePhysical[16] = {
        0x01u, 'm', 0, 'n', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    uint8_t previousPhysicalKey[480] = {};
    ProbeMatchState matches = {};
    GxIndexLeafTraversalResult traversal = {};
    const GxIndexLeafTraversalStatus traversalStatus =
        gxIndexLeafLookupChain(
            &provider, firstPageId, pageCount, ownerIndexId,
            pageCount - 1, probePhysical, sizeof(probePhysical),
            previousPhysicalKey, sizeof(previousPhysicalKey),
            probeVisitMatch, &matches, &traversal);
    if ((traversalStatus != GxIndexLeafTraversalStatus::EndOfChain &&
         traversalStatus != GxIndexLeafTraversalStatus::KeyBoundary) ||
        matches.invalid || matches.count != 64 || !matches.sawAnotherPage ||
        traversal.matchesVisited != 64 || traversal.pagesVisited < 3 ||
        pageProvider.releaseCount != traversal.pagesVisited ||
        pageProvider.active) {
        return 23;
    }

    // A key beyond the serialized chain must visit each supplied leaf and
    // stop only at the existing zero end marker.
    pageProvider.acquireCount = 0;
    pageProvider.releaseCount = 0;
    const uint8_t beyondAllKeys[13] = {
        0xFFu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    matches = ProbeMatchState();
    const GxIndexLeafTraversalStatus endStatus = gxIndexLeafLookupChain(
        &provider, firstPageId, pageCount, ownerIndexId,
        pageCount - 1, beyondAllKeys, sizeof(beyondAllKeys),
        previousPhysicalKey, sizeof(previousPhysicalKey),
        probeVisitMatch, &matches, &traversal);
    if (endStatus != GxIndexLeafTraversalStatus::EndOfChain ||
        matches.count != 0 || traversal.pagesVisited != leafCount ||
        pageProvider.releaseCount != traversal.pagesVisited ||
        pageProvider.active) {
        return 24;
    }

    // A provider error after a successful borrow still releases that borrow.
    pageProvider.acquireCount = 0;
    pageProvider.releaseCount = 0;
    pageProvider.failOnAcquire = 2;
    matches = ProbeMatchState();
    const GxIndexLeafTraversalStatus providerFailure =
        gxIndexLeafLookupChain(
            &provider, firstPageId, pageCount, ownerIndexId,
            pageCount - 1, probePhysical, sizeof(probePhysical),
            previousPhysicalKey, sizeof(previousPhysicalKey),
            probeVisitMatch, &matches, &traversal);
    if (providerFailure != GxIndexLeafTraversalStatus::ProviderFailure ||
        pageProvider.releaseCount != 1 || pageProvider.active) {
        return 25;
    }

    // Corrupt only the existing next-leaf field in a production-writer page
    // payload. The portable traversal rejects it and releases the borrow.
    pageProvider.acquireCount = 0;
    pageProvider.releaseCount = 0;
    pageProvider.failOnAcquire = 0;
    uint8_t badFirstLeaf[480] = {};
    size_t firstIndex = leafCount;
    for (size_t i = 0; i < leafCount; ++i) {
        if (leafPageIds[i] == firstPageId) firstIndex = i;
    }
    if (firstIndex == leafCount || leafLengths[firstIndex] != sizeof(badFirstLeaf)) {
        return 26;
    }
    for (size_t i = 0; i < sizeof(badFirstLeaf); ++i) {
        badFirstLeaf[i] = leafPayloads[firstIndex][i];
    }
    storeProbeLe64(badFirstLeaf + 8, pageCount);
    pageProvider.overridePageId = firstPageId;
    pageProvider.overrideBytes = badFirstLeaf;
    pageProvider.overrideLength = sizeof(badFirstLeaf);
    pageProvider.hasOverride = true;
    matches = ProbeMatchState();
    const GxIndexLeafTraversalStatus badLinkStatus =
        gxIndexLeafLookupChain(
            &provider, firstPageId, pageCount, ownerIndexId,
            pageCount - 1, probePhysical, sizeof(probePhysical),
            previousPhysicalKey, sizeof(previousPhysicalKey),
            probeVisitMatch, &matches, &traversal);
    if (badLinkStatus != GxIndexLeafTraversalStatus::InvalidPageId ||
        pageProvider.releaseCount != 1 || pageProvider.active) {
        return 27;
    }
    return 0;
}
