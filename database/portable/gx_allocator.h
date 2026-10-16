#ifndef GX_PORTABLE_ALLOCATOR_H
#define GX_PORTABLE_ALLOCATOR_H

#include <stddef.h>

#include "gx_status.h"

typedef bool (*GxAllocateCallback)(void* context, size_t size, size_t alignment,
                                   void** outPointer);
typedef void (*GxFreeCallback)(void* context, void* pointer);

struct GxAllocator {
    void* context;
    GxAllocateCallback allocate;
    GxFreeCallback deallocate;
    size_t maxAllocation;
    size_t maxAlignment;
};

// A zero-size request succeeds with *outPointer == nullptr and does not call
// the backend. Nonzero success always returns a non-null pointer.
GxStatus gxAllocatorAllocate(const GxAllocator* allocator, size_t size,
                             size_t alignment, void** outPointer);
void gxAllocatorFree(const GxAllocator* allocator, void* pointer);

#endif
