#include "gx_recovered_pages.h"

#include <stddef.h>

#include "database_format.h"
#include "gx_page_codec.h"

namespace {

void zeroCollection(GxRecoveredPageCollection* collection) {
    collection->allocator.context = 0;
    collection->allocator.allocate = 0;
    collection->allocator.deallocate = 0;
    collection->allocator.maxAllocation = 0;
    collection->allocator.maxAlignment = 0;
    collection->head = 0;
    collection->tail = 0;
    collection->count = 0;
    collection->maximumCount = 0;
    collection->pageSize = 0;
    collection->retainedBytes = 0;
    collection->maximumBytes = 0;
    collection->initialized = false;
}

void copyBytes(uint8_t* destination, const uint8_t* source, size_t length) {
    for (size_t i = 0; i < length; ++i) destination[i] = source[i];
}

} // namespace

GxStatus gxRecoveredPagesInit(GxRecoveredPageCollection* collection,
                              const GxAllocator* allocator, size_t pageSize,
                              size_t maximumCount, size_t maximumBytes) {
    if (!collection || !allocator || collection->initialized ||
        !allocator->allocate || !allocator->deallocate || maximumCount == 0 ||
        pageSize < gxos::db::kMinPageSize ||
        pageSize > gxos::db::kMaxPageSize ||
        (pageSize & (pageSize - 1)) != 0) {
        return GxStatus::InvalidArgument;
    }
    collection->allocator = *allocator;
    collection->head = 0;
    collection->tail = 0;
    collection->count = 0;
    collection->maximumCount = maximumCount;
    collection->pageSize = pageSize;
    collection->retainedBytes = 0;
    collection->maximumBytes = maximumBytes;
    collection->initialized = true;
    return GxStatus::Ok;
}

GxStatus gxRecoveredPagesRetain(GxRecoveredPageCollection* collection,
                                uint64_t pageId, const uint8_t* image,
                                size_t imageSize) {
    if (!collection || !collection->initialized || !image ||
        pageId == gxos::db::kHeaderPageId ||
        pageId >= gxos::db::kMaxPageCount ||
        imageSize != collection->pageSize) {
        return GxStatus::InvalidArgument;
    }

    GxDatabasePageInfo pageInfo = {};
    const GxStatus inspectStatus = gxDatabasePageInspect(
        image, static_cast<uint32_t>(collection->pageSize), pageId, &pageInfo);
    if (inspectStatus != GxStatus::Ok) return inspectStatus;

    GxRecoveredPage* previous = 0;
    GxRecoveredPage* existing = collection->head;
    while (existing && existing->pageId != pageId) {
        previous = existing;
        existing = existing->next;
    }
    if (!existing && collection->count >= collection->maximumCount) {
        return GxStatus::ResourceLimit;
    }

    const size_t imageOffset = sizeof(GxRecoveredPage);
    if (collection->pageSize > static_cast<size_t>(-1) - imageOffset) {
        return GxStatus::Overflow;
    }
    if (existing && collection->retainedBytes < collection->pageSize) {
        return GxStatus::Overflow;
    }
    const size_t baseBytes = collection->retainedBytes -
                             (existing ? collection->pageSize : 0);
    if (baseBytes > collection->maximumBytes ||
        collection->pageSize > collection->maximumBytes - baseBytes) {
        return GxStatus::ResourceLimit;
    }

    const size_t allocationSize = imageOffset + collection->pageSize;
    void* allocation = 0;
    const GxStatus allocationStatus = gxAllocatorAllocate(
        &collection->allocator, allocationSize, alignof(GxRecoveredPage),
        &allocation);
    if (allocationStatus != GxStatus::Ok) return allocationStatus;

    GxRecoveredPage* replacement = static_cast<GxRecoveredPage*>(allocation);
    replacement->pageId = pageId;
    replacement->next = 0;
    copyBytes(reinterpret_cast<uint8_t*>(replacement) + imageOffset,
              image, collection->pageSize);

    if (existing) {
        if (previous) previous->next = existing->next;
        else collection->head = existing->next;
        if (collection->tail == existing) collection->tail = previous;
        gxAllocatorFree(&collection->allocator, existing);
    } else {
        ++collection->count;
        collection->retainedBytes += collection->pageSize;
    }

    if (collection->tail) collection->tail->next = replacement;
    else collection->head = replacement;
    collection->tail = replacement;
    return GxStatus::Ok;
}

GxStatus gxRecoveredPagesMove(GxRecoveredPageCollection* destination,
                              GxRecoveredPageCollection* source) {
    if (!destination || !source || destination == source) {
        return GxStatus::InvalidArgument;
    }
    if (!source->initialized) return GxStatus::InvalidArgument;
    if (destination->initialized) gxRecoveredPagesDestroy(destination);
    else zeroCollection(destination);

    *destination = *source;
    zeroCollection(source);
    return GxStatus::Ok;
}

void gxRecoveredPagesClear(GxRecoveredPageCollection* collection) {
    if (!collection || !collection->initialized) return;
    GxRecoveredPage* page = collection->head;
    while (page) {
        GxRecoveredPage* next = page->next;
        gxAllocatorFree(&collection->allocator, page);
        page = next;
    }
    collection->head = 0;
    collection->tail = 0;
    collection->count = 0;
    collection->retainedBytes = 0;
}

void gxRecoveredPagesDestroy(GxRecoveredPageCollection* collection) {
    if (!collection || !collection->initialized) return;
    gxRecoveredPagesClear(collection);
    zeroCollection(collection);
}

size_t gxRecoveredPagesCount(const GxRecoveredPageCollection* collection) {
    return collection && collection->initialized ? collection->count : 0;
}

size_t gxRecoveredPagesBytes(const GxRecoveredPageCollection* collection) {
    return collection && collection->initialized ? collection->retainedBytes : 0;
}

const GxRecoveredPage* gxRecoveredPagesFirst(
    const GxRecoveredPageCollection* collection) {
    return collection && collection->initialized ? collection->head : 0;
}

const GxRecoveredPage* gxRecoveredPageNext(const GxRecoveredPage* page) {
    return page ? page->next : 0;
}

const uint8_t* gxRecoveredPageImage(const GxRecoveredPage* page) {
    return page ? reinterpret_cast<const uint8_t*>(page) +
                      sizeof(GxRecoveredPage)
                : 0;
}
