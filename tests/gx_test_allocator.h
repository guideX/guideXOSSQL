#ifndef GX_TEST_ALLOCATOR_H
#define GX_TEST_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"

struct GxArenaBlock {
    size_t size;
    GxArenaBlock* next;
    GxArenaBlock* previous;
    uint32_t free;
};

struct GxArena {
    alignas(8) uint8_t bytes[32768];
    GxArenaBlock* first;
    size_t maxRequest;
    size_t allocationAttempts;
    size_t successfulAllocations;
    size_t freeCalls;
    size_t invalidFrees;
    size_t outstanding;
    size_t failAtSuccessCount;
    bool failureEnabled;
};

void gxArenaInit(GxArena* arena, size_t maxRequest);
void gxArenaMakeAllocator(GxArena* arena, GxAllocator* outAllocator);
void gxArenaFailAfter(GxArena* arena, size_t successfulAllocations);
void gxArenaDisableFailure(GxArena* arena);

#endif
