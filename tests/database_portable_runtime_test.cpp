#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gx_allocator_hosted.h"
#include "gx_array.h"
#include "gx_owner.h"
#include "gx_string.h"
#include "gx_test_allocator.h"

namespace {

int gPass = 0;
int gFail = 0;

void check(bool value, const char* expression, int line) {
    if (value) ++gPass;
    else {
        ++gFail;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(value) check((value), #value, __LINE__)

struct MisalignedAllocatorState {
    alignas(8) uint8_t bytes[32];
    size_t freeCalls;
};

bool allocateNullSuccess(void*, size_t, size_t, void** outPointer) {
    *outPointer = 0;
    return true;
}

bool allocateMisaligned(void* context, size_t, size_t, void** outPointer) {
    MisalignedAllocatorState* state = static_cast<MisalignedAllocatorState*>(context);
    *outPointer = state->bytes + 1;
    return true;
}

void countMisalignedFree(void* context, void*) {
    ++static_cast<MisalignedAllocatorState*>(context)->freeCalls;
}

uint32_t nextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

bool arrayEquals(const GxArray& array, const std::vector<uint8_t>& reference) {
    if (gxArraySize(&array) != reference.size()) return false;
    for (size_t i = 0; i < reference.size(); ++i) {
        const uint8_t* byte = static_cast<const uint8_t*>(gxArrayAtConst(&array, i));
        if (!byte || *byte != reference[i]) return false;
    }
    return true;
}

bool stringEquals(const GxString& string, const std::string& reference) {
    return gxStringLength(&string) == reference.size() &&
           (reference.empty() ||
            std::memcmp(gxStringData(&string), reference.data(), reference.size()) == 0);
}

void testAllocatorContract() {
    GxArena arena;
    gxArenaInit(&arena, 4096);
    GxAllocator allocator = {};
    gxArenaMakeAllocator(&arena, &allocator);

    void* pointer = reinterpret_cast<void*>(1);
    CHECK(gxAllocatorAllocate(&allocator, 0, 1, &pointer) == GxStatus::Ok);
    CHECK(pointer == 0);
    CHECK(arena.allocationAttempts == 0);

    CHECK(gxAllocatorAllocate(&allocator, 8, 3, &pointer) == GxStatus::InvalidArgument);
    CHECK(pointer == 0);
    CHECK(gxAllocatorAllocate(&allocator, 4097, 1, &pointer) == GxStatus::ResourceLimit);
    CHECK(pointer == 0);
    CHECK(gxAllocatorAllocate(&allocator, 16, 16, &pointer) ==
          GxStatus::UnsupportedAlignment);

    GxAllocator broken = {0, 0, 0, 4096, 8};
    CHECK(gxAllocatorAllocate(&broken, 8, 1, &pointer) == GxStatus::InvalidArgument);

    GxAllocator nullSuccess = {0, allocateNullSuccess, countMisalignedFree, 4096, 8};
    CHECK(gxAllocatorAllocate(&nullSuccess, 8, 1, &pointer) == GxStatus::OutOfMemory);
    CHECK(pointer == 0);
    MisalignedAllocatorState badAlignment = {};
    GxAllocator misaligned = {&badAlignment, allocateMisaligned,
                              countMisalignedFree, 4096, 8};
    CHECK(gxAllocatorAllocate(&misaligned, 8, 8, &pointer) ==
          GxStatus::UnsupportedAlignment);
    CHECK(pointer == 0);
    CHECK(badAlignment.freeCalls == 1);

    CHECK(gxAllocatorAllocate(&allocator, 32, 8, &pointer) == GxStatus::Ok);
    CHECK(pointer != 0);
    CHECK(reinterpret_cast<uintptr_t>(pointer) % 8 == 0);
    void* firstAddress = pointer;
    gxAllocatorFree(&allocator, pointer);
    pointer = 0;
    CHECK(gxAllocatorAllocate(&allocator, 32, 8, &pointer) == GxStatus::Ok);
    CHECK(pointer == firstAddress);
    gxAllocatorFree(&allocator, pointer);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);

    void* fill[128] = {};
    size_t count = 0;
    while (count < sizeof(fill) / sizeof(fill[0]) &&
           gxAllocatorAllocate(&allocator, 512, 8, &fill[count]) == GxStatus::Ok) {
        ++count;
    }
    CHECK(count > 0);
    CHECK(count < sizeof(fill) / sizeof(fill[0]));
    CHECK(gxAllocatorAllocate(&allocator, 512, 8, &pointer) == GxStatus::OutOfMemory);
    CHECK(pointer == 0);
    for (size_t i = 0; i < count; ++i) gxAllocatorFree(&allocator, fill[i]);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
}

void testAllocationFailure() {
    GxArena arena;
    gxArenaInit(&arena, 8192);
    GxAllocator allocator = {};
    gxArenaMakeAllocator(&arena, &allocator);

    GxArray array = {};
    CHECK(gxArrayInit(&array, &allocator, 1, 64) == GxStatus::Ok);
    const uint8_t seed[] = {1, 2, 3, 4};
    CHECK(gxArrayAppend(&array, seed, sizeof(seed)) == GxStatus::Ok);
    const size_t oldCapacity = gxArrayCapacity(&array);
    gxArenaFailAfter(&arena, 0);
    const uint8_t fifth = 5;
    CHECK(gxArrayAppend(&array, &fifth, 1) == GxStatus::OutOfMemory);
    CHECK(gxArraySize(&array) == 4);
    CHECK(gxArrayCapacity(&array) == oldCapacity);
    CHECK(std::memcmp(gxArrayAt(&array, 0), seed, sizeof(seed)) == 0);
    gxArenaDisableFailure(&arena);
    gxArrayDestroy(&array);
    CHECK(arena.outstanding == 0);

    GxString string = {};
    CHECK(gxStringInit(&string, &allocator, 64) == GxStatus::Ok);
    CHECK(gxStringAssign(&string, "abc", 3) == GxStatus::Ok);
    const size_t oldStringLength = gxStringLength(&string);
    const size_t oldStringCapacity = gxStringCapacity(&string);
    gxArenaFailAfter(&arena, 0);
    CHECK(gxStringAppend(&string, "defghijklmnop", 13) == GxStatus::OutOfMemory);
    CHECK(gxStringLength(&string) == oldStringLength);
    CHECK(gxStringCapacity(&string) == oldStringCapacity);
    CHECK(std::memcmp(gxStringData(&string), "abc", 3) == 0);
    gxArenaDisableFailure(&arena);
    gxStringDestroy(&string);
    CHECK(arena.outstanding == 0);

    GxArray firstFailure = {};
    CHECK(gxArrayInit(&firstFailure, &allocator, 1, 8) == GxStatus::Ok);
    gxArenaFailAfter(&arena, 0);
    CHECK(gxArrayAppend(&firstFailure, &fifth, 1) == GxStatus::OutOfMemory);
    CHECK(gxArraySize(&firstFailure) == 0);
    CHECK(gxArrayCapacity(&firstFailure) == 0);
    gxArenaDisableFailure(&arena);
    gxArrayDestroy(&firstFailure);
    CHECK(arena.outstanding == 0);
}

struct PartialBundle {
    GxAllocator* allocator;
    void* first;
    void* second;
    void* third;
    size_t* cleanupCount;
};

void destroyPartialBundle(void*, void* pointer) {
    PartialBundle* bundle = static_cast<PartialBundle*>(pointer);
    gxAllocatorFree(bundle->allocator, bundle->third);
    gxAllocatorFree(bundle->allocator, bundle->second);
    gxAllocatorFree(bundle->allocator, bundle->first);
    ++*bundle->cleanupCount;
    bundle->first = bundle->second = bundle->third = 0;
}

void testOwnerPartialCleanup() {
    GxArena arena;
    gxArenaInit(&arena, 4096);
    GxAllocator allocator = {};
    gxArenaMakeAllocator(&arena, &allocator);
    size_t cleanupCount = 0;
    PartialBundle bundle = {&allocator, 0, 0, 0, &cleanupCount};
    GxOwner owner = {};
    GxOwner moved = {};
    CHECK(gxOwnerInit(&owner) == GxStatus::Ok);
    CHECK(gxOwnerInit(&moved) == GxStatus::Ok);
    CHECK(gxOwnerAdopt(&owner, &bundle, 0, destroyPartialBundle) == GxStatus::Ok);

    gxArenaFailAfter(&arena, 2);
    CHECK(gxAllocatorAllocate(&allocator, 32, 8, &bundle.first) == GxStatus::Ok);
    CHECK(gxAllocatorAllocate(&allocator, 48, 8, &bundle.second) == GxStatus::Ok);
    CHECK(gxAllocatorAllocate(&allocator, 64, 8, &bundle.third) == GxStatus::OutOfMemory);
    CHECK(bundle.third == 0);
    CHECK(gxOwnerGet(&owner) == &bundle);
    gxOwnerReset(&owner);
    CHECK(cleanupCount == 1);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    gxArenaDisableFailure(&arena);

    CHECK(gxOwnerAdopt(&owner, &bundle, 0, destroyPartialBundle) == GxStatus::Ok);
    CHECK(gxOwnerMove(&moved, &owner) == GxStatus::Ok);
    CHECK(gxOwnerGet(&owner) == 0);
    CHECK(gxOwnerGet(&moved) == &bundle);
    CHECK(gxOwnerMove(&moved, &moved) == GxStatus::Ok);
    gxOwnerDestroy(&moved);
    gxOwnerDestroy(&moved);
    gxOwnerDestroy(&owner);
    CHECK(cleanupCount == 2);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(gxOwnerInit(&owner) == GxStatus::Ok);
    gxOwnerDestroy(&owner);
}

void testArrayEdgesAndReferenceModel() {
    GxArena arena;
    gxArenaInit(&arena, 8192);
    GxAllocator allocator = {};
    gxArenaMakeAllocator(&arena, &allocator);

    GxArray overflow = {};
    CHECK(gxArrayInit(&overflow, &allocator, 2,
                      static_cast<size_t>(-1) / 2 + 1) == GxStatus::Overflow);
    CHECK(!overflow.initialized);
    CHECK(gxArrayInit(&overflow, &allocator, 1, static_cast<size_t>(-1)) ==
          GxStatus::Ok);
    CHECK(gxArrayReserve(&overflow, static_cast<size_t>(-1)) == GxStatus::Overflow);
    CHECK(gxArraySize(&overflow) == 0);
    CHECK(gxArrayCapacity(&overflow) == 0);
    gxArrayDestroy(&overflow);

    GxArray limited = {};
    CHECK(gxArrayInit(&limited, &allocator, 1, 5) == GxStatus::Ok);
    const uint8_t three[] = {3, 2, 1};
    CHECK(gxArrayAppend(&limited, three, 3) == GxStatus::Ok);
    CHECK(gxArrayInsert(&limited, 1, gxArrayAt(&limited, 0), 2) == GxStatus::Ok);
    const std::vector<uint8_t> inserted = {3, 3, 2, 2, 1};
    CHECK(arrayEquals(limited, inserted));
    const uint8_t extra = 0;
    CHECK(gxArrayAppend(&limited, &extra, 1) == GxStatus::ResourceLimit);
    CHECK(gxArraySize(&limited) == 5);
    gxArrayDestroy(&limited);
    CHECK(arena.outstanding == 0);

    GxArray array = {};
    CHECK(gxArrayInit(&array, &allocator, 1, 256) == GxStatus::Ok);
    std::vector<uint8_t> reference;
    uint32_t random = 0x8d12a4c3u;
    for (size_t step = 0; step < 2500; ++step) {
        const uint32_t op = nextRandom(random) % 6;
        if (op == 0 || reference.empty()) {
            const size_t count = 1 + nextRandom(random) % 5;
            if (reference.size() + count <= 256) {
                uint8_t bytes[5];
                for (size_t i = 0; i < count; ++i) bytes[i] =
                    static_cast<uint8_t>(nextRandom(random));
                CHECK(gxArrayAppend(&array, bytes, count) == GxStatus::Ok);
                reference.insert(reference.end(), bytes, bytes + count);
            }
        } else if (op == 1) {
            const size_t count = 1 + nextRandom(random) % 5;
            if (reference.size() + count <= 256) {
                const size_t at = nextRandom(random) % (reference.size() + 1);
                uint8_t bytes[5];
                for (size_t i = 0; i < count; ++i) bytes[i] =
                    static_cast<uint8_t>(nextRandom(random));
                CHECK(gxArrayInsert(&array, at, bytes, count) == GxStatus::Ok);
                reference.insert(reference.begin() + at, bytes, bytes + count);
            }
        } else if (op == 2) {
            const size_t at = nextRandom(random) % reference.size();
            const size_t count = nextRandom(random) % (reference.size() - at + 1);
            CHECK(gxArrayErase(&array, at, count) == GxStatus::Ok);
            reference.erase(reference.begin() + at, reference.begin() + at + count);
        } else if (op == 3) {
            CHECK(gxArrayReserve(&array, nextRandom(random) % 257) == GxStatus::Ok);
        } else if (op == 4) {
            CHECK(gxArrayClear(&array) == GxStatus::Ok);
            reference.clear();
        } else if (reference.size() < 128) {
            const size_t count = reference.empty() ? 0 : nextRandom(random) %
                                                           (reference.size() + 1);
            const size_t start = reference.empty() ? 0 : nextRandom(random) %
                                                              (reference.size() - count + 1);
            const size_t oldSize = reference.size();
            CHECK(gxArrayAppend(&array, count ? gxArrayAt(&array, start) : 0, count) ==
                  GxStatus::Ok);
            if (count) {
                std::vector<uint8_t> snapshot(reference.begin() + start,
                                              reference.begin() + start + count);
                reference.insert(reference.end(), snapshot.begin(), snapshot.end());
            }
            CHECK(gxArraySize(&array) == oldSize + count);
        }
        CHECK(arrayEquals(array, reference));
        CHECK(gxArrayAt(&array, reference.size()) == 0);
    }
    gxArrayDestroy(&array);
    gxArrayDestroy(&array);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(gxArrayInit(&array, &allocator, 1, 8) == GxStatus::Ok);
    gxArrayDestroy(&array);
}

void testStringEdgesAndReferenceModel() {
    GxArena arena;
    gxArenaInit(&arena, 16384);
    GxAllocator allocator = {};
    gxArenaMakeAllocator(&arena, &allocator);

    GxString oversized = {};
    CHECK(gxStringInit(&oversized, &allocator, 3) == GxStatus::Ok);
    CHECK(gxStringAppend(&oversized, "abc", 3) == GxStatus::Ok);
    CHECK(gxStringLength(&oversized) == 3);
    CHECK(gxStringAppend(&oversized, "d", 1) == GxStatus::ResourceLimit);
    CHECK(gxStringLength(&oversized) == 3);
    gxStringDestroy(&oversized);

    GxString overflow = {};
    CHECK(gxStringInit(&overflow, &allocator, static_cast<size_t>(-1)) == GxStatus::Ok);
    CHECK(gxStringReserve(&overflow, static_cast<size_t>(-1)) == GxStatus::Overflow);
    CHECK(gxStringLength(&overflow) == 0);
    CHECK(gxStringCapacity(&overflow) == 0);
    gxStringDestroy(&overflow);

    GxString string = {};
    GxString copy = {};
    CHECK(gxStringInit(&string, &allocator, 8192) == GxStatus::Ok);
    CHECK(gxStringInit(&copy, &allocator, 8192) == GxStatus::Ok);
    CHECK(gxStringCopy(&string, &string) == GxStatus::Ok);
    std::string reference;
    uint32_t random = 0x1a77beef;
    for (size_t step = 0; step < 2200; ++step) {
        const uint32_t op = nextRandom(random) % 5;
        if (op <= 1) {
            const size_t count = 1 + nextRandom(random) % 7;
            if (reference.size() + count <= 8192) {
                uint8_t bytes[7];
                for (size_t i = 0; i < count; ++i) bytes[i] =
                    static_cast<uint8_t>(nextRandom(random));
                CHECK(gxStringAppend(&string, bytes, count) == GxStatus::Ok);
                reference.append(reinterpret_cast<const char*>(bytes), count);
            }
        } else if (op == 2) {
            const size_t count = reference.empty() ? 0 : nextRandom(random) %
                                                           (reference.size() + 1);
            const size_t start = reference.empty() ? 0 : nextRandom(random) %
                                                              (reference.size() - count + 1);
            std::string snapshot = reference.substr(start, count);
            CHECK(gxStringAppend(&string, count ? gxStringData(&string) + start : 0,
                                 count) == GxStatus::Ok);
            reference.append(snapshot);
        } else if (op == 3) {
            const size_t count = reference.empty() ? 0 : nextRandom(random) %
                                                           (reference.size() + 1);
            const size_t start = reference.empty() ? 0 : nextRandom(random) %
                                                              (reference.size() - count + 1);
            std::string snapshot = reference.substr(start, count);
            CHECK(gxStringAssign(&string, count ? gxStringData(&string) + start : 0,
                                 count) == GxStatus::Ok);
            reference = snapshot;
        } else if (op == 4 && reference.size() > 5000) {
            CHECK(gxStringClear(&string) == GxStatus::Ok);
            reference.clear();
        }
        CHECK(stringEquals(string, reference));
        CHECK(gxStringCopy(&copy, &string) == GxStatus::Ok);
        CHECK(stringEquals(copy, reference));
        int comparison = 99;
        CHECK(gxStringCompare(&string, &copy, &comparison) == GxStatus::Ok);
        CHECK(comparison == 0);
    }

    const uint8_t embedded[] = {'a', 0, 'b'};
    CHECK(gxStringAssign(&string, embedded, sizeof(embedded)) == GxStatus::Ok);
    CHECK(gxStringLength(&string) == sizeof(embedded));
    CHECK(std::memcmp(gxStringData(&string), embedded, sizeof(embedded)) == 0);
    const char* cString = 0;
    CHECK(gxStringCString(&string, &cString) == GxStatus::InvalidArgument);
    CHECK(cString == 0);
    CHECK(gxStringClear(&string) == GxStatus::Ok);
    CHECK(gxStringCString(&string, &cString) == GxStatus::Ok);
    CHECK(std::strcmp(cString, "") == 0);

    const uint8_t highBytes[] = {0x7f, 0x80};
    CHECK(gxStringAssign(&string, highBytes, sizeof(highBytes)) == GxStatus::Ok);
    CHECK(gxStringAssign(&copy, "z", 1) == GxStatus::Ok);
    int comparison = 0;
    CHECK(gxStringCompare(&string, &copy, &comparison) == GxStatus::Ok);
    CHECK(comparison > 0);

    gxStringDestroy(&string);
    gxStringDestroy(&copy);
    gxStringDestroy(&string);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(gxStringInit(&string, &allocator, 8) == GxStatus::Ok);
    gxStringDestroy(&string);
}

void testHostedAdapterAndStatusMappingSurface() {
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 1024 * 1024) == GxStatus::Ok);
    GxString string = {};
    CHECK(gxStringInit(&string, &allocator, 64) == GxStatus::Ok);
    CHECK(gxStringAssign(&string, "hosted", 6) == GxStatus::Ok);
    CHECK(gxStringLength(&string) == 6);
    gxStringDestroy(&string);
    CHECK(gxAllocatorAllocate(&allocator, 8, 8, 0) == GxStatus::InvalidArgument);
}

} // namespace

int main() {
    testAllocatorContract();
    testAllocationFailure();
    testOwnerPartialCleanup();
    testArrayEdgesAndReferenceModel();
    testStringEdgesAndReferenceModel();
    testHostedAdapterAndStatusMappingSurface();
    std::printf("database_portable_runtime_test: %d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
