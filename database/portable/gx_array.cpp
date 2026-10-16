#include "gx_array.h"

static bool gxMulSize(size_t left, size_t right, size_t* out) {
    if (right != 0 && left > static_cast<size_t>(-1) / right) return false;
    *out = left * right;
    return true;
}

static void gxCopyBytes(uint8_t* destination, const uint8_t* source,
                        size_t count) {
    for (size_t i = 0; i < count; ++i) destination[i] = source[i];
}

static void gxMoveBytes(uint8_t* destination, const uint8_t* source,
                        size_t count) {
    if (destination == source || count == 0) return;
    if (reinterpret_cast<uintptr_t>(destination) <
        reinterpret_cast<uintptr_t>(source)) {
        for (size_t i = 0; i < count; ++i) destination[i] = source[i];
    } else {
        for (size_t i = count; i != 0; --i) destination[i - 1] = source[i - 1];
    }
}

static bool gxArrayReady(const GxArray* array) {
    return array && array->initialized && array->elementSize != 0;
}

static bool gxArrayAlias(const GxArray* array, const void* source,
                         size_t bytes, size_t* offset) {
    if (!array->data || !source) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(array->data);
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    if (address < base || address - base > array->size * array->elementSize) {
        return false;
    }
    *offset = static_cast<size_t>(address - base);
    return bytes <= array->size * array->elementSize - *offset;
}

GxStatus gxArrayInit(GxArray* array, const GxAllocator* allocator,
                     size_t elementSize, size_t maximumElements) {
    if (!array || !allocator || !allocator->allocate || !allocator->deallocate ||
        elementSize == 0 || array->initialized) {
        return GxStatus::InvalidArgument;
    }
    size_t ignored = 0;
    if (!gxMulSize(maximumElements, elementSize, &ignored)) {
        return GxStatus::Overflow;
    }
    array->allocator = *allocator;
    array->data = 0;
    array->size = 0;
    array->capacity = 0;
    array->maximum = maximumElements;
    array->elementSize = elementSize;
    array->initialized = true;
    return GxStatus::Ok;
}

GxStatus gxArrayReserve(GxArray* array, size_t elements) {
    if (!gxArrayReady(array)) return GxStatus::InvalidArgument;
    if (elements <= array->capacity) return GxStatus::Ok;
    if (elements > array->maximum) return GxStatus::ResourceLimit;

    size_t newCapacity = array->capacity == 0 ? 1 : array->capacity;
    while (newCapacity < elements) {
        if (newCapacity > static_cast<size_t>(-1) / 2) {
            return GxStatus::Overflow;
        }
        newCapacity *= 2;
        if (newCapacity > array->maximum) newCapacity = array->maximum;
    }
    size_t newBytes = 0;
    size_t oldBytes = 0;
    if (!gxMulSize(newCapacity, array->elementSize, &newBytes) ||
        !gxMulSize(array->size, array->elementSize, &oldBytes)) {
        return GxStatus::Overflow;
    }

    void* allocation = 0;
    const GxStatus status = gxAllocatorAllocate(&array->allocator, newBytes, 1,
                                                 &allocation);
    if (status != GxStatus::Ok) return status;
    uint8_t* replacement = static_cast<uint8_t*>(allocation);
    if (oldBytes) gxCopyBytes(replacement, array->data, oldBytes);
    uint8_t* old = array->data;
    array->data = replacement;
    array->capacity = newCapacity;
    gxAllocatorFree(&array->allocator, old);
    return GxStatus::Ok;
}

GxStatus gxArrayAppend(GxArray* array, const void* records, size_t count) {
    if (!gxArrayReady(array) || (count != 0 && !records)) {
        return GxStatus::InvalidArgument;
    }
    if (count == 0) return GxStatus::Ok;
    if (count > static_cast<size_t>(-1) - array->size) return GxStatus::Overflow;
    const size_t newSize = array->size + count;
    if (newSize > array->maximum) return GxStatus::ResourceLimit;
    size_t copyBytes = 0;
    if (!gxMulSize(count, array->elementSize, &copyBytes)) return GxStatus::Overflow;

    size_t sourceOffset = 0;
    const bool aliases = gxArrayAlias(array, records, copyBytes, &sourceOffset);
    if (gxArrayReady(array) && array->data &&
        reinterpret_cast<uintptr_t>(records) >= reinterpret_cast<uintptr_t>(array->data) &&
        reinterpret_cast<uintptr_t>(records) - reinterpret_cast<uintptr_t>(array->data) <=
            array->size * array->elementSize && !aliases) {
        return GxStatus::InvalidArgument;
    }
    if (aliases && sourceOffset % array->elementSize != 0) {
        return GxStatus::InvalidArgument;
    }
    GxStatus status = gxArrayReserve(array, newSize);
    if (status != GxStatus::Ok) return status;
    const uint8_t* source = aliases ? array->data + sourceOffset
                                    : static_cast<const uint8_t*>(records);
    size_t destinationOffset = 0;
    if (!gxMulSize(array->size, array->elementSize, &destinationOffset)) {
        return GxStatus::Overflow;
    }
    gxMoveBytes(array->data + destinationOffset, source, copyBytes);
    array->size = newSize;
    return GxStatus::Ok;
}

GxStatus gxArrayInsert(GxArray* array, size_t index,
                       const void* records, size_t count) {
    if (!gxArrayReady(array) || index > array->size ||
        (count != 0 && !records)) {
        return GxStatus::InvalidArgument;
    }
    if (count == 0) return GxStatus::Ok;
    if (count > static_cast<size_t>(-1) - array->size) return GxStatus::Overflow;
    const size_t newSize = array->size + count;
    if (newSize > array->maximum) return GxStatus::ResourceLimit;
    size_t copyBytes = 0;
    size_t tailBytes = 0;
    size_t insertOffset = 0;
    if (!gxMulSize(count, array->elementSize, &copyBytes) ||
        !gxMulSize(array->size - index, array->elementSize, &tailBytes) ||
        !gxMulSize(index, array->elementSize, &insertOffset)) {
        return GxStatus::Overflow;
    }

    size_t sourceOffset = 0;
    const bool aliases = gxArrayAlias(array, records, copyBytes, &sourceOffset);
    if (array->data && reinterpret_cast<uintptr_t>(records) >=
            reinterpret_cast<uintptr_t>(array->data) &&
        reinterpret_cast<uintptr_t>(records) - reinterpret_cast<uintptr_t>(array->data) <=
            array->size * array->elementSize && !aliases) {
        return GxStatus::InvalidArgument;
    }
    if (aliases && sourceOffset % array->elementSize != 0) {
        return GxStatus::InvalidArgument;
    }

    void* stagedAllocation = 0;
    if (aliases) {
        const GxStatus stageStatus = gxAllocatorAllocate(&array->allocator,
                                                          copyBytes, 1,
                                                          &stagedAllocation);
        if (stageStatus != GxStatus::Ok) return stageStatus;
        gxCopyBytes(static_cast<uint8_t*>(stagedAllocation), array->data + sourceOffset,
                    copyBytes);
    }
    const GxStatus reserveStatus = gxArrayReserve(array, newSize);
    if (reserveStatus != GxStatus::Ok) {
        gxAllocatorFree(&array->allocator, stagedAllocation);
        return reserveStatus;
    }
    gxMoveBytes(array->data + insertOffset + copyBytes,
                array->data + insertOffset, tailBytes);
    const uint8_t* source = aliases ? static_cast<const uint8_t*>(stagedAllocation)
                                    : static_cast<const uint8_t*>(records);
    gxCopyBytes(array->data + insertOffset, source, copyBytes);
    array->size = newSize;
    gxAllocatorFree(&array->allocator, stagedAllocation);
    return GxStatus::Ok;
}

GxStatus gxArrayErase(GxArray* array, size_t index, size_t count) {
    if (!gxArrayReady(array) || index > array->size || count > array->size - index) {
        return GxStatus::InvalidArgument;
    }
    if (count == 0) return GxStatus::Ok;
    const size_t elementSize = array->elementSize;
    const size_t offset = index * elementSize;
    const size_t removedBytes = count * elementSize;
    const size_t tailBytes = (array->size - index - count) * elementSize;
    gxMoveBytes(array->data + offset, array->data + offset + removedBytes, tailBytes);
    array->size -= count;
    return GxStatus::Ok;
}

GxStatus gxArrayClear(GxArray* array) {
    if (!gxArrayReady(array)) return GxStatus::InvalidArgument;
    array->size = 0;
    return GxStatus::Ok;
}

void* gxArrayAt(GxArray* array, size_t index) {
    if (!gxArrayReady(array) || index >= array->size) return 0;
    return array->data + index * array->elementSize;
}

const void* gxArrayAtConst(const GxArray* array, size_t index) {
    if (!gxArrayReady(array) || index >= array->size) return 0;
    return array->data + index * array->elementSize;
}

size_t gxArraySize(const GxArray* array) {
    return gxArrayReady(array) ? array->size : 0;
}

size_t gxArrayCapacity(const GxArray* array) {
    return gxArrayReady(array) ? array->capacity : 0;
}

void gxArrayDestroy(GxArray* array) {
    if (!array || !array->initialized) return;
    gxAllocatorFree(&array->allocator, array->data);
    array->data = 0;
    array->size = 0;
    array->capacity = 0;
    array->maximum = 0;
    array->elementSize = 0;
    array->initialized = false;
    array->allocator.context = 0;
    array->allocator.allocate = 0;
    array->allocator.deallocate = 0;
    array->allocator.maxAllocation = 0;
    array->allocator.maxAlignment = 0;
}
