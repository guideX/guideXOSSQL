#include "database_format.h"
#include "database_index.h"
#include "database_page.h"
#include "database_relational.h"
#include "database_sql.h"
#include "database_transaction.h"
#include "gx_page_codec.h"
#include "gx_transaction_page_index.h"
#include "gx_transaction_page_overlay.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace gxos::db;

namespace {

int gChecks = 0;
int gFailures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++gChecks;                                                              \
        if (!(condition)) {                                                     \
            ++gFailures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         #condition);                                           \
        }                                                                       \
    } while (0)

#define CHECK_STATUS(expression, expected)                                     \
    do {                                                                        \
        const DbResult p8Result = (expression);                                 \
        ++gChecks;                                                              \
        if (p8Result.status() != (expected)) {                                  \
            ++gFailures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s returned %s, expected %s\n", \
                         __FILE__, __LINE__, #expression,                      \
                         dbStatusName(p8Result.status()),                      \
                         dbStatusName(expected));                              \
        }                                                                       \
    } while (0)

struct FaultAllocatorState {
    size_t attempts;
    size_t successes;
    size_t frees;
    size_t outstanding;
    size_t failAtSuccess;
    bool failureEnabled;

    FaultAllocatorState()
        : attempts(0), successes(0), frees(0), outstanding(0),
          failAtSuccess(0), failureEnabled(false) {}
};

bool faultAllocate(void* context, size_t size, size_t alignment,
                   void** outPointer) {
    FaultAllocatorState* state = static_cast<FaultAllocatorState*>(context);
    ++state->attempts;
    *outPointer = 0;
    if (state->failureEnabled && state->successes >= state->failAtSuccess) {
        return false;
    }
    if (alignment > 8) return false;
    void* pointer = std::malloc(size);
    if (!pointer) return false;
    ++state->successes;
    ++state->outstanding;
    *outPointer = pointer;
    return true;
}

void faultFree(void* context, void* pointer) {
    FaultAllocatorState* state = static_cast<FaultAllocatorState*>(context);
    if (!pointer) return;
    if (state->outstanding == 0) {
        ++gFailures;
        std::fprintf(stderr, "FAIL allocator free with no outstanding block\n");
    } else {
        --state->outstanding;
    }
    ++state->frees;
    std::free(pointer);
}

GxAllocator makeFaultAllocator(FaultAllocatorState* state) {
    GxAllocator allocator = {};
    allocator.context = state;
    allocator.allocate = faultAllocate;
    allocator.deallocate = faultFree;
    allocator.maxAllocation = 1024 * 1024;
    allocator.maxAlignment = 8;
    return allocator;
}

void failAfter(FaultAllocatorState* state, size_t additionalSuccesses) {
    state->failAtSuccess = state->successes + additionalSuccesses;
    state->failureEnabled = true;
}

void disableFailure(FaultAllocatorState* state) {
    state->failureEnabled = false;
}

std::vector<uint8_t> payload(uint8_t value, size_t count = 3) {
    return std::vector<uint8_t>(count, value);
}

bool imageHasPayload(const GxTransactionPageEntry* entry,
                     const std::vector<uint8_t>& expected) {
    if (!entry || !entry->image) return false;
    GxDatabasePageInfo info = {};
    if (gxDatabasePageInspect(entry->image, 512, 512, entry->pageId, &info) !=
            GxStatus::Ok ||
        info.payloadSize != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (entry->image[kPageHeaderSize + i] != expected[i]) return false;
    }
    return true;
}

void testIndexCapacityCollisionsAndFailure() {
    std::printf("P8 portable bounded page index and collision handling\n");
    FaultAllocatorState state;
    GxAllocator allocator = makeFaultAllocator(&state);
    size_t capacity = 0;
    size_t bytes = 0;
    CHECK(gxTransactionPageIndexStorageSize(4096, &capacity, &bytes) ==
          GxStatus::Ok);
    CHECK(capacity == 8192);
    CHECK(bytes == capacity * sizeof(GxTransactionPageIndexSlot));
    CHECK(gxTransactionPageIndexStorageSize(0, &capacity, &bytes) ==
          GxStatus::InvalidArgument);
    CHECK(gxTransactionPageIndexStorageSize(static_cast<size_t>(-1),
                                             &capacity, &bytes) ==
          GxStatus::Overflow);

    GxTransactionPageIndex index = {};
    failAfter(&state, 0);
    CHECK(gxTransactionPageIndexInit(&index, &allocator, 3) ==
          GxStatus::OutOfMemory);
    CHECK(!index.initialized && state.outstanding == 0);
    disableFailure(&state);
    CHECK(gxTransactionPageIndexInit(&index, &allocator, 3) == GxStatus::Ok);
    CHECK(index.capacity == 8 && index.count == 0);

    bool found = true;
    size_t owner = 99;
    CHECK(gxTransactionPageIndexLookup(&index, 0, &found, &owner) ==
          GxStatus::InvalidArgument);
    CHECK(!found && owner == 0);
    CHECK(gxTransactionPageIndexLookup(&index, 7, &found, &owner) ==
          GxStatus::Ok);
    CHECK(!found);
    CHECK(gxTransactionPageIndexInsert(&index, 8, 3) ==
          GxStatus::InvalidArgument);
    CHECK(index.count == 0);

    const size_t attemptsBeforeInsert = state.attempts;
    CHECK(gxTransactionPageIndexInsert(&index, 7, 0) == GxStatus::Ok);
    uint64_t collisionKey = 0;
    for (uint64_t candidate = 8; candidate < 100000 && collisionKey == 0;
         ++candidate) {
        const uint64_t probesBefore = index.stats.lookupProbes;
        CHECK(gxTransactionPageIndexLookup(&index, candidate, &found, &owner) ==
              GxStatus::Ok);
        if (!found && index.stats.lookupProbes - probesBefore > 1) {
            collisionKey = candidate;
        }
    }
    CHECK(collisionKey != 0);
    CHECK(gxTransactionPageIndexInsert(&index, collisionKey, 1) ==
          GxStatus::Ok);
    CHECK(index.stats.collisions > 0);
    CHECK(gxTransactionPageIndexLookup(&index, collisionKey, &found, &owner) ==
          GxStatus::Ok);
    CHECK(found && owner == 1);
    CHECK(gxTransactionPageIndexLookup(&index, 7, &found, &owner) ==
          GxStatus::Ok);
    CHECK(found && owner == 0);
    CHECK(gxTransactionPageIndexLookup(&index, collisionKey + 1000000,
                                       &found, &owner) == GxStatus::Ok);
    CHECK(!found);
    CHECK(gxTransactionPageIndexInsert(&index, 7, 44) ==
          GxStatus::InvalidArgument);
    CHECK(gxTransactionPageIndexInsert(&index, 99, 2) == GxStatus::Ok);
    CHECK(gxTransactionPageIndexInsert(&index, 100, 0) ==
          GxStatus::ResourceLimit);
    CHECK(index.count == 3 && state.attempts == attemptsBeforeInsert);

    const size_t outstandingBeforeClone = state.outstanding;
    GxTransactionPageIndex failedClone = {};
    failAfter(&state, 0);
    CHECK(gxTransactionPageIndexClone(&failedClone, &index) ==
          GxStatus::OutOfMemory);
    disableFailure(&state);
    CHECK(!failedClone.initialized && state.outstanding == outstandingBeforeClone);
    GxTransactionPageIndex clone = {};
    CHECK(gxTransactionPageIndexClone(&clone, &index) == GxStatus::Ok);
    CHECK(clone.count == index.count && clone.capacity == index.capacity);
    GxTransactionPageIndex moved = {};
    CHECK(gxTransactionPageIndexMove(&moved, &clone) == GxStatus::Ok);
    CHECK(!clone.initialized && moved.count == 3);
    gxTransactionPageIndexClear(&moved);
    CHECK(moved.count == 0);
    CHECK(gxTransactionPageIndexLookup(&moved, 7, &found, &owner) ==
          GxStatus::Ok);
    CHECK(!found);
    gxTransactionPageIndexDestroy(&moved);
    gxTransactionPageIndexDestroy(&clone);
    gxTransactionPageIndexDestroy(&failedClone);
    gxTransactionPageIndexDestroy(&index);
    CHECK(state.outstanding == 0);
    CHECK(gxTransactionPageIndexInit(&index, &allocator, 1) == GxStatus::Ok);
    CHECK(index.capacity == 2);
    CHECK(gxTransactionPageIndexInsert(&index, kMaxPageCount - 1, 0) ==
          GxStatus::Ok);
    CHECK(gxTransactionPageIndexLookup(&index, kMaxPageCount - 1,
                                       &found, &owner) == GxStatus::Ok);
    CHECK(found && owner == 0);
    gxTransactionPageIndexDestroy(&index);
    gxTransactionPageIndexDestroy(&index);
    CHECK(state.outstanding == 0 && state.frees == state.successes);
}

void testNearLimitLookupDiagnostics() {
    std::printf("P8 near-limit lookup and allocation diagnostics\n");
    FaultAllocatorState state;
    GxAllocator allocator = makeFaultAllocator(&state);
    GxTransactionPageIndex index = {};
    CHECK(gxTransactionPageIndexInit(&index, &allocator, 4096) == GxStatus::Ok);
    CHECK(index.capacity == 8192 && index.allocationBytes == 8192 *
          sizeof(GxTransactionPageIndexSlot));

    const auto insertStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < 4096; ++i) {
        const uint64_t pageId = static_cast<uint64_t>(i) * 1000003u + 1u;
        CHECK(gxTransactionPageIndexInsert(&index, pageId, i) == GxStatus::Ok);
    }
    const auto insertEnd = std::chrono::steady_clock::now();
    CHECK(index.count == 4096);
    CHECK(gxTransactionPageIndexInsert(&index, 5000, 0) ==
          GxStatus::ResourceLimit);

    bool found = false;
    size_t owner = 0;
    const uint64_t beforeSuccess = index.stats.lookupProbes;
    const auto successStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < 4096; ++i) {
        const uint64_t pageId = static_cast<uint64_t>(i) * 1000003u + 1u;
        CHECK(gxTransactionPageIndexLookup(&index, pageId, &found, &owner) ==
              GxStatus::Ok);
        CHECK(found && owner == i);
    }
    const auto successEnd = std::chrono::steady_clock::now();
    const uint64_t successProbes = index.stats.lookupProbes - beforeSuccess;

    const uint64_t beforeMiss = index.stats.lookupProbes;
    const auto missStart = std::chrono::steady_clock::now();
    for (uint64_t pageId = 100000; pageId < 104096; ++pageId) {
        CHECK(gxTransactionPageIndexLookup(&index, pageId, &found, &owner) ==
              GxStatus::Ok);
        CHECK(!found);
    }
    const auto missEnd = std::chrono::steady_clock::now();
    const uint64_t missProbes = index.stats.lookupProbes - beforeMiss;

    const size_t outstandingBeforeClear = state.outstanding;
    gxTransactionPageIndexClear(&index);
    CHECK(index.count == 0 && state.outstanding == outstandingBeforeClear);
    CHECK(gxTransactionPageIndexLookup(&index, 1, &found, &owner) == GxStatus::Ok);
    CHECK(!found);

    const double insertUs = std::chrono::duration<double, std::micro>(
        insertEnd - insertStart).count();
    const double successUs = std::chrono::duration<double, std::micro>(
        successEnd - successStart).count();
    const double missUs = std::chrono::duration<double, std::micro>(
        missEnd - missStart).count();

    std::map<uint64_t, size_t> hostedReference;
    for (size_t i = 0; i < 4096; ++i) {
        hostedReference[static_cast<uint64_t>(i) * 1000003u + 1u] = i;
    }
    const auto mapHitStart = std::chrono::steady_clock::now();
    size_t mapHits = 0;
    for (size_t i = 0; i < 4096; ++i) {
        mapHits += hostedReference.count(static_cast<uint64_t>(i) * 1000003u + 1u);
    }
    const auto mapHitEnd = std::chrono::steady_clock::now();
    const auto mapMissStart = std::chrono::steady_clock::now();
    size_t mapMisses = 0;
    for (uint64_t pageId = 100000; pageId < 104096; ++pageId) {
        mapMisses += hostedReference.count(pageId);
    }
    const auto mapMissEnd = std::chrono::steady_clock::now();
    CHECK(mapHits == 4096 && mapMisses == 0);
    const double mapHitUs = std::chrono::duration<double, std::micro>(
        mapHitEnd - mapHitStart).count();
    const double mapMissUs = std::chrono::duration<double, std::micro>(
        mapMissEnd - mapMissStart).count();
    std::printf("P8 index diagnostics: cap=%llu bytes=%llu entries=4096 "
                "insert-us=%.1f success-us=%.1f success-probes=%llu "
                "miss-us=%.1f miss-probes=%llu map-hit-us=%.1f map-miss-us=%.1f "
                "collisions=%llu max-probe=%llu\n",
                static_cast<unsigned long long>(index.capacity),
                static_cast<unsigned long long>(index.allocationBytes),
                insertUs, successUs,
                static_cast<unsigned long long>(successProbes), missUs,
                static_cast<unsigned long long>(missProbes), mapHitUs, mapMissUs,
                static_cast<unsigned long long>(index.stats.collisions),
                static_cast<unsigned long long>(index.stats.maximumProbeLength));
    gxTransactionPageIndexDestroy(&index);
    CHECK(state.outstanding == 0 && state.frees == state.successes);
}

void testOverlayOrderReplacementAndSnapshotFailures() {
    std::printf("P8 overlay ordering, replacement, snapshot, and cleanup\n");
    FaultAllocatorState state;
    GxAllocator allocator = makeFaultAllocator(&state);
    GxTransactionPageOverlay overlay = {};
    CHECK(gxTransactionPageOverlayInit(&overlay, &allocator, 512, 8) ==
          GxStatus::Ok);
    CHECK(overlay.pages.count == 0 && overlay.index.count == 0);

    bool replaced = true;
    const std::vector<uint8_t> first = payload(1);
    const std::vector<uint8_t> second = payload(2);
    const std::vector<uint8_t> third = payload(3);
    const std::vector<uint8_t> latest9 = payload(9, 9);
    const std::vector<uint8_t> latest3 = payload(8, 7);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 9, static_cast<uint16_t>(PageType::Data), 0, 0,
              first.data(), first.size(), &replaced) == GxStatus::Ok);
    CHECK(!replaced);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 3, static_cast<uint16_t>(PageType::Data), 0, 0,
              second.data(), second.size(), &replaced) == GxStatus::Ok);
    CHECK(!replaced);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 9, static_cast<uint16_t>(PageType::Data), 1, 2,
              latest9.data(), latest9.size(), &replaced) == GxStatus::Ok);
    CHECK(replaced);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 12, static_cast<uint16_t>(PageType::Data), 0, 0,
              third.data(), third.size(), &replaced) == GxStatus::Ok);
    CHECK(!replaced);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 3, static_cast<uint16_t>(PageType::Data), 1, 4,
              latest3.data(), latest3.size(), &replaced) == GxStatus::Ok);
    CHECK(replaced);
    CHECK(overlay.pages.count == 3 && overlay.index.count == 3);
    CHECK(gxTransactionPageOverlayAt(&overlay, 0)->pageId == 9);
    CHECK(gxTransactionPageOverlayAt(&overlay, 1)->pageId == 3);
    CHECK(gxTransactionPageOverlayAt(&overlay, 2)->pageId == 12);
    CHECK(imageHasPayload(gxTransactionPageOverlayAt(&overlay, 0), latest9));
    CHECK(imageHasPayload(gxTransactionPageOverlayAt(&overlay, 1), latest3));

    const size_t attemptsBeforeFailedReplacement = state.attempts;
    failAfter(&state, 0);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 9, static_cast<uint16_t>(PageType::Data), 2, 9,
              first.data(), first.size(), &replaced) == GxStatus::OutOfMemory);
    disableFailure(&state);
    CHECK(!replaced && state.attempts == attemptsBeforeFailedReplacement + 1);
    CHECK(overlay.pages.count == 3 && overlay.index.count == 3);
    CHECK(imageHasPayload(gxTransactionPageOverlayAt(&overlay, 0), latest9));
    bool found = false;
    size_t owner = 0;
    CHECK(gxTransactionPageOverlayFind(&overlay, 9, &found, &owner) ==
          GxStatus::Ok);
    CHECK(found && owner == 0);
    CHECK(gxTransactionPageOverlayFind(&overlay, 77, &found, &owner) ==
          GxStatus::Ok);
    CHECK(!found);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 0, static_cast<uint16_t>(PageType::Data), 0, 0,
              first.data(), first.size(), &replaced) == GxStatus::InvalidArgument);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, kMaxPageCount,
              static_cast<uint16_t>(PageType::Data), 0, 0,
              first.data(), first.size(), &replaced) == GxStatus::InvalidArgument);

    const size_t attemptsBeforeFailedInsert = state.attempts;
    failAfter(&state, 0);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 77, static_cast<uint16_t>(PageType::Data), 0, 0,
              first.data(), first.size(), &replaced) == GxStatus::OutOfMemory);
    disableFailure(&state);
    CHECK(overlay.pages.count == 3 && overlay.index.count == 3);
    CHECK(state.attempts == attemptsBeforeFailedInsert + 1);
    CHECK(gxTransactionPageOverlayFind(&overlay, 77, &found, &owner) ==
          GxStatus::Ok);
    CHECK(!found);

    const auto replaceStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < 100; ++i) {
        const std::vector<uint8_t> repeated = payload(static_cast<uint8_t>(i),
                                                     8 + (i % 8));
        CHECK(gxTransactionPageOverlayStage(
                  &overlay, 9, static_cast<uint16_t>(PageType::Data), 0, 0,
                  repeated.data(), repeated.size(), &replaced) == GxStatus::Ok);
        CHECK(replaced);
    }
    const auto replaceEnd = std::chrono::steady_clock::now();
    const double replacementUs = std::chrono::duration<double, std::micro>(
        replaceEnd - replaceStart).count();

    const size_t outstandingBeforeClone = state.outstanding;
    GxTransactionPageOverlay failedClone = {};
    failAfter(&state, overlay.pages.count + 1); // Owner clone succeeds; index fails.
    CHECK(gxTransactionPageOverlayClone(&failedClone, &overlay) ==
          GxStatus::OutOfMemory);
    disableFailure(&state);
    CHECK(!failedClone.initialized && state.outstanding == outstandingBeforeClone);
    CHECK(overlay.pages.count == overlay.index.count &&
          imageHasPayload(gxTransactionPageOverlayAt(&overlay, 0),
                          payload(99, 11)));

    GxTransactionPageOverlay savepoint = {};
    const auto snapshotStart = std::chrono::steady_clock::now();
    CHECK(gxTransactionPageOverlayClone(&savepoint, &overlay) == GxStatus::Ok);
    const auto snapshotEnd = std::chrono::steady_clock::now();
    const std::vector<uint8_t> changed = payload(0x55, 13);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 9, static_cast<uint16_t>(PageType::Data), 0, 0,
              changed.data(), changed.size(), &replaced) == GxStatus::Ok);
    const std::vector<uint8_t> extra = payload(0x66);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 15, static_cast<uint16_t>(PageType::Data), 0, 0,
              extra.data(), extra.size(), &replaced) == GxStatus::Ok);
    const size_t outstandingBeforeMove = state.outstanding;
    failAfter(&state, 0);
    const auto restoreStart = std::chrono::steady_clock::now();
    CHECK(gxTransactionPageOverlayMove(&overlay, &savepoint) == GxStatus::Ok);
    const auto restoreEnd = std::chrono::steady_clock::now();
    disableFailure(&state);
    CHECK(state.outstanding < outstandingBeforeMove);
    CHECK(overlay.pages.count == 3 && overlay.index.count == 3);
    CHECK(imageHasPayload(gxTransactionPageOverlayAt(&overlay, 0),
                          payload(99, 11)));
    CHECK(gxTransactionPageOverlayAt(&overlay, 0)->pageId == 9);
    CHECK(gxTransactionPageOverlayFind(&overlay, 15, &found, &owner) ==
          GxStatus::Ok);
    CHECK(!found);
    CHECK(gxTransactionPageOverlayFind(&overlay, 3, &found, &owner) ==
          GxStatus::Ok);
    CHECK(found && owner == 1);

    gxTransactionPageOverlayClear(&overlay);
    CHECK(overlay.pages.count == 0 && overlay.index.count == 0);
    CHECK(gxTransactionPageOverlayFind(&overlay, 9, &found, &owner) ==
          GxStatus::Ok);
    CHECK(!found);
    const std::vector<uint8_t> reused = payload(0x44);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, 21, static_cast<uint16_t>(PageType::Data), 0, 0,
              reused.data(), reused.size(), &replaced) == GxStatus::Ok);
    CHECK(overlay.pages.count == 1 && overlay.index.count == 1);
    const std::vector<uint8_t> maxPagePayload = payload(0x7f);
    CHECK(gxTransactionPageOverlayStage(
              &overlay, kMaxPageCount - 1,
              static_cast<uint16_t>(PageType::Data), 0, 0,
              maxPagePayload.data(), maxPagePayload.size(), &replaced) ==
          GxStatus::Ok);
    CHECK(overlay.pages.count == 2 && overlay.index.count == 2);
    const double snapshotUs = std::chrono::duration<double, std::micro>(
        snapshotEnd - snapshotStart).count();
    const double restoreUs = std::chrono::duration<double, std::micro>(
        restoreEnd - restoreStart).count();
    std::printf("P8 overlay diagnostics: 100 replacements=%.1f us "
                "snapshot-clone=%.1f us restore-move=%.1f us "
                "index-bytes=%llu owner-descriptors=%llu\n",
                replacementUs, snapshotUs, restoreUs,
                static_cast<unsigned long long>(overlay.index.allocationBytes),
                static_cast<unsigned long long>(overlay.pages.capacity *
                    sizeof(GxTransactionPageEntry)));
    gxTransactionPageOverlayDestroy(&overlay);
    gxTransactionPageOverlayDestroy(&savepoint);
    gxTransactionPageOverlayDestroy(&failedClone);
    CHECK(state.outstanding == 0 && state.frees == state.successes);
}

std::string uniquePath(const char* prefix) {
    const uint64_t stamp = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    char name[128];
    std::snprintf(name, sizeof(name), "%s_%llu.gxdb", prefix,
                  static_cast<unsigned long long>(stamp));
    return name;
}

std::string walPath(const std::string& path) { return walPathFor(path); }

void removeDatabaseFiles(const std::string& path) {
    std::remove(path.c_str());
    std::remove(walPath(path).c_str());
}

bool samePage(const DatabasePage& left, const DatabasePage& right) {
    return left.pageId == right.pageId && left.type == right.type &&
           left.flags == right.flags && left.generation == right.generation &&
           left.payload == right.payload;
}

struct WalFailureState {
    uint64_t budget;
    uint64_t written;
    bool exhausted;

    WalFailureState() : budget(UINT64_MAX), written(0), exhausted(false) {}
};

class WalBudgetFile : public IDatabaseFile {
public:
    explicit WalBudgetFile(const std::shared_ptr<WalFailureState>& state)
        : _state(state), _inner(new HostDatabaseFile()) {}

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override {
        return _inner->open(path, mode, truncateExisting);
    }
    DbResult close() override { return _inner->close(); }
    bool isOpen() const override { return _inner->isOpen(); }
    bool isReadOnly() const override { return _inner->isReadOnly(); }
    DbResult getSize(uint64_t& outSize) override {
        return _inner->getSize(outSize);
    }
    bool readAt(uint64_t offset, void* buffer, size_t length) override {
        return _inner->readAt(offset, buffer, length);
    }
    bool writeAt(uint64_t offset, const void* buffer, size_t length) override {
        const uint64_t remaining = _state->written < _state->budget
            ? _state->budget - _state->written : 0;
        if (static_cast<uint64_t>(length) > remaining) {
            const size_t allowed = static_cast<size_t>(remaining);
            if (allowed > 0 && !_inner->writeAt(offset, buffer, allowed)) return false;
            _state->written += allowed;
            _state->exhausted = true;
            return false;
        }
        if (!_inner->writeAt(offset, buffer, length)) return false;
        _state->written += length;
        return true;
    }
    bool truncateTo(uint64_t size) override { return _inner->truncateTo(size); }
    bool flush() override { return _inner->flush(); }
    const std::string& path() const override { return _inner->path(); }

private:
    std::shared_ptr<WalFailureState> _state;
    std::unique_ptr<HostDatabaseFile> _inner;
};

class WalBudgetFileSystem : public IDatabaseFileSystem {
public:
    explicit WalBudgetFileSystem(const std::shared_ptr<WalFailureState>& state)
        : _state(state) {}
    std::unique_ptr<IDatabaseFile> createFile() override {
        return std::unique_ptr<IDatabaseFile>(new WalBudgetFile(_state));
    }
    DbResult exists(const std::string& path, bool& outExists) override {
        HostFileSystem host;
        return host.exists(path, outExists);
    }
    DbResult remove(const std::string& path) override {
        HostFileSystem host;
        return host.remove(path);
    }

private:
    std::shared_ptr<WalFailureState> _state;
};

void testProductionIndexAllocationAndSavepointRestore() {
    std::printf("P8 production transaction index allocation and savepoint restore\n");
    const std::string path = uniquePath("p8_faults");
    removeDatabaseFiles(path);
    FaultAllocatorState state;
    GxAllocator allocator = makeFaultAllocator(&state);
    DatabaseCreateOptions options;
    options.pageSize = 512;
    options.bufferCapacity = 2;
    options.transactionPageAllocator = &allocator;
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, options, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    std::unique_ptr<Transaction> tx;
    failAfter(&state, 1); // Owner descriptors allocate, fixed index allocation fails.
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::NoSpace);
    disableFailure(&state);
    CHECK(tx == nullptr && state.outstanding == 0);
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    CHECK(tx && tx->pageIndexCapacity() == 8192);

    DatabasePage original;
    CHECK_STATUS(tx->readPage(1, original), DbStatus::Ok);
    DatabasePage changed = original;
    changed.payload[0] ^= 0x31;
    CHECK_STATUS(tx->writePage(changed), DbStatus::Ok);
    uint64_t allocated = 0;
    CHECK_STATUS(tx->allocatePage(PageType::Data, allocated), DbStatus::Ok);
    DatabasePage newPage;
    CHECK_STATUS(tx->readPage(allocated, newPage), DbStatus::Ok);
    newPage.payload.assign(20, 0x5c);
    newPage.payloadSize = static_cast<uint32_t>(newPage.payload.size());
    CHECK_STATUS(tx->writePage(newPage), DbStatus::Ok);
    CHECK(tx->modifiedPageCount() == 2);
    CHECK(tx->pageIndexStats().lookupHits > 0);

    const size_t outstandingBeforeSnapshot = state.outstanding;
    TransactionSavepoint failedSnapshot;
    failAfter(&state, tx->modifiedPageCount() + 1);
    CHECK_STATUS(tx->beginStatement(failedSnapshot), DbStatus::NoSpace);
    disableFailure(&state);
    CHECK(state.outstanding == outstandingBeforeSnapshot);
    CHECK(tx->modifiedPageCount() == 2);
    DatabasePage overlayPage;
    CHECK_STATUS(tx->readPage(1, overlayPage), DbStatus::Ok);
    CHECK(samePage(overlayPage, changed));

    TransactionSavepoint savepoint;
    CHECK_STATUS(tx->beginStatement(savepoint), DbStatus::Ok);
    DatabasePage replacement = changed;
    replacement.payload[1] ^= 0x62;
    CHECK_STATUS(tx->writePage(replacement), DbStatus::Ok);
    uint64_t secondAllocated = 0;
    CHECK_STATUS(tx->allocatePage(PageType::Data, secondAllocated), DbStatus::Ok);
    const size_t outstandingBeforeRestore = state.outstanding;
    failAfter(&state, 0);
    tx->rollbackStatement(savepoint);
    disableFailure(&state);
    CHECK(state.outstanding < outstandingBeforeRestore);
    CHECK(tx->modifiedPageCount() == 2);
    CHECK_STATUS(tx->readPage(1, overlayPage), DbStatus::Ok);
    CHECK(samePage(overlayPage, changed));
    DatabasePage absent;
    CHECK_STATUS(tx->readPage(secondAllocated, absent), DbStatus::OutOfBounds);
    CHECK(tx->pageIndexStats().lookupHits > 0);

    DatabasePage committed;
    CHECK_STATUS(db->readPage(1, committed), DbStatus::Ok);
    CHECK(samePage(committed, original));
    CHECK_STATUS(tx->rollback(), DbStatus::Ok);
    CHECK(tx->modifiedPageCount() == 0);
    CHECK(tx->pageIndexStats().lookupHits > 0);
    CHECK_STATUS(db->readPage(1, committed), DbStatus::Ok);
    CHECK(samePage(committed, original));
    tx.reset();
    CHECK(state.outstanding == 0 && state.frees == state.successes);
    CHECK_STATUS(db->close(), DbStatus::Ok);
    db.reset();
    removeDatabaseFiles(path);
}

void testCollisionOverlayBeforeWalFailureRecovery() {
    std::printf("P8 collision-heavy transaction before WAL failure/recovery\n");
    const std::string path = uniquePath("p8_collision_crash");
    removeDatabaseFiles(path);
    std::unique_ptr<Database> created;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), created),
                 DbStatus::Ok);
    CHECK(created != nullptr);
    if (!created) return;
    const uint64_t originalPageCount = created->pageCount();
    CHECK_STATUS(created->close(), DbStatus::Ok);
    created.reset();

    std::shared_ptr<WalFailureState> state(new WalFailureState());
    WalBudgetFileSystem filesystem(state);
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db, &filesystem),
                 DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) {
        removeDatabaseFiles(path);
        return;
    }
    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    if (!tx) {
        db->close();
        removeDatabaseFiles(path);
        return;
    }

    for (size_t i = 0; i < 4096; ++i) {
        uint64_t pageId = 0;
        CHECK_STATUS(tx->allocatePage(PageType::Data, pageId), DbStatus::Ok);
    }
    CHECK(tx->modifiedPageCount() == 4096);
    CHECK(tx->pageIndexCapacity() == 8192);
    CHECK(tx->pageIndexStats().collisions > 0);
    const uint64_t collisionsBeforeWalFailure =
        tx->pageIndexStats().collisions;
    CHECK(tx->pageIndexAllocationBytes() == 8192 *
          sizeof(GxTransactionPageIndexSlot));

    // Preserve BEGIN and cut a PAGE_IMAGE record before the durable COMMIT.
    state->budget = state->written + 128;
    const DbResult commit = tx->commit();
    CHECK(commit.status() == DbStatus::CommitFailed);
    CHECK(state->exhausted);
    CHECK(!tx->isActive());
    tx.reset();
    const DbResult closeAfterFailure = db->close();
    CHECK(closeAfterFailure.status() == DbStatus::IoError);
    db.reset();

    std::unique_ptr<Database> recovered;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), recovered),
                 DbStatus::Ok);
    CHECK(recovered != nullptr);
    if (recovered) {
        CHECK(recovered->pageCount() == originalPageCount);
        std::string integrity;
        CHECK_STATUS(recovered->validateIntegrity(integrity), DbStatus::Ok);
        CHECK_STATUS(recovered->close(), DbStatus::Ok);
    }
    std::printf("P8 collision crash diagnostic: entries=4096 capacity=8192 "
                "collisions=%llu bytes-written=%llu failure=%s\n",
                static_cast<unsigned long long>(collisionsBeforeWalFailure),
                static_cast<unsigned long long>(state->written),
                state->exhausted ? "injected" : "missing");
    removeDatabaseFiles(path);
}

SqlExecutionResult runSql(SqlEngine& engine, const std::string& sql,
                          bool expected = true) {
    SqlExecutionResult result = engine.execute(sql);
    ++gChecks;
    if (result.ok != expected) {
        ++gFailures;
        std::fprintf(stderr, "FAIL SQL `%s`: expected %s, got %s\n",
                     sql.c_str(), expected ? "success" : "failure",
                     result.ok ? "success" : result.error.message.c_str());
    }
    return result;
}

void testSql9ProductionPreparedWorkload() {
    std::printf("P8 SQL9 prepared DML, indexes, FK, rollback, commit, reopen\n");
    const std::string path = uniquePath("p8_sql9");
    removeDatabaseFiles(path);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    createOptions.bufferCapacity = 3;
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    uint64_t lookupHitsBeforeReads = 0;
    double repeatedSelectUs = 0;
    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE Parents (Id INT64 PRIMARY KEY, Name TEXT NOT NULL);").ok);
        CHECK(runSql(engine,
                     "CREATE TABLE Items (Id INT64 PRIMARY KEY, ParentId INT64 NOT NULL, "
                     "Tag TEXT NOT NULL, Payload TEXT NOT NULL, FOREIGN KEY (ParentId) "
                     "REFERENCES Parents (Id));").ok);
        CHECK(runSql(engine,
                     "CREATE INDEX IX_Items_Tag ON Items (Tag);").ok);

        SqlPreparedStatement insertParent;
        SqlPreparedStatement insertItem;
        CHECK(engine.prepare("INSERT INTO Parents VALUES (?, ?);",
                             insertParent).isOk());
        CHECK(engine.prepare("INSERT INTO Items VALUES (?, ?, ?, ?);",
                             insertItem).isOk());
        CHECK(runSql(engine, "BEGIN;").ok);
        for (int64_t id = 1; id <= 32; ++id) {
            CHECK(insertParent.bindInt64(0, id).isOk());
            CHECK(insertParent.bindText(1, "parent-" + std::to_string(id)).isOk());
            CHECK(insertParent.execute().ok);
            CHECK(insertItem.bindInt64(0, id).isOk());
            CHECK(insertItem.bindInt64(1, id).isOk());
            CHECK(insertItem.bindText(2, "tag-" + std::to_string(id % 4)).isOk());
            CHECK(insertItem.bindText(3, "payload-" + std::to_string(id) +
                                         std::string(32 + id, 'x')).isOk());
            CHECK(insertItem.execute().ok);
        }
        Transaction* active = db->activeTransaction();
        CHECK(active != nullptr);
        if (active) {
            CHECK(active->pageIndexCapacity() == 8192);
            CHECK(active->pageIndexAllocationBytes() == 8192 *
                  sizeof(GxTransactionPageIndexSlot));
            CHECK(active->modifiedPageCount() > 3);
            CHECK(active->pageIndexStats().lookupHits > 0);
            CHECK(active->pageIndexStats().lookupMisses > 0);
            std::printf("P8 SQL overlay diagnostics: staged=%u index-bytes=%llu "
                        "hits=%llu misses=%llu probes=%llu collisions=%llu\n",
                        active->modifiedPageCount(),
                        static_cast<unsigned long long>(active->pageIndexAllocationBytes()),
                        static_cast<unsigned long long>(active->pageIndexStats().lookupHits),
                        static_cast<unsigned long long>(active->pageIndexStats().lookupMisses),
                        static_cast<unsigned long long>(active->pageIndexStats().lookupProbes),
                        static_cast<unsigned long long>(active->pageIndexStats().collisions));
        }

        SqlPreparedStatement selectItem;
        CHECK(engine.prepare("SELECT Id, Payload FROM Items WHERE Id = ?;",
                             selectItem).isOk());
        const auto selectStart = std::chrono::steady_clock::now();
        for (int64_t n = 0; n < 100; ++n) {
            CHECK(selectItem.bindInt64(0, (n % 32) + 1).isOk());
            SqlStatementResult result = selectItem.executeQuery();
            CHECK(result.ok && result.resultSet.rowCount() == 1);
        }
        const auto selectEnd = std::chrono::steady_clock::now();
        repeatedSelectUs = std::chrono::duration<double, std::micro>(
            selectEnd - selectStart).count();
        active = db->activeTransaction();
        if (active) {
            lookupHitsBeforeReads = active->pageIndexStats().lookupHits;
            CHECK(lookupHitsBeforeReads > 100);
        }

        SqlPreparedStatement updatePayload;
        SqlPreparedStatement invalidForeignKey;
        SqlPreparedStatement deleteItem;
        CHECK(engine.prepare("UPDATE Items SET Payload = ? WHERE Id = ?;",
                             updatePayload).isOk());
        CHECK(engine.prepare("UPDATE Items SET ParentId = ? WHERE Id = ?;",
                             invalidForeignKey).isOk());
        CHECK(engine.prepare("DELETE FROM Items WHERE Id = ?;",
                             deleteItem).isOk());
        const std::string relocated("relocated-" + std::string(350, 'r'));
        CHECK(updatePayload.bindText(0, relocated).isOk());
        CHECK(updatePayload.bindInt64(1, 3).isOk());
        CHECK(updatePayload.execute().ok);
        for (size_t version = 0; version < 5; ++version) {
            CHECK(updatePayload.bindText(
                0, std::string(70 + version * 30, static_cast<char>('a' + version))).isOk());
            CHECK(updatePayload.bindInt64(1, 3).isOk());
            CHECK(updatePayload.execute().ok);
        }
        CHECK(updatePayload.bindText(0, relocated).isOk());
        CHECK(updatePayload.bindInt64(1, 3).isOk());
        CHECK(updatePayload.execute().ok);
        CHECK(invalidForeignKey.bindInt64(0, 9999).isOk());
        CHECK(invalidForeignKey.bindInt64(1, 4).isOk());
        CHECK(!invalidForeignKey.execute().ok);

        CHECK(insertParent.bindInt64(0, 33).isOk());
        CHECK(insertParent.bindText(1, "parent-33").isOk());
        CHECK(insertParent.execute().ok);
        CHECK(insertItem.bindInt64(0, 33).isOk());
        CHECK(insertItem.bindInt64(1, 33).isOk());
        CHECK(insertItem.bindText(2, "new-tag").isOk());
        CHECK(insertItem.bindText(3, "after-failed-statement").isOk());
        CHECK(insertItem.execute().ok);
        CHECK(deleteItem.bindInt64(0, 5).isOk());
        CHECK(deleteItem.execute().ok);
        active = db->activeTransaction();
        CHECK(active != nullptr);
        if (active) {
            CHECK(active->pageStats().replacements > 0);
            CHECK(active->pageIndexStats().lookupHits > lookupHitsBeforeReads);
        }
        CHECK(runSql(engine, "COMMIT;").ok);

        // Duplicate-key statement B fails atomically between successful A and C.
        CHECK(runSql(engine, "BEGIN;").ok);
        CHECK(runSql(engine,
                     "INSERT INTO Parents VALUES (40, 'statement-a');").ok);
        CHECK(runSql(engine,
                     "INSERT INTO Parents VALUES (40, 'duplicate');", false).ok == false);
        CHECK(runSql(engine,
                     "INSERT INTO Parents VALUES (41, 'statement-c');").ok);
        CHECK(runSql(engine, "COMMIT;").ok);

        std::string integrity;
        CHECK_STATUS(db->validateIntegrity(integrity), DbStatus::Ok);
        std::vector<IndexInfo> indexes;
        CHECK_STATUS(db->listIndexes(indexes), DbStatus::Ok);
        CHECK(indexes.size() >= 3);
        for (size_t i = 0; i < indexes.size(); ++i) {
            IndexValidation validation;
            CHECK_STATUS(db->validateIndex(indexes[i].indexId, validation),
                         DbStatus::Ok);
            CHECK(validation.ok);
        }

        CHECK(runSql(engine, "BEGIN;").ok);
        CHECK(runSql(engine,
                     "UPDATE Items SET Payload = 'must-rollback' WHERE Id = 3;").ok);
        CHECK(runSql(engine, "ROLLBACK;").ok);

        SqlStatementResult row = selectItem.bindInt64(0, 3).isOk()
            ? selectItem.executeQuery() : SqlStatementResult();
        CHECK(row.ok && row.resultSet.rowCount() == 1);
        if (row.ok && row.resultSet.rowCount() == 1) {
            CHECK(row.resultSet.value(0, 1).textValue() == relocated);
        }
        SqlExecutionResult allItems = runSql(
            engine, "SELECT Id FROM Items ORDER BY Id;");
        CHECK(allItems.ok && allItems.statements.size() == 1);
        if (allItems.ok && allItems.statements.size() == 1) {
            const SqlResultSet& rows = allItems.statements[0].resultSet;
            CHECK(rows.rowCount() == 32);
            for (size_t i = 0; i < rows.rowCount(); ++i) {
                const int64_t expectedId = static_cast<int64_t>(i < 4 ? i + 1 : i + 2);
                CHECK(rows.value(i, 0).int64Value() == expectedId);
            }
        }
        SqlExecutionResult allParents = runSql(
            engine, "SELECT Id FROM Parents ORDER BY Id;");
        CHECK(allParents.ok && allParents.statements.size() == 1);
        if (allParents.ok && allParents.statements.size() == 1) {
            const SqlResultSet& rows = allParents.statements[0].resultSet;
            CHECK(rows.rowCount() == 35);
            size_t count40 = 0;
            size_t count41 = 0;
            for (size_t i = 0; i < rows.rowCount(); ++i) {
                const int64_t id = rows.value(i, 0).int64Value();
                if (id == 40) ++count40;
                if (id == 41) ++count41;
            }
            CHECK(count40 == 1 && count41 == 1);
        }
        std::unique_ptr<Table> itemsTable;
        CHECK_STATUS(db->openTable("Items", itemsTable), DbStatus::Ok);
        CHECK(itemsTable && itemsTable->heapPageCount() > 1);
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    db.reset();
    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    CHECK(reopened != nullptr);
    if (reopened) {
        SqlEngine engine(*reopened);
        SqlPreparedStatement select;
        CHECK(engine.prepare("SELECT Payload FROM Items WHERE Id = ?;",
                             select).isOk());
        CHECK(select.bindInt64(0, 3).isOk());
        SqlStatementResult row = select.executeQuery();
        CHECK(row.ok && row.resultSet.rowCount() == 1);
        if (row.ok && row.resultSet.rowCount() == 1) {
            CHECK(row.resultSet.value(0, 0).textValue() ==
                  "relocated-" + std::string(350, 'r'));
        }
        std::string integrity;
        CHECK_STATUS(reopened->validateIntegrity(integrity), DbStatus::Ok);
        CHECK_STATUS(reopened->close(), DbStatus::Ok);
    }
    std::printf("P8 SQL lookup diagnostic: 100 prepared indexed SELECTs in %.1f us\n",
                repeatedSelectUs);
    removeDatabaseFiles(path);
}

} // namespace

int main() {
    testIndexCapacityCollisionsAndFailure();
    testNearLimitLookupDiagnostics();
    testOverlayOrderReplacementAndSnapshotFailures();
    testProductionIndexAllocationAndSavepointRestore();
    testCollisionOverlayBeforeWalFailureRecovery();
    testSql9ProductionPreparedWorkload();
    std::printf("NDB1R4P8: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
