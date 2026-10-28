#include "gx_index_leaf_traversal.h"

#include "gx_index_physical_key.h"

namespace {

const size_t kPhysicalLocatorBytes = 12;

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

void copyBytes(uint8_t* destination, const uint8_t* source, size_t length) {
    for (size_t i = 0; i < length; ++i) destination[i] = source[i];
}

void storeU64Big(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56u - 8u * i));
    }
}

void storeU32Big(uint8_t* bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}

GxIndexLeafTraversalStatus publish(
    GxIndexLeafTraversalStatus status,
    GxIndexLeafTraversalResult* outResult,
    const GxIndexLeafTraversalResult& result) {
    if (outResult != nullptr) *outResult = result;
    return status;
}

} // namespace

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
    GxIndexLeafTraversalResult* outResult) {
    GxIndexLeafTraversalResult result = {};
    result.providerStatus = GxIndexPageProviderStatus::Ok;
    result.leafError = GxIndexLeafError::None;

    if (provider == nullptr || provider->acquire == nullptr ||
        provider->release == nullptr || visitor == nullptr ||
        outResult == nullptr || physicalLowerBound == nullptr ||
        previousPhysicalKey == nullptr ||
        physicalLowerBoundLength < kPhysicalLocatorBytes ||
        previousPhysicalKeyCapacity == 0 || pageCount <= 1 ||
        maxPagesToVisit == 0) {
        return GxIndexLeafTraversalStatus::InvalidArgument;
    }
    if (firstPageId == 0 || firstPageId >= pageCount) {
        return GxIndexLeafTraversalStatus::InvalidPageId;
    }
    if (rangesOverlap(outResult, sizeof(*outResult), provider,
                      sizeof(*provider)) ||
        rangesOverlap(outResult, sizeof(*outResult), physicalLowerBound,
                      physicalLowerBoundLength) ||
        rangesOverlap(outResult, sizeof(*outResult), previousPhysicalKey,
                      previousPhysicalKeyCapacity) ||
        rangesOverlap(previousPhysicalKey, previousPhysicalKeyCapacity,
                      physicalLowerBound, physicalLowerBoundLength)) {
        return GxIndexLeafTraversalStatus::InvalidArgument;
    }

    const size_t logicalKeyLength =
        physicalLowerBoundLength - kPhysicalLocatorBytes;
    for (size_t i = logicalKeyLength; i < physicalLowerBoundLength; ++i) {
        if (physicalLowerBound[i] != 0) {
            return GxIndexLeafTraversalStatus::InvalidArgument;
        }
    }

    const uint64_t ordinaryPageCount = pageCount - 1;
    const uint64_t pageLimit = maxPagesToVisit < ordinaryPageCount
        ? maxPagesToVisit : ordinaryPageCount;
    uint64_t currentPageId = firstPageId;
    size_t previousPhysicalKeyLength = 0;
    bool hasPreviousPhysicalKey = false;

    while (currentPageId != 0) {
        if (result.pagesVisited >= pageLimit) {
            return publish(GxIndexLeafTraversalStatus::TraversalLimit,
                           outResult, result);
        }

        GxIndexPageView view = {};
        const GxIndexPageProviderStatus acquireStatus =
            provider->acquire(provider->context, currentPageId, &view);
        if (acquireStatus != GxIndexPageProviderStatus::Ok) {
            result.providerStatus = acquireStatus;
            if (acquireStatus == GxIndexPageProviderStatus::MissingPage) {
                return publish(GxIndexLeafTraversalStatus::MissingPage,
                               outResult, result);
            }
            if (acquireStatus == GxIndexPageProviderStatus::InvalidPageId) {
                return publish(GxIndexLeafTraversalStatus::InvalidPageId,
                               outResult, result);
            }
            return publish(GxIndexLeafTraversalStatus::ProviderFailure,
                           outResult, result);
        }

        ++result.pagesVisited;
        result.lastPageId = currentPageId;

        // A successful acquire always requires a release, even when the view
        // is malformed or aliases caller memory.
        if (view.bytes == nullptr || view.length == 0 ||
            rangesOverlap(view.bytes, view.length, physicalLowerBound,
                          physicalLowerBoundLength) ||
            rangesOverlap(view.bytes, view.length, previousPhysicalKey,
                          previousPhysicalKeyCapacity) ||
            rangesOverlap(view.bytes, view.length, outResult,
                          sizeof(*outResult))) {
            provider->release(provider->context, currentPageId, &view);
            if (view.bytes == nullptr || view.length == 0) {
                return publish(GxIndexLeafTraversalStatus::CorruptPage,
                               outResult, result);
            }
            // Do not publish into an output or scratch span backed by a page.
            return GxIndexLeafTraversalStatus::InvalidArgument;
        }

        GxIndexLeafTraversalStatus pageStatus =
            GxIndexLeafTraversalStatus::EndOfChain;
        GxIndexLeafError leafError = GxIndexLeafError::None;
        GxIndexLeafLookupResult lowerBound = {};
        GxIndexLeafPageInfo pageInfo = {};
        int comparison = 0;

        GxStatus lookupStatus = gxIndexLeafLookup(
            view.bytes, view.length, physicalLowerBound,
            physicalLowerBoundLength, &lowerBound, &leafError);
        if (lookupStatus != GxStatus::Ok) {
            pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
            result.leafError = leafError;
        } else if (lowerBound.continuation.page.ownerIndexId !=
                   expectedOwnerIndexId) {
            pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
        } else if (lowerBound.continuation.page.nextLeafPageId != 0 &&
                   lowerBound.continuation.page.nextLeafPageId >= pageCount) {
            pageStatus = GxIndexLeafTraversalStatus::InvalidPageId;
        } else {
            pageInfo = lowerBound.continuation.page;

            // Capture the page boundary while the borrow is active. This also
            // checks the first key against the previous nonempty leaf.
            GxIndexLeafCursor boundaryCursor = {};
            lookupStatus = gxIndexLeafCursorInit(
                view.bytes, view.length, &boundaryCursor, &leafError);
            if (lookupStatus != GxStatus::Ok) {
                pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
                result.leafError = leafError;
            } else {
                bool hasEntry = false;
                bool hasPageEntry = false;
                for (;;) {
                    GxIndexLeafEntryView boundaryEntry = {};
                    lookupStatus = gxIndexLeafCursorReadNext(
                        &boundaryCursor, &boundaryEntry, &hasEntry,
                        &leafError);
                    if (lookupStatus != GxStatus::Ok) {
                        pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
                        result.leafError = leafError;
                        break;
                    }
                    if (!hasEntry) break;

                    if (!hasPageEntry && hasPreviousPhysicalKey) {
                        const GxStatus compareStatus =
                            gxCompareIndexPhysicalKeyViews(
                                previousPhysicalKey,
                                previousPhysicalKeyLength, false, 0, 0,
                                boundaryEntry.keyBytes,
                                boundaryEntry.keyLength, true,
                                boundaryEntry.locatorPageId,
                                boundaryEntry.locatorSlot, &comparison);
                        if (compareStatus != GxStatus::Ok || comparison >= 0) {
                            pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
                            break;
                        }
                    }

                    hasPageEntry = true;
                    if (boundaryEntry.keyLength >
                        static_cast<size_t>(-1) - kPhysicalLocatorBytes) {
                        pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
                        break;
                    }
                    const size_t physicalLength = boundaryEntry.keyLength +
                        kPhysicalLocatorBytes;
                    if (physicalLength > previousPhysicalKeyCapacity) {
                        pageStatus = GxIndexLeafTraversalStatus::InvalidArgument;
                        break;
                    }
                    copyBytes(previousPhysicalKey, boundaryEntry.keyBytes,
                              boundaryEntry.keyLength);
                    storeU64Big(previousPhysicalKey + boundaryEntry.keyLength,
                                boundaryEntry.locatorPageId);
                    storeU32Big(previousPhysicalKey + boundaryEntry.keyLength + 8,
                                boundaryEntry.locatorSlot);
                    previousPhysicalKeyLength = physicalLength;
                    hasPreviousPhysicalKey = true;
                }

                if (lookupStatus == GxStatus::Ok &&
                    pageStatus == GxIndexLeafTraversalStatus::EndOfChain) {
                    GxIndexLeafEntryView entry = lowerBound.entry;
                    bool hasCandidate = lowerBound.hasEntry;
                    GxIndexLeafCursor cursor = lowerBound.continuation;
                    while (hasCandidate) {
                        comparison = 0;
                        const GxStatus compareStatus =
                            gxCompareIndexPhysicalKeyViews(
                                entry.keyBytes, entry.keyLength, false, 0, 0,
                                physicalLowerBound, logicalKeyLength, false,
                                0, 0, &comparison);
                        if (compareStatus != GxStatus::Ok) {
                            pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
                            break;
                        }
                        if (comparison > 0) {
                            pageStatus = GxIndexLeafTraversalStatus::KeyBoundary;
                            break;
                        }
                        if (comparison == 0) {
                            if (result.matchesVisited ==
                                static_cast<uint64_t>(-1)) {
                                pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
                                break;
                            }
                            ++result.matchesVisited;
                            const GxIndexLeafVisitControl visitControl =
                                visitor(visitorContext, currentPageId, &entry);
                            if (visitControl == GxIndexLeafVisitControl::Stop) {
                                pageStatus = GxIndexLeafTraversalStatus::VisitorStopped;
                                break;
                            }
                            if (visitControl != GxIndexLeafVisitControl::Continue) {
                                pageStatus = GxIndexLeafTraversalStatus::VisitorFailure;
                                break;
                            }
                        }

                        lookupStatus = gxIndexLeafCursorReadNext(
                            &cursor, &entry, &hasCandidate, &leafError);
                        if (lookupStatus != GxStatus::Ok) {
                            pageStatus = GxIndexLeafTraversalStatus::CorruptPage;
                            result.leafError = leafError;
                            break;
                        }
                    }
                }
            }
        }

        const uint64_t nextPageId = pageInfo.nextLeafPageId;
        provider->release(provider->context, currentPageId, &view);

        if (pageStatus != GxIndexLeafTraversalStatus::EndOfChain) {
            return publish(pageStatus, outResult, result);
        }
        if (nextPageId == 0) {
            return publish(GxIndexLeafTraversalStatus::EndOfChain,
                           outResult, result);
        }
        currentPageId = nextPageId;
    }

    return publish(GxIndexLeafTraversalStatus::EndOfChain, outResult, result);
}
