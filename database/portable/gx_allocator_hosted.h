#ifndef GX_PORTABLE_ALLOCATOR_HOSTED_H
#define GX_PORTABLE_ALLOCATOR_HOSTED_H

#include <stddef.h>

#include "gx_allocator.h"

// Hosted adapter used by the independent test target. The portable containers
// still use the same explicit callback contract as a future kernel adapter.
GxStatus gxHostedAllocatorInit(GxAllocator* outAllocator,
                              size_t maxAllocation);

#endif
