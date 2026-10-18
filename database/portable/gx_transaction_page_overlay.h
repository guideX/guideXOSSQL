#ifndef GX_PORTABLE_TRANSACTION_PAGE_OVERLAY_H
#define GX_PORTABLE_TRANSACTION_PAGE_OVERLAY_H

#include "gx_transaction_page_index.h"
#include "gx_transaction_pages.h"

// The transaction overlay combines the single payload owner with a lookup-only
// index. The page owner remains the authority for bytes and insertion order.
struct GxTransactionPageOverlay {
    GxTransactionPages pages;
    GxTransactionPageIndex index;
    bool initialized;
};

// Zero-initialize an overlay before its first init. Owner descriptors and
// lookup slots are allocated once; page images are allocated per staged page.
GxStatus gxTransactionPageOverlayInit(GxTransactionPageOverlay* overlay,
                                      const GxAllocator* allocator,
                                      size_t pageSize, size_t maximumPages);
void gxTransactionPageOverlayDestroy(GxTransactionPageOverlay* overlay);
void gxTransactionPageOverlayClear(GxTransactionPageOverlay* overlay);

GxStatus gxTransactionPageOverlayFind(GxTransactionPageOverlay* overlay,
                                      uint64_t pageId, bool* outFound,
                                      size_t* outOwnerIndex);
GxStatus gxTransactionPageOverlayStage(
    GxTransactionPageOverlay* overlay, uint64_t pageId,
    uint16_t pageType, uint16_t pageFlags, uint32_t pageGeneration,
    const uint8_t* payload, size_t payloadSize, bool* outReplaced);

GxStatus gxTransactionPageOverlayClone(GxTransactionPageOverlay* destination,
                                       const GxTransactionPageOverlay* source);
GxStatus gxTransactionPageOverlayMove(GxTransactionPageOverlay* destination,
                                       GxTransactionPageOverlay* source);

size_t gxTransactionPageOverlayCount(const GxTransactionPageOverlay* overlay);
const GxTransactionPageEntry* gxTransactionPageOverlayAt(
    const GxTransactionPageOverlay* overlay, size_t index);

#endif
