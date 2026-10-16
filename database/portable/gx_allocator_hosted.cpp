#include "gx_allocator_hosted.h"

#include <stdlib.h>

static bool gxHostedAllocate(void*, size_t size, size_t, void** outPointer) {
    *outPointer = malloc(size);
    return *outPointer != 0;
}

static void gxHostedFree(void*, void* pointer) {
    free(pointer);
}

GxStatus gxHostedAllocatorInit(GxAllocator* outAllocator,
                              size_t maxAllocation) {
    if (!outAllocator) return GxStatus::InvalidArgument;
    outAllocator->context = 0;
    outAllocator->allocate = gxHostedAllocate;
    outAllocator->deallocate = gxHostedFree;
    outAllocator->maxAllocation = maxAllocation;
    // Match the Server allocator contract inspected for NDB1R4: 8-byte
    // alignment. malloc provides at least this alignment on hosted targets.
    outAllocator->maxAlignment = 8;
    return GxStatus::Ok;
}
