#include "gx_transaction_page_index.h"

#include <stdint.h>

namespace {

void saturatingIncrement(uint64_t* value) {
    if (*value != UINT64_MAX) ++*value;
}

void recordProbe(GxTransactionPageIndex* index, size_t probeLength,
                 bool insertion) {
    if (insertion) {
        if (index->stats.insertionProbes <= UINT64_MAX - probeLength) {
            index->stats.insertionProbes += static_cast<uint64_t>(probeLength);
        } else {
            index->stats.insertionProbes = UINT64_MAX;
        }
        if (probeLength > 1) {
            const size_t collisions = probeLength - 1;
            if (index->stats.collisions <= UINT64_MAX - collisions) {
                index->stats.collisions += static_cast<uint64_t>(collisions);
            } else {
                index->stats.collisions = UINT64_MAX;
            }
        }
    } else if (index->stats.lookupProbes <= UINT64_MAX - probeLength) {
        index->stats.lookupProbes += static_cast<uint64_t>(probeLength);
    } else {
        index->stats.lookupProbes = UINT64_MAX;
    }
    if (probeLength > index->stats.maximumProbeLength) {
        index->stats.maximumProbeLength = probeLength;
    }
}

size_t homeSlot(uint64_t pageId, size_t capacity) {
    // SplitMix64's finalizer. Unsigned multiplication intentionally wraps
    // modulo 2^64, as required by this hash mixer.
    uint64_t value = pageId;
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return static_cast<size_t>(value) & (capacity - 1);
}

size_t nextSlot(size_t slot, size_t capacity) {
    ++slot;
    return slot == capacity ? 0 : slot;
}

} // namespace

GxStatus gxTransactionPageIndexStorageSize(size_t maximumEntries,
                                           size_t* outCapacity,
                                           size_t* outBytes) {
    if (outCapacity) *outCapacity = 0;
    if (outBytes) *outBytes = 0;
    if (!outCapacity || !outBytes || maximumEntries == 0) {
        return GxStatus::InvalidArgument;
    }
    if (maximumEntries > static_cast<size_t>(-1) / 2) {
        return GxStatus::Overflow;
    }
    const size_t minimumCapacity = maximumEntries * 2;
    size_t capacity = 1;
    while (capacity < minimumCapacity) {
        if (capacity > static_cast<size_t>(-1) / 2) {
            return GxStatus::Overflow;
        }
        capacity *= 2;
    }
    if (capacity > static_cast<size_t>(-1) /
                       sizeof(GxTransactionPageIndexSlot)) {
        return GxStatus::Overflow;
    }
    *outCapacity = capacity;
    *outBytes = capacity * sizeof(GxTransactionPageIndexSlot);
    return GxStatus::Ok;
}

GxStatus gxTransactionPageIndexInit(GxTransactionPageIndex* index,
                                    const GxAllocator* allocator,
                                    size_t maximumEntries) {
    if (!index || index->initialized || !allocator) {
        return GxStatus::InvalidArgument;
    }
    size_t capacity = 0;
    size_t bytes = 0;
    GxStatus status = gxTransactionPageIndexStorageSize(
        maximumEntries, &capacity, &bytes);
    if (status != GxStatus::Ok) return status;

    index->allocator = *allocator;
    index->slots = 0;
    index->capacity = 0;
    index->count = 0;
    index->maximumEntries = 0;
    index->allocationBytes = 0;
    index->stats = {};
    index->initialized = true;

    void* allocation = 0;
    status = gxAllocatorAllocate(&index->allocator, bytes,
                                 alignof(GxTransactionPageIndexSlot),
                                 &allocation);
    if (status != GxStatus::Ok) {
        index->initialized = false;
        return status;
    }
    index->slots = static_cast<GxTransactionPageIndexSlot*>(allocation);
    index->capacity = capacity;
    index->maximumEntries = maximumEntries;
    index->allocationBytes = bytes;
    for (size_t i = 0; i < capacity; ++i) {
        index->slots[i].pageId = 0;
        index->slots[i].ownerIndex = 0;
    }
    return GxStatus::Ok;
}

void gxTransactionPageIndexDestroy(GxTransactionPageIndex* index) {
    if (!index || !index->initialized) return;
    gxAllocatorFree(&index->allocator, index->slots);
    index->slots = 0;
    index->capacity = 0;
    index->count = 0;
    index->maximumEntries = 0;
    index->allocationBytes = 0;
    index->initialized = false;
}

GxStatus gxTransactionPageIndexLookup(GxTransactionPageIndex* index,
                                      uint64_t pageId, bool* outFound,
                                      size_t* outOwnerIndex) {
    if (outFound) *outFound = false;
    if (outOwnerIndex) *outOwnerIndex = 0;
    if (!index || !index->initialized || !outFound || !outOwnerIndex ||
        pageId == 0 || index->capacity == 0 ||
        (index->capacity & (index->capacity - 1)) != 0) {
        return GxStatus::InvalidArgument;
    }

    size_t slot = homeSlot(pageId, index->capacity);
    for (size_t probe = 0; probe < index->capacity; ++probe) {
        const size_t probeLength = probe + 1;
        const uint64_t candidate = index->slots[slot].pageId;
        if (candidate == pageId) {
            recordProbe(index, probeLength, false);
            *outFound = true;
            *outOwnerIndex = index->slots[slot].ownerIndex;
            saturatingIncrement(&index->stats.lookupHits);
            return GxStatus::Ok;
        }
        if (candidate == 0) {
            recordProbe(index, probeLength, false);
            saturatingIncrement(&index->stats.lookupMisses);
            return GxStatus::Ok;
        }
        slot = nextSlot(slot, index->capacity);
    }
    recordProbe(index, index->capacity, false);
    saturatingIncrement(&index->stats.lookupMisses);
    return GxStatus::CorruptData;
}

GxStatus gxTransactionPageIndexInsert(GxTransactionPageIndex* index,
                                      uint64_t pageId, size_t ownerIndex) {
    if (!index || !index->initialized || pageId == 0 ||
        ownerIndex >= index->maximumEntries ||
        index->capacity == 0) {
        return GxStatus::InvalidArgument;
    }
    size_t slot = homeSlot(pageId, index->capacity);
    for (size_t probe = 0; probe < index->capacity; ++probe) {
        const size_t probeLength = probe + 1;
        const uint64_t candidate = index->slots[slot].pageId;
        if (candidate == pageId) {
            recordProbe(index, probeLength, true);
            return GxStatus::InvalidArgument;
        }
        if (candidate == 0) {
            recordProbe(index, probeLength, true);
            if (index->count >= index->maximumEntries) {
                return GxStatus::ResourceLimit;
            }
            index->slots[slot].ownerIndex = ownerIndex;
            index->slots[slot].pageId = pageId;
            ++index->count;
            return GxStatus::Ok;
        }
        slot = nextSlot(slot, index->capacity);
    }
    return GxStatus::CorruptData;
}

void gxTransactionPageIndexClear(GxTransactionPageIndex* index) {
    if (!index || !index->initialized) return;
    for (size_t i = 0; i < index->capacity; ++i) {
        index->slots[i].pageId = 0;
        index->slots[i].ownerIndex = 0;
    }
    index->count = 0;
}

GxStatus gxTransactionPageIndexClone(GxTransactionPageIndex* destination,
                                     const GxTransactionPageIndex* source) {
    if (!destination || !source || !source->initialized ||
        destination->initialized) {
        return GxStatus::InvalidArgument;
    }
    GxStatus status = gxTransactionPageIndexInit(
        destination, &source->allocator, source->maximumEntries);
    if (status != GxStatus::Ok) return status;
    if (destination->capacity != source->capacity ||
        destination->allocationBytes != source->allocationBytes) {
        gxTransactionPageIndexDestroy(destination);
        return GxStatus::CorruptData;
    }
    for (size_t i = 0; i < source->capacity; ++i) {
        destination->slots[i] = source->slots[i];
    }
    destination->count = source->count;
    destination->stats = source->stats;
    return GxStatus::Ok;
}

GxStatus gxTransactionPageIndexMove(GxTransactionPageIndex* destination,
                                    GxTransactionPageIndex* source) {
    if (!destination || !source || !source->initialized) {
        return GxStatus::InvalidArgument;
    }
    if (destination == source) return GxStatus::Ok;
    gxTransactionPageIndexDestroy(destination);
    *destination = *source;
    source->slots = 0;
    source->capacity = 0;
    source->count = 0;
    source->maximumEntries = 0;
    source->allocationBytes = 0;
    source->stats = {};
    source->initialized = false;
    return GxStatus::Ok;
}
