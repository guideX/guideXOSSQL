#include "gx_transaction_pages.h"

#include "database_endian.h"
#include "database_format.h"
#include "gx_page_codec.h"

namespace {

void saturatingIncrement(uint64_t* value) {
    if (*value != UINT64_MAX) ++*value;
}

GxStatus prepareImage(GxTransactionPages* pages, uint64_t pageId,
                      uint16_t pageType, uint16_t pageFlags,
                      uint32_t pageGeneration, const uint8_t* payload,
                      size_t payloadSize, uint8_t** outImage) {
    if (outImage) *outImage = 0;
    if (!pages || !pages->initialized || !outImage || pageId == 0 ||
        pageId >= gxos::db::kMaxPageCount ||
        (payloadSize != 0 && payload == 0) ||
        payloadSize > pages->pageSize - gxos::db::kPageHeaderSize) {
        return GxStatus::InvalidArgument;
    }
    void* allocation = 0;
    const GxStatus allocationStatus = gxAllocatorAllocate(
        &pages->allocator, pages->pageSize, alignof(uint8_t), &allocation);
    if (allocationStatus != GxStatus::Ok) {
        saturatingIncrement(&pages->stats.allocationFailures);
        return allocationStatus;
    }
    uint8_t* image = static_cast<uint8_t*>(allocation);
    const GxStatus encodeStatus = gxDatabasePageEncode(
        image, static_cast<uint32_t>(pages->pageSize), pageId, pageType,
        pageFlags, pageGeneration, payload, payloadSize);
    if (encodeStatus != GxStatus::Ok) {
        gxAllocatorFree(&pages->allocator, image);
        return encodeStatus;
    }
    *outImage = image;
    return GxStatus::Ok;
}

void releaseImage(GxTransactionPages* pages, uint8_t* image) {
    if (!pages || !pages->initialized || !image) return;
    gxAllocatorFree(&pages->allocator, image);
    if (pages->stats.currentBytes >= pages->pageSize) {
        pages->stats.currentBytes -= pages->pageSize;
    } else {
        pages->stats.currentBytes = 0;
    }
}

} // namespace

GxStatus gxTransactionPagesInit(GxTransactionPages* pages,
                                const GxAllocator* allocator,
                                size_t pageSize, size_t maximumPages) {
    if (!pages || pages->initialized || !allocator || maximumPages == 0 ||
        pageSize < gxos::db::kMinPageSize ||
        pageSize > gxos::db::kMaxPageSize ||
        !gxos::db::isSupportedPageSize(static_cast<uint32_t>(pageSize))) {
        return GxStatus::InvalidArgument;
    }
    if (maximumPages > static_cast<size_t>(-1) / sizeof(GxTransactionPageEntry) ||
        maximumPages > static_cast<size_t>(-1) / pageSize) {
        return GxStatus::Overflow;
    }

    pages->allocator = *allocator;
    pages->entries = 0;
    pages->count = 0;
    pages->capacity = maximumPages;
    pages->pageSize = pageSize;
    pages->maximumOwnedBytes = maximumPages * pageSize;
    pages->stats = {};
    pages->initialized = true;

    void* allocation = 0;
    const size_t descriptorBytes = maximumPages * sizeof(GxTransactionPageEntry);
    const GxStatus status = gxAllocatorAllocate(
        &pages->allocator, descriptorBytes, alignof(GxTransactionPageEntry),
        &allocation);
    if (status != GxStatus::Ok) {
        pages->initialized = false;
        pages->capacity = 0;
        pages->pageSize = 0;
        pages->maximumOwnedBytes = 0;
        return status;
    }
    pages->entries = static_cast<GxTransactionPageEntry*>(allocation);
    for (size_t i = 0; i < maximumPages; ++i) {
        pages->entries[i].pageId = 0;
        pages->entries[i].image = 0;
    }
    return GxStatus::Ok;
}

void gxTransactionPagesDestroy(GxTransactionPages* pages) {
    if (!pages || !pages->initialized) return;
    gxTransactionPagesClear(pages);
    gxAllocatorFree(&pages->allocator, pages->entries);
    pages->entries = 0;
    pages->count = 0;
    pages->capacity = 0;
    pages->pageSize = 0;
    pages->maximumOwnedBytes = 0;
    pages->initialized = false;
}

GxStatus gxTransactionPagesAppendPage(GxTransactionPages* pages,
                                      uint64_t pageId, uint16_t pageType,
                                      uint16_t pageFlags,
                                      uint32_t pageGeneration,
                                      const uint8_t* payload,
                                      size_t payloadSize,
                                      size_t* outIndex) {
    if (outIndex) *outIndex = static_cast<size_t>(-1);
    if (!pages || !pages->initialized || !outIndex) return GxStatus::InvalidArgument;
    if (pages->count >= pages->capacity) return GxStatus::ResourceLimit;
    if (pages->stats.currentBytes > pages->maximumOwnedBytes ||
        pages->pageSize > pages->maximumOwnedBytes - pages->stats.currentBytes) {
        return GxStatus::ResourceLimit;
    }
    uint8_t* image = 0;
    const GxStatus status = prepareImage(pages, pageId, pageType, pageFlags,
                                         pageGeneration, payload, payloadSize,
                                         &image);
    if (status != GxStatus::Ok) return status;
    const size_t index = pages->count;
    pages->entries[index].pageId = pageId;
    pages->entries[index].image = image;
    ++pages->count;
    pages->stats.currentBytes += pages->pageSize;
    if (pages->stats.currentBytes > pages->stats.peakBytes) {
        pages->stats.peakBytes = pages->stats.currentBytes;
    }
    saturatingIncrement(&pages->stats.imageAllocations);
    *outIndex = index;
    return GxStatus::Ok;
}

GxStatus gxTransactionPagesReplacePage(GxTransactionPages* pages,
                                       size_t index, uint64_t pageId,
                                       uint16_t pageType,
                                       uint16_t pageFlags,
                                       uint32_t pageGeneration,
                                       const uint8_t* payload,
                                       size_t payloadSize) {
    if (!pages || !pages->initialized || index >= pages->count ||
        pages->entries[index].pageId != pageId) {
        return GxStatus::InvalidArgument;
    }
    uint8_t* replacement = 0;
    const GxStatus status = prepareImage(pages, pageId, pageType, pageFlags,
                                         pageGeneration, payload, payloadSize,
                                         &replacement);
    if (status != GxStatus::Ok) return status;

    uint8_t* previous = pages->entries[index].image;
    pages->entries[index].image = replacement;
    // Replacement has a transient extra page allocation, but steady-state
    // ownership stays at the same bounded byte count.
    if (pages->pageSize <= static_cast<size_t>(-1) - pages->stats.currentBytes &&
        pages->stats.currentBytes + pages->pageSize > pages->stats.peakBytes) {
        pages->stats.peakBytes = pages->stats.currentBytes + pages->pageSize;
    }
    gxAllocatorFree(&pages->allocator, previous);
    saturatingIncrement(&pages->stats.replacements);
    saturatingIncrement(&pages->stats.imageAllocations);
    if (pages->stats.currentBytes > pages->stats.peakBytes) {
        pages->stats.peakBytes = pages->stats.currentBytes;
    }
    return GxStatus::Ok;
}

GxStatus gxTransactionPagesPopLast(GxTransactionPages* pages,
                                   size_t expectedIndex) {
    if (!pages || !pages->initialized || pages->count == 0 ||
        expectedIndex != pages->count - 1) {
        return GxStatus::InvalidArgument;
    }
    GxTransactionPageEntry& entry = pages->entries[expectedIndex];
    releaseImage(pages, entry.image);
    entry.image = 0;
    entry.pageId = 0;
    --pages->count;
    return GxStatus::Ok;
}

void gxTransactionPagesClear(GxTransactionPages* pages) {
    if (!pages || !pages->initialized) return;
    while (pages->count != 0) {
        (void)gxTransactionPagesPopLast(pages, pages->count - 1);
    }
    pages->stats.currentBytes = 0;
}

const GxTransactionPageEntry* gxTransactionPagesAt(
    const GxTransactionPages* pages, size_t index) {
    if (!pages || !pages->initialized || index >= pages->count) return 0;
    return &pages->entries[index];
}

size_t gxTransactionPagesCount(const GxTransactionPages* pages) {
    return pages && pages->initialized ? pages->count : 0;
}

GxStatus gxTransactionPagesClone(GxTransactionPages* destination,
                                 const GxTransactionPages* source) {
    if (!destination || !source || !source->initialized || destination->initialized) {
        return GxStatus::InvalidArgument;
    }
    const GxStatus initStatus = gxTransactionPagesInit(
        destination, &source->allocator, source->pageSize, source->capacity);
    if (initStatus != GxStatus::Ok) return initStatus;
    for (size_t i = 0; i < source->count; ++i) {
        GxDatabasePageInfo info = {};
        const GxStatus inspect = gxDatabasePageInspect(
            source->entries[i].image, static_cast<uint32_t>(source->pageSize),
            source->entries[i].pageId, &info);
        if (inspect != GxStatus::Ok) {
            gxTransactionPagesDestroy(destination);
            return inspect;
        }
        size_t appended = 0;
        const GxStatus appendStatus = gxTransactionPagesAppendPage(
            destination, info.pageId, info.pageType, info.flags, info.generation,
            info.payloadSize == 0 ? 0 : source->entries[i].image +
                gxos::db::kPageHeaderSize,
            info.payloadSize, &appended);
        if (appendStatus != GxStatus::Ok) {
            gxTransactionPagesDestroy(destination);
            return appendStatus;
        }
    }
    // Cloning creates savepoint scratch storage, not new logical mutations to
    // the transaction overlay. Preserve the source's bounded overlay counters.
    destination->stats = source->stats;
    return GxStatus::Ok;
}

GxStatus gxTransactionPagesMove(GxTransactionPages* destination,
                                GxTransactionPages* source) {
    if (!destination || !source || !source->initialized) return GxStatus::InvalidArgument;
    if (destination == source) return GxStatus::Ok;
    gxTransactionPagesDestroy(destination);
    *destination = *source;
    source->entries = 0;
    source->count = 0;
    source->capacity = 0;
    source->pageSize = 0;
    source->maximumOwnedBytes = 0;
    source->stats = {};
    source->initialized = false;
    return GxStatus::Ok;
}
