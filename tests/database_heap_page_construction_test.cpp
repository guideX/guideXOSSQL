#include "database_format.h"
#include "database_endian.h"
#include "database_heap.h"
#include "database_index.h"
#include "database_relational.h"
#include "database_sql.h"
#include "database_transaction.h"
#include "database_wal.h"
#include "gx_allocator_hosted.h"
#include "gx_heap_page_builder.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace {

struct HostedAllocationFault {
    size_t targetSize;
    size_t matchingAttempts;
    size_t matchingSuccesses;
    size_t matchingFailures;
    size_t failAfterSuccesses;
    bool armed;
};

thread_local HostedAllocationFault gHostedAllocationFault = {0, 0, 0, 0, 0, false};

void* allocateHostedTestMemory(size_t size) {
    HostedAllocationFault& fault = gHostedAllocationFault;
    if (fault.armed && size == fault.targetSize) {
        ++fault.matchingAttempts;
        if (fault.matchingSuccesses >= fault.failAfterSuccesses) {
            ++fault.matchingFailures;
            fault.armed = false;
            throw std::bad_alloc();
        }
        ++fault.matchingSuccesses;
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}

class ScopedHostedAllocationFailure {
public:
    ScopedHostedAllocationFailure(size_t targetSize, size_t successfulMatches) {
        gHostedAllocationFault.targetSize = targetSize;
        gHostedAllocationFault.matchingAttempts = 0;
        gHostedAllocationFault.matchingSuccesses = 0;
        gHostedAllocationFault.matchingFailures = 0;
        gHostedAllocationFault.failAfterSuccesses = successfulMatches;
        gHostedAllocationFault.armed = true;
    }

    ~ScopedHostedAllocationFailure() { gHostedAllocationFault.armed = false; }

    size_t matchingAttempts() const {
        return gHostedAllocationFault.matchingAttempts;
    }
    size_t matchingSuccesses() const {
        return gHostedAllocationFault.matchingSuccesses;
    }
    size_t matchingFailures() const {
        return gHostedAllocationFault.matchingFailures;
    }

    ScopedHostedAllocationFailure(const ScopedHostedAllocationFailure&) = delete;
    ScopedHostedAllocationFailure& operator=(
        const ScopedHostedAllocationFailure&) = delete;
};

} // namespace

// Keep deterministic hosted allocation failure injection local to this test
// executable; production code uses the ordinary hosted allocator.
void* operator new(std::size_t size) {
    return allocateHostedTestMemory(size);
}

void* operator new[](std::size_t size) {
    return allocateHostedTestMemory(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace gxos::db;

namespace {

int gChecks = 0;
int gFailures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++gChecks;                                                              \
        if (!(condition)) {                                                     \
            ++gFailures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #condition);                                           \
        }                                                                       \
    } while (0)

#define CHECK_DB(expression, expected)                                          \
    do {                                                                        \
        const DbResult result = (expression);                                   \
        ++gChecks;                                                              \
        if (result.status() != (expected)) {                                    \
            ++gFailures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s returned %s, expected %s\n",\
                         __FILE__, __LINE__, #expression,                      \
                         dbStatusName(result.status()),                        \
                         dbStatusName(expected));                              \
        }                                                                       \
    } while (0)

struct AllocationState {
    size_t attempts;
    size_t successes;
    size_t frees;
    size_t outstanding;
    size_t matchingSize;
    size_t matchingAttempts;
    size_t matchingSuccesses;
    size_t matchingFailures;
    size_t failAtMatchingSuccess;
    bool failureEnabled;
    size_t failAtAttempt;
    bool failAtAttemptEnabled;
    size_t allocationFailures;

    AllocationState()
        : attempts(0), successes(0), frees(0), outstanding(0),
          matchingSize(0), matchingAttempts(0), matchingSuccesses(0),
          matchingFailures(0),
          failAtMatchingSuccess(0), failureEnabled(false), failAtAttempt(0),
          failAtAttemptEnabled(false), allocationFailures(0) {}
};

bool allocateBytes(void* context, size_t size, size_t alignment,
                   void** outPointer) {
    AllocationState* state = static_cast<AllocationState*>(context);
    ++state->attempts;
    *outPointer = 0;
    if (state->failAtAttemptEnabled && state->attempts >= state->failAtAttempt) {
        state->failAtAttemptEnabled = false;
        ++state->allocationFailures;
        return false;
    }
    if (alignment == 0 || alignment > 8) return false;
    if (state->failureEnabled && size == state->matchingSize) {
        ++state->matchingAttempts;
        if (state->matchingSuccesses >= state->failAtMatchingSuccess) {
            ++state->matchingFailures;
            return false;
        }
    }
    void* pointer = std::malloc(size);
    if (!pointer) return false;
    ++state->successes;
    ++state->outstanding;
    if (size == state->matchingSize) ++state->matchingSuccesses;
    *outPointer = pointer;
    return true;
}

void freeBytes(void* context, void* pointer) {
    AllocationState* state = static_cast<AllocationState*>(context);
    if (!pointer) return;
    if (state->outstanding == 0) {
        ++gFailures;
        std::fprintf(stderr, "FAIL allocator released an unknown allocation\n");
    } else {
        --state->outstanding;
    }
    ++state->frees;
    std::free(pointer);
}

GxAllocator makeAllocator(AllocationState* state, size_t maximum = 1024 * 1024) {
    GxAllocator allocator = {};
    allocator.context = state;
    allocator.allocate = allocateBytes;
    allocator.deallocate = freeBytes;
    allocator.maxAllocation = maximum;
    allocator.maxAlignment = 8;
    return allocator;
}

void failMatchingAfter(AllocationState* state, size_t size,
                       size_t additionalSuccesses) {
    state->matchingSize = size;
    state->failAtMatchingSuccess = state->matchingSuccesses + additionalSuccesses;
    state->failureEnabled = true;
}

void disableFailure(AllocationState* state) {
    state->failureEnabled = false;
    state->failAtAttemptEnabled = false;
}

void failAtAttempt(AllocationState* state, size_t attemptsFromNow) {
    state->failAtAttempt = state->attempts + attemptsFromNow;
    state->failAtAttemptEnabled = true;
}

std::vector<uint8_t> makeRow(size_t length, uint8_t fill = 0x41) {
    if (length < 8) length = 8;
    std::vector<uint8_t> row(length, fill);
    storeLe32(row.data(), static_cast<uint32_t>(length));
    storeLe16(row.data() + 4, 0);
    storeLe16(row.data() + 6, 0);
    return row;
}

struct VectorRows {
    std::vector<std::vector<uint8_t> > rows;
};

GxStatus vectorRowAt(const void* context, size_t index,
                     const uint8_t** outBytes, size_t* outLength) {
    if (!context || !outBytes || !outLength) return GxStatus::InvalidArgument;
    const VectorRows* source = static_cast<const VectorRows*>(context);
    if (index >= source->rows.size()) return GxStatus::InvalidArgument;
    const std::vector<uint8_t>& row = source->rows[index];
    *outBytes = row.empty() ? 0 : row.data();
    *outLength = row.size();
    return GxStatus::Ok;
}

struct FailingRows {
    VectorRows source;
    size_t calls;
    size_t failAt;

    FailingRows() : source(), calls(0), failAt(0) {}
};

GxStatus failingRowAt(const void* context, size_t index,
                      const uint8_t** outBytes, size_t* outLength) {
    FailingRows* source = const_cast<FailingRows*>(
        static_cast<const FailingRows*>(context));
    if (!source || source->calls++ == source->failAt) {
        return GxStatus::OutOfMemory;
    }
    return vectorRowAt(&source->source, index, outBytes, outLength);
}

struct ChangingRow {
    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    size_t calls;

    ChangingRow() : first(), second(), calls(0) {}
};

GxStatus changingRowAt(const void* context, size_t index,
                       const uint8_t** outBytes, size_t* outLength) {
    if (!context || index != 0 || !outBytes || !outLength) {
        return GxStatus::InvalidArgument;
    }
    ChangingRow* row = const_cast<ChangingRow*>(
        static_cast<const ChangingRow*>(context));
    const std::vector<uint8_t>& bytes = row->calls++ == 0
        ? row->first : row->second;
    *outBytes = bytes.data();
    *outLength = bytes.size();
    return GxStatus::Ok;
}

GxHeapPageRowSource rowSource(const VectorRows& rows) {
    GxHeapPageRowSource source = {};
    source.context = &rows;
    source.rowCount = rows.rows.size();
    source.rowAt = vectorRowAt;
    return source;
}

std::vector<uint8_t> expectedHeapPayload(
    uint32_t pageSize, uint64_t nextPageId,
    const std::vector<std::vector<uint8_t> >& rows) {
    const size_t capacity = pageSize - kPageHeaderSize;
    std::vector<uint8_t> expected(capacity, 0);
    size_t rowOffset = kHeapHeaderSize;
    for (size_t i = 0; i < rows.size(); ++i) {
        const std::vector<uint8_t>& row = rows[i];
        const size_t slotOffset = capacity - 8u * (i + 1u);
        std::copy(row.begin(), row.end(), expected.begin() + rowOffset);
        storeLe32(expected.data() + slotOffset,
                  static_cast<uint32_t>(rowOffset));
        storeLe32(expected.data() + slotOffset + 4u,
                  static_cast<uint32_t>(row.size()));
        rowOffset += row.size();
    }
    storeLe32(expected.data() + kHeapMagicOffset, kHeapMagic);
    storeLe16(expected.data() + kHeapVersionOffset, kHeapVersion);
    storeLe16(expected.data() + kHeapFlagsOffset, 0);
    storeLe64(expected.data() + kHeapNextPageOffset, nextPageId);
    storeLe32(expected.data() + kHeapSlotCountOffset,
              static_cast<uint32_t>(rows.size()));
    storeLe32(expected.data() + kHeapRowAreaEndOffset,
              static_cast<uint32_t>(rowOffset));
    return expected;
}

std::vector<uint8_t> expectedIndexLeaf(
    uint32_t pageSize, uint32_t indexId,
    const std::vector<IndexEntry>& entries) {
    const uint32_t capacity = pageSize - kPageHeaderSize;
    std::vector<uint8_t> expected(capacity, 0);
    storeLe32(expected.data(), kIndexLeafMagic);
    storeLe16(expected.data() + 4, kIndexPageVersion);
    storeLe16(expected.data() + 6, 0);
    storeLe64(expected.data() + 8, 0);
    storeLe32(expected.data() + 16, static_cast<uint32_t>(entries.size()));
    storeLe32(expected.data() + 20, indexId);
    uint32_t entryOffset = kIndexLeafHeaderSize;
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint32_t keyLength = static_cast<uint32_t>(entries[i].key.size());
        const uint32_t entryLength = kIndexLeafEntryOverhead + keyLength;
        storeLe32(expected.data() + entryOffset, keyLength);
        std::copy(entries[i].key.begin(), entries[i].key.end(),
                  expected.begin() + entryOffset + 4);
        storeLe64(expected.data() + entryOffset + 4 + keyLength,
                  entries[i].locator.pageId);
        storeLe32(expected.data() + entryOffset + 4 + keyLength + 8,
                  entries[i].locator.slot);
        const uint32_t slotOffset =
            capacity - kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        storeLe32(expected.data() + slotOffset, entryOffset);
        storeLe32(expected.data() + slotOffset + 4, entryLength);
        entryOffset += entryLength;
    }
    return expected;
}

bool arrayEquals(const GxArray& array, const std::vector<uint8_t>& expected) {
    return gxArraySize(&array) == expected.size() &&
           std::equal(expected.begin(), expected.end(), array.data);
}

GxStatus prepare(GxArray* output, uint32_t pageSize, uint64_t nextPageId,
                 const VectorRows& rows) {
    GxHeapPageRowSource source = rowSource(rows);
    return gxHeapPagePrepare(output, pageSize, nextPageId, &source);
}

void testHeapByteConstruction() {
    std::printf("NDB1R4P11R2 portable heap byte construction\n");
    AllocationState state;
    GxAllocator allocator = makeAllocator(&state);
    GxArray output = {};
    CHECK(gxArrayInit(&output, &allocator, 1, 4064) == GxStatus::Ok);

    VectorRows rows;
    CHECK(prepare(&output, 512, 0x1122334455667788ull, rows) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(512, 0x1122334455667788ull,
                                                  rows.rows)));

    rows.rows.push_back(makeRow(17, 0x00));
    CHECK(prepare(&output, 512, 9, rows) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(512, 9, rows.rows)));
    CHECK(output.data[kHeapHeaderSize + 8u] == 0);

    rows.rows.push_back(makeRow(53, 0xA5));
    rows.rows.push_back(makeRow(31, 0x00));
    CHECK(prepare(&output, 512, 10, rows) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(512, 10, rows.rows)));

    // Repack a compacted row list and reuse the same GxArray storage.
    rows.rows.erase(rows.rows.begin() + 1);
    CHECK(prepare(&output, 512, 11, rows) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(512, 11, rows.rows)));
    CHECK(loadLe32(output.data + kHeapSlotCountOffset) == 2);
    CHECK(loadLe32(output.data + 512 - kPageHeaderSize - 8) ==
          kHeapHeaderSize);

    VectorRows maximum;
    maximum.rows.push_back(makeRow(448, 0x7C));
    CHECK(prepare(&output, 512, 12, maximum) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(512, 12, maximum.rows)));
    maximum.rows[0] = makeRow(449, 0x7C);
    CHECK(prepare(&output, 512, 12, maximum) == GxStatus::ResourceLimit);

    VectorRows normalMaximum;
    normalMaximum.rows.push_back(makeRow(4032, 0x31));
    CHECK(prepare(&output, 4096, 13, normalMaximum) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(4096, 13,
                                                  normalMaximum.rows)));

    VectorRows minimumRows;
    for (size_t i = 0; i < 28; ++i) minimumRows.rows.push_back(makeRow(8));
    CHECK(prepare(&output, 512, 14, minimumRows) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(512, 14,
                                                  minimumRows.rows)));
    minimumRows.rows.push_back(makeRow(8));
    CHECK(prepare(&output, 512, 14, minimumRows) == GxStatus::ResourceLimit);

    // The largest supported page can hold 252 minimum-length rows and their
    // slot entries; the next append exceeds the combined row/slot budget.
    VectorRows largePageMinimumRows;
    for (size_t i = 0; i < 252; ++i) {
        largePageMinimumRows.rows.push_back(
            makeRow(8, static_cast<uint8_t>(i)));
    }
    CHECK(prepare(&output, 4096, 15, largePageMinimumRows) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(
                                  4096, 15, largePageMinimumRows.rows)));
    largePageMinimumRows.rows.push_back(makeRow(8));
    CHECK(prepare(&output, 4096, 15, largePageMinimumRows) ==
          GxStatus::ResourceLimit);

    CHECK(prepare(&output, 513, 0, rows) == GxStatus::InvalidArgument);
    VectorRows invalid;
    invalid.rows.push_back(std::vector<uint8_t>(7, 0));
    CHECK(prepare(&output, 512, 0, invalid) == GxStatus::InvalidArgument);
    invalid.rows[0] = makeRow(8);
    storeLe32(invalid.rows[0].data(), 7);
    CHECK(prepare(&output, 512, 0, invalid) == GxStatus::CorruptData);

    VectorRows overflowRows;
    GxHeapPageRowSource overflowSource = {};
    overflowSource.context = &overflowRows;
    overflowSource.rowCount = static_cast<size_t>(-1);
    overflowSource.rowAt = vectorRowAt;
    CHECK(gxHeapPagePrepare(&output, 512, 0, &overflowSource) ==
          GxStatus::Overflow);

    gxArrayDestroy(&output);
    CHECK(state.outstanding == 0);
}

std::vector<uint8_t> expectedAppendPayload(
    const std::vector<uint8_t>& existing, const std::vector<uint8_t>& row) {
    std::vector<uint8_t> expected(existing);
    const size_t capacity = existing.size();
    const uint32_t oldCount = loadLe32(existing.data() + kHeapSlotCountOffset);
    const uint32_t rowAreaEnd =
        loadLe32(existing.data() + kHeapRowAreaEndOffset);
    const size_t slotOffset = capacity - 8u * (oldCount + 1u);
    std::copy(row.begin(), row.end(), expected.begin() + rowAreaEnd);
    storeLe32(expected.data() + slotOffset, rowAreaEnd);
    storeLe32(expected.data() + slotOffset + 4u,
              static_cast<uint32_t>(row.size()));
    storeLe32(expected.data() + kHeapSlotCountOffset, oldCount + 1u);
    storeLe32(expected.data() + kHeapRowAreaEndOffset,
              rowAreaEnd + static_cast<uint32_t>(row.size()));
    return expected;
}

void testHeapAppendPreparation() {
    std::printf("NDB1R4P11R3J portable heap append preparation\n");
    AllocationState state;
    GxAllocator allocator = makeAllocator(&state);
    GxArray output = {};
    CHECK(gxArrayInit(&output, &allocator, 1, 4064) == GxStatus::Ok);

    VectorRows baseRows;
    baseRows.rows.push_back(makeRow(8, 0x11));
    CHECK(prepare(&output, 512, 77, baseRows) == GxStatus::Ok);
    std::vector<uint8_t> existing(output.data,
                                  output.data + gxArraySize(&output));
    std::vector<uint8_t> exactFit = makeRow(432, 0x00);
    CHECK(gxHeapPageAppendPrepare(&output, 512, existing.data(),
                                  existing.size(), exactFit.data(),
                                  exactFit.size()) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedAppendPayload(existing, exactFit)));
    CHECK(loadLe64(output.data + kHeapNextPageOffset) == 77);
    CHECK(output.data[kHeapHeaderSize + 8u + 8u] == 0);

    const std::vector<uint8_t> fullPage(output.data,
                                        output.data + gxArraySize(&output));
    std::vector<uint8_t> oneByteTooLarge = makeRow(433, 0x22);
    CHECK(gxHeapPageAppendPrepare(&output, 512, fullPage.data(),
                                  fullPage.size(), oneByteTooLarge.data(),
                                  oneByteTooLarge.size()) ==
          GxStatus::ResourceLimit);
    CHECK(std::equal(fullPage.begin(), fullPage.end(), output.data));

    // The same append encoding supports the configured 4,096-byte pages.
    VectorRows normalBaseRows;
    normalBaseRows.rows.push_back(makeRow(8, 0x33));
    CHECK(prepare(&output, 4096, 88, normalBaseRows) == GxStatus::Ok);
    existing.assign(output.data, output.data + gxArraySize(&output));
    exactFit = makeRow(4016, 0x5A);
    CHECK(gxHeapPageAppendPrepare(&output, 4096, existing.data(),
                                  existing.size(), exactFit.data(),
                                  exactFit.size()) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedAppendPayload(existing, exactFit)));

    const std::vector<uint8_t> prior(output.data,
                                     output.data + gxArraySize(&output));
    const std::vector<uint8_t> allocationSource =
        expectedHeapPayload(4096, 88, normalBaseRows.rows);
    const std::vector<uint8_t> smallRow = makeRow(8);
    std::vector<uint8_t> invalidRow = makeRow(8);
    storeLe32(invalidRow.data(), 9);
    CHECK(gxHeapPageAppendPrepare(&output, 4096, prior.data(), prior.size(),
                                  invalidRow.data(), invalidRow.size()) ==
          GxStatus::CorruptData);
    CHECK(std::equal(prior.begin(), prior.end(), output.data));

    std::vector<uint8_t> corruptExisting = allocationSource;
    storeLe32(corruptExisting.data() + 4096 - kPageHeaderSize - 4u, 9u);
    CHECK(gxHeapPageAppendPrepare(&output, 4096, corruptExisting.data(),
                                  corruptExisting.size(), smallRow.data(),
                                  smallRow.size()) == GxStatus::CorruptData);
    CHECK(std::equal(prior.begin(), prior.end(), output.data));

    GxArray limited = {};
    CHECK(gxArrayInit(&limited, &allocator, 1, 479) == GxStatus::Ok);
    CHECK(gxHeapPageAppendPrepare(&limited, 512, allocationSource.data(),
                                  480, smallRow.data(), smallRow.size()) ==
          GxStatus::ResourceLimit);
    CHECK(gxArraySize(&limited) == 0);
    gxArrayDestroy(&limited);

    failMatchingAfter(&state, 4064, 0);
    CHECK(gxHeapPageAppendPrepare(&output, 4096, allocationSource.data(),
                                  allocationSource.size(), smallRow.data(),
                                  smallRow.size()) ==
          GxStatus::OutOfMemory);
    CHECK(std::equal(prior.begin(), prior.end(), output.data));
    CHECK(state.outstanding == 1);
    disableFailure(&state);
    gxArrayDestroy(&output);
    CHECK(state.outstanding == 0);
}

void testAllocationFailureAndRetry() {
    std::printf("NDB1R4P11R2 heap preparation allocation failures\n");
    AllocationState state;
    GxAllocator allocator = makeAllocator(&state);
    GxArray output = {};
    CHECK(gxArrayInit(&output, &allocator, 1, 4064) == GxStatus::Ok);
    VectorRows rows;
    rows.rows.push_back(makeRow(80, 0x5A));

    failMatchingAfter(&state, 512, 0);
    CHECK(prepare(&output, 512, 1, rows) == GxStatus::OutOfMemory);
    CHECK(gxArraySize(&output) == 0);
    CHECK(state.outstanding == 0);
    disableFailure(&state);
    CHECK(prepare(&output, 512, 1, rows) == GxStatus::Ok);
    const std::vector<uint8_t> previous(output.data,
                                        output.data + gxArraySize(&output));
    CHECK(state.outstanding == 1);

    failMatchingAfter(&state, 4064, 0);
    VectorRows wider;
    wider.rows.push_back(makeRow(4000, 0x6B));
    CHECK(prepare(&output, 4096, 2, wider) == GxStatus::OutOfMemory);
    CHECK(gxArraySize(&output) == previous.size());
    CHECK(std::equal(previous.begin(), previous.end(), output.data));
    CHECK(state.outstanding == 1);
    disableFailure(&state);
    CHECK(prepare(&output, 4096, 2, wider) == GxStatus::Ok);
    CHECK(arrayEquals(output, expectedHeapPayload(4096, 2, wider.rows)));
    CHECK(state.outstanding == 1);
    gxArrayDestroy(&output);
    CHECK(state.outstanding == 0);

    GxAllocator limitedAllocator = makeAllocator(&state, 479);
    GxArray limited = {};
    CHECK(gxArrayInit(&limited, &limitedAllocator, 1, 480) == GxStatus::Ok);
    CHECK(prepare(&limited, 512, 0, rows) == GxStatus::ResourceLimit);
    CHECK(gxArraySize(&limited) == 0);
    gxArrayDestroy(&limited);

    GxAllocator incompleteAllocator = {};
    GxArray invalidAllocatorOutput = {};
    CHECK(gxArrayInit(&invalidAllocatorOutput, &incompleteAllocator, 1, 480) ==
          GxStatus::InvalidArgument);

    GxAllocator hostedAllocator = {};
    CHECK(gxHostedAllocatorInit(&hostedAllocator, 1024 * 1024) == GxStatus::Ok);
    GxArray hosted = {};
    CHECK(gxArrayInit(&hosted, &hostedAllocator, 1, 480) == GxStatus::Ok);
    CHECK(prepare(&hosted, 512, 3, rows) == GxStatus::Ok);
    CHECK(arrayEquals(hosted, expectedHeapPayload(512, 3, rows.rows)));
    gxArrayDestroy(&hosted);

    FailingRows failingRows;
    failingRows.source.rows.push_back(makeRow(20, 0x11));
    failingRows.source.rows.push_back(makeRow(21, 0x22));
    failingRows.failAt = 3; // After reserve and one row has been written.
    GxArray partial = {};
    CHECK(gxArrayInit(&partial, &allocator, 1, 480) == GxStatus::Ok);
    CHECK(prepare(&partial, 512, 9, failingRows.source) == GxStatus::Ok);
    const std::vector<uint8_t> previousPartial(
        partial.data, partial.data + gxArraySize(&partial));
    failingRows.calls = 0;
    GxHeapPageRowSource failingSource = {};
    failingSource.context = &failingRows;
    failingSource.rowCount = failingRows.source.rows.size();
    failingSource.rowAt = failingRowAt;
    CHECK(gxHeapPagePrepare(&partial, 512, 0, &failingSource) ==
          GxStatus::OutOfMemory);
    CHECK(gxArraySize(&partial) == 480);
    CHECK(std::equal(previousPartial.begin(), previousPartial.end(), partial.data));
    CHECK(state.outstanding == 1);

    ChangingRow changing;
    changing.first = makeRow(8, 0x33);
    changing.second = makeRow(449, 0x44);
    GxHeapPageRowSource changingSource = {};
    changingSource.context = &changing;
    changingSource.rowCount = 1;
    changingSource.rowAt = changingRowAt;
    CHECK(gxHeapPagePrepare(&partial, 512, 0, &changingSource) ==
          GxStatus::ResourceLimit);
    CHECK(gxArraySize(&partial) == 480);
    CHECK(std::equal(previousPartial.begin(), previousPartial.end(), partial.data));
    CHECK(state.outstanding == 1);
    gxArrayDestroy(&partial);
    CHECK(state.outstanding == 0);
}

std::string testPath() {
    const char* base = std::getenv("TEMP");
    if (!base) base = std::getenv("TMP");
    if (!base) base = ".";
    const std::string directory = std::string(base) + "/gxos_heap_page_r2_tests";
#if defined(_WIN32)
    _mkdir(directory.c_str());
#else
    mkdir(directory.c_str(), 0700);
#endif
    return directory + "/heap_page_construction.gxdb";
}

void removeDatabase(const std::string& path) {
    std::remove(walPathFor(path).c_str());
    std::remove(path.c_str());
}

bool runSql(SqlEngine& engine, const std::string& sql) {
    const SqlExecutionResult result = engine.execute(sql);
    if (!result.ok) {
        std::fprintf(stderr, "SQL failed: %s\n", result.error.describe().c_str());
    }
    return result.ok;
}

int64_t rowCount(SqlEngine& engine) {
    const SqlExecutionResult result = engine.execute(
        "SELECT COUNT(*) FROM HeapRows;");
    CHECK(result.ok && result.statements.size() == 1);
    if (!result.ok || result.statements.size() != 1 ||
        result.statements[0].resultSet.rowCount() != 1) {
        return -1;
    }
    return result.statements[0].resultSet.value(0, 0).int64Value();
}

int64_t countRows(SqlEngine& engine, const std::string& table) {
    const SqlExecutionResult result =
        engine.execute("SELECT COUNT(*) FROM " + table + ";");
    CHECK(result.ok && result.statements.size() == 1);
    if (!result.ok || result.statements.size() != 1 ||
        result.statements[0].resultSet.rowCount() != 1) {
        return -1;
    }
    return result.statements[0].resultSet.value(0, 0).int64Value();
}

void checkIndexes(Database& db) {
    std::vector<IndexInfo> indexes;
    CHECK_DB(db.listIndexes(indexes), DbStatus::Ok);
    CHECK(indexes.size() >= 2);
    for (size_t i = 0; i < indexes.size(); ++i) {
        IndexValidation validation;
        CHECK_DB(db.validateIndex(indexes[i].indexId, validation), DbStatus::Ok);
        CHECK(validation.ok);
    }
}

void testProductionMutationFailureAndRelocation() {
    std::printf("NDB1R4P11R2 production SQL mutation and savepoint failure\n");
    const std::string path = testPath();
    removeDatabase(path);

    AllocationState allocationState;
    GxAllocator allocator = makeAllocator(&allocationState);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    createOptions.bufferCapacity = 128;
    createOptions.pageCacheAllocator = &allocator;
    std::unique_ptr<Database> db;
    CHECK_DB(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE HeapRows (Id INT64 PRIMARY KEY, K TEXT NOT NULL, "
                     "V TEXT NOT NULL);"));
        CHECK(runSql(engine, "CREATE INDEX IX_HeapRows_K ON HeapRows (K);"));
        CHECK(runSql(engine, "BEGIN;"));
        for (int64_t id = 0; id < 24; ++id) {
            const std::string sql =
                "INSERT INTO HeapRows VALUES (" + std::to_string(id) +
                ", 'key-" + std::to_string(id) + "', '" +
                std::string(80, static_cast<char>('a' + id % 20)) + "');";
            CHECK(runSql(engine, sql));
        }
        CHECK(runSql(engine, "COMMIT;"));

        std::unique_ptr<Table> table;
        CHECK_DB(db->openTable("HeapRows", table), DbStatus::Ok);
        CHECK(table && table->heapPageCount() > 1);

        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE HeapRows SET V = 'prior-success' WHERE Id = 1;"));
        const size_t successesBeforeDelete =
            allocationState.matchingSuccesses;
        failMatchingAfter(&allocationState, 480, 1);
        const SqlExecutionResult failedDelete =
            engine.execute("DELETE FROM HeapRows WHERE Id >= 0;");
        disableFailure(&allocationState);
        CHECK(!failedDelete.ok && failedDelete.statements.size() == 1 &&
              !failedDelete.statements[0].ok);
        CHECK(failedDelete.transactionActiveAfter && engine.inTransaction());
        CHECK(allocationState.matchingSuccesses == successesBeforeDelete + 1);
        CHECK(allocationState.matchingFailures >= 1);

        CHECK(rowCount(engine) == 24);
        const SqlExecutionResult priorValue = engine.execute(
            "SELECT V FROM HeapRows WHERE Id = 1;");
        CHECK(priorValue.ok && priorValue.statements.size() == 1 &&
              priorValue.statements[0].resultSet.rowCount() == 1);
        if (priorValue.ok && priorValue.statements.size() == 1 &&
            priorValue.statements[0].resultSet.rowCount() == 1) {
            CHECK(priorValue.statements[0].resultSet.value(0, 0).textValue() ==
                  "prior-success");
        }
        const SqlExecutionResult secondaryLookup = engine.execute(
            "SELECT Id FROM HeapRows WHERE K = 'key-1';");
        CHECK(secondaryLookup.ok && secondaryLookup.statements.size() == 1 &&
              secondaryLookup.statements[0].resultSet.rowCount() == 1);
        if (secondaryLookup.ok && secondaryLookup.statements.size() == 1 &&
            secondaryLookup.statements[0].resultSet.rowCount() == 1) {
            CHECK(secondaryLookup.statements[0].resultSet.value(0, 0).int64Value() ==
                  1);
        }
        checkIndexes(*db);

        // Reuse the still-active transaction and retry the failed statement.
        CHECK(runSql(engine, "DELETE FROM HeapRows WHERE Id >= 0;"));
        CHECK(rowCount(engine) == 0);
        checkIndexes(*db);
        CHECK(runSql(engine, "ROLLBACK;"));
        CHECK(!engine.inTransaction());
        CHECK(rowCount(engine) == 24);
        const SqlExecutionResult rolledBackValue = engine.execute(
            "SELECT V FROM HeapRows WHERE Id = 1;");
        CHECK(rolledBackValue.ok && rolledBackValue.statements.size() == 1 &&
              rolledBackValue.statements[0].resultSet.rowCount() == 1);
        if (rolledBackValue.ok && rolledBackValue.statements.size() == 1 &&
            rolledBackValue.statements[0].resultSet.rowCount() == 1) {
            CHECK(rolledBackValue.statements[0].resultSet.value(0, 0).textValue() ==
                  std::string(80, 'b'));
        }
        checkIndexes(*db);

        // Grow an indexed variable-width row so later rows in its page relocate.
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE HeapRows SET K = 'moved-key', V = '" +
                     std::string(350, 'z') + "' WHERE Id = 2;"));
        CHECK(runSql(engine, "COMMIT;"));
        CHECK(rowCount(engine) == 24);
        const SqlExecutionResult movedLookup = engine.execute(
            "SELECT Id FROM HeapRows WHERE K = 'moved-key';");
        CHECK(movedLookup.ok && movedLookup.statements.size() == 1 &&
              movedLookup.statements[0].resultSet.rowCount() == 1);
        if (movedLookup.ok && movedLookup.statements.size() == 1 &&
            movedLookup.statements[0].resultSet.rowCount() == 1) {
            CHECK(movedLookup.statements[0].resultSet.value(0, 0).int64Value() == 2);
        }
        checkIndexes(*db);
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
    }

    CHECK_DB(db->close(), DbStatus::Ok);
    db.reset();
    CHECK(allocationState.outstanding == 0);

    DatabaseOpenOptions openOptions;
    openOptions.pageCacheAllocator = &allocator;
    std::unique_ptr<Database> reopened;
    CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
    if (reopened) {
        {
            SqlEngine engine(*reopened);
            CHECK(rowCount(engine) == 24);
            const SqlExecutionResult movedLookup = engine.execute(
                "SELECT Id, V FROM HeapRows WHERE K = 'moved-key';");
            CHECK(movedLookup.ok && movedLookup.statements.size() == 1 &&
                  movedLookup.statements[0].resultSet.rowCount() == 1);
            if (movedLookup.ok && movedLookup.statements.size() == 1 &&
                movedLookup.statements[0].resultSet.rowCount() == 1) {
                CHECK(movedLookup.statements[0].resultSet.value(0, 0).int64Value() ==
                      2);
                CHECK(movedLookup.statements[0].resultSet.value(0, 1).textValue() ==
                      std::string(350, 'z'));
            }
            checkIndexes(*reopened);
        }
        CHECK_DB(reopened->close(), DbStatus::Ok);
    }
    reopened.reset();
    CHECK(allocationState.outstanding == 0);
    removeDatabase(path);
}

void testInsertSavepointFailureAtomicity() {
    std::printf("NDB1R4P11R3K checked existing-leaf publication and INSERT savepoint\n");
    const std::string path = testPath() + ".r3i.gxdb";
    removeDatabase(path);

    AllocationState cacheState;
    AllocationState transactionState;
    GxAllocator cacheAllocator = makeAllocator(&cacheState);
    GxAllocator transactionAllocator = makeAllocator(&transactionState);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    createOptions.bufferCapacity = 128;
    createOptions.pageCacheAllocator = &cacheAllocator;
    createOptions.transactionPageAllocator = &transactionAllocator;

    std::unique_ptr<Database> db;
    CHECK_DB(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE ParentRows (Id INT64 PRIMARY KEY);"));
        CHECK(runSql(engine, "INSERT INTO ParentRows VALUES (1);"));
        CHECK(runSql(engine,
                     "CREATE TABLE PriorRows (Id INT64 PRIMARY KEY, "
                     "V TEXT NOT NULL);"));
        CHECK(runSql(engine, "CREATE INDEX IX_PriorRows_V ON PriorRows (V);"));
        CHECK(runSql(engine, "INSERT INTO PriorRows VALUES (1, 'before');"));
        CHECK(runSql(engine,
                     "CREATE TABLE ChildRows (Id INT64 PRIMARY KEY, "
                     "ParentId INT64, Code TEXT NOT NULL, FOREIGN KEY (ParentId) "
                     "REFERENCES ParentRows (Id));"));
        CHECK(runSql(engine,
                     "CREATE UNIQUE INDEX IX_ChildRows_Code ON ChildRows (Code);"));
        CHECK(runSql(engine,
                     "INSERT INTO ChildRows VALUES (1, 1, 'existing-code');"));

        SqlPreparedStatement insertChild;
        CHECK(engine.prepare("INSERT INTO ChildRows VALUES (?, ?, ?);",
                             insertChild).isOk());

        // First prove a partially constructed later page snapshot is discarded
        // before INSERT starts, while the preceding UPDATE remains visible.
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE PriorRows SET V = 'rollback-probe' WHERE Id = 1;"));
        Transaction* tx = db->activeTransaction();
        CHECK(tx != nullptr && tx->modifiedPageCount() >= 2);
        const uint32_t snapshotPageCount =
            tx ? tx->modifiedPageCount() : 0;
        const uint64_t snapshotNextPage = tx ? tx->finalPageCount() : 0;
        const size_t snapshotOwnedBytes = transactionState.outstanding;
        const size_t snapshotAttempts = transactionState.attempts;
        const size_t snapshotSuccesses = transactionState.successes;
        const size_t snapshotFailures = transactionState.allocationFailures;
        failAtAttempt(&transactionState, 3);
        CHECK(insertChild.bindInt64(0, 10).isOk());
        CHECK(insertChild.bindNull(1).isOk());
        CHECK(insertChild.bindText(2, "snapshot-fail").isOk());
        const SqlStatementResult snapshotFailure = insertChild.execute();
        disableFailure(&transactionState);
        CHECK(!snapshotFailure.ok && snapshotFailure.transactionActiveAfter);
        CHECK(snapshotFailure.error.message ==
              "statement page snapshot allocation failed");
        CHECK(transactionState.attempts == snapshotAttempts + 3);
        CHECK(transactionState.successes == snapshotSuccesses + 2);
        CHECK(transactionState.allocationFailures == snapshotFailures + 1);
        CHECK(transactionState.outstanding == snapshotOwnedBytes);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == snapshotPageCount &&
              db->activeTransaction()->finalPageCount() == snapshotNextPage);
        CHECK(countRows(engine, "ChildRows") == 1);
        const SqlExecutionResult priorAfterSnapshotFailure = engine.execute(
            "SELECT V FROM PriorRows WHERE Id = 1;");
        CHECK(priorAfterSnapshotFailure.ok &&
              priorAfterSnapshotFailure.statements[0].resultSet.value(0, 0).textValue() ==
                  "rollback-probe");
        CHECK(runSql(engine, "ROLLBACK;"));
        CHECK(countRows(engine, "ChildRows") == 1);
        const SqlExecutionResult priorAfterExplicitRollback = engine.execute(
            "SELECT V FROM PriorRows WHERE Id = 1;");
        CHECK(priorAfterExplicitRollback.ok &&
              priorAfterExplicitRollback.statements[0].resultSet.value(0, 0).textValue() ==
                  "before");

        // First fail the transaction-allocator-owned candidate for the
        // existing primary leaf after the heap page is staged. A fitting leaf
        // preparation error must return directly and must not request a split.
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE PriorRows SET V = 'commit-probe' WHERE Id = 1;"));
        tx = db->activeTransaction();
        CHECK(tx != nullptr && tx->modifiedPageCount() >= 2);
        const uint32_t writerPageCount = tx ? tx->modifiedPageCount() : 0;
        const uint64_t writerNextPage = tx ? tx->finalPageCount() : 0;
        const size_t writerOwnedBytes = transactionState.outstanding;
        const uint64_t writerImageAllocations = tx->pageStats().imageAllocations;

        CHECK(insertChild.bindInt64(0, 10).isOk());
        CHECK(insertChild.bindNull(1).isOk());
        CHECK(insertChild.bindText(2, "first-writer-fail").isOk());
        SqlStatementResult writerFailure;
        const size_t firstLeafAttempts = transactionState.matchingAttempts;
        const size_t firstLeafMatches = transactionState.matchingSuccesses;
        const size_t firstLeafFailures = transactionState.matchingFailures;
        failMatchingAfter(&transactionState, 480, 1);
        writerFailure = insertChild.execute();
        disableFailure(&transactionState);
        CHECK(!writerFailure.ok && writerFailure.transactionActiveAfter);
        CHECK(writerFailure.error.message ==
              "index leaf preparation allocation failed");
        CHECK(transactionState.matchingAttempts - firstLeafAttempts == 2);
        CHECK(transactionState.matchingSuccesses - firstLeafMatches == 1);
        CHECK(transactionState.matchingFailures == firstLeafFailures + 1);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->pageStats().imageAllocations ==
                  writerImageAllocations + 1);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == writerPageCount &&
              db->activeTransaction()->finalPageCount() == writerNextPage);
        CHECK(transactionState.outstanding == writerOwnedBytes);
        CHECK(countRows(engine, "ChildRows") == 1);

        // The candidate is complete before staging. Fail the real transaction
        // page-owner allocation after the savepoint clone and heap-page stage.
        CHECK(insertChild.bindText(2, "stage-writer-fail").isOk());
        const uint64_t stageImageAllocations =
            db->activeTransaction()->pageStats().imageAllocations;
        const size_t stageAttempts = transactionState.matchingAttempts;
        const size_t stageSuccesses = transactionState.matchingSuccesses;
        const size_t stageFailures = transactionState.matchingFailures;
        failMatchingAfter(&transactionState, 512, writerPageCount + 1);
        writerFailure = insertChild.execute();
        disableFailure(&transactionState);
        CHECK(!writerFailure.ok && writerFailure.transactionActiveAfter);
        CHECK(writerFailure.error.message == "transaction page staging failed");
        CHECK(transactionState.matchingAttempts - stageAttempts ==
              writerPageCount + 2);
        CHECK(transactionState.matchingSuccesses - stageSuccesses ==
              writerPageCount + 1);
        CHECK(transactionState.matchingFailures == stageFailures + 1);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->pageStats().imageAllocations ==
                  stageImageAllocations + 1);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == writerPageCount &&
              db->activeTransaction()->finalPageCount() == writerNextPage);
        CHECK(transactionState.outstanding == writerOwnedBytes);
        CHECK(countRows(engine, "ChildRows") == 1);

        // Fail the checked hosted view while INSERT reads an existing index
        // leaf. Its status must propagate through the statement savepoint and
        // leave the earlier UPDATE staged.
        CHECK(insertChild.bindText(2, "insert-writer-fail").isOk());
        {
            ScopedHostedAllocationFailure failure(480, 10);
            writerFailure = insertChild.execute();
            CHECK(failure.matchingAttempts() == 11);
            CHECK(failure.matchingSuccesses() == 10);
            CHECK(failure.matchingFailures() == 1);
        }
        CHECK(!writerFailure.ok && writerFailure.transactionActiveAfter);
        CHECK(writerFailure.error.message ==
              "hosted compatibility page payload allocation failed");
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == writerPageCount &&
              db->activeTransaction()->finalPageCount() == writerNextPage);
        CHECK(transactionState.outstanding == writerOwnedBytes);
        CHECK(countRows(engine, "ChildRows") == 1);

        // Let heap staging and the primary leaf replacement complete, then
        // fail the secondary leaf candidate. The two successful 480-byte
        // allocations prove the fault reached the later index after earlier
        // index work had already been staged.
        CHECK(insertChild.bindText(2, "writer-fail").isOk());
        const size_t secondaryLeafMatches = transactionState.matchingSuccesses;
        const size_t secondaryLeafFailures = transactionState.matchingFailures;
        const size_t secondaryLeafAttempts = transactionState.matchingAttempts;
        const uint64_t secondaryLeafImageAllocations =
            db->activeTransaction()->pageStats().imageAllocations;
        failMatchingAfter(&transactionState, 480, 2);
        writerFailure = insertChild.execute();
        disableFailure(&transactionState);
        CHECK(!writerFailure.ok && writerFailure.transactionActiveAfter);
        CHECK(writerFailure.error.message ==
              "index leaf preparation allocation failed");
        CHECK(transactionState.matchingAttempts - secondaryLeafAttempts == 3);
        CHECK(transactionState.matchingSuccesses - secondaryLeafMatches == 2);
        CHECK(transactionState.matchingFailures == secondaryLeafFailures + 1);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->pageStats().imageAllocations ==
                  secondaryLeafImageAllocations + 2);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == writerPageCount &&
              db->activeTransaction()->finalPageCount() == writerNextPage);
        CHECK(transactionState.outstanding == writerOwnedBytes);
        CHECK(countRows(engine, "ChildRows") == 1);

        std::vector<IndexInfo> stagedIndexes;
        CHECK_DB(db->listIndexes(stagedIndexes), DbStatus::Ok);
        for (size_t i = 0; i < stagedIndexes.size(); ++i) {
            if (stagedIndexes[i].tableName == "ChildRows") {
                CHECK(stagedIndexes[i].entryCount == 1);
                IndexValidation validation;
                CHECK_DB(db->validateIndex(stagedIndexes[i].indexId, validation),
                         DbStatus::Ok);
                CHECK(validation.ok && validation.entryCount == 1);
            }
        }
        const SqlExecutionResult retainedPrimaryRow = engine.execute(
            "SELECT Code FROM ChildRows WHERE Id = 1;");
        CHECK(retainedPrimaryRow.ok && retainedPrimaryRow.statements.size() == 1 &&
              retainedPrimaryRow.statements[0].accessPath == "IndexLookup" &&
              retainedPrimaryRow.statements[0].resultSet.rowCount() == 1 &&
              retainedPrimaryRow.statements[0].resultSet.value(0, 0).textValue() ==
                  "existing-code");
        const SqlExecutionResult retainedSecondaryRow = engine.execute(
            "SELECT Id FROM ChildRows WHERE Code = 'existing-code';");
        CHECK(retainedSecondaryRow.ok && retainedSecondaryRow.statements.size() == 1 &&
              retainedSecondaryRow.statements[0].accessPath == "IndexLookup" &&
              retainedSecondaryRow.statements[0].resultSet.rowCount() == 1 &&
              retainedSecondaryRow.statements[0].resultSet.value(0, 0).int64Value() ==
                  1);
        const SqlExecutionResult priorAfterWriterFailure = engine.execute(
            "SELECT V FROM PriorRows WHERE V = 'commit-probe';");
        CHECK(priorAfterWriterFailure.ok &&
              priorAfterWriterFailure.statements[0].resultSet.rowCount() == 1 &&
              priorAfterWriterFailure.statements[0].accessPath == "IndexLookup");

        // The failed statement left the explicit transaction usable. FK
        // rejection still happens before mutation, then a prepared retry with
        // a valid parent updates every child index and can commit.
        const SqlExecutionResult invalidForeignKey = engine.execute(
            "INSERT INTO ChildRows VALUES (11, 999, 'bad-fk');");
        CHECK(!invalidForeignKey.ok && invalidForeignKey.transactionActiveAfter);
        CHECK(invalidForeignKey.error.message.find("foreign key constraint violation") !=
              std::string::npos);
        CHECK(countRows(engine, "ChildRows") == 1);

        CHECK(insertChild.bindInt64(0, 10).isOk());
        CHECK(insertChild.bindInt64(1, 1).isOk());
        CHECK(insertChild.bindText(2, "valid-code").isOk());
        CHECK(insertChild.execute().ok);
        CHECK(countRows(engine, "ChildRows") == 2);
        checkIndexes(*db);
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);

        std::vector<IndexInfo> indexes;
        CHECK_DB(db->listIndexes(indexes), DbStatus::Ok);
        uint32_t primaryIndexId = 0;
        uint32_t codeIndexId = 0;
        uint64_t primaryRootPageId = 0;
        uint64_t codeRootPageId = 0;
        for (size_t i = 0; i < indexes.size(); ++i) {
            if (indexes[i].tableName == "ChildRows") {
                CHECK(indexes[i].entryCount == 2);
                if (indexes[i].columnName == "Id") {
                    primaryIndexId = indexes[i].indexId;
                    primaryRootPageId = indexes[i].rootPageId;
                }
                if (indexes[i].columnName == "Code") {
                    codeIndexId = indexes[i].indexId;
                    codeRootPageId = indexes[i].rootPageId;
                }
            }
        }
        CHECK(primaryIndexId != 0 && codeIndexId != 0);
        std::vector<uint8_t> encodedKey;
        std::vector<RowLocator> primaryLocators;
        std::vector<RowLocator> codeLocators;
        CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(10), encodedKey));
        CHECK_DB(db->indexLookup(primaryIndexId, encodedKey, primaryLocators),
                 DbStatus::Ok);
        CHECK(encodeIndexKey(DbType::Text, DbValue::text("valid-code"), encodedKey));
        CHECK_DB(db->indexLookup(codeIndexId, encodedKey, codeLocators), DbStatus::Ok);
        CHECK(primaryLocators.size() == 1 && codeLocators.size() == 1);
        if (primaryLocators.size() == 1 && codeLocators.size() == 1) {
            CHECK(primaryLocators[0].pageId == codeLocators[0].pageId &&
                  primaryLocators[0].slot == codeLocators[0].slot);

            std::vector<uint8_t> primaryKey10;
            std::vector<uint8_t> primaryKey1;
            CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(10), primaryKey10));
            CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(1), primaryKey1));
            std::vector<RowLocator> oldPrimaryLocators;
            CHECK_DB(db->indexLookup(primaryIndexId, primaryKey1,
                                     oldPrimaryLocators), DbStatus::Ok);
            CHECK(oldPrimaryLocators.size() == 1);
            std::vector<IndexEntry> primaryEntries;
            if (oldPrimaryLocators.size() == 1) {
                IndexEntry oldEntry;
                oldEntry.key = primaryKey1;
                oldEntry.locator = oldPrimaryLocators[0];
                primaryEntries.push_back(oldEntry);
            }
            IndexEntry newPrimaryEntry;
            newPrimaryEntry.key = primaryKey10;
            newPrimaryEntry.locator = primaryLocators[0];
            primaryEntries.push_back(newPrimaryEntry);
            DatabasePage primaryLeaf;
            CHECK_DB(db->pages().readPage(primaryRootPageId, primaryLeaf),
                     DbStatus::Ok);
            CHECK(primaryLeaf.payload == expectedIndexLeaf(
                      512, primaryIndexId, primaryEntries));

            std::vector<uint8_t> codeKey;
            std::vector<uint8_t> oldCodeKey;
            CHECK(encodeIndexKey(DbType::Text, DbValue::text("valid-code"), codeKey));
            CHECK(encodeIndexKey(DbType::Text, DbValue::text("existing-code"),
                                 oldCodeKey));
            std::vector<RowLocator> oldCodeLocators;
            CHECK_DB(db->indexLookup(codeIndexId, oldCodeKey, oldCodeLocators),
                     DbStatus::Ok);
            CHECK(oldCodeLocators.size() == 1);
            std::vector<IndexEntry> codeEntries;
            if (oldCodeLocators.size() == 1) {
                IndexEntry oldEntry;
                oldEntry.key = oldCodeKey;
                oldEntry.locator = oldCodeLocators[0];
                codeEntries.push_back(oldEntry);
            }
            IndexEntry newCodeEntry;
            newCodeEntry.key = codeKey;
            newCodeEntry.locator = codeLocators[0];
            codeEntries.push_back(newCodeEntry);
            DatabasePage codeLeaf;
            CHECK_DB(db->pages().readPage(codeRootPageId, codeLeaf), DbStatus::Ok);
            CHECK(codeLeaf.payload == expectedIndexLeaf(
                      512, codeIndexId, codeEntries));
        }

        // The primary key and unique secondary key still reject duplicates
        // before any heap or index page mutation is published.
        const SqlExecutionResult duplicatePrimary = engine.execute(
            "INSERT INTO ChildRows VALUES (10, 1, 'other-code');");
        CHECK(!duplicatePrimary.ok && duplicatePrimary.transactionActiveAfter &&
              duplicatePrimary.error.message.find(
                  "unique index constraint violation") != std::string::npos);
        const SqlExecutionResult duplicateUniqueSecondary = engine.execute(
            "INSERT INTO ChildRows VALUES (12, 1, 'valid-code');");
        CHECK(!duplicateUniqueSecondary.ok &&
              duplicateUniqueSecondary.transactionActiveAfter &&
              duplicateUniqueSecondary.error.message.find(
                  "unique index constraint violation") != std::string::npos);
        CHECK(countRows(engine, "ChildRows") == 2);
        CHECK(db->validateIntegrity(integrity).isOk());
        CHECK(runSql(engine, "COMMIT;"));
        CHECK(!engine.inTransaction());

        // A later successful existing-leaf INSERT remains transaction-owned
        // and disappears from both indexes and the heap on explicit rollback.
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "INSERT INTO ChildRows VALUES (20, 1, 'rollback-code');"));
        CHECK(countRows(engine, "ChildRows") == 3);
        const SqlExecutionResult rollbackProbe = engine.execute(
            "SELECT Id FROM ChildRows WHERE Code = 'rollback-code';");
        CHECK(rollbackProbe.ok && rollbackProbe.statements.size() == 1 &&
              rollbackProbe.statements[0].accessPath == "IndexLookup" &&
              rollbackProbe.statements[0].resultSet.rowCount() == 1);
        CHECK(runSql(engine, "ROLLBACK;"));
        CHECK(countRows(engine, "ChildRows") == 2);
        const SqlExecutionResult rolledBackIndexProbe = engine.execute(
            "SELECT Id FROM ChildRows WHERE Code = 'rollback-code';");
        CHECK(rolledBackIndexProbe.ok && rolledBackIndexProbe.statements.size() == 1 &&
              rolledBackIndexProbe.statements[0].resultSet.rowCount() == 0);
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
        checkIndexes(*db);
    }

    CHECK_DB(db->close(), DbStatus::Ok);
    db.reset();
    CHECK(cacheState.outstanding == 0);
    CHECK(transactionState.outstanding == 0);

    DatabaseOpenOptions openOptions;
    openOptions.pageCacheAllocator = &cacheAllocator;
    openOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> reopened;
    CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
    if (reopened) {
        {
            SqlEngine engine(*reopened);
            CHECK(countRows(engine, "ChildRows") == 2);
            const SqlExecutionResult prior = engine.execute(
                "SELECT V FROM PriorRows WHERE Id = 1;");
            CHECK(prior.ok && prior.statements.size() == 1 &&
                  prior.statements[0].resultSet.value(0, 0).textValue() ==
                      "commit-probe");
            const SqlExecutionResult byPrimary = engine.execute(
                "SELECT Code FROM ChildRows WHERE Id = 10;");
            CHECK(byPrimary.ok && byPrimary.statements.size() == 1 &&
                  byPrimary.statements[0].accessPath == "IndexLookup" &&
                  byPrimary.statements[0].resultSet.rowCount() == 1 &&
                  byPrimary.statements[0].resultSet.value(0, 0).textValue() ==
                      "valid-code");
            const SqlExecutionResult bySecondary = engine.execute(
                "SELECT Id FROM ChildRows WHERE Code = 'valid-code';");
            CHECK(bySecondary.ok && bySecondary.statements.size() == 1 &&
                  bySecondary.statements[0].accessPath == "IndexLookup" &&
                  bySecondary.statements[0].resultSet.rowCount() == 1 &&
                  bySecondary.statements[0].resultSet.value(0, 0).int64Value() == 10);
            checkIndexes(*reopened);
            std::string integrity;
            CHECK_DB(reopened->validateIntegrity(integrity), DbStatus::Ok);
        }
        CHECK_DB(reopened->close(), DbStatus::Ok);
    }
    reopened.reset();
    CHECK(cacheState.outstanding == 0);
    CHECK(transactionState.outstanding == 0);
    removeDatabase(path);
}

std::string appendInsertSql(int64_t id, char value) {
    return "INSERT INTO AppendRows VALUES (" + std::to_string(id) + ", '" +
           std::string(40, value) + "');";
}

bool lookupAppendLocator(Database& db, uint32_t indexId, int64_t id,
                         RowLocator* outLocator) {
    std::vector<uint8_t> key;
    if (!encodeIndexKey(DbType::Int64, DbValue::int64(id), key)) return false;
    std::vector<RowLocator> locators;
    const DbResult result = db.indexLookup(indexId, key, locators);
    if (!result.isOk() || locators.size() != 1) return false;
    if (outLocator != nullptr) *outLocator = locators[0];
    return true;
}

bool samePagePayload(const DatabasePage& left, const DatabasePage& right) {
    return left.pageId == right.pageId && left.type == right.type &&
           left.flags == right.flags && left.payloadSize == right.payloadSize &&
           left.generation == right.generation && left.payload == right.payload;
}

void testHeapTailResolutionAllocationFailure() {
    std::printf("NDB1R4P11R3J heap tail resolution allocation failure\n");
    const std::string path = testPath() + ".r3j-tail-resolution.gxdb";
    removeDatabase(path);

    std::unique_ptr<Database> db;
    CHECK_DB(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine, "CREATE TABLE HeapTailRows (Id INT64 NOT NULL);"));
        CHECK(runSql(engine, "INSERT INTO HeapTailRows VALUES (1);"));

        std::unique_ptr<Table> table;
        CHECK_DB(db->openTable("HeapTailRows", table), DbStatus::Ok);
        CHECK(table != nullptr && table->heapPageCount() == 1);
        CHECK(runSql(engine, "BEGIN;"));

        std::vector<DbValue> values;
        values.push_back(DbValue::int64(2));
        DbResult failedAppend;
        {
            ScopedHostedAllocationFailure failure(8, 0);
            failedAppend = table->insert(values);
            CHECK(failure.matchingAttempts() == 1);
            CHECK(failure.matchingFailures() == 1);
        }
        CHECK(!failedAppend.isOk() && failedAppend.status() == DbStatus::NoSpace &&
              failedAppend.message() == "heap tail resolution allocation failed");
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == 0);
        CHECK(table->heapPageCount() == 1);
        CHECK(countRows(engine, "HeapTailRows") == 1);

        DbResult failedTailRead;
        {
            ScopedHostedAllocationFailure failure(4064, 0);
            failedTailRead = table->insert(values);
            CHECK(failure.matchingAttempts() == 1);
            CHECK(failure.matchingFailures() == 1);
        }
        CHECK(!failedTailRead.isOk() &&
              failedTailRead.status() == DbStatus::NoSpace &&
              failedTailRead.message() ==
                  "hosted compatibility page payload allocation failed");
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == 0);
        CHECK(table->heapPageCount() == 1);
        CHECK(countRows(engine, "HeapTailRows") == 1);

        // Retry through the same Table object. Failed chain walks and checked
        // reads must not leave its tail cache marked as fully resolved.
        CHECK_DB(table->insert(values), DbStatus::Ok);
        CHECK(table->heapPageCount() == 1);
        CHECK(countRows(engine, "HeapTailRows") == 2);
        CHECK(runSql(engine, "COMMIT;"));
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
    }

    CHECK_DB(db->close(), DbStatus::Ok);
    db.reset();
    removeDatabase(path);
}

void testCheckedHeapAppendPageSize(uint32_t pageSize) {
    std::printf("NDB1R4P11R3J SQL append publication on %u-byte pages\n",
                pageSize);
    const std::string suffix = pageSize == 512 ? ".r3j512.gxdb" : ".r3j4096.gxdb";
    const std::string path = testPath() + suffix;
    removeDatabase(path);

    AllocationState cacheState;
    AllocationState transactionState;
    GxAllocator cacheAllocator = makeAllocator(&cacheState);
    GxAllocator transactionAllocator = makeAllocator(&transactionState);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = pageSize;
    createOptions.bufferCapacity = 128;
    createOptions.pageCacheAllocator = &cacheAllocator;
    createOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> db;
    CHECK_DB(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    uint32_t primaryIndexId = 0;
    uint32_t valueIndexId = 0;
    const size_t capacity = pageSize - kPageHeaderSize;
    const size_t maxRowsPerPage = (capacity - kHeapHeaderSize) / (61u + 8u);
    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE AppendRows (Id INT64 PRIMARY KEY, V TEXT NOT NULL);"));
        CHECK(runSql(engine,
                     "CREATE INDEX IX_AppendRows_V ON AppendRows (V);"));
        CHECK(runSql(engine, appendInsertSql(1, 'a')));
        CHECK(runSql(engine, appendInsertSql(2, 'b')));

        std::vector<IndexInfo> indexes;
        CHECK_DB(db->listIndexes(indexes), DbStatus::Ok);
        for (size_t i = 0; i < indexes.size(); ++i) {
            if (indexes[i].tableName == "AppendRows" &&
                indexes[i].columnName == "Id") {
                primaryIndexId = indexes[i].indexId;
            } else if (indexes[i].tableName == "AppendRows" &&
                       indexes[i].columnName == "V") {
                valueIndexId = indexes[i].indexId;
            }
        }
        CHECK(primaryIndexId != 0 && valueIndexId != 0);

        // The earlier UPDATE and a failed existing-tail candidate share one
        // explicit transaction. The failed append must preserve the exact
        // staged tail bytes and the existing index locator.
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE AppendRows SET V = '" + std::string(40, 'p') +
                     "' WHERE Id = 1;"));
        Transaction* tx = db->activeTransaction();
        CHECK(tx != nullptr);
        const uint32_t baselineModified = tx ? tx->modifiedPageCount() : 0;
        const uint64_t baselinePageCount = tx ? tx->finalPageCount() : 0;
        const size_t baselineOwnedBytes = transactionState.outstanding;
        RowLocator priorLocator;
        CHECK(lookupAppendLocator(*db, primaryIndexId, 1, &priorLocator));
        DatabasePage priorTail;
        CHECK_DB(db->pages().readPage(priorLocator.pageId, priorTail),
                 DbStatus::Ok);

        const size_t candidateFailuresBefore = transactionState.matchingFailures;
        failMatchingAfter(&transactionState, capacity, 0);
        const SqlExecutionResult existingCandidateFailure =
            engine.execute(appendInsertSql(3, 'c'));
        disableFailure(&transactionState);
        CHECK(!existingCandidateFailure.ok &&
              existingCandidateFailure.transactionActiveAfter);
        CHECK(existingCandidateFailure.error.message ==
              "heap page preparation allocation failed");
        CHECK(transactionState.matchingFailures == candidateFailuresBefore + 1);
        CHECK(transactionState.outstanding == baselineOwnedBytes);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == baselineModified &&
              db->activeTransaction()->finalPageCount() == baselinePageCount);
        CHECK(countRows(engine, "AppendRows") == 2);
        const SqlExecutionResult priorUpdate = engine.execute(
            "SELECT V FROM AppendRows WHERE Id = 1;");
        CHECK(priorUpdate.ok && priorUpdate.statements.size() == 1 &&
              priorUpdate.statements[0].resultSet.value(0, 0).textValue() ==
                  std::string(40, 'p'));
        CHECK(!lookupAppendLocator(*db, primaryIndexId, 3, nullptr));
        RowLocator priorLocatorAfter;
        CHECK(lookupAppendLocator(*db, primaryIndexId, 1, &priorLocatorAfter));
        CHECK(priorLocator.pageId == priorLocatorAfter.pageId &&
              priorLocator.slot == priorLocatorAfter.slot);
        DatabasePage priorTailAfter;
        CHECK_DB(db->pages().readPage(priorLocator.pageId, priorTailAfter),
                 DbStatus::Ok);
        CHECK(samePagePayload(priorTail, priorTailAfter));
        checkIndexes(*db);

        // A retry publishes the row and both index entries, then commits.
        CHECK(runSql(engine, appendInsertSql(3, 'r')));
        CHECK(countRows(engine, "AppendRows") == 3);
        CHECK(runSql(engine, "COMMIT;"));

        // Fill the current page to its real row/slot boundary. The next
        // 61-byte encoded row must use a new heap page.
        for (size_t i = 3; i < maxRowsPerPage; ++i) {
            CHECK(runSql(engine,
                         appendInsertSql(static_cast<int64_t>(100 + i), 'f')));
        }
        CHECK(countRows(engine, "AppendRows") ==
              static_cast<int64_t>(maxRowsPerPage));
        std::unique_ptr<Table> table;
        CHECK_DB(db->openTable("AppendRows", table), DbStatus::Ok);
        CHECK(table != nullptr && table->heapPageCount() == 1);

        // A new-page candidate allocation failure happens after the successful
        // UPDATE but before any page ID or page image is staged.
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE AppendRows SET V = '" + std::string(40, 'q') +
                     "' WHERE Id = 1;"));
        tx = db->activeTransaction();
        CHECK(tx != nullptr);
        const uint32_t beforeNewCandidateModified =
            tx ? tx->modifiedPageCount() : 0;
        const uint64_t beforeNewCandidatePageCount =
            tx ? tx->finalPageCount() : 0;
        const size_t beforeNewCandidateBytes = transactionState.outstanding;
        const size_t newCandidateFailuresBefore =
            transactionState.matchingFailures;
        failMatchingAfter(&transactionState, capacity, 0);
        const SqlExecutionResult newCandidateFailure =
            engine.execute(appendInsertSql(900, 'n'));
        disableFailure(&transactionState);
        CHECK(!newCandidateFailure.ok &&
              newCandidateFailure.transactionActiveAfter);
        CHECK(newCandidateFailure.error.message ==
              "heap page preparation allocation failed");
        CHECK(transactionState.matchingFailures ==
              newCandidateFailuresBefore + 1);
        CHECK(transactionState.outstanding == beforeNewCandidateBytes);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() ==
                  beforeNewCandidateModified &&
              db->activeTransaction()->finalPageCount() ==
                  beforeNewCandidatePageCount);
        CHECK(table->heapPageCount() == 1);
        CHECK(countRows(engine, "AppendRows") ==
              static_cast<int64_t>(maxRowsPerPage));
        CHECK(!lookupAppendLocator(*db, primaryIndexId, 900, nullptr));
        const SqlExecutionResult priorNewCandidateUpdate = engine.execute(
            "SELECT V FROM AppendRows WHERE Id = 1;");
        CHECK(priorNewCandidateUpdate.ok &&
              priorNewCandidateUpdate.statements[0].resultSet.value(0, 0)
                      .textValue() == std::string(40, 'q'));

        // Fail the real transaction-owned page image allocation. The staged
        // tail, page count, and earlier UPDATE remain exactly as they were.
        const size_t stageFailuresBefore = transactionState.matchingFailures;
        const size_t stageMatchesBefore = transactionState.matchingSuccesses;
        failMatchingAfter(&transactionState, pageSize,
                          beforeNewCandidateModified);
        const SqlExecutionResult newPageStageFailure =
            engine.execute(appendInsertSql(901, 's'));
        disableFailure(&transactionState);
        CHECK(!newPageStageFailure.ok && newPageStageFailure.transactionActiveAfter);
        CHECK(newPageStageFailure.error.message ==
              "transaction page staging failed");
        CHECK(transactionState.matchingSuccesses - stageMatchesBefore ==
              beforeNewCandidateModified);
        CHECK(transactionState.matchingFailures == stageFailuresBefore + 1);
        CHECK(transactionState.outstanding == beforeNewCandidateBytes);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() ==
                  beforeNewCandidateModified &&
              db->activeTransaction()->finalPageCount() ==
                  beforeNewCandidatePageCount);
        CHECK(table->heapPageCount() == 1);
        CHECK(countRows(engine, "AppendRows") ==
              static_cast<int64_t>(maxRowsPerPage));
        CHECK(!lookupAppendLocator(*db, primaryIndexId, 901, nullptr));
        checkIndexes(*db);

        // Let the new page stage succeed and fail the subsequent tail-link
        // replacement. The statement savepoint must remove that staged page.
        const size_t linkFailuresBefore = transactionState.matchingFailures;
        const size_t linkMatchesBefore = transactionState.matchingSuccesses;
        failMatchingAfter(&transactionState, pageSize,
                          beforeNewCandidateModified + 1u);
        const SqlExecutionResult tailLinkFailure =
            engine.execute(appendInsertSql(902, 'l'));
        disableFailure(&transactionState);
        CHECK(!tailLinkFailure.ok && tailLinkFailure.transactionActiveAfter);
        CHECK(tailLinkFailure.error.message ==
              "transaction page staging failed");
        CHECK(transactionState.matchingSuccesses - linkMatchesBefore ==
              beforeNewCandidateModified + 1u);
        CHECK(transactionState.matchingFailures == linkFailuresBefore + 1);
        CHECK(transactionState.outstanding == beforeNewCandidateBytes);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() ==
                  beforeNewCandidateModified &&
              db->activeTransaction()->finalPageCount() ==
                  beforeNewCandidatePageCount);
        CHECK(table->heapPageCount() == 1);
        CHECK(countRows(engine, "AppendRows") ==
              static_cast<int64_t>(maxRowsPerPage));
        CHECK(!lookupAppendLocator(*db, primaryIndexId, 902, nullptr));
        checkIndexes(*db);

        // Retry the failed new-page append, verify both index paths and commit.
        CHECK(runSql(engine, appendInsertSql(902, 'l')));
        CHECK(countRows(engine, "AppendRows") ==
              static_cast<int64_t>(maxRowsPerPage + 1u));
        CHECK(table->heapPageCount() == 2);
        checkIndexes(*db);
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
        CHECK(runSql(engine, "COMMIT;"));

        // Explicit transaction rollback removes a later failed statement and
        // its successful preceding UPDATE from the transaction as a whole.
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE AppendRows SET V = '" + std::string(40, 'z') +
                     "' WHERE Id = 1;"));
        tx = db->activeTransaction();
        CHECK(tx != nullptr);
        const uint32_t rollbackModified = tx ? tx->modifiedPageCount() : 0;
        const uint64_t rollbackPageCount = tx ? tx->finalPageCount() : 0;
        const size_t rollbackBytes = transactionState.outstanding;
        failMatchingAfter(&transactionState, capacity, 0);
        const SqlExecutionResult failedBeforeExplicitRollback =
            engine.execute(appendInsertSql(903, 'x'));
        disableFailure(&transactionState);
        CHECK(!failedBeforeExplicitRollback.ok &&
              failedBeforeExplicitRollback.transactionActiveAfter);
        CHECK(transactionState.outstanding == rollbackBytes);
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == rollbackModified &&
              db->activeTransaction()->finalPageCount() == rollbackPageCount);
        CHECK(countRows(engine, "AppendRows") ==
              static_cast<int64_t>(maxRowsPerPage + 1u));
        const SqlExecutionResult priorBeforeRollback = engine.execute(
            "SELECT V FROM AppendRows WHERE Id = 1;");
        CHECK(priorBeforeRollback.ok &&
              priorBeforeRollback.statements[0].resultSet.value(0, 0)
                      .textValue() == std::string(40, 'z'));
        CHECK(runSql(engine, "ROLLBACK;"));
        const SqlExecutionResult priorAfterRollback = engine.execute(
            "SELECT V FROM AppendRows WHERE Id = 1;");
        CHECK(priorAfterRollback.ok &&
              priorAfterRollback.statements[0].resultSet.value(0, 0)
                      .textValue() == std::string(40, 'q'));
        CHECK(!lookupAppendLocator(*db, primaryIndexId, 903, nullptr));
        CHECK(countRows(engine, "AppendRows") ==
              static_cast<int64_t>(maxRowsPerPage + 1u));
        checkIndexes(*db);
    }

    CHECK_DB(db->close(), DbStatus::Ok);
    db.reset();
    CHECK(cacheState.outstanding == 0);
    CHECK(transactionState.outstanding == 0);

    DatabaseOpenOptions openOptions;
    openOptions.pageCacheAllocator = &cacheAllocator;
    openOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> reopened;
    CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
    if (reopened) {
        {
            SqlEngine engine(*reopened);
            CHECK(countRows(engine, "AppendRows") ==
                  static_cast<int64_t>(maxRowsPerPage + 1u));
            const SqlExecutionResult byPrimary = engine.execute(
                "SELECT V FROM AppendRows WHERE Id = 902;");
            CHECK(byPrimary.ok && byPrimary.statements.size() == 1 &&
                  byPrimary.statements[0].accessPath == "IndexLookup" &&
                  byPrimary.statements[0].resultSet.rowCount() == 1 &&
                  byPrimary.statements[0].resultSet.value(0, 0).textValue() ==
                      std::string(40, 'l'));
            const SqlExecutionResult bySecondary = engine.execute(
                "SELECT Id FROM AppendRows WHERE V = '" +
                std::string(40, 'l') + "';");
            CHECK(bySecondary.ok && bySecondary.statements.size() == 1 &&
                  bySecondary.statements[0].accessPath == "IndexLookup" &&
                  bySecondary.statements[0].resultSet.rowCount() == 1 &&
                  bySecondary.statements[0].resultSet.value(0, 0).int64Value() ==
                      902);
            RowLocator primaryLocator;
            RowLocator secondaryLocator;
            CHECK(lookupAppendLocator(*reopened, primaryIndexId, 902,
                                      &primaryLocator));
            std::vector<uint8_t> secondaryKey;
            CHECK(encodeIndexKey(DbType::Text,
                                 DbValue::text(std::string(40, 'l')),
                                 secondaryKey));
            std::vector<RowLocator> secondaryLocators;
            CHECK_DB(reopened->indexLookup(valueIndexId, secondaryKey,
                                           secondaryLocators), DbStatus::Ok);
            CHECK(secondaryLocators.size() == 1);
            if (secondaryLocators.size() == 1) {
                secondaryLocator = secondaryLocators[0];
                CHECK(primaryLocator.pageId == secondaryLocator.pageId &&
                      primaryLocator.slot == secondaryLocator.slot);
            }
            const SqlExecutionResult prior = engine.execute(
                "SELECT V FROM AppendRows WHERE Id = 1;");
            CHECK(prior.ok && prior.statements.size() == 1 &&
                  prior.statements[0].resultSet.value(0, 0).textValue() ==
                      std::string(40, 'q'));
            std::unique_ptr<Table> table;
            CHECK_DB(reopened->openTable("AppendRows", table), DbStatus::Ok);
            CHECK(table != nullptr && table->heapPageCount() == 2);
            checkIndexes(*reopened);
            std::string integrity;
            CHECK_DB(reopened->validateIntegrity(integrity), DbStatus::Ok);
        }
        CHECK_DB(reopened->close(), DbStatus::Ok);
    }
    reopened.reset();
    CHECK(cacheState.outstanding == 0);
    CHECK(transactionState.outstanding == 0);
    removeDatabase(path);
}

void testCheckedHeapAppendPublication() {
    testHeapTailResolutionAllocationFailure();
    testCheckedHeapAppendPageSize(512);
    testCheckedHeapAppendPageSize(4096);
}

bool findIndexInfo(Database& db, const std::string& tableName,
                   const std::string& columnName, IndexInfo& out) {
    std::vector<IndexInfo> indexes;
    if (!db.listIndexes(indexes).isOk()) return false;
    for (size_t i = 0; i < indexes.size(); ++i) {
        if (indexes[i].tableName == tableName &&
            indexes[i].columnName == columnName) {
            out = indexes[i];
            return true;
        }
    }
    return false;
}

bool findIndexChild(const DatabasePage& page, uint32_t capacity, uint32_t index,
                    uint64_t& outPageId, std::vector<uint8_t>& outLowerBound) {
    if (page.type != PageType::IndexInternal || page.payload.size() != capacity ||
        index >= loadLe32(page.payload.data() + 16)) {
        return false;
    }
    const uint32_t slotOffset = capacity -
        kIndexSlotSize * (index + 1u);
    const uint32_t childOffset = loadLe32(page.payload.data() + slotOffset);
    const uint32_t childLength = loadLe32(page.payload.data() + slotOffset + 4);
    if (childOffset > capacity || childLength > capacity - childOffset ||
        childLength < kIndexInternalEntryOverhead) {
        return false;
    }
    const uint8_t* child = page.payload.data() + childOffset;
    const uint32_t keyLength = loadLe32(child + 8);
    if (keyLength + kIndexInternalEntryOverhead != childLength) return false;
    outPageId = loadLe64(child);
    outLowerBound.assign(child + 12, child + 12 + keyLength);
    return true;
}

std::string rootSplitTextKey(uint32_t textBytes, uint32_t ordinal) {
    std::string suffix = std::to_string(ordinal);
    if (suffix.size() > 8 || textBytes < 8) return std::string();
    suffix.insert(suffix.begin(), 8 - suffix.size(), '0');
    return std::string(textBytes - 8, 'x') + suffix;
}

uint32_t maxInternalChildrenForSeparator(uint32_t pageCapacity,
                                         uint32_t separatorBytes) {
    uint32_t count = 0;
    for (uint32_t candidate = 2; candidate < pageCapacity; ++candidate) {
        const uint64_t used = kIndexInternalHeaderSize +
            static_cast<uint64_t>(candidate) * kIndexSlotSize +
            static_cast<uint64_t>(candidate) * kIndexInternalEntryOverhead +
            static_cast<uint64_t>(candidate - 1) * separatorBytes;
        if (used > pageCapacity) break;
        count = candidate;
    }
    return count;
}

uint32_t maxLeafEntriesForKey(uint32_t pageCapacity, uint32_t keyBytes) {
    uint32_t count = 0;
    for (uint32_t candidate = 1; candidate < pageCapacity; ++candidate) {
        const uint64_t used = kIndexLeafHeaderSize +
            static_cast<uint64_t>(candidate) * kIndexSlotSize +
            static_cast<uint64_t>(candidate) *
                (kIndexLeafEntryOverhead + static_cast<uint64_t>(keyBytes));
        if (used > pageCapacity) break;
        count = candidate;
    }
    return count;
}

bool readInternalChildren(Database& db, uint64_t pageId, uint32_t pageCapacity,
                          std::vector<uint64_t>& childIds,
                          std::vector<std::vector<uint8_t> >& lowerBounds) {
    childIds.clear();
    lowerBounds.clear();
    DatabasePage page;
    if (!db.pages().readPage(pageId, page).isOk() ||
        page.type != PageType::IndexInternal ||
        page.payload.size() != pageCapacity) {
        return false;
    }
    const uint32_t childCount = loadLe32(page.payload.data() + 16);
    childIds.reserve(childCount);
    lowerBounds.reserve(childCount);
    for (uint32_t i = 0; i < childCount; ++i) {
        uint64_t childId = 0;
        std::vector<uint8_t> lowerBound;
        if (!findIndexChild(page, pageCapacity, i, childId, lowerBound)) {
            return false;
        }
        childIds.push_back(childId);
        lowerBounds.push_back(lowerBound);
    }
    return true;
}

bool internalCanAcceptSeparator(Database& db, uint64_t pageId,
                                uint32_t pageCapacity,
                                size_t separatorBytes) {
    DatabasePage page;
    if (!db.pages().readPage(pageId, page).isOk() ||
        page.type != PageType::IndexInternal ||
        page.payload.size() != pageCapacity) {
        return false;
    }
    const uint32_t childCount = loadLe32(page.payload.data() + 16);
    uint64_t used = kIndexInternalHeaderSize +
        static_cast<uint64_t>(childCount + 1) * kIndexSlotSize +
        kIndexInternalEntryOverhead + separatorBytes;
    for (uint32_t i = 0; i < childCount; ++i) {
        const uint32_t slotOffset =
            pageCapacity - kIndexSlotSize * (i + 1u);
        const uint32_t offset = loadLe32(page.payload.data() + slotOffset);
        const uint32_t length =
            loadLe32(page.payload.data() + slotOffset + 4);
        if (offset > pageCapacity || length > pageCapacity - offset ||
            length < kIndexInternalEntryOverhead) {
            return false;
        }
        used += length;
    }
    return used <= pageCapacity;
}

size_t rootInsertHeapStageCount(Database& db, uint32_t pageSize,
                                uint32_t textBytes, bool& outAllocatesPage) {
    outAllocatesPage = false;
    const Catalog::TableRecord* table = db.catalog().findTable("RootRows");
    if (table == nullptr || table->firstHeapPageId == 0) return 0;
    uint64_t pageId = table->firstHeapPageId;
    DatabasePage page;
    uint64_t visited = 0;
    for (;;) {
        if (++visited > db.pageCount() ||
            !db.pages().readPage(pageId, page).isOk() ||
            page.type != PageType::Data || page.payload.size() !=
                DatabasePage::payloadCapacity(pageSize)) {
            return 0;
        }
        const uint64_t next = loadLe64(page.payload.data() + kHeapNextPageOffset);
        if (next == 0) break;
        pageId = next;
    }
    const uint32_t slots = loadLe32(page.payload.data() + kHeapSlotCountOffset);
    const uint32_t rowAreaEnd = loadLe32(page.payload.data() + kHeapRowAreaEndOffset);
    const uint64_t encodedRowBytes = 8u + 1u + 2u * 4u +
        static_cast<uint64_t>(2) * textBytes;
    const uint64_t usedAfterAppend = rowAreaEnd + encodedRowBytes +
        static_cast<uint64_t>(slots + 1u) * kIndexSlotSize;
    if (usedAfterAppend <= page.payload.size()) return 1;
    outAllocatesPage = true;
    return 2; // New data page plus the old tail's next-page replacement.
}

bool readLeafFirstPhysicalKey(const DatabasePage& page, uint32_t capacity,
                              std::vector<uint8_t>& outPhysicalKey) {
    if (page.type != PageType::IndexLeaf || page.payload.size() != capacity ||
        loadLe32(page.payload.data() + 16) == 0) {
        return false;
    }
    const uint32_t slotOffset = capacity - kIndexSlotSize;
    const uint32_t entryOffset = loadLe32(page.payload.data() + slotOffset);
    const uint32_t entryLength = loadLe32(page.payload.data() + slotOffset + 4);
    if (entryOffset > capacity || entryLength > capacity - entryOffset ||
        entryLength < kIndexLeafEntryOverhead) {
        return false;
    }
    const uint8_t* entry = page.payload.data() + entryOffset;
    const uint32_t keyLength = loadLe32(entry);
    if (keyLength + kIndexLeafEntryOverhead != entryLength) return false;
    IndexEntry first;
    first.key.assign(entry + 4, entry + 4 + keyLength);
    first.locator.pageId = loadLe64(entry + 4 + keyLength);
    first.locator.slot = loadLe32(entry + 4 + keyLength + 8);
    outPhysicalKey = IndexTree::makePhysicalKey(first.key, first.locator);
    return true;
}

void checkSplitIndexShape(Database& db, uint32_t pageSize,
                          const IndexInfo& primaryIndex,
                          const IndexInfo& secondaryIndex,
                          int32_t insertedId,
                          uint64_t expectedFirstNewLeafId,
                          uint32_t splitCount) {
    const uint32_t expectedLeafCount = 2 + splitCount;
    IndexValidation primaryValidation;
    CHECK_DB(db.validateIndex(primaryIndex.indexId, primaryValidation),
             DbStatus::Ok);
    CHECK(primaryValidation.ok &&
          primaryValidation.entryCount == static_cast<uint64_t>(insertedId) &&
          primaryValidation.height == 2 &&
          primaryValidation.pagesVisited == expectedLeafCount + 1);
    IndexValidation secondaryValidation;
    CHECK_DB(db.validateIndex(secondaryIndex.indexId, secondaryValidation),
             DbStatus::Ok);
    CHECK(secondaryValidation.ok &&
          secondaryValidation.entryCount == static_cast<uint64_t>(insertedId) &&
          secondaryValidation.height == 1 && secondaryValidation.pagesVisited == 1);

    const uint32_t payloadCapacity = pageSize - kPageHeaderSize;
    DatabasePage root;
    CHECK_DB(db.pages().readPage(primaryIndex.rootPageId, root), DbStatus::Ok);
    CHECK(root.type == PageType::IndexInternal);
    CHECK(root.payload.size() == payloadCapacity);
    CHECK(loadLe32(root.payload.data() + 16) == expectedLeafCount);

    uint64_t childIds[4] = {0, 0, 0, 0};
    std::vector<uint8_t> separators[4];
    for (uint32_t i = 0; i < expectedLeafCount; ++i) {
        CHECK(findIndexChild(root, payloadCapacity, i, childIds[i], separators[i]));
    }
    for (uint32_t i = 0; i < expectedLeafCount; ++i) {
        CHECK(childIds[i] != 0);
        for (uint32_t j = 0; j < i; ++j) CHECK(childIds[i] != childIds[j]);
    }
    for (uint32_t i = 2; i < expectedLeafCount; ++i) {
        CHECK(childIds[i] == expectedFirstNewLeafId + (i - 2));
    }
    CHECK(separators[0].empty());

    for (uint32_t i = 1; i < expectedLeafCount; ++i) {
        std::vector<uint8_t> encoded;
        CHECK(encodeIndexKey(DbType::Int32,
                             DbValue::int32(static_cast<int32_t>(i + 1)), encoded));
        std::vector<RowLocator> locators;
        CHECK_DB(db.indexLookup(primaryIndex.indexId, encoded, locators),
                 DbStatus::Ok);
        CHECK(locators.size() == 1);
        if (locators.size() == 1) {
            const std::vector<uint8_t> expected =
                IndexTree::makePhysicalKey(encoded, locators[0]);
            CHECK(separators[i] == expected);
        }
    }

    for (uint32_t i = 0; i < expectedLeafCount; ++i) {
        const int32_t groupStart = static_cast<int32_t>(i + 1);
        const int32_t groupEnd = i + 1 < expectedLeafCount
            ? groupStart : insertedId;
        DatabasePage leaf;
        CHECK_DB(db.pages().readPage(childIds[i], leaf), DbStatus::Ok);
        CHECK(leaf.pageId == childIds[i] && leaf.type == PageType::IndexLeaf);
        const uint64_t leafNext = i + 1 < expectedLeafCount
            ? childIds[i + 1] : 0;
        CHECK(loadLe64(leaf.payload.data() + 8) == leafNext);
        CHECK(loadLe32(leaf.payload.data() + 16) ==
              static_cast<uint32_t>(groupEnd - groupStart + 1));

        std::vector<IndexEntry> expectedEntries;
        for (int32_t id = groupStart; id <= groupEnd; ++id) {
            std::vector<uint8_t> encoded;
            CHECK(encodeIndexKey(DbType::Int32, DbValue::int32(id), encoded));
            std::vector<RowLocator> locators;
            CHECK_DB(db.indexLookup(primaryIndex.indexId, encoded, locators),
                     DbStatus::Ok);
            CHECK(locators.size() == 1);
            if (locators.size() == 1) {
                IndexEntry entry;
                entry.key = encoded;
                entry.locator = locators[0];
                expectedEntries.push_back(entry);
            }
        }
        std::vector<uint8_t> expected =
            expectedIndexLeaf(pageSize, primaryIndex.indexId, expectedEntries);
        storeLe64(expected.data() + 8, leafNext);
        CHECK(leaf.payload == expected);
        std::vector<uint8_t> firstPhysical;
        CHECK(readLeafFirstPhysicalKey(leaf, payloadCapacity, firstPhysical));
        if (i != 0) CHECK(separators[i] == firstPhysical);
    }

    std::vector<uint8_t> trueKey;
    CHECK(encodeIndexKey(DbType::Boolean, DbValue::boolean(true), trueKey));
    std::vector<RowLocator> secondaryLocators;
    CHECK_DB(db.indexLookup(secondaryIndex.indexId, trueKey, secondaryLocators),
             DbStatus::Ok);
    CHECK(secondaryLocators.size() == static_cast<size_t>(insertedId));
    for (int32_t id = 1; id <= insertedId; ++id) {
        std::vector<uint8_t> encoded;
        CHECK(encodeIndexKey(DbType::Int32, DbValue::int32(id), encoded));
        std::vector<RowLocator> primaryLocators;
        CHECK_DB(db.indexLookup(primaryIndex.indexId, encoded, primaryLocators),
                 DbStatus::Ok);
        CHECK(primaryLocators.size() == 1);
        bool secondaryHasLocator = false;
        if (primaryLocators.size() == 1) {
            for (size_t j = 0; j < secondaryLocators.size(); ++j) {
                if (secondaryLocators[j].pageId == primaryLocators[0].pageId &&
                    secondaryLocators[j].slot == primaryLocators[0].slot) {
                    secondaryHasLocator = true;
                    break;
                }
            }
        }
        CHECK(secondaryHasLocator);
    }
}

void checkCheckedRootSplitShape(Database& db, const IndexInfo& index,
                                uint64_t oldRootPageId,
                                const std::vector<uint64_t>& oldLeafIds,
                                uint64_t oldSplitLeafId,
                                uint64_t expectedEntries,
                                uint32_t pageSize,
                                uint64_t* outNewLeafPageId,
                                std::vector<uint8_t>* outPromotedSeparator) {
    const uint32_t pageCapacity = pageSize - kPageHeaderSize;
    const uint32_t maxPhysicalBytes = maxIndexKeyBytes(pageSize) + 12u;
    DatabasePage newRoot;
    CHECK(index.rootPageId != oldRootPageId);
    CHECK_DB(db.pages().readPage(index.rootPageId, newRoot), DbStatus::Ok);
    CHECK(newRoot.type == PageType::IndexInternal &&
          newRoot.payload.size() == pageCapacity &&
          loadLe32(newRoot.payload.data() + 16) == 2);
    if (newRoot.type != PageType::IndexInternal ||
        newRoot.payload.size() != pageCapacity ||
        loadLe32(newRoot.payload.data() + 16) != 2) {
        return;
    }

    std::vector<uint64_t> rootChildIds;
    std::vector<std::vector<uint8_t> > rootBounds;
    CHECK(readInternalChildren(db, index.rootPageId, pageCapacity,
                               rootChildIds, rootBounds));
    if (rootChildIds.size() != 2 || rootBounds.size() != 2) return;
    CHECK(rootChildIds[0] == oldRootPageId && rootBounds[0].empty());
    CHECK(!rootBounds[1].empty() &&
          rootBounds[1].size() == maxPhysicalBytes);

    std::vector<uint64_t> leftLeafIds;
    std::vector<uint64_t> rightLeafIds;
    std::vector<std::vector<uint8_t> > leftBounds;
    std::vector<std::vector<uint8_t> > rightBounds;
    CHECK(readInternalChildren(db, rootChildIds[0], pageCapacity,
                               leftLeafIds, leftBounds));
    CHECK(readInternalChildren(db, rootChildIds[1], pageCapacity,
                               rightLeafIds, rightBounds));
    CHECK(leftLeafIds.size() >= 2 && rightLeafIds.size() >= 2 &&
          leftLeafIds.size() + rightLeafIds.size() == oldLeafIds.size() + 1);
    if (leftLeafIds.size() < 2 || rightLeafIds.size() < 2 ||
        leftLeafIds.size() + rightLeafIds.size() != oldLeafIds.size() + 1) {
        return;
    }
    CHECK(rightBounds[0] == rootBounds[1]);

    std::vector<uint64_t> allLeafIds = leftLeafIds;
    allLeafIds.insert(allLeafIds.end(), rightLeafIds.begin(), rightLeafIds.end());
    uint64_t newLeafId = UINT64_MAX;
    for (size_t i = 0; i < allLeafIds.size(); ++i) {
        for (size_t j = 0; j < i; ++j) CHECK(allLeafIds[j] != allLeafIds[i]);
        if (std::find(oldLeafIds.begin(), oldLeafIds.end(), allLeafIds[i]) ==
            oldLeafIds.end()) {
            CHECK(newLeafId == UINT64_MAX);
            newLeafId = allLeafIds[i];
        }
        DatabasePage leaf;
        CHECK_DB(db.pages().readPage(allLeafIds[i], leaf), DbStatus::Ok);
        CHECK(leaf.type == PageType::IndexLeaf);
        std::vector<uint8_t> firstPhysical;
        CHECK(readLeafFirstPhysicalKey(leaf, pageCapacity, firstPhysical));
        const bool inLeft = i < leftLeafIds.size();
        const size_t localIndex = inLeft ? i : i - leftLeafIds.size();
        const std::vector<uint8_t>& bound = inLeft
            ? leftBounds[localIndex] : rightBounds[localIndex];
        if (localIndex != 0) CHECK(bound == firstPhysical);
        if (!inLeft && localIndex == 0) CHECK(bound == firstPhysical);
    }
    CHECK(newLeafId != UINT64_MAX);
    if (newLeafId == UINT64_MAX) return;
    for (size_t i = 0; i < oldLeafIds.size(); ++i) {
        CHECK(std::count(allLeafIds.begin(), allLeafIds.end(), oldLeafIds[i]) == 1);
    }
    if (outNewLeafPageId != nullptr) *outNewLeafPageId = newLeafId;
    if (outPromotedSeparator != nullptr) *outPromotedSeparator = rootBounds[1];

    DatabasePage oldSplitLeaf;
    DatabasePage newLeaf;
    CHECK_DB(db.pages().readPage(oldSplitLeafId, oldSplitLeaf), DbStatus::Ok);
    CHECK_DB(db.pages().readPage(newLeafId, newLeaf), DbStatus::Ok);
    CHECK(oldSplitLeaf.type == PageType::IndexLeaf &&
          loadLe64(oldSplitLeaf.payload.data() + 8) == newLeafId);
    CHECK(newLeaf.type == PageType::IndexLeaf &&
          loadLe64(newLeaf.payload.data() + 8) == 0);

    IndexValidation validation;
    CHECK_DB(db.validateIndex(index.indexId, validation), DbStatus::Ok);
    CHECK(validation.ok && validation.entryCount == expectedEntries &&
          validation.height == 3 &&
          validation.pagesVisited == oldLeafIds.size() + 4);
}

void checkCheckedGrandparentSplitShape(
    Database& db, const IndexInfo& index, uint64_t oldRootPageId,
    uint64_t oldParentPageId,
    const std::vector<uint64_t>& oldRootChildren,
    const std::vector<std::vector<uint8_t> >& oldRootBounds,
    const std::vector<uint64_t>& oldParentChildren,
    const std::vector<uint64_t>& oldLeafIds, uint64_t expectedEntries,
    uint32_t pageSize, uint64_t* outNewLeafPageId,
    std::vector<uint8_t>* outPromotedSeparator) {
    const uint32_t pageCapacity = pageSize - kPageHeaderSize;
    const uint32_t maxPhysicalBytes = maxIndexKeyBytes(pageSize) + 12u;
    CHECK(index.rootPageId == oldRootPageId);

    DatabasePage root;
    CHECK_DB(db.pages().readPage(index.rootPageId, root), DbStatus::Ok);
    CHECK(root.type == PageType::IndexInternal &&
          root.payload.size() == pageCapacity &&
          loadLe32(root.payload.data() + 16) == oldRootChildren.size() + 1);

    std::vector<uint64_t> rootChildren;
    std::vector<std::vector<uint8_t> > rootBounds;
    CHECK(readInternalChildren(db, index.rootPageId, pageCapacity,
                               rootChildren, rootBounds));
    CHECK(rootChildren.size() == oldRootChildren.size() + 1 &&
          rootBounds.size() == rootChildren.size());
    if (rootChildren.size() != oldRootChildren.size() + 1 ||
        rootBounds.size() != rootChildren.size() || oldRootChildren.size() != 2) {
        return;
    }
    CHECK(rootChildren[0] == oldRootChildren[0] &&
          rootChildren[1] == oldParentPageId &&
          rootBounds[0] == oldRootBounds[0] &&
          rootBounds[1] == oldRootBounds[1]);
    const uint64_t newRightParentId = rootChildren[2];
    CHECK(newRightParentId != oldRootChildren[0] &&
          newRightParentId != oldParentPageId &&
          !rootBounds[2].empty() &&
          rootBounds[2].size() == maxPhysicalBytes);

    std::vector<uint64_t> leftChildren;
    std::vector<uint64_t> rightChildren;
    std::vector<std::vector<uint8_t> > leftBounds;
    std::vector<std::vector<uint8_t> > rightBounds;
    CHECK(readInternalChildren(db, oldParentPageId, pageCapacity,
                               leftChildren, leftBounds));
    CHECK(readInternalChildren(db, newRightParentId, pageCapacity,
                               rightChildren, rightBounds));
    CHECK(leftChildren.size() == 2 && rightChildren.size() == 2 &&
          oldParentChildren.size() == 3);
    if (leftChildren.size() != 2 || rightChildren.size() != 2 ||
        oldParentChildren.size() != 3) {
        return;
    }
    CHECK(leftChildren[0] == oldParentChildren[0] &&
          leftChildren[1] == oldParentChildren[1] &&
          rightChildren[0] == oldParentChildren[2]);
    CHECK(rootBounds[2] == rightBounds[0]);

    uint64_t newLeafId = UINT64_MAX;
    for (size_t i = 0; i < oldParentChildren.size(); ++i) {
        CHECK(std::count(leftChildren.begin(), leftChildren.end(),
                         oldParentChildren[i]) +
              std::count(rightChildren.begin(), rightChildren.end(),
                         oldParentChildren[i]) == 1);
    }
    for (size_t i = 0; i < rightChildren.size(); ++i) {
        if (std::find(oldLeafIds.begin(), oldLeafIds.end(),
                      rightChildren[i]) == oldLeafIds.end()) {
            CHECK(newLeafId == UINT64_MAX);
            newLeafId = rightChildren[i];
        }
    }
    CHECK(newLeafId != UINT64_MAX);
    if (newLeafId == UINT64_MAX) return;
    if (outNewLeafPageId != nullptr) *outNewLeafPageId = newLeafId;
    if (outPromotedSeparator != nullptr) *outPromotedSeparator = rootBounds[2];

    std::vector<uint64_t> allLeafIds;
    for (size_t i = 0; i < rootChildren.size(); ++i) {
        std::vector<uint64_t> childIds;
        std::vector<std::vector<uint8_t> > childBounds;
        CHECK(readInternalChildren(db, rootChildren[i], pageCapacity,
                                   childIds, childBounds));
        CHECK(!childIds.empty() && childIds.size() == childBounds.size());
        if (childIds.empty() || childIds.size() != childBounds.size()) continue;
        if (i != 0) CHECK(rootBounds[i] == childBounds[0]);
        for (size_t j = 0; j < childIds.size(); ++j) {
            CHECK(std::find(allLeafIds.begin(), allLeafIds.end(),
                            childIds[j]) == allLeafIds.end());
            allLeafIds.push_back(childIds[j]);
            DatabasePage leaf;
            CHECK_DB(db.pages().readPage(childIds[j], leaf), DbStatus::Ok);
            CHECK(leaf.type == PageType::IndexLeaf);
            std::vector<uint8_t> firstPhysical;
            CHECK(readLeafFirstPhysicalKey(leaf, pageCapacity, firstPhysical));
            if (j != 0 || i != 0) CHECK(childBounds[j] == firstPhysical);
        }
    }
    CHECK(allLeafIds.size() == oldLeafIds.size() + 1);
    for (size_t i = 0; i < oldLeafIds.size(); ++i) {
        CHECK(std::count(allLeafIds.begin(), allLeafIds.end(),
                         oldLeafIds[i]) == 1);
    }
    CHECK(std::count(allLeafIds.begin(), allLeafIds.end(), newLeafId) == 1);
    for (size_t i = 0; i < allLeafIds.size(); ++i) {
        DatabasePage leaf;
        CHECK_DB(db.pages().readPage(allLeafIds[i], leaf), DbStatus::Ok);
        const uint64_t expectedNext = i + 1 < allLeafIds.size()
            ? allLeafIds[i + 1] : 0;
        CHECK(loadLe64(leaf.payload.data() + 8) == expectedNext);
    }

    IndexValidation validation;
    CHECK_DB(db.validateIndex(index.indexId, validation), DbStatus::Ok);
    CHECK(validation.ok && validation.entryCount == expectedEntries &&
          validation.height == 3 &&
          validation.pagesVisited == allLeafIds.size() + 4);
}

void testCheckedNonCascadingLeafSplit(uint32_t pageSize, bool injectFailures) {
    std::printf("NDB1R4P11R3L checked non-cascading leaf split (%u-byte pages)\n",
                pageSize);
    const std::string path = testPath() +
        (pageSize == 512 ? ".r3l512.gxdb" : ".r3l4096.gxdb");
    removeDatabase(path);

    AllocationState cacheState;
    AllocationState transactionState;
    GxAllocator cacheAllocator = makeAllocator(&cacheState);
    GxAllocator transactionAllocator = makeAllocator(&transactionState);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = pageSize;
    createOptions.bufferCapacity = injectFailures ? 1 : 128;
    createOptions.pageCacheAllocator = &cacheAllocator;
    createOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> db;
    CHECK_DB(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    const uint32_t payloadCapacity = pageSize - kPageHeaderSize;
    const int32_t maxPrimaryEntries = static_cast<int32_t>(
        (payloadCapacity - kIndexLeafHeaderSize) /
        (kIndexSlotSize + kIndexLeafEntryOverhead + 5));
    const int32_t baselineRows = maxPrimaryEntries + 1;
    const int32_t splitId = baselineRows + 1;
    const int32_t finalInsertedId = splitId + (pageSize == 4096 ? 1 : 0);
    const uint32_t splitCount = pageSize == 4096 ? 2 : 1;
    const size_t candidateBytes = payloadCapacity;
    IndexInfo primaryIndex;
    IndexInfo secondaryIndex;
    uint64_t rootChildIdsBefore[2] = {0, 0};
    uint64_t splitNewPageId = 0;
    bool splitRetriedInPriorUpdateTransaction = false;
    DatabasePage originalParent;
    DatabasePage originalRightLeaf;

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE PriorRows (Id INT32 PRIMARY KEY, V TEXT NOT NULL);"));
        CHECK(runSql(engine, "INSERT INTO PriorRows VALUES (1, 'before');"));
        CHECK(runSql(engine,
                     "CREATE TABLE SplitRows (Id INT32 PRIMARY KEY, Flag BOOLEAN NOT NULL);"));
        CHECK(runSql(engine,
                     "CREATE INDEX IX_SplitRows_Flag ON SplitRows (Flag);"));
        CHECK(findIndexInfo(*db, "SplitRows", "Id", primaryIndex));
        CHECK(findIndexInfo(*db, "SplitRows", "Flag", secondaryIndex));
        CHECK(runSql(engine, "BEGIN;"));
        for (int32_t id = 1; id <= maxPrimaryEntries; ++id) {
            CHECK(runSql(engine,
                         "INSERT INTO SplitRows VALUES (" + std::to_string(id) +
                         ", TRUE);"));
        }
        IndexValidation atCapacity;
        CHECK_DB(db->validateIndex(primaryIndex.indexId, atCapacity), DbStatus::Ok);
        CHECK(atCapacity.ok &&
              atCapacity.entryCount == static_cast<uint64_t>(maxPrimaryEntries) &&
              atCapacity.height == 1 && atCapacity.pagesVisited == 1);
        CHECK(runSql(engine,
                     "INSERT INTO SplitRows VALUES (" +
                     std::to_string(baselineRows) + ", TRUE);"));
        CHECK(runSql(engine, "COMMIT;"));

        CHECK(findIndexInfo(*db, "SplitRows", "Id", primaryIndex));
        CHECK(findIndexInfo(*db, "SplitRows", "Flag", secondaryIndex));
        IndexValidation beforePrimary;
        CHECK_DB(db->validateIndex(primaryIndex.indexId, beforePrimary),
                 DbStatus::Ok);
        CHECK(beforePrimary.ok &&
              beforePrimary.entryCount == static_cast<uint64_t>(baselineRows) &&
              beforePrimary.height == 2 && beforePrimary.pagesVisited == 3);
        IndexValidation beforeSecondary;
        CHECK_DB(db->validateIndex(secondaryIndex.indexId, beforeSecondary),
                 DbStatus::Ok);
        CHECK(beforeSecondary.ok &&
              beforeSecondary.entryCount == static_cast<uint64_t>(baselineRows) &&
              beforeSecondary.height == 1 && beforeSecondary.pagesVisited == 1);

        DatabasePage parentBefore;
        CHECK_DB(db->pages().readPage(primaryIndex.rootPageId, parentBefore),
                 DbStatus::Ok);
        originalParent = parentBefore;
        std::vector<uint8_t> ignoredBound;
        CHECK(findIndexChild(parentBefore, payloadCapacity, 0,
                             rootChildIdsBefore[0],
                             ignoredBound));
        CHECK(findIndexChild(parentBefore, payloadCapacity, 1,
                             rootChildIdsBefore[1], ignoredBound));
        CHECK(loadLe32(parentBefore.payload.data() + 16) == 2);
        CHECK_DB(db->pages().readPage(rootChildIdsBefore[1], originalRightLeaf),
                 DbStatus::Ok);

        if (injectFailures) {
            // A malformed separator that still routes the target into the
            // right leaf must be rejected while preparing the complete parent
            // replacement. Statement rollback keeps the prior UPDATE and the
            // pre-existing transaction corruption together.
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'before-corrupt-split' WHERE Id = 1;"));
            Transaction* tx = db->activeTransaction();
            CHECK(tx != nullptr);
            DatabasePage corruptParent;
            CHECK(tx != nullptr &&
                  tx->readPage(primaryIndex.rootPageId, corruptParent).isOk());
            std::vector<uint8_t> id10Key;
            CHECK(encodeIndexKey(DbType::Int32, DbValue::int32(10), id10Key));
            std::vector<RowLocator> id10Locators;
            CHECK_DB(db->indexLookup(primaryIndex.indexId, id10Key, id10Locators),
                     DbStatus::Ok);
            CHECK(id10Locators.size() == 1);
            if (id10Locators.size() == 1) {
                const std::vector<uint8_t> badSeparator =
                    IndexTree::makePhysicalKey(id10Key, id10Locators[0]);
                const uint32_t slotOffset = payloadCapacity - kIndexSlotSize * 2;
                const uint32_t childOffset =
                    loadLe32(corruptParent.payload.data() + slotOffset);
                const uint32_t childLength =
                    loadLe32(corruptParent.payload.data() + slotOffset + 4);
                const uint32_t keyLength =
                    loadLe32(corruptParent.payload.data() + childOffset + 8);
                CHECK(keyLength == badSeparator.size() &&
                      childLength == kIndexInternalEntryOverhead + keyLength);
                if (keyLength == badSeparator.size()) {
                    std::copy(badSeparator.begin(), badSeparator.end(),
                              corruptParent.payload.begin() + childOffset + 12);
                    CHECK_DB(tx->writePage(corruptParent), DbStatus::Ok);
                }
            }
            const uint32_t modifiedBeforeCorruptInsert = tx->modifiedPageCount();
            const uint64_t pageCountBeforeCorruptInsert = tx->finalPageCount();
            const size_t ownedBeforeCorruptInsert = transactionState.outstanding;
            const SqlExecutionResult corruptSplit = engine.execute(
                "INSERT INTO SplitRows VALUES (" + std::to_string(splitId) +
                ", TRUE);");
            CHECK(!corruptSplit.ok && corruptSplit.transactionActiveAfter &&
                  corruptSplit.error.message.find(
                      "index internal candidate bounds are not increasing") !=
                      std::string::npos);
            CHECK(tx->modifiedPageCount() == modifiedBeforeCorruptInsert &&
                  tx->finalPageCount() == pageCountBeforeCorruptInsert &&
                  transactionState.outstanding == ownedBeforeCorruptInsert);
            DatabasePage retainedCorruptParent;
            CHECK(tx->readPage(primaryIndex.rootPageId, retainedCorruptParent).isOk());
            CHECK(retainedCorruptParent.payload == corruptParent.payload);
            CHECK(countRows(engine, "SplitRows") == baselineRows);
            const SqlExecutionResult prior = engine.execute(
                "SELECT V FROM PriorRows WHERE Id = 1;");
            CHECK(prior.ok && prior.statements.size() == 1 &&
                  prior.statements[0].resultSet.value(0, 0).textValue() ==
                      "before-corrupt-split");
            CHECK(runSql(engine, "ROLLBACK;"));
            CHECK_DB(db->validateIndex(primaryIndex.indexId, beforePrimary),
                     DbStatus::Ok);
            CHECK(beforePrimary.ok &&
                  beforePrimary.entryCount == static_cast<uint64_t>(baselineRows));

            const int candidateFailureAfter[] = {1, 2, 3};
            for (size_t faultIndex = 0; faultIndex < 3; ++faultIndex) {
                CHECK(runSql(engine, "BEGIN;"));
                CHECK(runSql(engine,
                             "UPDATE PriorRows SET V = 'prior-update' WHERE Id = 1;"));
                tx = db->activeTransaction();
                CHECK(tx != nullptr);
                if (tx == nullptr) break;
                const uint32_t modifiedBefore = tx->modifiedPageCount();
                const uint64_t pageCountBefore = tx->finalPageCount();
                const size_t ownedBefore = transactionState.outstanding;
                const uint64_t imageAllocationsBefore =
                    tx->pageStats().imageAllocations;
                const size_t attemptsBefore = transactionState.matchingAttempts;
                const size_t successesBefore = transactionState.matchingSuccesses;
                const size_t failuresBefore = transactionState.matchingFailures;
                failMatchingAfter(&transactionState, candidateBytes,
                                  static_cast<size_t>(candidateFailureAfter[faultIndex]));
                const SqlExecutionResult failedInsert = engine.execute(
                    "INSERT INTO SplitRows VALUES (" + std::to_string(splitId) +
                    ", TRUE);");
                disableFailure(&transactionState);
                const size_t allowed =
                    static_cast<size_t>(candidateFailureAfter[faultIndex]);
                const char* expectedPreparationError = faultIndex < 2
                    ? "index leaf preparation allocation failed"
                    : "index internal preparation allocation failed";
                CHECK(!failedInsert.ok && failedInsert.transactionActiveAfter &&
                      failedInsert.error.message == expectedPreparationError);
                CHECK(transactionState.matchingAttempts - attemptsBefore ==
                      allowed + 1);
                CHECK(transactionState.matchingSuccesses - successesBefore == allowed);
                CHECK(transactionState.matchingFailures == failuresBefore + 1);
                CHECK(tx->modifiedPageCount() == modifiedBefore &&
                      tx->finalPageCount() == pageCountBefore &&
                      transactionState.outstanding == ownedBefore);
                CHECK(tx->pageStats().imageAllocations ==
                      imageAllocationsBefore + 1);
                DatabasePage retainedParent;
                DatabasePage retainedRightLeaf;
                CHECK(tx->readPage(primaryIndex.rootPageId, retainedParent).isOk());
                CHECK(tx->readPage(rootChildIdsBefore[1], retainedRightLeaf).isOk());
                CHECK(retainedParent.payload == originalParent.payload &&
                      retainedRightLeaf.payload == originalRightLeaf.payload);
                CHECK(countRows(engine, "SplitRows") == baselineRows);
                const SqlExecutionResult absent = engine.execute(
                    "SELECT Id FROM SplitRows WHERE Id = " +
                    std::to_string(splitId) + ";");
                CHECK(absent.ok && absent.statements.size() == 1 &&
                      absent.statements[0].accessPath == "IndexLookup" &&
                      absent.statements[0].resultSet.rowCount() == 0);
                IndexValidation restored;
                CHECK_DB(db->validateIndex(primaryIndex.indexId, restored),
                         DbStatus::Ok);
                CHECK(restored.ok &&
                      restored.entryCount == static_cast<uint64_t>(baselineRows) &&
                      restored.height == 2 && restored.pagesVisited == 3);
                const SqlExecutionResult priorValue = engine.execute(
                    "SELECT V FROM PriorRows WHERE Id = 1;");
                CHECK(priorValue.ok && priorValue.statements.size() == 1 &&
                      priorValue.statements[0].resultSet.value(0, 0).textValue() ==
                          "prior-update");

                CHECK(runSql(engine,
                             "INSERT INTO SplitRows VALUES (" + std::to_string(splitId) +
                             ", TRUE);"));
                CHECK(countRows(engine, "SplitRows") == splitId);
                CHECK(runSql(engine, "ROLLBACK;"));
                CHECK(countRows(engine, "SplitRows") == baselineRows);
            }

            // The right leaf is staged first. Fail the following existing-leaf
            // stage after snapshot copies, the heap replacement, and that new
            // leaf image have all succeeded.
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'prior-update' WHERE Id = 1;"));
            tx = db->activeTransaction();
            CHECK(tx != nullptr);
            if (tx != nullptr) {
                const uint32_t modifiedBefore = tx->modifiedPageCount();
                const uint64_t pageCountBefore = tx->finalPageCount();
                const size_t ownedBefore = transactionState.outstanding;
                const uint64_t imageAllocationsBefore =
                    tx->pageStats().imageAllocations;
                const size_t attemptsBefore = transactionState.matchingAttempts;
                const size_t successesBefore = transactionState.matchingSuccesses;
                const size_t failuresBefore = transactionState.matchingFailures;
                const size_t allowedPageImages =
                    static_cast<size_t>(modifiedBefore) + 2;
                failMatchingAfter(&transactionState, pageSize, allowedPageImages);
                const SqlExecutionResult partialStageFailure = engine.execute(
                    "INSERT INTO SplitRows VALUES (" + std::to_string(splitId) +
                    ", TRUE);");
                disableFailure(&transactionState);
                CHECK(!partialStageFailure.ok &&
                      partialStageFailure.transactionActiveAfter &&
                      partialStageFailure.error.message ==
                          "transaction page staging failed");
                CHECK(transactionState.matchingAttempts - attemptsBefore ==
                      allowedPageImages + 1);
                CHECK(transactionState.matchingSuccesses - successesBefore ==
                      allowedPageImages);
                CHECK(transactionState.matchingFailures == failuresBefore + 1);
                CHECK(tx->pageStats().imageAllocations ==
                      imageAllocationsBefore + 2);
                CHECK(tx->modifiedPageCount() == modifiedBefore &&
                      tx->finalPageCount() == pageCountBefore &&
                      transactionState.outstanding == ownedBefore);
                DatabasePage retainedParent;
                DatabasePage retainedRightLeaf;
                CHECK(tx->readPage(primaryIndex.rootPageId, retainedParent).isOk());
                CHECK(tx->readPage(rootChildIdsBefore[1], retainedRightLeaf).isOk());
                CHECK(retainedParent.payload == originalParent.payload &&
                      retainedRightLeaf.payload == originalRightLeaf.payload);
                CHECK(countRows(engine, "SplitRows") == baselineRows);
                const SqlExecutionResult absent = engine.execute(
                    "SELECT Id FROM SplitRows WHERE Id = " +
                    std::to_string(splitId) + ";");
                CHECK(absent.ok && absent.statements.size() == 1 &&
                      absent.statements[0].accessPath == "IndexLookup" &&
                      absent.statements[0].resultSet.rowCount() == 0);
                IndexValidation restored;
                CHECK_DB(db->validateIndex(primaryIndex.indexId, restored),
                         DbStatus::Ok);
                CHECK(restored.ok &&
                      restored.entryCount == static_cast<uint64_t>(baselineRows) &&
                      restored.height == 2 && restored.pagesVisited == 3);
                const SqlExecutionResult priorValue = engine.execute(
                    "SELECT V FROM PriorRows WHERE Id = 1;");
                CHECK(priorValue.ok && priorValue.statements.size() == 1 &&
                      priorValue.statements[0].resultSet.value(0, 0).textValue() ==
                          "prior-update");
                CHECK(runSql(engine,
                             "INSERT INTO SplitRows VALUES (" + std::to_string(splitId) +
                             ", TRUE);"));
                CHECK(countRows(engine, "SplitRows") == splitId);
                if (injectFailures) {
                    splitNewPageId = pageCountBefore;
                    splitRetriedInPriorUpdateTransaction = true;
                    CHECK(tx->finalPageCount() == pageCountBefore + 1);
                    const SqlExecutionResult priorValue = engine.execute(
                        "SELECT V FROM PriorRows WHERE Id = 1;");
                    CHECK(priorValue.ok && priorValue.statements.size() == 1 &&
                          priorValue.statements[0].resultSet.value(0, 0).textValue() ==
                              "prior-update");
                } else {
                    CHECK(runSql(engine, "ROLLBACK;"));
                    CHECK(countRows(engine, "SplitRows") == baselineRows);
                }
            }
        }

        const SqlExecutionResult duplicate = engine.execute(
            "INSERT INTO SplitRows VALUES (1, TRUE);");
        CHECK(!duplicate.ok && duplicate.error.message.find(
                  "unique index constraint violation") != std::string::npos);
        CHECK(countRows(engine, "SplitRows") ==
              (splitRetriedInPriorUpdateTransaction ? finalInsertedId : baselineRows));

        uint64_t pageCountBeforeSplit = splitNewPageId;
        Transaction* tx = db->activeTransaction();
        if (!splitRetriedInPriorUpdateTransaction) {
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'committed-before-split' WHERE Id = 1;"));
            tx = db->activeTransaction();
            CHECK(tx != nullptr);
            pageCountBeforeSplit = tx ? tx->finalPageCount() : 0;
            splitNewPageId = pageCountBeforeSplit;
            CHECK(runSql(engine,
                         "INSERT INTO SplitRows VALUES (" + std::to_string(splitId) +
                         ", TRUE);"));
            CHECK(tx != nullptr &&
                  tx->finalPageCount() == pageCountBeforeSplit + 1);
            CHECK(countRows(engine, "SplitRows") == splitId);
        } else {
            CHECK(tx != nullptr &&
                  tx->finalPageCount() == pageCountBeforeSplit + 1);
            CHECK(countRows(engine, "SplitRows") == finalInsertedId);
        }

        const SqlExecutionResult insertedPrimary = engine.execute(
            "SELECT Id FROM SplitRows WHERE Id = " + std::to_string(splitId) + ";");
        CHECK(insertedPrimary.ok && insertedPrimary.statements.size() == 1 &&
              insertedPrimary.statements[0].accessPath == "IndexLookup" &&
              insertedPrimary.statements[0].resultSet.rowCount() == 1 &&
              insertedPrimary.statements[0].resultSet.value(0, 0).int32Value() ==
                  splitId);
        if (pageSize == 4096) {
            const int32_t followupId = splitId + 1;
            CHECK(runSql(engine,
                         "INSERT INTO SplitRows VALUES (" +
                         std::to_string(followupId) + ", TRUE);"));
            CHECK(tx != nullptr &&
                  tx->finalPageCount() == pageCountBeforeSplit + 2);
            CHECK(countRows(engine, "SplitRows") == followupId);
            const SqlExecutionResult followupPrimary = engine.execute(
                "SELECT Id FROM SplitRows WHERE Id = " +
                std::to_string(followupId) + ";");
            CHECK(followupPrimary.ok && followupPrimary.statements.size() == 1 &&
                  followupPrimary.statements[0].accessPath == "IndexLookup" &&
                  followupPrimary.statements[0].resultSet.rowCount() == 1 &&
                  followupPrimary.statements[0].resultSet.value(0, 0).int32Value() ==
                      followupId);
        }
        const SqlExecutionResult secondaryRows = engine.execute(
            "SELECT Id FROM SplitRows WHERE Flag = TRUE;");
        CHECK(secondaryRows.ok && secondaryRows.statements.size() == 1 &&
              secondaryRows.statements[0].accessPath == "IndexLookup" &&
              secondaryRows.statements[0].resultSet.rowCount() ==
                  static_cast<size_t>(finalInsertedId));
        checkIndexes(*db);
        if (injectFailures) {
            // Reuse P6's established cache fault point: two cache allocations
            // load the catalog root during commit preparation; fail the next
            // allocation at committed-page publication, after durable COMMIT.
            const size_t cacheAttemptsBefore = cacheState.attempts;
            const size_t cacheSuccessesBefore = cacheState.successes;
            const size_t cacheFailuresBefore = cacheState.allocationFailures;
            failAtAttempt(&cacheState, 3);
            const SqlExecutionResult durableCommitFailure =
                engine.execute("COMMIT;");
            disableFailure(&cacheState);
            CHECK(!durableCommitFailure.ok &&
                  db->diagnostics().recoveryRequired);
            CHECK(cacheState.attempts == cacheAttemptsBefore + 3 &&
                  cacheState.successes == cacheSuccessesBefore + 2 &&
                  cacheState.allocationFailures == cacheFailuresBefore + 1);
            DatabasePage blocked;
            CHECK_DB(db->readPage(primaryIndex.rootPageId, blocked),
                     DbStatus::RecoveryRequired);
            CHECK_DB(db->flush(), DbStatus::RecoveryRequired);
        } else {
            CHECK(runSql(engine, "COMMIT;"));
            CHECK(countRows(engine, "SplitRows") == finalInsertedId);
            CHECK(findIndexInfo(*db, "SplitRows", "Id", primaryIndex));
            CHECK(findIndexInfo(*db, "SplitRows", "Flag", secondaryIndex));
            CHECK(primaryIndex.rootPageId != 0 && secondaryIndex.rootPageId != 0);
            CHECK(db->pageCount() == pageCountBeforeSplit + splitCount);
            checkSplitIndexShape(*db, pageSize, primaryIndex, secondaryIndex,
                                 finalInsertedId, splitNewPageId, splitCount);
            std::string integrity;
            CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
        }
    }

    CHECK_DB(db->close(), injectFailures ? DbStatus::RecoveryRequired
                                        : DbStatus::Ok);
    CHECK(!db->isOpen());
    CHECK(cacheState.outstanding == 0);
    if (injectFailures) {
        std::ifstream wal(walPathFor(path).c_str(),
                          std::ios::in | std::ios::binary | std::ios::ate);
        CHECK(wal.is_open() && wal.tellg() > 32);
    }
    db.reset();
    CHECK(cacheState.outstanding == 0);
    CHECK(transactionState.outstanding == 0);

    DatabaseOpenOptions openOptions;
    openOptions.bufferCapacity = injectFailures ? 1 : 128;
    openOptions.pageCacheAllocator = &cacheAllocator;
    openOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> reopened;
    CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
    if (reopened) {
        {
            SqlEngine engine(*reopened);
            CHECK(reopened->pageCount() == splitNewPageId + splitCount);
            CHECK(countRows(engine, "SplitRows") == finalInsertedId);
            CHECK(findIndexInfo(*reopened, "SplitRows", "Id", primaryIndex));
            CHECK(findIndexInfo(*reopened, "SplitRows", "Flag", secondaryIndex));
            checkSplitIndexShape(*reopened, pageSize, primaryIndex,
                                 secondaryIndex, finalInsertedId,
                                 splitNewPageId, splitCount);
            const SqlExecutionResult byPrimary = engine.execute(
                "SELECT Flag FROM SplitRows WHERE Id = " +
                std::to_string(splitId) + ";");
            CHECK(byPrimary.ok && byPrimary.statements.size() == 1 &&
                  byPrimary.statements[0].accessPath == "IndexLookup" &&
                  byPrimary.statements[0].resultSet.rowCount() == 1 &&
                  byPrimary.statements[0].resultSet.value(0, 0).booleanValue());
            const SqlExecutionResult bySecondary = engine.execute(
                "SELECT Id FROM SplitRows WHERE Flag = TRUE;");
            CHECK(bySecondary.ok && bySecondary.statements.size() == 1 &&
                  bySecondary.statements[0].accessPath == "IndexLookup" &&
                  bySecondary.statements[0].resultSet.rowCount() ==
                      static_cast<size_t>(finalInsertedId));
            const SqlExecutionResult prior = engine.execute(
                "SELECT V FROM PriorRows WHERE Id = 1;");
            CHECK(prior.ok && prior.statements.size() == 1 &&
                  prior.statements[0].resultSet.value(0, 0).textValue() ==
                      (injectFailures ? "prior-update" : "committed-before-split"));
            std::string integrity;
            CHECK_DB(reopened->validateIntegrity(integrity), DbStatus::Ok);
        }
        CHECK_DB(reopened->close(), DbStatus::Ok);
    }
    reopened.reset();
    CHECK(cacheState.outstanding == 0);
    CHECK(transactionState.outstanding == 0);
    removeDatabase(path);
}

void testCheckedInternalRootSplit(uint32_t pageSize, bool injectFailures) {
    std::printf("NDB1R4P11R3M checked full internal-root split (%u-byte pages)\n",
                pageSize);
    const std::string path = testPath() +
        (pageSize == 512 ? ".r3m512.gxdb" : ".r3m4096.gxdb");
    removeDatabase(path);

    AllocationState cacheState;
    AllocationState transactionState;
    GxAllocator cacheAllocator = makeAllocator(&cacheState);
    GxAllocator transactionAllocator = makeAllocator(&transactionState);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = pageSize;
    createOptions.bufferCapacity = injectFailures ? 1 : 128;
    createOptions.pageCacheAllocator = &cacheAllocator;
    createOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> db;
    CHECK_DB(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    const uint32_t pageCapacity = pageSize - kPageHeaderSize;
    const uint32_t maxTextKeyBytes = maxIndexKeyBytes(pageSize);
    CHECK(maxTextKeyBytes > 8);
    const uint32_t textBytes = maxTextKeyBytes - 1;
    const uint32_t separatorBytes = maxIndexKeyBytes(pageSize) + 12u;
    const uint32_t fullRootChildren =
        maxInternalChildrenForSeparator(pageCapacity, separatorBytes);
    const uint32_t maxLeafEntries =
        maxLeafEntriesForKey(pageCapacity, maxTextKeyBytes);
    CHECK(fullRootChildren >= 4 && maxLeafEntries >= 2);

    int32_t baselineRows = 0;
    int32_t splitId = 0;
    uint64_t originalPrimaryRootId = 0;
    uint64_t originalSecondaryRootId = 0;
    uint64_t originalPageCount = 0;
    uint64_t originalPrimarySplitLeafId = 0;
    uint64_t originalSecondarySplitLeafId = 0;
    std::vector<uint64_t> originalPrimaryLeafIds;
    std::vector<uint64_t> originalSecondaryLeafIds;
    DatabasePage originalPrimaryRoot;
    DatabasePage originalSecondaryRoot;
    DatabasePage originalPrimarySplitLeaf;
    DatabasePage originalSecondarySplitLeaf;
    size_t appendStageCount = 0;
    bool appendAllocatesNewPage = false;
    std::string splitKey;
    IndexInfo primaryIndex;
    IndexInfo secondaryIndex;

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE ParentRows (Id INT32 PRIMARY KEY);"));
        CHECK(runSql(engine, "INSERT INTO ParentRows VALUES (1);"));
        CHECK(runSql(engine,
                     "CREATE TABLE PriorRows (Id INT32 PRIMARY KEY, ParentId INT32 NOT NULL, V TEXT NOT NULL, FOREIGN KEY (ParentId) REFERENCES ParentRows (Id));"));
        CHECK(runSql(engine,
                     "INSERT INTO PriorRows VALUES (1, 1, 'before');"));
        CHECK(runSql(engine,
                     "CREATE TABLE RootRows (K TEXT PRIMARY KEY, Mirror TEXT NOT NULL);"));
        CHECK(runSql(engine,
                     "CREATE UNIQUE INDEX IX_RootRows_Mirror ON RootRows (Mirror);"));
        CHECK(findIndexInfo(*db, "RootRows", "K", primaryIndex));
        CHECK(findIndexInfo(*db, "RootRows", "Mirror", secondaryIndex));
        CHECK(runSql(engine, "BEGIN;"));

        bool fullRootFound = false;
        const uint32_t maximumFixtureRows = pageSize == 512 ? 100u : 500u;
        for (uint32_t ordinal = 1; ordinal <= maximumFixtureRows; ++ordinal) {
            const std::string key = rootSplitTextKey(textBytes, ordinal);
            CHECK(key.size() == textBytes);
            if (!runSql(engine,
                        "INSERT INTO RootRows VALUES ('" + key + "', '" + key + "');")) {
                break;
            }
            baselineRows = static_cast<int32_t>(ordinal);
            CHECK(findIndexInfo(*db, "RootRows", "K", primaryIndex));
            DatabasePage root;
            CHECK_DB(db->pages().readPage(primaryIndex.rootPageId, root),
                     DbStatus::Ok);
            if (root.type == PageType::IndexInternal &&
                loadLe32(root.payload.data() + 16) == fullRootChildren) {
                fullRootFound = true;
                break;
            }
        }
        CHECK(fullRootFound);
        CHECK(baselineRows > 0);
        if (!fullRootFound || baselineRows == 0) {
            CHECK(runSql(engine, "ROLLBACK;"));
            CHECK_DB(db->close(), DbStatus::Ok);
            removeDatabase(path);
            return;
        }

        CHECK(findIndexInfo(*db, "RootRows", "K", primaryIndex));
        CHECK(findIndexInfo(*db, "RootRows", "Mirror", secondaryIndex));
        DatabasePage root;
        CHECK_DB(db->pages().readPage(primaryIndex.rootPageId, root), DbStatus::Ok);
        CHECK(root.type == PageType::IndexInternal &&
              loadLe32(root.payload.data() + 16) == fullRootChildren);
        uint64_t rightmostLeafId = 0;
        std::vector<uint8_t> ignoredBound;
        CHECK(findIndexChild(root, pageCapacity, fullRootChildren - 1,
                             rightmostLeafId, ignoredBound));
        DatabasePage rightmostLeaf;
        CHECK_DB(db->pages().readPage(rightmostLeafId, rightmostLeaf),
                 DbStatus::Ok);
        uint32_t rightmostCount = loadLe32(rightmostLeaf.payload.data() + 16);
        while (rightmostCount < maxLeafEntries) {
            ++baselineRows;
            const std::string key = rootSplitTextKey(
                textBytes, static_cast<uint32_t>(baselineRows));
            CHECK(runSql(engine,
                         "INSERT INTO RootRows VALUES ('" + key + "', '" + key + "');"));
            CHECK(findIndexInfo(*db, "RootRows", "K", primaryIndex));
            CHECK_DB(db->pages().readPage(primaryIndex.rootPageId, root), DbStatus::Ok);
            CHECK(root.type == PageType::IndexInternal &&
                  loadLe32(root.payload.data() + 16) == fullRootChildren);
            CHECK(findIndexChild(root, pageCapacity, fullRootChildren - 1,
                                 rightmostLeafId, ignoredBound));
            CHECK_DB(db->pages().readPage(rightmostLeafId, rightmostLeaf),
                     DbStatus::Ok);
            rightmostCount = loadLe32(rightmostLeaf.payload.data() + 16);
        }
        CHECK(rightmostCount == maxLeafEntries);
        CHECK(findIndexInfo(*db, "RootRows", "Mirror", secondaryIndex));

        IndexValidation beforePrimary;
        IndexValidation beforeSecondary;
        CHECK_DB(db->validateIndex(primaryIndex.indexId, beforePrimary),
                 DbStatus::Ok);
        CHECK_DB(db->validateIndex(secondaryIndex.indexId, beforeSecondary),
                 DbStatus::Ok);
        CHECK(beforePrimary.ok && beforePrimary.entryCount ==
                  static_cast<uint64_t>(baselineRows) && beforePrimary.height == 2 &&
              beforeSecondary.ok && beforeSecondary.entryCount ==
                  static_cast<uint64_t>(baselineRows) && beforeSecondary.height == 2);

        splitId = baselineRows + 1;
        splitKey = rootSplitTextKey(textBytes, static_cast<uint32_t>(splitId));
        std::vector<uint8_t> maximumEncodedKey;
        CHECK(encodeIndexKey(DbType::Text,
                             DbValue::text(rootSplitTextKey(textBytes, 1)),
                             maximumEncodedKey));
        CHECK(maximumEncodedKey.size() == maxTextKeyBytes);

        CHECK_DB(db->pages().readPage(primaryIndex.rootPageId, originalPrimaryRoot),
                 DbStatus::Ok);
        CHECK_DB(db->pages().readPage(secondaryIndex.rootPageId,
                                      originalSecondaryRoot), DbStatus::Ok);
        std::vector<std::vector<uint8_t> > primaryRootBounds;
        std::vector<std::vector<uint8_t> > secondaryRootBounds;
        CHECK(readInternalChildren(*db, primaryIndex.rootPageId, pageCapacity,
                                   originalPrimaryLeafIds, primaryRootBounds));
        CHECK(readInternalChildren(*db, secondaryIndex.rootPageId, pageCapacity,
                                   originalSecondaryLeafIds, secondaryRootBounds));
        CHECK(originalPrimaryLeafIds.size() == fullRootChildren &&
              originalSecondaryLeafIds.size() == fullRootChildren);
        originalPrimaryRootId = primaryIndex.rootPageId;
        originalSecondaryRootId = secondaryIndex.rootPageId;
        originalPrimarySplitLeafId = originalPrimaryLeafIds.back();
        originalSecondarySplitLeafId = originalSecondaryLeafIds.back();
        CHECK_DB(db->pages().readPage(originalPrimarySplitLeafId,
                                      originalPrimarySplitLeaf), DbStatus::Ok);
        CHECK_DB(db->pages().readPage(originalSecondarySplitLeafId,
                                      originalSecondarySplitLeaf), DbStatus::Ok);
        CHECK(loadLe32(originalPrimarySplitLeaf.payload.data() + 16) ==
              maxLeafEntries);
        CHECK(loadLe32(originalSecondarySplitLeaf.payload.data() + 16) ==
              maxLeafEntries);

        uint64_t rootUsed = kIndexInternalHeaderSize +
            static_cast<uint64_t>(fullRootChildren) * kIndexSlotSize;
        for (size_t i = 0; i < primaryRootBounds.size(); ++i) {
            rootUsed += kIndexInternalEntryOverhead + primaryRootBounds[i].size();
        }
        CHECK(rootUsed <= pageCapacity &&
              rootUsed + kIndexSlotSize + kIndexInternalEntryOverhead +
                  separatorBytes > pageCapacity);
        CHECK(primaryRootBounds == secondaryRootBounds);

        appendStageCount = rootInsertHeapStageCount(
            *db, pageSize, textBytes, appendAllocatesNewPage);
        CHECK(appendStageCount == (appendAllocatesNewPage ? 2u : 1u));
        CHECK(runSql(engine, "COMMIT;"));
        originalPageCount = db->pageCount();
        CHECK(originalPageCount != 0);
    }

    const std::string splitSql = "INSERT INTO RootRows VALUES ('" + splitKey +
        "', '" + splitKey + "');";

    const auto verifyBaselineAfterFailure = [&](SqlEngine& engine,
                                                Transaction* tx,
                                                const std::string& expectedPrior) {
        IndexInfo currentPrimary;
        IndexInfo currentSecondary;
        CHECK(findIndexInfo(*db, "RootRows", "K", currentPrimary));
        CHECK(findIndexInfo(*db, "RootRows", "Mirror", currentSecondary));
        CHECK(currentPrimary.rootPageId == originalPrimaryRootId &&
              currentSecondary.rootPageId == originalSecondaryRootId &&
              currentPrimary.entryCount == static_cast<uint64_t>(baselineRows) &&
              currentSecondary.entryCount == static_cast<uint64_t>(baselineRows));
        DatabasePage primaryRoot;
        DatabasePage secondaryRoot;
        DatabasePage primaryLeaf;
        DatabasePage secondaryLeaf;
        CHECK((tx != nullptr ? tx->readPage(originalPrimaryRootId, primaryRoot)
                             : db->pages().readPage(originalPrimaryRootId, primaryRoot))
                  .isOk());
        CHECK((tx != nullptr ? tx->readPage(originalSecondaryRootId, secondaryRoot)
                             : db->pages().readPage(originalSecondaryRootId,
                                                    secondaryRoot)).isOk());
        CHECK((tx != nullptr ? tx->readPage(originalPrimarySplitLeafId, primaryLeaf)
                             : db->pages().readPage(originalPrimarySplitLeafId,
                                                    primaryLeaf)).isOk());
        CHECK((tx != nullptr ? tx->readPage(originalSecondarySplitLeafId,
                                             secondaryLeaf)
                             : db->pages().readPage(originalSecondarySplitLeafId,
                                                    secondaryLeaf)).isOk());
        CHECK(primaryRoot.payload == originalPrimaryRoot.payload &&
              secondaryRoot.payload == originalSecondaryRoot.payload &&
              primaryLeaf.payload == originalPrimarySplitLeaf.payload &&
              secondaryLeaf.payload == originalSecondarySplitLeaf.payload);
        CHECK(countRows(engine, "RootRows") == baselineRows);
        if (tx != nullptr) {
            IndexValidation p;
            IndexValidation s;
            CHECK_DB(db->validateIndex(currentPrimary.indexId, p), DbStatus::Ok);
            CHECK_DB(db->validateIndex(currentSecondary.indexId, s), DbStatus::Ok);
            CHECK(p.ok && p.height == 2 && p.entryCount ==
                      static_cast<uint64_t>(baselineRows));
            CHECK(s.ok && s.height == 2 && s.entryCount ==
                      static_cast<uint64_t>(baselineRows));
        }
        const SqlExecutionResult prior = engine.execute(
            "SELECT V FROM PriorRows WHERE Id = 1;");
        CHECK(prior.ok && prior.statements.size() == 1 &&
              prior.statements[0].resultSet.value(0, 0).textValue() ==
                  expectedPrior);
        const SqlExecutionResult parent = engine.execute(
            "SELECT ParentId FROM PriorRows WHERE Id = 1;");
        CHECK(parent.ok && parent.statements.size() == 1 &&
              parent.statements[0].resultSet.value(0, 0).int32Value() == 1);
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
    };
    CHECK(originalPrimaryRootId != 0 && originalSecondaryRootId != 0 &&
          originalPageCount != 0 && splitId == baselineRows + 1);

    if (injectFailures) {
        const size_t candidateFailureAfter[] = {3, 4, 5, 8};
        for (size_t fault = 0; fault < 4; ++fault) {
            SqlEngine engine(*db);
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'prior-update' WHERE Id = 1;"));
            Transaction* tx = db->activeTransaction();
            CHECK(tx != nullptr);
            if (tx == nullptr) break;
            const uint32_t modifiedBefore = tx->modifiedPageCount();
            const uint64_t pageCountBefore = tx->finalPageCount();
            const size_t ownedBefore = transactionState.outstanding;
            const size_t attemptsBefore = transactionState.matchingAttempts;
            const size_t successesBefore = transactionState.matchingSuccesses;
            const size_t failuresBefore = transactionState.matchingFailures;
            const size_t allowed = candidateFailureAfter[fault];
            failMatchingAfter(&transactionState, pageCapacity, allowed);
            const SqlExecutionResult failedInsert = engine.execute(splitSql);
            disableFailure(&transactionState);
            CHECK(!failedInsert.ok && failedInsert.transactionActiveAfter &&
                  failedInsert.error.message ==
                      "index internal preparation allocation failed");
            CHECK(transactionState.matchingAttempts - attemptsBefore == allowed + 1 &&
                  transactionState.matchingSuccesses - successesBefore == allowed &&
                  transactionState.matchingFailures == failuresBefore + 1);
            CHECK(tx->modifiedPageCount() == modifiedBefore &&
                  tx->finalPageCount() == pageCountBefore &&
                  transactionState.outstanding == ownedBefore);
            verifyBaselineAfterFailure(engine, tx, "prior-update");
            CHECK(countRows(engine, "RootRows") == baselineRows);
            const SqlExecutionResult absent = engine.execute(
                "SELECT K FROM RootRows WHERE K = '" + splitKey + "';");
            CHECK(absent.ok && absent.statements.size() == 1 &&
                  absent.statements[0].accessPath == "IndexLookup" &&
                  absent.statements[0].resultSet.rowCount() == 0);
            const SqlExecutionResult absentSecondary = engine.execute(
                "SELECT K FROM RootRows WHERE Mirror = '" + splitKey + "';");
            CHECK(absentSecondary.ok && absentSecondary.statements.size() == 1 &&
                  absentSecondary.statements[0].accessPath == "IndexLookup" &&
                  absentSecondary.statements[0].resultSet.rowCount() == 0);
            const SqlExecutionResult prior = engine.execute(
                "SELECT V FROM PriorRows WHERE Id = 1;");
            CHECK(prior.ok && prior.statements.size() == 1 &&
                  prior.statements[0].resultSet.value(0, 0).textValue() ==
                      "prior-update");

            CHECK(runSql(engine, splitSql));
            IndexInfo grownPrimary;
            IndexInfo grownSecondary;
            CHECK(findIndexInfo(*db, "RootRows", "K", grownPrimary));
            CHECK(findIndexInfo(*db, "RootRows", "Mirror", grownSecondary));
            uint64_t newPrimaryLeaf = 0;
            uint64_t newSecondaryLeaf = 0;
            std::vector<uint8_t> promoted;
            checkCheckedRootSplitShape(*db, grownPrimary, originalPrimaryRootId,
                                       originalPrimaryLeafIds,
                                       originalPrimarySplitLeafId,
                                       static_cast<uint64_t>(baselineRows + 1),
                                       pageSize, &newPrimaryLeaf, &promoted);
            checkCheckedRootSplitShape(*db, grownSecondary,
                                       originalSecondaryRootId,
                                       originalSecondaryLeafIds,
                                       originalSecondarySplitLeafId,
                                       static_cast<uint64_t>(baselineRows + 1),
                                       pageSize, &newSecondaryLeaf, nullptr);
            CHECK(tx->finalPageCount() == pageCountBefore +
                  (appendAllocatesNewPage ? 1u : 0u) + 6u);
            CHECK(runSql(engine, "ROLLBACK;"));
            CHECK(db->pageCount() == originalPageCount);
            verifyBaselineAfterFailure(engine, nullptr, "before");
            CHECK(countRows(engine, "RootRows") == baselineRows);
            checkIndexes(*db);
        }

        // The first index's entire root-growth group is staged and its catalog
        // root is updated before the equivalent secondary-index candidate
        // allocation fails. INSERT savepoint restoration must undo that work.
        {
            SqlEngine engine(*db);
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'prior-update' WHERE Id = 1;"));
            Transaction* tx = db->activeTransaction();
            CHECK(tx != nullptr);
            if (tx != nullptr) {
                const uint32_t modifiedBefore = tx->modifiedPageCount();
                const uint64_t pageCountBefore = tx->finalPageCount();
                const size_t ownedBefore = transactionState.outstanding;
                const uint64_t imageAllocationsBefore =
                    tx->pageStats().imageAllocations;
                const size_t attemptsBefore = transactionState.matchingAttempts;
                const size_t successesBefore = transactionState.matchingSuccesses;
                const size_t failuresBefore = transactionState.matchingFailures;
                failMatchingAfter(&transactionState, pageCapacity, 8);
                const SqlExecutionResult failedInsert = engine.execute(splitSql);
                disableFailure(&transactionState);
                CHECK(!failedInsert.ok && failedInsert.transactionActiveAfter &&
                      failedInsert.error.message ==
                          "index internal preparation allocation failed");
                CHECK(transactionState.matchingAttempts - attemptsBefore == 9 &&
                      transactionState.matchingSuccesses - successesBefore == 8 &&
                      transactionState.matchingFailures == failuresBefore + 1);
                CHECK(tx->pageStats().imageAllocations >=
                          imageAllocationsBefore + 5 &&
                      tx->modifiedPageCount() == modifiedBefore &&
                      tx->finalPageCount() == pageCountBefore &&
                      transactionState.outstanding == ownedBefore);
                verifyBaselineAfterFailure(engine, tx, "prior-update");
                const SqlExecutionResult absent = engine.execute(
                    "SELECT K FROM RootRows WHERE K = '" + splitKey + "';");
                CHECK(absent.ok && absent.statements.size() == 1 &&
                      absent.statements[0].resultSet.rowCount() == 0);
                const SqlExecutionResult absentSecondary = engine.execute(
                    "SELECT K FROM RootRows WHERE Mirror = '" + splitKey + "';");
                CHECK(absentSecondary.ok && absentSecondary.statements.size() == 1 &&
                      absentSecondary.statements[0].accessPath == "IndexLookup" &&
                      absentSecondary.statements[0].resultSet.rowCount() == 0);
                const SqlExecutionResult prior = engine.execute(
                    "SELECT V FROM PriorRows WHERE Id = 1;");
                CHECK(prior.ok && prior.statements.size() == 1 &&
                      prior.statements[0].resultSet.value(0, 0).textValue() ==
                          "prior-update");
                CHECK(runSql(engine, splitSql));
                CHECK(runSql(engine, "ROLLBACK;"));
                verifyBaselineAfterFailure(engine, nullptr, "before");
                CHECK(countRows(engine, "RootRows") == baselineRows);
            }
        }

        // The heap append has the measured one or two page-image stages below.
        // Then the split stages its new leaf, right internal node, and new
        // root; fail the following existing-leaf stage after the new root is
        // already present in the transaction page owner.
        {
            SqlEngine engine(*db);
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'prior-update' WHERE Id = 1;"));
            Transaction* tx = db->activeTransaction();
            CHECK(tx != nullptr);
            if (tx != nullptr) {
                const uint32_t modifiedBefore = tx->modifiedPageCount();
                const uint64_t pageCountBefore = tx->finalPageCount();
                const size_t ownedBefore = transactionState.outstanding;
                const size_t attemptsBefore = transactionState.matchingAttempts;
                const size_t successesBefore = transactionState.matchingSuccesses;
                const size_t failuresBefore = transactionState.matchingFailures;
                const size_t allowed = modifiedBefore + appendStageCount + 3;
                failMatchingAfter(&transactionState, pageSize, allowed);
                const SqlExecutionResult failedInsert = engine.execute(splitSql);
                disableFailure(&transactionState);
                CHECK(!failedInsert.ok && failedInsert.transactionActiveAfter &&
                      failedInsert.error.message == "transaction page staging failed");
                CHECK(transactionState.matchingAttempts - attemptsBefore == allowed + 1 &&
                      transactionState.matchingSuccesses - successesBefore == allowed &&
                      transactionState.matchingFailures == failuresBefore + 1);
                CHECK(tx->modifiedPageCount() == modifiedBefore &&
                      tx->finalPageCount() == pageCountBefore &&
                      transactionState.outstanding == ownedBefore);
                verifyBaselineAfterFailure(engine, tx, "prior-update");
                CHECK(countRows(engine, "RootRows") == baselineRows);
                const SqlExecutionResult absent = engine.execute(
                    "SELECT K FROM RootRows WHERE K = '" + splitKey + "';");
                CHECK(absent.ok && absent.statements.size() == 1 &&
                      absent.statements[0].accessPath == "IndexLookup" &&
                      absent.statements[0].resultSet.rowCount() == 0);
                const SqlExecutionResult absentSecondary = engine.execute(
                    "SELECT K FROM RootRows WHERE Mirror = '" + splitKey + "';");
                CHECK(absentSecondary.ok && absentSecondary.statements.size() == 1 &&
                      absentSecondary.statements[0].accessPath == "IndexLookup" &&
                      absentSecondary.statements[0].resultSet.rowCount() == 0);
                CHECK(runSql(engine, splitSql));
                CHECK(runSql(engine, "ROLLBACK;"));
                verifyBaselineAfterFailure(engine, nullptr, "before");
            }
        }
    }

    uint64_t finalPageCount = 0;
    uint64_t finalPrimaryRootId = 0;
    uint64_t finalSecondaryRootId = 0;
    uint64_t finalPrimaryNewLeafId = 0;
    std::vector<uint8_t> finalPromotedSeparator;
    std::string finalPriorValue = injectFailures
        ? "committed-root-growth" : "after-root-commit";
    {
        SqlEngine engine(*db);
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE PriorRows SET V = 'committed-root-growth' WHERE Id = 1;"));
        Transaction* tx = db->activeTransaction();
        CHECK(tx != nullptr);
        const uint64_t pageCountBefore = tx == nullptr ? 0 : tx->finalPageCount();
        CHECK(runSql(engine, splitSql));
        CHECK(findIndexInfo(*db, "RootRows", "K", primaryIndex));
        CHECK(findIndexInfo(*db, "RootRows", "Mirror", secondaryIndex));
        finalPrimaryRootId = primaryIndex.rootPageId;
        finalSecondaryRootId = secondaryIndex.rootPageId;
        checkCheckedRootSplitShape(*db, primaryIndex, originalPrimaryRootId,
                                   originalPrimaryLeafIds,
                                   originalPrimarySplitLeafId,
                                   static_cast<uint64_t>(baselineRows + 1),
                                   pageSize, &finalPrimaryNewLeafId,
                                   &finalPromotedSeparator);
        uint64_t finalSecondaryNewLeaf = 0;
        checkCheckedRootSplitShape(*db, secondaryIndex,
                                   originalSecondaryRootId,
                                   originalSecondaryLeafIds,
                                   originalSecondarySplitLeafId,
                                   static_cast<uint64_t>(baselineRows + 1),
                                   pageSize, &finalSecondaryNewLeaf, nullptr);
        CHECK(tx != nullptr && tx->finalPageCount() == pageCountBefore +
              (appendAllocatesNewPage ? 1u : 0u) + 6u);
        CHECK(countRows(engine, "RootRows") == baselineRows + 1);
        const SqlExecutionResult prior = engine.execute(
            "SELECT V FROM PriorRows WHERE Id = 1;");
        CHECK(prior.ok && prior.statements.size() == 1 &&
              prior.statements[0].resultSet.value(0, 0).textValue() ==
                  "committed-root-growth");
        finalPageCount = tx == nullptr ? 0 : tx->finalPageCount();

        if (injectFailures) {
            // The larger fixture has more catalog records than the accepted
            // R3L fixture. Stage the transaction's catalog image first so
            // the one-shot cache fault below lands on the first committed
            // page-image publication after WAL durability, not on the
            // catalog's pre-COMMIT cache reads.
            CHECK(tx != nullptr);
            const size_t catalogCacheAttemptsBefore = cacheState.attempts;
            if (tx != nullptr) {
                CHECK_DB(db->catalog().save(*tx, db->file().rootPageId(),
                                            tx->pageSize()), DbStatus::Ok);
            }
            const size_t catalogCacheReads =
                cacheState.attempts - catalogCacheAttemptsBefore;
            std::printf("R3M catalog serialization cache allocations=%lu\n",
                        static_cast<unsigned long>(catalogCacheReads));
            const size_t attemptsBefore = cacheState.attempts;
            const size_t successesBefore = cacheState.successes;
            const size_t failuresBefore = cacheState.allocationFailures;
            failAtAttempt(&cacheState, 1);
            const SqlExecutionResult durableCommitFailure =
                engine.execute("COMMIT;");
            disableFailure(&cacheState);
            std::printf("R3M cache commit pageSize=%u ok=%u error=%s attempts=+%lu successes=+%lu failures=+%lu recovery=%u root=%llu pageCount=%llu\n",
                        pageSize, durableCommitFailure.ok ? 1u : 0u,
                        durableCommitFailure.error.message.c_str(),
                        static_cast<unsigned long>(cacheState.attempts - attemptsBefore),
                        static_cast<unsigned long>(cacheState.successes - successesBefore),
                        static_cast<unsigned long>(cacheState.allocationFailures - failuresBefore),
                        db->diagnostics().recoveryRequired ? 1u : 0u,
                        static_cast<unsigned long long>(finalPrimaryRootId),
                        static_cast<unsigned long long>(db->pageCount()));
            CHECK(!durableCommitFailure.ok &&
                  db->diagnostics().recoveryRequired);
            CHECK(cacheState.attempts == attemptsBefore + 1 &&
                  cacheState.successes == successesBefore &&
                  cacheState.allocationFailures == failuresBefore + 1);
        } else {
            CHECK(runSql(engine, "COMMIT;"));
            IndexValidation primaryAfterCommit;
            IndexValidation secondaryAfterCommit;
            CHECK_DB(db->validateIndex(primaryIndex.indexId, primaryAfterCommit),
                     DbStatus::Ok);
            CHECK_DB(db->validateIndex(secondaryIndex.indexId,
                                       secondaryAfterCommit), DbStatus::Ok);
            CHECK(primaryAfterCommit.ok && primaryAfterCommit.height == 3 &&
                  secondaryAfterCommit.ok && secondaryAfterCommit.height == 3);
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'after-root-commit' WHERE Id = 1;"));
        }

        DatabasePage finalRoot;
        CHECK_DB(db->readPage(finalPrimaryRootId, finalRoot),
                 injectFailures ? DbStatus::RecoveryRequired : DbStatus::Ok);
    }

    CHECK_DB(db->close(), injectFailures ? DbStatus::RecoveryRequired
                                        : DbStatus::Ok);
    if (injectFailures) {
        std::ifstream wal(walPathFor(path).c_str(),
                          std::ios::in | std::ios::binary | std::ios::ate);
        CHECK(wal.is_open() && wal.tellg() > 32);
    }
    db.reset();
    CHECK(cacheState.outstanding == 0 && transactionState.outstanding == 0);

    DatabaseOpenOptions openOptions;
    openOptions.bufferCapacity = injectFailures ? 1 : 128;
    openOptions.pageCacheAllocator = &cacheAllocator;
    openOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> reopened;
    CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
    for (int reopenPass = 0; reopened && reopenPass < 2; ++reopenPass) {
        {
            SqlEngine engine(*reopened);
            CHECK(reopened->pageCount() == finalPageCount);
            CHECK(countRows(engine, "RootRows") == baselineRows + 1);
            CHECK(findIndexInfo(*reopened, "RootRows", "K", primaryIndex));
            CHECK(findIndexInfo(*reopened, "RootRows", "Mirror", secondaryIndex));
            CHECK(primaryIndex.rootPageId == finalPrimaryRootId &&
                  secondaryIndex.rootPageId == finalSecondaryRootId);
            checkCheckedRootSplitShape(*reopened, primaryIndex,
                                       originalPrimaryRootId,
                                       originalPrimaryLeafIds,
                                       originalPrimarySplitLeafId,
                                       static_cast<uint64_t>(baselineRows + 1),
                                       pageSize, nullptr, nullptr);
            checkCheckedRootSplitShape(*reopened, secondaryIndex,
                                       originalSecondaryRootId,
                                       originalSecondaryLeafIds,
                                       originalSecondarySplitLeafId,
                                       static_cast<uint64_t>(baselineRows + 1),
                                       pageSize, nullptr, nullptr);
            for (int32_t ordinal = 1; ordinal <= baselineRows + 1; ++ordinal) {
                const std::string key = rootSplitTextKey(
                    textBytes, static_cast<uint32_t>(ordinal));
                std::vector<uint8_t> encoded;
                CHECK(encodeIndexKey(DbType::Text, DbValue::text(key), encoded));
                std::vector<RowLocator> primaryLocators;
                std::vector<RowLocator> secondaryLocators;
                CHECK_DB(reopened->indexLookup(primaryIndex.indexId, encoded,
                                               primaryLocators), DbStatus::Ok);
                CHECK_DB(reopened->indexLookup(secondaryIndex.indexId, encoded,
                                               secondaryLocators), DbStatus::Ok);
                CHECK(primaryLocators.size() == 1 &&
                      secondaryLocators.size() == 1 &&
                      primaryLocators[0].pageId == secondaryLocators[0].pageId &&
                      primaryLocators[0].slot == secondaryLocators[0].slot);
            }
            DbValue promotedValue;
            CHECK(finalPromotedSeparator.size() >= 12 &&
                  decodeIndexKey(DbType::Text, finalPromotedSeparator.data(),
                                 finalPromotedSeparator.size() - 12,
                                 promotedValue));
            if (finalPromotedSeparator.size() >= 12) {
                const std::string promotedText = promotedValue.textValue();
                const std::string suffix = promotedText.substr(
                    promotedText.size() - 8);
                const int32_t boundaryOrdinal = static_cast<int32_t>(
                    std::strtol(suffix.c_str(), nullptr, 10));
                CHECK(promotedText == rootSplitTextKey(
                          textBytes, static_cast<uint32_t>(boundaryOrdinal)));
                for (int32_t ordinal = boundaryOrdinal - 1;
                     ordinal <= boundaryOrdinal + 1; ++ordinal) {
                    if (ordinal < 1 || ordinal > baselineRows + 1) continue;
                    const std::string key = rootSplitTextKey(
                        textBytes, static_cast<uint32_t>(ordinal));
                    const SqlExecutionResult boundary = engine.execute(
                        "SELECT K FROM RootRows WHERE K = '" + key + "';");
                    CHECK(boundary.ok && boundary.statements.size() == 1 &&
                          boundary.statements[0].accessPath == "IndexLookup" &&
                          boundary.statements[0].resultSet.rowCount() == 1 &&
                          boundary.statements[0].resultSet.value(0, 0).textValue() == key);
                }
            }
            const SqlExecutionResult prior = engine.execute(
                "SELECT V FROM PriorRows WHERE Id = 1;");
            CHECK(prior.ok && prior.statements.size() == 1 &&
                  prior.statements[0].resultSet.value(0, 0).textValue() ==
                      finalPriorValue);
            checkIndexes(*reopened);
            std::string integrity;
            CHECK_DB(reopened->validateIntegrity(integrity), DbStatus::Ok);
        }
        CHECK_DB(reopened->close(), DbStatus::Ok);
        reopened.reset();
        CHECK(cacheState.outstanding == 0 && transactionState.outstanding == 0);
        if (reopenPass == 0) {
            CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
        }
    }

    if (originalPrimaryRootId != 0 && finalPrimaryRootId != 0) {
        DatabasePage root;
        std::vector<uint64_t> rootChildren;
        std::vector<std::vector<uint8_t> > rootBounds;
        std::unique_ptr<Database> reportDb;
        CHECK_DB(Database::open(path, openOptions, reportDb), DbStatus::Ok);
        if (reportDb) {
            IndexInfo reportIndex;
            CHECK(findIndexInfo(*reportDb, "RootRows", "K", reportIndex));
            CHECK(readInternalChildren(*reportDb, reportIndex.rootPageId,
                                       pageCapacity, rootChildren, rootBounds));
            std::vector<uint64_t> rightChildren;
            std::vector<std::vector<uint8_t> > rightBounds;
            std::vector<uint64_t> leftChildren;
            std::vector<std::vector<uint8_t> > leftBounds;
            if (rootChildren.size() == 2) {
                CHECK(readInternalChildren(*reportDb, rootChildren[0], pageCapacity,
                                           leftChildren, leftBounds));
                CHECK(readInternalChildren(*reportDb, rootChildren[1], pageCapacity,
                                           rightChildren, rightBounds));
            }
            std::printf("R3M observed pageSize=%u rows=%d oldRoot=%llu newRoot=%llu rightInternal=%llu newLeaf=%llu promotedBytes=%lu pageCount=%llu->%llu fullChildren=%u splitChildren=%lu/%lu\n",
                        pageSize, baselineRows,
                        static_cast<unsigned long long>(originalPrimaryRootId),
                        static_cast<unsigned long long>(finalPrimaryRootId),
                        static_cast<unsigned long long>(rootChildren.size() > 1
                            ? rootChildren[1] : 0),
                        static_cast<unsigned long long>(finalPrimaryNewLeafId),
                        static_cast<unsigned long>(finalPromotedSeparator.size()),
                        static_cast<unsigned long long>(originalPageCount),
                        static_cast<unsigned long long>(finalPageCount),
                        fullRootChildren,
                        static_cast<unsigned long>(leftChildren.size()),
                        static_cast<unsigned long>(rightChildren.size()));
            CHECK_DB(reportDb->close(), DbStatus::Ok);
        }
    }
    CHECK(cacheState.outstanding == 0 && transactionState.outstanding == 0);
    removeDatabase(path);
}

void testCheckedInternalGrandparentSplit(uint32_t pageSize,
                                         bool injectFailures) {
    std::printf("NDB1R4P11R3N checked nonroot internal split (%u-byte pages)\n",
                pageSize);
    const std::string path = testPath() +
        (pageSize == 512 ? ".r3n512.gxdb" : ".r3n4096.gxdb");
    removeDatabase(path);

    AllocationState cacheState;
    AllocationState transactionState;
    GxAllocator cacheAllocator = makeAllocator(&cacheState);
    GxAllocator transactionAllocator = makeAllocator(&transactionState);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = pageSize;
    createOptions.bufferCapacity = injectFailures ? 1 : 128;
    createOptions.pageCacheAllocator = &cacheAllocator;
    createOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> db;
    CHECK_DB(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    struct SavedTree {
        IndexInfo index;
        uint64_t rootPageId;
        uint64_t parentPageId;
        uint64_t splitLeafPageId;
        std::vector<uint64_t> rootChildren;
        std::vector<std::vector<uint8_t> > rootBounds;
        std::vector<uint64_t> parentChildren;
        std::vector<uint64_t> leafIds;
        DatabasePage rootImage;
        DatabasePage parentImage;
        DatabasePage splitLeafImage;

        SavedTree()
            : rootPageId(0), parentPageId(0), splitLeafPageId(0) {}
    } primary, secondary;

    const uint32_t pageCapacity = pageSize - kPageHeaderSize;
    const uint32_t maxTextKeyBytes = maxIndexKeyBytes(pageSize);
    const uint32_t textBytes = maxTextKeyBytes - 1;
    const uint32_t physicalSeparatorBytes = maxTextKeyBytes + 12u;
    const uint32_t maxLeafEntries = maxLeafEntriesForKey(
        pageCapacity, maxTextKeyBytes);
    CHECK(maxLeafEntries == 4);
    CHECK(physicalSeparatorBytes == (pageSize == 512 ? 102u : 998u));

    auto captureTree = [&](const IndexInfo& index, SavedTree& saved) {
        saved.index = index;
        saved.rootPageId = index.rootPageId;
        CHECK_DB(db->pages().readPage(saved.rootPageId, saved.rootImage),
                 DbStatus::Ok);
        CHECK(saved.rootImage.type == PageType::IndexInternal);
        CHECK(readInternalChildren(*db, saved.rootPageId, pageCapacity,
                                   saved.rootChildren, saved.rootBounds));
        CHECK(saved.rootChildren.size() == 2 && saved.rootBounds.size() == 2);
        if (saved.rootChildren.size() != 2) return;
        saved.parentPageId = saved.rootChildren[1];
        CHECK_DB(db->pages().readPage(saved.parentPageId, saved.parentImage),
                 DbStatus::Ok);
        std::vector<std::vector<uint8_t> > parentBounds;
        CHECK(readInternalChildren(*db, saved.parentPageId, pageCapacity,
                                   saved.parentChildren, parentBounds));
        CHECK(saved.parentChildren.size() == 3);
        if (saved.parentChildren.size() != 3) return;
        saved.splitLeafPageId = saved.parentChildren.back();
        CHECK_DB(db->pages().readPage(saved.splitLeafPageId,
                                      saved.splitLeafImage), DbStatus::Ok);
        CHECK(saved.splitLeafImage.type == PageType::IndexLeaf &&
              loadLe32(saved.splitLeafImage.payload.data() + 16) ==
                  maxLeafEntries);
        for (size_t i = 0; i < saved.rootChildren.size(); ++i) {
            std::vector<uint64_t> childIds;
            std::vector<std::vector<uint8_t> > childBounds;
            CHECK(readInternalChildren(*db, saved.rootChildren[i], pageCapacity,
                                       childIds, childBounds));
            saved.leafIds.insert(saved.leafIds.end(), childIds.begin(),
                                 childIds.end());
        }
    };

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE ParentRows (Id INT32 PRIMARY KEY);"));
        CHECK(runSql(engine, "INSERT INTO ParentRows VALUES (1);"));
        CHECK(runSql(engine,
                     "CREATE TABLE PriorRows (Id INT32 PRIMARY KEY, ParentId INT32 NOT NULL, V TEXT NOT NULL, FOREIGN KEY (ParentId) REFERENCES ParentRows (Id));"));
        CHECK(runSql(engine,
                     "INSERT INTO PriorRows VALUES (1, 1, 'before');"));
        CHECK(runSql(engine,
                     "CREATE TABLE RootRows (K TEXT PRIMARY KEY, Mirror TEXT NOT NULL);"));
        CHECK(runSql(engine,
                     "CREATE UNIQUE INDEX IX_RootRows_Mirror ON RootRows (Mirror);"));
        CHECK(runSql(engine, "BEGIN;"));
        for (uint32_t ordinal = 1; ordinal <= 8; ++ordinal) {
            const std::string key = rootSplitTextKey(textBytes, ordinal);
            CHECK(key.size() == textBytes);
            CHECK(runSql(engine, "INSERT INTO RootRows VALUES ('" + key +
                                 "', '" + key + "');"));
        }
        CHECK(runSql(engine, "COMMIT;"));
        CHECK(findIndexInfo(*db, "RootRows", "K", primary.index));
        CHECK(findIndexInfo(*db, "RootRows", "Mirror", secondary.index));
        captureTree(primary.index, primary);
        captureTree(secondary.index, secondary);

        CHECK(primary.rootPageId == (pageSize == 512 ? 24u : 23u));
        CHECK(primary.parentPageId == (pageSize == 512 ? 23u : 22u));
        CHECK(primary.splitLeafPageId == (pageSize == 512 ? 22u : 21u));
        CHECK(db->pageCount() == (pageSize == 512 ? 28u : 27u));
        CHECK(primary.rootChildren[0] != primary.parentPageId &&
              primary.rootChildren[1] == primary.parentPageId);
        CHECK(!internalCanAcceptSeparator(*db, primary.parentPageId,
                                          pageCapacity,
                                          physicalSeparatorBytes));
        CHECK(internalCanAcceptSeparator(*db, primary.rootPageId,
                                         pageCapacity,
                                         physicalSeparatorBytes));
        CHECK(secondary.rootChildren.size() == 2 &&
              secondary.parentChildren.size() == 3 &&
              loadLe32(secondary.splitLeafImage.payload.data() + 16) ==
                  maxLeafEntries);
        IndexValidation beforePrimary;
        IndexValidation beforeSecondary;
        CHECK_DB(db->validateIndex(primary.index.indexId, beforePrimary),
                 DbStatus::Ok);
        CHECK_DB(db->validateIndex(secondary.index.indexId, beforeSecondary),
                 DbStatus::Ok);
        CHECK(beforePrimary.ok && beforePrimary.entryCount == 8 &&
              beforePrimary.height == 3);
        CHECK(beforeSecondary.ok && beforeSecondary.entryCount == 8 &&
              beforeSecondary.height == 3);
    }

    const uint64_t originalPageCount = db->pageCount();
    bool appendAllocatesNewPage = false;
    const size_t appendStageCount = rootInsertHeapStageCount(
        *db, pageSize, textBytes, appendAllocatesNewPage);
    CHECK(appendStageCount == (appendAllocatesNewPage ? 2u : 1u));
    const int32_t baselineRows = 8;
    const int32_t splitId = baselineRows + 1;
    const std::string splitKey =
        rootSplitTextKey(textBytes, static_cast<uint32_t>(splitId));
    const std::string splitSql = "INSERT INTO RootRows VALUES ('" + splitKey +
        "', '" + splitKey + "');";

    const auto verifyBaselineAfterFailure = [&](SqlEngine& engine,
                                                Transaction* tx,
                                                const std::string& expectedPrior) {
        IndexInfo currentPrimary;
        IndexInfo currentSecondary;
        CHECK(findIndexInfo(*db, "RootRows", "K", currentPrimary));
        CHECK(findIndexInfo(*db, "RootRows", "Mirror", currentSecondary));
        CHECK(currentPrimary.rootPageId == primary.rootPageId &&
              currentSecondary.rootPageId == secondary.rootPageId &&
              currentPrimary.entryCount == baselineRows &&
              currentSecondary.entryCount == baselineRows);
        DatabasePage rootPage;
        DatabasePage parentPage;
        DatabasePage leafPage;
        CHECK_DB(db->pages().readPage(primary.rootPageId, rootPage),
                 DbStatus::Ok);
        CHECK_DB(db->pages().readPage(primary.parentPageId, parentPage),
                 DbStatus::Ok);
        CHECK_DB(db->pages().readPage(primary.splitLeafPageId, leafPage),
                 DbStatus::Ok);
        CHECK(rootPage.payload == primary.rootImage.payload &&
              parentPage.payload == primary.parentImage.payload &&
              leafPage.payload == primary.splitLeafImage.payload);
        CHECK_DB(db->pages().readPage(secondary.rootPageId, rootPage),
                 DbStatus::Ok);
        CHECK_DB(db->pages().readPage(secondary.parentPageId, parentPage),
                 DbStatus::Ok);
        CHECK_DB(db->pages().readPage(secondary.splitLeafPageId, leafPage),
                 DbStatus::Ok);
        CHECK(rootPage.payload == secondary.rootImage.payload &&
              parentPage.payload == secondary.parentImage.payload &&
              leafPage.payload == secondary.splitLeafImage.payload);
        CHECK(countRows(engine, "RootRows") == baselineRows);
        if (tx != nullptr) CHECK(tx->finalPageCount() == originalPageCount);
        IndexValidation primaryValidation;
        IndexValidation secondaryValidation;
        CHECK_DB(db->validateIndex(primary.index.indexId, primaryValidation),
                 DbStatus::Ok);
        CHECK_DB(db->validateIndex(secondary.index.indexId,
                                   secondaryValidation), DbStatus::Ok);
        CHECK(primaryValidation.ok && primaryValidation.height == 3 &&
              primaryValidation.entryCount == baselineRows);
        CHECK(secondaryValidation.ok && secondaryValidation.height == 3 &&
              secondaryValidation.entryCount == baselineRows);
        for (size_t i = 0; i < 2; ++i) {
            const char* column = i == 0 ? "K" : "Mirror";
            const SqlExecutionResult absent = engine.execute(
                "SELECT K FROM RootRows WHERE " + std::string(column) +
                " = '" + splitKey + "';");
            CHECK(absent.ok && absent.statements.size() == 1 &&
                  absent.statements[0].accessPath == "IndexLookup" &&
                  absent.statements[0].resultSet.rowCount() == 0);
        }
        const SqlExecutionResult prior = engine.execute(
            "SELECT V FROM PriorRows WHERE Id = 1;");
        CHECK(prior.ok && prior.statements.size() == 1 &&
              prior.statements[0].resultSet.value(0, 0).textValue() ==
                  expectedPrior);
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
    };

    if (injectFailures) {
        // Five allocator-owned candidates are prepared in order: left/right
        // leaf, left/right internal parent, then the grandparent replacement.
        const size_t candidateFailureAfter[] = {0, 1, 2, 3, 4, 5, 6};
        for (size_t fault = 0; fault < 7; ++fault) {
            SqlEngine engine(*db);
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'prior-update' WHERE Id = 1;"));
            Transaction* tx = db->activeTransaction();
            CHECK(tx != nullptr);
            if (tx == nullptr) break;
            const uint32_t modifiedBefore = tx->modifiedPageCount();
            const uint64_t pageCountBefore = tx->finalPageCount();
            const size_t ownedBefore = transactionState.outstanding;
            const uint64_t imageAllocationsBefore =
                tx->pageStats().imageAllocations;
            const size_t attemptsBefore = transactionState.matchingAttempts;
            const size_t successesBefore = transactionState.matchingSuccesses;
            const size_t failuresBefore = transactionState.matchingFailures;
            const size_t allowed = candidateFailureAfter[fault];
            failMatchingAfter(&transactionState, pageCapacity, allowed);
            const SqlExecutionResult failedInsert = engine.execute(splitSql);
            disableFailure(&transactionState);
            std::printf("R3N candidate fault pageSize=%u allowed=%lu error=%s matches=%lu/%lu imageStages=%llu\n",
                        pageSize, static_cast<unsigned long>(allowed),
                        failedInsert.error.message.c_str(),
                        static_cast<unsigned long>(transactionState.matchingSuccesses - successesBefore),
                        static_cast<unsigned long>(transactionState.matchingAttempts - attemptsBefore),
                        static_cast<unsigned long long>(tx->pageStats().imageAllocations - imageAllocationsBefore));
            const char* expectedError = allowed == 0
                ? "heap page preparation allocation failed"
                : ((allowed <= 2 || allowed == 6)
                    ? "index leaf preparation allocation failed"
                    : "index internal preparation allocation failed");
            CHECK(!failedInsert.ok && failedInsert.transactionActiveAfter &&
                  failedInsert.error.message == expectedError);
            CHECK(transactionState.matchingAttempts - attemptsBefore ==
                      allowed + 1 &&
                  transactionState.matchingSuccesses - successesBefore ==
                      allowed &&
                  transactionState.matchingFailures == failuresBefore + 1);
            CHECK(tx->modifiedPageCount() == modifiedBefore &&
                  tx->finalPageCount() == pageCountBefore &&
                  transactionState.outstanding == ownedBefore);
            if (allowed == 6) {
                CHECK(tx->pageStats().imageAllocations >=
                          imageAllocationsBefore + appendStageCount + 5);
            }
            verifyBaselineAfterFailure(engine, tx, "prior-update");

            CHECK(runSql(engine, splitSql));
            IndexInfo grownPrimary;
            IndexInfo grownSecondary;
            CHECK(findIndexInfo(*db, "RootRows", "K", grownPrimary));
            CHECK(findIndexInfo(*db, "RootRows", "Mirror", grownSecondary));
            checkCheckedGrandparentSplitShape(
                *db, grownPrimary, primary.rootPageId, primary.parentPageId,
                primary.rootChildren, primary.rootBounds,
                primary.parentChildren, primary.leafIds,
                static_cast<uint64_t>(baselineRows + 1), pageSize,
                nullptr, nullptr);
            checkCheckedGrandparentSplitShape(
                *db, grownSecondary, secondary.rootPageId,
                secondary.parentPageId, secondary.rootChildren,
                secondary.rootBounds, secondary.parentChildren,
                secondary.leafIds,
                static_cast<uint64_t>(baselineRows + 1), pageSize,
                nullptr, nullptr);
            CHECK(tx->finalPageCount() == pageCountBefore +
                  (appendAllocatesNewPage ? 1u : 0u) + 4u);
            CHECK(runSql(engine, "ROLLBACK;"));
            verifyBaselineAfterFailure(engine, nullptr, "before");
        }

        // The converted parent/grandparent plans and their physical separator
        // each reserve one bounded transaction-allocator buffer before the
        // first structural page image is staged.
        struct SplitPlanRecordLayout {
            uint64_t pageId;
            const uint8_t* lowerBound;
            uint32_t lowerBoundSize;
        };
        const size_t plannerAllocationSizes[] = {
            (primary.parentChildren.size() + 1) * sizeof(SplitPlanRecordLayout),
            physicalSeparatorBytes,
            (primary.rootChildren.size() + 1) * sizeof(SplitPlanRecordLayout)
        };
        const char* plannerAllocationErrors[] = {
            "index parent split planning allocation failed",
            "index split separator allocation failed",
            "index grandparent split planning allocation failed"
        };
        for (size_t fault = 0; fault < 3; ++fault) {
            SqlEngine engine(*db);
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'prior-update' WHERE Id = 1;"));
            Transaction* tx = db->activeTransaction();
            CHECK(tx != nullptr);
            if (tx == nullptr) break;
            const uint32_t modifiedBefore = tx->modifiedPageCount();
            const uint64_t pageCountBefore = tx->finalPageCount();
            const size_t ownedBefore = transactionState.outstanding;
            const uint64_t imageAllocationsBefore =
                tx->pageStats().imageAllocations;
            const size_t attemptsBefore = transactionState.matchingAttempts;
            const size_t successesBefore = transactionState.matchingSuccesses;
            const size_t failuresBefore = transactionState.matchingFailures;
            failMatchingAfter(&transactionState,
                              plannerAllocationSizes[fault], 0);
            const SqlExecutionResult failedInsert = engine.execute(splitSql);
            disableFailure(&transactionState);

            CHECK(!failedInsert.ok && failedInsert.transactionActiveAfter &&
                  failedInsert.error.code == SqlErrorCode::ResourceLimit &&
                  failedInsert.error.message == plannerAllocationErrors[fault]);
            CHECK(transactionState.matchingAttempts - attemptsBefore == 1 &&
                  transactionState.matchingSuccesses - successesBefore == 0 &&
                  transactionState.matchingFailures == failuresBefore + 1);
            CHECK(tx->modifiedPageCount() == modifiedBefore &&
                  tx->finalPageCount() == pageCountBefore &&
                  tx->pageStats().imageAllocations ==
                      imageAllocationsBefore + appendStageCount &&
                  transactionState.outstanding == ownedBefore);
            std::printf("R3O planner fault pageSize=%u site=%s bytes=%lu error=%s structuralImages=0\n",
                        pageSize, plannerAllocationErrors[fault],
                        static_cast<unsigned long>(plannerAllocationSizes[fault]),
                        failedInsert.error.message.c_str());
            verifyBaselineAfterFailure(engine, tx, "prior-update");

            CHECK(runSql(engine, splitSql));
            CHECK(countRows(engine, "RootRows") == baselineRows + 1);
            CHECK(runSql(engine, "ROLLBACK;"));
            verifyBaselineAfterFailure(engine, nullptr, "before");
        }

        // Fail after the new leaf has staged, then after both new pages and
        // the old leaf/parent replacements have staged but before the fitting
        // grandparent replacement. Both must restore the INSERT savepoint.
        const size_t stageSuccesses[] = {1, 4};
        for (size_t fault = 0; fault < 2; ++fault) {
            SqlEngine engine(*db);
            CHECK(runSql(engine, "BEGIN;"));
            CHECK(runSql(engine,
                         "UPDATE PriorRows SET V = 'prior-update' WHERE Id = 1;"));
            Transaction* tx = db->activeTransaction();
            CHECK(tx != nullptr);
            if (tx == nullptr) break;
            const uint32_t modifiedBefore = tx->modifiedPageCount();
            const uint64_t pageCountBefore = tx->finalPageCount();
            const size_t ownedBefore = transactionState.outstanding;
            const size_t attemptsBefore = transactionState.matchingAttempts;
            const size_t successesBefore = transactionState.matchingSuccesses;
            const size_t failuresBefore = transactionState.matchingFailures;
            const size_t allowed = modifiedBefore + appendStageCount +
                stageSuccesses[fault];
            failMatchingAfter(&transactionState, pageSize, allowed);
            const SqlExecutionResult failedInsert = engine.execute(splitSql);
            disableFailure(&transactionState);
            std::printf("R3N stage fault pageSize=%u structuralStagesBeforeFailure=%lu error=%s\n",
                        pageSize,
                        static_cast<unsigned long>(stageSuccesses[fault]),
                        failedInsert.error.message.c_str());
            CHECK(!failedInsert.ok && failedInsert.transactionActiveAfter &&
                  failedInsert.error.message ==
                      "transaction page staging failed");
            CHECK(transactionState.matchingAttempts - attemptsBefore ==
                      allowed + 1 &&
                  transactionState.matchingSuccesses - successesBefore ==
                      allowed &&
                  transactionState.matchingFailures == failuresBefore + 1);
            CHECK(tx->modifiedPageCount() == modifiedBefore &&
                  tx->finalPageCount() == pageCountBefore &&
                  transactionState.outstanding == ownedBefore);
            verifyBaselineAfterFailure(engine, tx, "prior-update");
            CHECK(runSql(engine, splitSql));
            CHECK(countRows(engine, "RootRows") == baselineRows + 1);
            CHECK(runSql(engine, "ROLLBACK;"));
            verifyBaselineAfterFailure(engine, nullptr, "before");
        }
    }

    uint64_t finalPageCount = 0;
    uint64_t finalPrimaryRootId = 0;
    uint64_t finalSecondaryRootId = 0;
    uint64_t finalPrimaryNewLeafId = 0;
    std::vector<uint8_t> finalPromotedSeparator;
    const std::string finalPriorValue = "committed-grandparent-split";
    {
        SqlEngine engine(*db);
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE PriorRows SET V = 'committed-grandparent-split' WHERE Id = 1;"));
        Transaction* tx = db->activeTransaction();
        CHECK(tx != nullptr);
        const uint64_t pageCountBefore =
            tx == nullptr ? 0 : tx->finalPageCount();
        CHECK(runSql(engine, splitSql));
        CHECK(findIndexInfo(*db, "RootRows", "K", primary.index));
        CHECK(findIndexInfo(*db, "RootRows", "Mirror", secondary.index));
        finalPrimaryRootId = primary.index.rootPageId;
        finalSecondaryRootId = secondary.index.rootPageId;
        uint64_t secondaryNewLeafId = 0;
        std::vector<uint8_t> secondaryPromoted;
        checkCheckedGrandparentSplitShape(
            *db, primary.index, primary.rootPageId, primary.parentPageId,
            primary.rootChildren, primary.rootBounds, primary.parentChildren,
            primary.leafIds, static_cast<uint64_t>(baselineRows + 1),
            pageSize, &finalPrimaryNewLeafId, &finalPromotedSeparator);
        checkCheckedGrandparentSplitShape(
            *db, secondary.index, secondary.rootPageId,
            secondary.parentPageId, secondary.rootChildren,
            secondary.rootBounds, secondary.parentChildren,
            secondary.leafIds, static_cast<uint64_t>(baselineRows + 1),
            pageSize, &secondaryNewLeafId, &secondaryPromoted);
        CHECK(finalPromotedSeparator == secondaryPromoted);
        CHECK(tx != nullptr && tx->finalPageCount() == pageCountBefore +
              (appendAllocatesNewPage ? 1u : 0u) + 4u);
        CHECK(countRows(engine, "RootRows") == baselineRows + 1);

        for (int32_t ordinal = 1; ordinal <= baselineRows + 1; ++ordinal) {
            const std::string key = rootSplitTextKey(
                textBytes, static_cast<uint32_t>(ordinal));
            std::vector<uint8_t> encoded;
            CHECK(encodeIndexKey(DbType::Text, DbValue::text(key), encoded));
            std::vector<RowLocator> primaryLocators;
            std::vector<RowLocator> secondaryLocators;
            CHECK_DB(db->indexLookup(primary.index.indexId, encoded,
                                     primaryLocators), DbStatus::Ok);
            CHECK_DB(db->indexLookup(secondary.index.indexId, encoded,
                                     secondaryLocators), DbStatus::Ok);
            CHECK(primaryLocators.size() == 1 &&
                  secondaryLocators.size() == 1 &&
                  primaryLocators[0].pageId == secondaryLocators[0].pageId &&
                  primaryLocators[0].slot == secondaryLocators[0].slot);
        }
        IndexTree primaryTree(db->pages(), DbType::Text,
                              primary.index.indexId, tx);
        IndexTree secondaryTree(db->pages(), DbType::Text,
                                secondary.index.indexId, tx);
        IndexRangeBound allKeys;
        std::vector<IndexEntry> primaryOrdered;
        std::vector<IndexEntry> secondaryOrdered;
        CHECK_DB(primaryTree.rangeScan(allKeys, finalPrimaryRootId,
                                       primaryOrdered), DbStatus::Ok);
        CHECK_DB(secondaryTree.rangeScan(allKeys, finalSecondaryRootId,
                                         secondaryOrdered), DbStatus::Ok);
        CHECK(primaryOrdered.size() == baselineRows + 1 &&
              secondaryOrdered.size() == primaryOrdered.size());
        for (size_t i = 0; i < primaryOrdered.size(); ++i) {
            const std::string key = rootSplitTextKey(
                textBytes, static_cast<uint32_t>(i + 1));
            std::vector<uint8_t> encoded;
            CHECK(encodeIndexKey(DbType::Text, DbValue::text(key), encoded));
            CHECK(primaryOrdered[i].key == encoded &&
                  secondaryOrdered[i].key == encoded &&
                  primaryOrdered[i].locator.pageId ==
                      secondaryOrdered[i].locator.pageId &&
                  primaryOrdered[i].locator.slot ==
                      secondaryOrdered[i].locator.slot);
        }

        DbValue promotedValue;
        CHECK(finalPromotedSeparator.size() == physicalSeparatorBytes &&
              decodeIndexKey(DbType::Text, finalPromotedSeparator.data(),
                             finalPromotedSeparator.size() - 12,
                             promotedValue));
        if (finalPromotedSeparator.size() >= 12) {
            const std::string promotedText = promotedValue.textValue();
            const std::string suffix =
                promotedText.substr(promotedText.size() - 8);
            const int32_t boundaryOrdinal = static_cast<int32_t>(
                std::strtol(suffix.c_str(), nullptr, 10));
            CHECK(promotedText == rootSplitTextKey(
                      textBytes, static_cast<uint32_t>(boundaryOrdinal)));
            for (int32_t ordinal = boundaryOrdinal - 1;
                 ordinal <= boundaryOrdinal + 1; ++ordinal) {
                if (ordinal < 1 || ordinal > baselineRows + 1) continue;
                const std::string key = rootSplitTextKey(
                    textBytes, static_cast<uint32_t>(ordinal));
                const SqlExecutionResult primaryLookup = engine.execute(
                    "SELECT K FROM RootRows WHERE K = '" + key + "';");
                const SqlExecutionResult secondaryLookup = engine.execute(
                    "SELECT K FROM RootRows WHERE Mirror = '" + key + "';");
                CHECK(primaryLookup.ok && primaryLookup.statements.size() == 1 &&
                      primaryLookup.statements[0].accessPath == "IndexLookup" &&
                      primaryLookup.statements[0].resultSet.rowCount() == 1);
                CHECK(secondaryLookup.ok && secondaryLookup.statements.size() == 1 &&
                      secondaryLookup.statements[0].accessPath == "IndexLookup" &&
                      secondaryLookup.statements[0].resultSet.rowCount() == 1);
            }
        }
        const SqlExecutionResult duplicate = engine.execute(
            "INSERT INTO RootRows VALUES ('" +
            rootSplitTextKey(textBytes, 1) + "', '" + splitKey + "');");
        CHECK(!duplicate.ok && duplicate.transactionActiveAfter &&
              duplicate.error.message.find(
                  "unique index constraint violation") != std::string::npos);
        CHECK(countRows(engine, "RootRows") == baselineRows + 1);
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
        finalPageCount = tx == nullptr ? 0 : tx->finalPageCount();
        std::vector<uint64_t> finalRootChildren;
        std::vector<std::vector<uint8_t> > finalRootBounds;
        CHECK(readInternalChildren(*db, finalPrimaryRootId, pageCapacity,
                                   finalRootChildren, finalRootBounds));
        CHECK(finalRootChildren.size() == 3);
        std::printf("R3N observed pageSize=%u rows=%d->%d root=%llu parent=%llu splitLeaf=%llu newLeaf=%llu newRightParent=%llu children=3/2+2 height=3 pageCount=%llu->%llu promotedBytes=%lu\n",
                    pageSize, baselineRows, baselineRows + 1,
                    static_cast<unsigned long long>(finalPrimaryRootId),
                    static_cast<unsigned long long>(primary.parentPageId),
                    static_cast<unsigned long long>(primary.splitLeafPageId),
                    static_cast<unsigned long long>(finalPrimaryNewLeafId),
                    static_cast<unsigned long long>(finalRootChildren.size() > 2
                        ? finalRootChildren[2] : 0),
                    static_cast<unsigned long long>(originalPageCount),
                    static_cast<unsigned long long>(finalPageCount),
                    static_cast<unsigned long>(finalPromotedSeparator.size()));

        if (injectFailures) {
            const size_t cacheAttemptsBeforeCatalog = cacheState.attempts;
            if (tx != nullptr) {
                CHECK_DB(db->catalog().save(*tx, db->file().rootPageId(),
                                            tx->pageSize()), DbStatus::Ok);
            }
            const size_t catalogCacheReads =
                cacheState.attempts - cacheAttemptsBeforeCatalog;
            std::printf("R3N catalog serialization cache allocations=%lu\n",
                        static_cast<unsigned long>(catalogCacheReads));
            const size_t attemptsBefore = cacheState.attempts;
            const size_t successesBefore = cacheState.successes;
            const size_t failuresBefore = cacheState.allocationFailures;
            failAtAttempt(&cacheState, 1);
            const SqlExecutionResult durableCommitFailure =
                engine.execute("COMMIT;");
            disableFailure(&cacheState);
            std::printf("R3N durable commit pageSize=%u ok=%u error=%s attempts=+%lu successes=+%lu failures=+%lu recovery=%u root=%llu pageCount=%llu\n",
                        pageSize, durableCommitFailure.ok ? 1u : 0u,
                        durableCommitFailure.error.message.c_str(),
                        static_cast<unsigned long>(cacheState.attempts - attemptsBefore),
                        static_cast<unsigned long>(cacheState.successes - successesBefore),
                        static_cast<unsigned long>(cacheState.allocationFailures - failuresBefore),
                        db->diagnostics().recoveryRequired ? 1u : 0u,
                        static_cast<unsigned long long>(finalPrimaryRootId),
                        static_cast<unsigned long long>(finalPageCount));
            CHECK(!durableCommitFailure.ok &&
                  db->diagnostics().recoveryRequired);
            CHECK(cacheState.attempts == attemptsBefore + 1 &&
                  cacheState.successes == successesBefore &&
                  cacheState.allocationFailures == failuresBefore + 1);
        } else {
            CHECK(runSql(engine, "COMMIT;"));
            IndexValidation primaryAfterCommit;
            IndexValidation secondaryAfterCommit;
            CHECK_DB(db->validateIndex(primary.index.indexId,
                                       primaryAfterCommit), DbStatus::Ok);
            CHECK_DB(db->validateIndex(secondary.index.indexId,
                                       secondaryAfterCommit), DbStatus::Ok);
            CHECK(primaryAfterCommit.ok && primaryAfterCommit.height == 3 &&
                  secondaryAfterCommit.ok && secondaryAfterCommit.height == 3);
        }
        DatabasePage finalRoot;
        CHECK_DB(db->readPage(finalPrimaryRootId, finalRoot),
                 injectFailures ? DbStatus::RecoveryRequired : DbStatus::Ok);
    }

    CHECK_DB(db->close(), injectFailures ? DbStatus::RecoveryRequired
                                        : DbStatus::Ok);
    if (injectFailures) {
        std::ifstream wal(walPathFor(path).c_str(),
                          std::ios::in | std::ios::binary | std::ios::ate);
        CHECK(wal.is_open() && wal.tellg() > 32);
    }
    db.reset();
    CHECK(cacheState.outstanding == 0 && transactionState.outstanding == 0);

    DatabaseOpenOptions openOptions;
    openOptions.bufferCapacity = injectFailures ? 1 : 128;
    openOptions.pageCacheAllocator = &cacheAllocator;
    openOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> reopened;
    CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
    for (int reopenPass = 0; reopened && reopenPass < 2; ++reopenPass) {
        {
            SqlEngine engine(*reopened);
            CHECK(reopened->pageCount() == finalPageCount);
            CHECK(countRows(engine, "RootRows") == baselineRows + 1);
            CHECK(findIndexInfo(*reopened, "RootRows", "K", primary.index));
            CHECK(findIndexInfo(*reopened, "RootRows", "Mirror", secondary.index));
            CHECK(primary.index.rootPageId == finalPrimaryRootId &&
                  secondary.index.rootPageId == finalSecondaryRootId &&
                  finalPrimaryRootId == primary.rootPageId &&
                  finalSecondaryRootId == secondary.rootPageId);
            checkCheckedGrandparentSplitShape(
                *reopened, primary.index, primary.rootPageId,
                primary.parentPageId, primary.rootChildren, primary.rootBounds,
                primary.parentChildren, primary.leafIds,
                static_cast<uint64_t>(baselineRows + 1), pageSize,
                nullptr, &finalPromotedSeparator);
            checkCheckedGrandparentSplitShape(
                *reopened, secondary.index, secondary.rootPageId,
                secondary.parentPageId, secondary.rootChildren,
                secondary.rootBounds, secondary.parentChildren,
                secondary.leafIds,
                static_cast<uint64_t>(baselineRows + 1), pageSize,
                nullptr, nullptr);
            for (int32_t ordinal = 1; ordinal <= baselineRows + 1; ++ordinal) {
                const std::string key = rootSplitTextKey(
                    textBytes, static_cast<uint32_t>(ordinal));
                std::vector<uint8_t> encoded;
                CHECK(encodeIndexKey(DbType::Text, DbValue::text(key), encoded));
                std::vector<RowLocator> primaryLocators;
                std::vector<RowLocator> secondaryLocators;
                CHECK_DB(reopened->indexLookup(primary.index.indexId, encoded,
                                               primaryLocators), DbStatus::Ok);
                CHECK_DB(reopened->indexLookup(secondary.index.indexId, encoded,
                                               secondaryLocators), DbStatus::Ok);
                CHECK(primaryLocators.size() == 1 &&
                      secondaryLocators.size() == 1 &&
                      primaryLocators[0].pageId == secondaryLocators[0].pageId &&
                      primaryLocators[0].slot == secondaryLocators[0].slot);
            }
            DbValue promotedValue;
            CHECK(finalPromotedSeparator.size() == physicalSeparatorBytes &&
                  decodeIndexKey(DbType::Text, finalPromotedSeparator.data(),
                                 finalPromotedSeparator.size() - 12,
                                 promotedValue));
            if (finalPromotedSeparator.size() >= 12) {
                const std::string promotedText = promotedValue.textValue();
                const std::string suffix = promotedText.substr(
                    promotedText.size() - 8);
                const int32_t boundaryOrdinal = static_cast<int32_t>(
                    std::strtol(suffix.c_str(), nullptr, 10));
                CHECK(promotedText == rootSplitTextKey(
                          textBytes, static_cast<uint32_t>(boundaryOrdinal)));
                for (int32_t ordinal = boundaryOrdinal - 1;
                     ordinal <= boundaryOrdinal + 1; ++ordinal) {
                    if (ordinal < 1 || ordinal > baselineRows + 1) continue;
                    const std::string key = rootSplitTextKey(
                        textBytes, static_cast<uint32_t>(ordinal));
                    const SqlExecutionResult primaryLookup = engine.execute(
                        "SELECT K FROM RootRows WHERE K = '" + key + "';");
                    const SqlExecutionResult secondaryLookup = engine.execute(
                        "SELECT K FROM RootRows WHERE Mirror = '" + key + "';");
                    CHECK(primaryLookup.ok && primaryLookup.statements.size() == 1 &&
                          primaryLookup.statements[0].accessPath == "IndexLookup" &&
                          primaryLookup.statements[0].resultSet.rowCount() == 1);
                    CHECK(secondaryLookup.ok && secondaryLookup.statements.size() == 1 &&
                          secondaryLookup.statements[0].accessPath == "IndexLookup" &&
                          secondaryLookup.statements[0].resultSet.rowCount() == 1);
                }
            }
            const SqlExecutionResult prior = engine.execute(
                "SELECT V FROM PriorRows WHERE Id = 1;");
            CHECK(prior.ok && prior.statements.size() == 1 &&
                  prior.statements[0].resultSet.value(0, 0).textValue() ==
                      finalPriorValue);

            // Exercise index operations after the split, including a reused
            // prepared UPDATE and DELETE, then roll the verification transaction
            // back so the recovered committed tree remains the fixture.
            CHECK(runSql(engine, "BEGIN;"));
            const std::string firstKey = rootSplitTextKey(textBytes, 1);
            const std::string lowerMirror = firstKey.substr(
                0, firstKey.size() - 8) + "00000000";
            SqlPreparedStatement indexedUpdate;
            CHECK(engine.prepare(
                "UPDATE RootRows SET Mirror = ? WHERE K = ?;",
                indexedUpdate).isOk());
            CHECK(indexedUpdate.bindText(0, lowerMirror).isOk());
            CHECK(indexedUpdate.bindText(1, firstKey).isOk());
            SqlStatementResult updateResult = indexedUpdate.execute();
            CHECK(updateResult.ok && updateResult.affectedRows == 1);
            const SqlExecutionResult changedSecondary = engine.execute(
                "SELECT K FROM RootRows WHERE Mirror = '" + lowerMirror + "';");
            CHECK(changedSecondary.ok && changedSecondary.statements.size() == 1 &&
                  changedSecondary.statements[0].accessPath == "IndexLookup" &&
                  changedSecondary.statements[0].resultSet.rowCount() == 1);
            CHECK(indexedUpdate.bindText(0, firstKey).isOk());
            CHECK(indexedUpdate.bindText(1, firstKey).isOk());
            updateResult = indexedUpdate.execute();
            CHECK(updateResult.ok && updateResult.affectedRows == 1);

            const std::string extraKey = rootSplitTextKey(
                textBytes, static_cast<uint32_t>(baselineRows + 2));
            SqlPreparedStatement indexedInsert;
            CHECK(engine.prepare(
                "INSERT INTO RootRows VALUES (?, ?);", indexedInsert).isOk());
            CHECK(indexedInsert.bindText(0, extraKey).isOk());
            CHECK(indexedInsert.bindText(1, extraKey).isOk());
            SqlStatementResult insertResult = indexedInsert.execute();
            CHECK(insertResult.ok && insertResult.affectedRows == 1);
            IndexValidation afterInsert;
            CHECK_DB(reopened->validateIndex(primary.index.indexId, afterInsert),
                     DbStatus::Ok);
            CHECK(afterInsert.ok && afterInsert.entryCount == baselineRows + 2 &&
                  afterInsert.height == 3);
            SqlPreparedStatement indexedDelete;
            CHECK(engine.prepare("DELETE FROM RootRows WHERE K = ?;",
                                 indexedDelete).isOk());
            CHECK(indexedDelete.bindText(0, extraKey).isOk());
            SqlStatementResult deleteResult = indexedDelete.execute();
            CHECK(deleteResult.ok && deleteResult.affectedRows == 1);
            IndexValidation afterDelete;
            CHECK_DB(reopened->validateIndex(primary.index.indexId, afterDelete),
                     DbStatus::Ok);
            CHECK(afterDelete.ok && afterDelete.entryCount == baselineRows + 1 &&
                  afterDelete.height == 3);
            CHECK(indexedDelete.bindText(0,
                                         rootSplitTextKey(textBytes, 2)).isOk());
            deleteResult = indexedDelete.execute();
            CHECK(deleteResult.ok && deleteResult.affectedRows == 1);
            IndexValidation afterExistingDelete;
            CHECK_DB(reopened->validateIndex(primary.index.indexId,
                                             afterExistingDelete), DbStatus::Ok);
            CHECK(afterExistingDelete.ok &&
                  afterExistingDelete.entryCount == baselineRows &&
                  afterExistingDelete.height == 3);
            CHECK(runSql(engine, "ROLLBACK;"));
            CHECK(countRows(engine, "RootRows") == baselineRows + 1);
            checkIndexes(*reopened);
            std::string integrity;
            CHECK_DB(reopened->validateIntegrity(integrity), DbStatus::Ok);
        }
        CHECK_DB(reopened->close(), DbStatus::Ok);
        reopened.reset();
        CHECK(cacheState.outstanding == 0 && transactionState.outstanding == 0);
        if (reopenPass == 0) {
            CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
        }
    }

    CHECK(cacheState.outstanding == 0 && transactionState.outstanding == 0);
    removeDatabase(path);
}

void testVariableWidthExactFitSplit() {
    std::printf("NDB1R4P11R3L variable-width and exact-fit index split\n");
    const std::string path = testPath() + ".r3l-text.gxdb";
    removeDatabase(path);

    AllocationState cacheState;
    AllocationState transactionState;
    GxAllocator cacheAllocator = makeAllocator(&cacheState);
    GxAllocator transactionAllocator = makeAllocator(&transactionState);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    createOptions.bufferCapacity = 32;
    createOptions.pageCacheAllocator = &cacheAllocator;
    createOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> db;
    CHECK_DB(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    const uint32_t payloadCapacity = 512 - kPageHeaderSize;
    const uint32_t maxKeyBytes = maxIndexKeyBytes(512);
    CHECK(maxKeyBytes == 90);
    std::vector<std::string> keys;
    keys.push_back("a");
    for (int suffix = 1; suffix <= 4; ++suffix) {
        std::string key("b");
        key.append(87, 'x');
        key.push_back(static_cast<char>('0' + suffix));
        keys.push_back(key);
    }
    std::string lastLargeKey("z");
    lastLargeKey.append(87, 'z');
    lastLargeKey.push_back('5');
    keys.push_back(lastLargeKey);
    std::vector<uint8_t> largeEncoding;
    CHECK(encodeIndexKey(DbType::Text, DbValue::text(keys[1]), largeEncoding));
    CHECK(largeEncoding.size() == maxKeyBytes);
    CHECK(IndexTree::makePhysicalKey(largeEncoding, RowLocator(0, 0)).size() ==
          maxKeyBytes + 12);

    uint32_t indexId = 0;
    uint64_t rootPageId = 0;
    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE TextSplitRows (K TEXT PRIMARY KEY, V INT32 NOT NULL);"));
        IndexInfo primary;
        CHECK(findIndexInfo(*db, "TextSplitRows", "K", primary));
        indexId = primary.indexId;
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "INSERT INTO TextSplitRows VALUES ('" + keys[0] + "', 1);"));
        for (int32_t i = 1; i <= 4; ++i) {
            CHECK(runSql(engine,
                         "INSERT INTO TextSplitRows VALUES ('" + keys[i] + "', " +
                         std::to_string(i + 1) + ");"));
        }
        CHECK(runSql(engine, "COMMIT;"));
        CHECK(findIndexInfo(*db, "TextSplitRows", "K", primary));
        rootPageId = primary.rootPageId;

        IndexValidation before;
        CHECK_DB(db->validateIndex(indexId, before), DbStatus::Ok);
        CHECK(before.ok && before.entryCount == 5 && before.height == 2 &&
              before.pagesVisited == 3);
        DatabasePage root;
        CHECK_DB(db->pages().readPage(rootPageId, root), DbStatus::Ok);
        uint64_t leftId = 0;
        uint64_t fullLeafId = 0;
        std::vector<uint8_t> firstBound;
        std::vector<uint8_t> fullLeafBound;
        CHECK(findIndexChild(root, payloadCapacity, 0, leftId, firstBound));
        CHECK(findIndexChild(root, payloadCapacity, 1, fullLeafId,
                             fullLeafBound));
        CHECK(fullLeafBound.size() == maxKeyBytes + 12);
        DatabasePage fullLeaf;
        CHECK_DB(db->pages().readPage(fullLeafId, fullLeaf), DbStatus::Ok);
        CHECK(fullLeaf.type == PageType::IndexLeaf &&
              loadLe32(fullLeaf.payload.data() + 16) == 4);
        uint32_t dataEnd = kIndexLeafHeaderSize;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t slotOffset =
                payloadCapacity - kIndexSlotSize * (i + 1u);
            const uint32_t offset = loadLe32(fullLeaf.payload.data() + slotOffset);
            const uint32_t length =
                loadLe32(fullLeaf.payload.data() + slotOffset + 4);
            if (offset + length > dataEnd) dataEnd = offset + length;
        }
        CHECK(dataEnd + 4 * kIndexSlotSize == payloadCapacity);

        const uint64_t pageCountBeforeSplit = db->pageCount();
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "INSERT INTO TextSplitRows VALUES ('" + keys[5] + "', 6);"));
        Transaction* tx = db->activeTransaction();
        CHECK(tx != nullptr && tx->finalPageCount() > pageCountBeforeSplit &&
              tx->finalPageCount() <= pageCountBeforeSplit + 2);
        IndexValidation after;
        CHECK_DB(db->validateIndex(indexId, after), DbStatus::Ok);
        CHECK(after.ok && after.entryCount == 6 && after.height == 2 &&
              after.pagesVisited == 4);

        DatabasePage splitRoot;
        CHECK_DB(tx->readPage(rootPageId, splitRoot), DbStatus::Ok);
        CHECK(loadLe32(splitRoot.payload.data() + 16) == 3);
        uint64_t childIds[3] = {0, 0, 0};
        std::vector<uint8_t> separators[3];
        for (uint32_t i = 0; i < 3; ++i) {
            CHECK(findIndexChild(splitRoot, payloadCapacity, i,
                                 childIds[i], separators[i]));
        }
        const uint64_t newLeafId = tx != nullptr
            ? tx->finalPageCount() - 1 : 0;
        CHECK(childIds[0] == leftId && childIds[2] == newLeafId);
        CHECK(separators[1].size() == maxKeyBytes + 12 &&
              separators[2].size() == maxKeyBytes + 12);
        for (uint32_t i = 0; i < 3; ++i) {
            DatabasePage leaf;
            CHECK_DB(tx->readPage(childIds[i], leaf), DbStatus::Ok);
            const uint64_t next = i < 2 ? childIds[i + 1] : 0;
            CHECK(loadLe64(leaf.payload.data() + 8) == next);
            std::vector<uint8_t> firstPhysical;
            CHECK(readLeafFirstPhysicalKey(leaf, payloadCapacity, firstPhysical));
            if (i > 0) CHECK(separators[i] == firstPhysical);
        }

        IndexTree tree(db->pages(), DbType::Text, indexId, tx);
        IndexRangeBound allKeys;
        std::vector<IndexEntry> ordered;
        CHECK_DB(tree.rangeScan(allKeys, rootPageId, ordered), DbStatus::Ok);
        CHECK(ordered.size() == keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            std::vector<uint8_t> encoded;
            CHECK(encodeIndexKey(DbType::Text, DbValue::text(keys[i]), encoded));
            if (i < ordered.size()) CHECK(ordered[i].key == encoded);
            const SqlExecutionResult lookup = engine.execute(
                "SELECT V FROM TextSplitRows WHERE K = '" + keys[i] + "';");
            CHECK(lookup.ok && lookup.statements.size() == 1 &&
                  lookup.statements[0].accessPath == "IndexLookup" &&
                  lookup.statements[0].resultSet.rowCount() == 1 &&
                  lookup.statements[0].resultSet.value(0, 0).int32Value() ==
                      static_cast<int32_t>(i + 1));
        }
        const SqlExecutionResult duplicate = engine.execute(
            "INSERT INTO TextSplitRows VALUES ('" + keys[1] + "', 99);");
        CHECK(!duplicate.ok && duplicate.transactionActiveAfter &&
              duplicate.error.message.find(
                  "unique index constraint violation") != std::string::npos);
        CHECK(countRows(engine, "TextSplitRows") == 6);
        const uint64_t finalTransactionPageCount =
            tx != nullptr ? tx->finalPageCount() : 0;
        CHECK(runSql(engine, "COMMIT;"));
        CHECK(db->pageCount() == finalTransactionPageCount);
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);
    }

    CHECK_DB(db->close(), DbStatus::Ok);
    db.reset();
    CHECK(cacheState.outstanding == 0 && transactionState.outstanding == 0);
    DatabaseOpenOptions openOptions;
    openOptions.pageCacheAllocator = &cacheAllocator;
    openOptions.transactionPageAllocator = &transactionAllocator;
    std::unique_ptr<Database> reopened;
    CHECK_DB(Database::open(path, openOptions, reopened), DbStatus::Ok);
    if (reopened) {
        IndexInfo primary;
        CHECK(findIndexInfo(*reopened, "TextSplitRows", "K", primary));
        IndexValidation validation;
        CHECK_DB(reopened->validateIndex(indexId, validation), DbStatus::Ok);
        CHECK(validation.ok && validation.entryCount == 6 &&
              validation.height == 2 && validation.pagesVisited == 4);
        CHECK(primary.rootPageId == rootPageId);
        SqlEngine engine(*reopened);
        CHECK(countRows(engine, "TextSplitRows") == 6);
        std::string integrity;
        CHECK_DB(reopened->validateIntegrity(integrity), DbStatus::Ok);
        CHECK_DB(reopened->close(), DbStatus::Ok);
    }
    reopened.reset();
    CHECK(cacheState.outstanding == 0 && transactionState.outstanding == 0);
    removeDatabase(path);
}

} // namespace

int main() {
    testHeapByteConstruction();
    testHeapAppendPreparation();
    testAllocationFailureAndRetry();
    testProductionMutationFailureAndRelocation();
    testInsertSavepointFailureAtomicity();
    testCheckedNonCascadingLeafSplit(512, true);
    testCheckedNonCascadingLeafSplit(4096, false);
    testCheckedInternalRootSplit(512, true);
    testCheckedInternalRootSplit(4096, false);
    testCheckedInternalGrandparentSplit(512, true);
    testCheckedInternalGrandparentSplit(4096, true);
    testVariableWidthExactFitSplit();
    testCheckedHeapAppendPublication();
    std::printf("NDB1R4P11R3J heap append publication: %d checks, %d failures\n",
                gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
