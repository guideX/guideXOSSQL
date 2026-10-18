#include "gx_transaction_page_overlay.h"

GxStatus gxTransactionPageOverlayInit(GxTransactionPageOverlay* overlay,
                                      const GxAllocator* allocator,
                                      size_t pageSize, size_t maximumPages) {
    if (!overlay || overlay->initialized || !allocator) {
        return GxStatus::InvalidArgument;
    }
    overlay->pages = {};
    overlay->index = {};
    overlay->initialized = true;

    GxStatus status = gxTransactionPagesInit(
        &overlay->pages, allocator, pageSize, maximumPages);
    if (status != GxStatus::Ok) {
        overlay->initialized = false;
        return status;
    }
    status = gxTransactionPageIndexInit(&overlay->index, allocator,
                                       maximumPages);
    if (status != GxStatus::Ok) {
        gxTransactionPagesDestroy(&overlay->pages);
        overlay->initialized = false;
        return status;
    }
    return GxStatus::Ok;
}

void gxTransactionPageOverlayDestroy(GxTransactionPageOverlay* overlay) {
    if (!overlay || !overlay->initialized) return;
    gxTransactionPageIndexDestroy(&overlay->index);
    gxTransactionPagesDestroy(&overlay->pages);
    overlay->initialized = false;
}

void gxTransactionPageOverlayClear(GxTransactionPageOverlay* overlay) {
    if (!overlay || !overlay->initialized) return;
    gxTransactionPagesClear(&overlay->pages);
    gxTransactionPageIndexClear(&overlay->index);
}

GxStatus gxTransactionPageOverlayFind(GxTransactionPageOverlay* overlay,
                                      uint64_t pageId, bool* outFound,
                                      size_t* outOwnerIndex) {
    if (outFound) *outFound = false;
    if (outOwnerIndex) *outOwnerIndex = 0;
    if (!overlay || !overlay->initialized || !outFound || !outOwnerIndex) {
        return GxStatus::InvalidArgument;
    }
    GxStatus status = gxTransactionPageIndexLookup(
        &overlay->index, pageId, outFound, outOwnerIndex);
    if (status != GxStatus::Ok || !*outFound) return status;

    const GxTransactionPageEntry* entry = gxTransactionPagesAt(
        &overlay->pages, *outOwnerIndex);
    if (!entry || !entry->image || entry->pageId != pageId) {
        *outFound = false;
        *outOwnerIndex = 0;
        return GxStatus::CorruptData;
    }
    return GxStatus::Ok;
}

GxStatus gxTransactionPageOverlayStage(
    GxTransactionPageOverlay* overlay, uint64_t pageId,
    uint16_t pageType, uint16_t pageFlags, uint32_t pageGeneration,
    const uint8_t* payload, size_t payloadSize, bool* outReplaced) {
    if (outReplaced) *outReplaced = false;
    if (!overlay || !overlay->initialized || !outReplaced || pageId == 0) {
        return GxStatus::InvalidArgument;
    }

    bool found = false;
    size_t ownerIndex = 0;
    GxStatus status = gxTransactionPageOverlayFind(
        overlay, pageId, &found, &ownerIndex);
    if (status != GxStatus::Ok) return status;
    if (found) {
        status = gxTransactionPagesReplacePage(
            &overlay->pages, ownerIndex, pageId, pageType, pageFlags,
            pageGeneration, payload, payloadSize);
        if (status == GxStatus::Ok) *outReplaced = true;
        return status;
    }

    const size_t ownerCount = gxTransactionPagesCount(&overlay->pages);
    if (ownerCount != overlay->index.count) return GxStatus::CorruptData;
    if (ownerCount >= overlay->pages.capacity ||
        ownerCount >= overlay->index.maximumEntries) {
        return GxStatus::ResourceLimit;
    }

    size_t appendedIndex = 0;
    status = gxTransactionPagesAppendPage(
        &overlay->pages, pageId, pageType, pageFlags, pageGeneration,
        payload, payloadSize, &appendedIndex);
    if (status != GxStatus::Ok) return status;

    // Index slots are allocated up front. If validation detects an internal
    // mismatch, pop the just-published owner entry before returning failure.
    status = gxTransactionPageIndexInsert(
        &overlay->index, pageId, appendedIndex);
    if (status != GxStatus::Ok) {
        (void)gxTransactionPagesPopLast(&overlay->pages, appendedIndex);
        return status;
    }
    return GxStatus::Ok;
}

GxStatus gxTransactionPageOverlayClone(GxTransactionPageOverlay* destination,
                                       const GxTransactionPageOverlay* source) {
    if (!destination || !source || !source->initialized ||
        destination->initialized) {
        return GxStatus::InvalidArgument;
    }
    if (source->pages.count != source->index.count) {
        return GxStatus::CorruptData;
    }
    destination->pages = {};
    destination->index = {};
    destination->initialized = true;
    GxStatus status = gxTransactionPagesClone(
        &destination->pages, &source->pages);
    if (status != GxStatus::Ok) {
        destination->initialized = false;
        return status;
    }
    status = gxTransactionPageIndexClone(&destination->index, &source->index);
    if (status != GxStatus::Ok) {
        gxTransactionPagesDestroy(&destination->pages);
        destination->initialized = false;
        return status;
    }
    bool consistent = destination->pages.count == destination->index.count;
    for (size_t i = 0; consistent && i < destination->pages.count; ++i) {
        const GxTransactionPageEntry* entry = gxTransactionPagesAt(
            &destination->pages, i);
        bool found = false;
        size_t ownerIndex = 0;
        consistent = entry && entry->image &&
            gxTransactionPageIndexLookup(&destination->index, entry->pageId,
                                         &found, &ownerIndex) == GxStatus::Ok &&
            found && ownerIndex == i;
    }
    if (!consistent) {
        gxTransactionPageIndexDestroy(&destination->index);
        gxTransactionPagesDestroy(&destination->pages);
        destination->initialized = false;
        return GxStatus::CorruptData;
    }
    return GxStatus::Ok;
}

GxStatus gxTransactionPageOverlayMove(GxTransactionPageOverlay* destination,
                                       GxTransactionPageOverlay* source) {
    if (!destination || !source || !source->initialized) {
        return GxStatus::InvalidArgument;
    }
    if (destination == source) return GxStatus::Ok;
    gxTransactionPageOverlayDestroy(destination);
    *destination = *source;
    source->pages = {};
    source->index = {};
    source->initialized = false;
    return GxStatus::Ok;
}

size_t gxTransactionPageOverlayCount(const GxTransactionPageOverlay* overlay) {
    if (!overlay || !overlay->initialized) return 0;
    return gxTransactionPagesCount(&overlay->pages);
}

const GxTransactionPageEntry* gxTransactionPageOverlayAt(
    const GxTransactionPageOverlay* overlay, size_t index) {
    if (!overlay || !overlay->initialized) return 0;
    return gxTransactionPagesAt(&overlay->pages, index);
}
