#include "gx_allocator.h"

#include <stdint.h>

static bool gxIsPowerOfTwo(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

GxStatus gxAllocatorAllocate(const GxAllocator* allocator, size_t size,
                             size_t alignment, void** outPointer) {
    if (!outPointer) return GxStatus::InvalidArgument;
    *outPointer = 0;
    if (!allocator || !allocator->allocate || !allocator->deallocate ||
        !gxIsPowerOfTwo(alignment)) {
        return GxStatus::InvalidArgument;
    }
    if (alignment > allocator->maxAlignment) {
        return GxStatus::UnsupportedAlignment;
    }
    if (size == 0) return GxStatus::Ok;
    if (size > allocator->maxAllocation) return GxStatus::ResourceLimit;

    void* pointer = 0;
    if (!allocator->allocate(allocator->context, size, alignment, &pointer)) {
        if (pointer) allocator->deallocate(allocator->context, pointer);
        return GxStatus::OutOfMemory;
    }
    if (!pointer) return GxStatus::OutOfMemory;
    if (reinterpret_cast<uintptr_t>(pointer) % alignment != 0) {
        allocator->deallocate(allocator->context, pointer);
        return GxStatus::UnsupportedAlignment;
    }
    *outPointer = pointer;
    return GxStatus::Ok;
}

void gxAllocatorFree(const GxAllocator* allocator, void* pointer) {
    if (!pointer || !allocator || !allocator->deallocate) return;
    allocator->deallocate(allocator->context, pointer);
}
