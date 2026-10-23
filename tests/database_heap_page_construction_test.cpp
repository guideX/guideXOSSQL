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

} // namespace

int main() {
    testHeapByteConstruction();
    testHeapAppendPreparation();
    testAllocationFailureAndRetry();
    testProductionMutationFailureAndRelocation();
    testInsertSavepointFailureAtomicity();
    testCheckedHeapAppendPublication();
    std::printf("NDB1R4P11R3J heap append publication: %d checks, %d failures\n",
                gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
