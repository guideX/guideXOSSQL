#include "gx_test_allocator.h"

static_assert(sizeof(GxArenaBlock) % 8 == 0, "arena metadata must be 8-byte aligned");

static size_t gxAlign8(size_t value, bool* valid) {
    if (value > static_cast<size_t>(-1) - 7) {
        *valid = false;
        return 0;
    }
    return (value + 7) & ~static_cast<size_t>(7);
}

static void gxArenaMergeNext(GxArenaBlock* block) {
    if (!block || !block->next || !block->next->free) return;
    GxArenaBlock* next = block->next;
    block->size += sizeof(GxArenaBlock) + next->size;
    block->next = next->next;
    if (block->next) block->next->previous = block;
}

static bool gxArenaAllocate(void* context, size_t size, size_t alignment,
                           void** outPointer) {
    GxArena* arena = static_cast<GxArena*>(context);
    *outPointer = 0;
    ++arena->allocationAttempts;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > 8 ||
        size > arena->maxRequest) {
        return false;
    }
    if (arena->failureEnabled &&
        arena->successfulAllocations >= arena->failAtSuccessCount) {
        return false;
    }
    bool valid = true;
    const size_t needed = gxAlign8(size, &valid);
    if (!valid) return false;

    for (GxArenaBlock* block = arena->first; block; block = block->next) {
        if (!block->free || block->size < needed) continue;
        const size_t remaining = block->size - needed;
        if (remaining >= sizeof(GxArenaBlock) + 8) {
            uint8_t* payload = reinterpret_cast<uint8_t*>(block + 1);
            GxArenaBlock* split = reinterpret_cast<GxArenaBlock*>(payload + needed);
            split->size = remaining - sizeof(GxArenaBlock);
            split->next = block->next;
            split->previous = block;
            split->free = 1;
            if (split->next) split->next->previous = split;
            block->next = split;
            block->size = needed;
        }
        block->free = 0;
        ++arena->successfulAllocations;
        ++arena->outstanding;
        *outPointer = block + 1;
        return true;
    }
    return false;
}

static void gxArenaFree(void* context, void* pointer) {
    if (!pointer) return;
    GxArena* arena = static_cast<GxArena*>(context);
    for (GxArenaBlock* block = arena->first; block; block = block->next) {
        if (static_cast<void*>(block + 1) != pointer) continue;
        if (block->free) {
            ++arena->invalidFrees;
            return;
        }
        block->free = 1;
        ++arena->freeCalls;
        --arena->outstanding;
        gxArenaMergeNext(block);
        if (block->previous && block->previous->free) {
            block = block->previous;
            gxArenaMergeNext(block);
        }
        return;
    }
    ++arena->invalidFrees;
}

void gxArenaInit(GxArena* arena, size_t maxRequest) {
    for (size_t i = 0; i < sizeof(arena->bytes); ++i) arena->bytes[i] = 0;
    arena->first = reinterpret_cast<GxArenaBlock*>(arena->bytes);
    arena->first->size = sizeof(arena->bytes) - sizeof(GxArenaBlock);
    arena->first->next = 0;
    arena->first->previous = 0;
    arena->first->free = 1;
    arena->maxRequest = maxRequest;
    arena->allocationAttempts = 0;
    arena->successfulAllocations = 0;
    arena->freeCalls = 0;
    arena->invalidFrees = 0;
    arena->outstanding = 0;
    arena->failAtSuccessCount = 0;
    arena->failureEnabled = false;
}

void gxArenaMakeAllocator(GxArena* arena, GxAllocator* outAllocator) {
    outAllocator->context = arena;
    outAllocator->allocate = gxArenaAllocate;
    outAllocator->deallocate = gxArenaFree;
    outAllocator->maxAllocation = arena->maxRequest;
    outAllocator->maxAlignment = 8;
}

void gxArenaFailAfter(GxArena* arena, size_t successfulAllocations) {
    arena->failAtSuccessCount = arena->successfulAllocations + successfulAllocations;
    arena->failureEnabled = true;
}

void gxArenaDisableFailure(GxArena* arena) {
    arena->failureEnabled = false;
}
