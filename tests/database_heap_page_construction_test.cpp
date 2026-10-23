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
    size_t matchingSuccesses;
    size_t matchingFailures;
    size_t failAtMatchingSuccess;
    bool failureEnabled;
    size_t failAtAttempt;
    bool failAtAttemptEnabled;
    size_t allocationFailures;

    AllocationState()
        : attempts(0), successes(0), frees(0), outstanding(0),
          matchingSize(0), matchingSuccesses(0), matchingFailures(0),
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
    if (state->failureEnabled && size == state->matchingSize &&
        state->matchingSuccesses >= state->failAtMatchingSuccess) {
        ++state->matchingFailures;
        return false;
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
    CHECK(output.data[kHeapHeaderSize + 8] == 0);

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
    std::printf("NDB1R4P11R3I INSERT savepoint and checked index writer\n");
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
        CHECK(runSql(engine, "CREATE INDEX IX_ChildRows_Code ON ChildRows (Code);"));

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
        CHECK(countRows(engine, "ChildRows") == 0);
        const SqlExecutionResult priorAfterSnapshotFailure = engine.execute(
            "SELECT V FROM PriorRows WHERE Id = 1;");
        CHECK(priorAfterSnapshotFailure.ok &&
              priorAfterSnapshotFailure.statements[0].resultSet.value(0, 0).textValue() ==
                  "rollback-probe");
        CHECK(runSql(engine, "ROLLBACK;"));
        CHECK(countRows(engine, "ChildRows") == 0);
        const SqlExecutionResult priorAfterExplicitRollback = engine.execute(
            "SELECT V FROM PriorRows WHERE Id = 1;");
        CHECK(priorAfterExplicitRollback.ok &&
              priorAfterExplicitRollback.statements[0].resultSet.value(0, 0).textValue() ==
                  "before");

        // First fail the primary leaf's payload preparation after the heap
        // page is staged. This also proves IndexTree::insert does not mistake
        // allocation NoSpace for a leaf-overflow split request.
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE PriorRows SET V = 'commit-probe' WHERE Id = 1;"));
        tx = db->activeTransaction();
        CHECK(tx != nullptr && tx->modifiedPageCount() >= 2);
        const uint32_t writerPageCount = tx ? tx->modifiedPageCount() : 0;
        const uint64_t writerNextPage = tx ? tx->finalPageCount() : 0;
        const size_t writerOwnedBytes = transactionState.outstanding;

        // Two matching allocations prepare the savepoint's Catalog copy and
        // stage the heap row; fail the next allocation in IndexTree::writeLeaf.
        CHECK(insertChild.bindInt64(0, 10).isOk());
        CHECK(insertChild.bindNull(1).isOk());
        CHECK(insertChild.bindText(2, "first-writer-fail").isOk());
        SqlStatementResult writerFailure;
        {
            ScopedHostedAllocationFailure failure(480, 2);
            writerFailure = insertChild.execute();
            CHECK(failure.matchingAttempts() == 3);
            CHECK(failure.matchingSuccesses() == 2);
            CHECK(failure.matchingFailures() == 1);
        }
        CHECK(!writerFailure.ok && writerFailure.transactionActiveAfter);
        CHECK(writerFailure.error.message ==
              "index leaf payload allocation failed");
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == writerPageCount &&
              db->activeTransaction()->finalPageCount() == writerNextPage);
        CHECK(transactionState.outstanding == writerOwnedBytes);
        CHECK(countRows(engine, "ChildRows") == 0);

        // Fail a later writeLeaf while IndexTree::insert is publishing the
        // primary entry. The error must stay an allocation failure rather
        // than being reinterpreted as a leaf split request.
        CHECK(insertChild.bindText(2, "insert-writer-fail").isOk());
        {
            ScopedHostedAllocationFailure failure(480, 7);
            writerFailure = insertChild.execute();
            CHECK(failure.matchingAttempts() == 8);
            CHECK(failure.matchingSuccesses() == 7);
            CHECK(failure.matchingFailures() == 1);
        }
        CHECK(!writerFailure.ok && writerFailure.transactionActiveAfter);
        CHECK(writerFailure.error.message ==
              "index leaf payload allocation failed");
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == writerPageCount &&
              db->activeTransaction()->finalPageCount() == writerNextPage);
        CHECK(transactionState.outstanding == writerOwnedBytes);
        CHECK(countRows(engine, "ChildRows") == 0);

        // Twelve successful page-sized allocations also let the first index
        // finish. Fail the next index leaf after that prior index publication.
        CHECK(insertChild.bindText(2, "writer-fail").isOk());
        {
            ScopedHostedAllocationFailure failure(480, 12);
            writerFailure = insertChild.execute();
            CHECK(failure.matchingAttempts() == 13);
            CHECK(failure.matchingSuccesses() == 12);
            CHECK(failure.matchingFailures() == 1);
        }
        CHECK(!writerFailure.ok && writerFailure.transactionActiveAfter);
        CHECK(writerFailure.error.message ==
              "index leaf payload allocation failed");
        CHECK(db->activeTransaction() != nullptr &&
              db->activeTransaction()->modifiedPageCount() == writerPageCount &&
              db->activeTransaction()->finalPageCount() == writerNextPage);
        CHECK(transactionState.outstanding == writerOwnedBytes);
        CHECK(countRows(engine, "ChildRows") == 0);

        std::vector<IndexInfo> stagedIndexes;
        CHECK_DB(db->listIndexes(stagedIndexes), DbStatus::Ok);
        for (size_t i = 0; i < stagedIndexes.size(); ++i) {
            if (stagedIndexes[i].tableName == "ChildRows") {
                CHECK(stagedIndexes[i].entryCount == 0);
                IndexValidation validation;
                CHECK_DB(db->validateIndex(stagedIndexes[i].indexId, validation),
                         DbStatus::Ok);
                CHECK(validation.ok && validation.entryCount == 0);
            }
        }
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
        CHECK(countRows(engine, "ChildRows") == 0);

        CHECK(insertChild.bindInt64(0, 10).isOk());
        CHECK(insertChild.bindInt64(1, 1).isOk());
        CHECK(insertChild.bindText(2, "valid-code").isOk());
        CHECK(insertChild.execute().ok);
        CHECK(countRows(engine, "ChildRows") == 1);
        checkIndexes(*db);
        std::string integrity;
        CHECK_DB(db->validateIntegrity(integrity), DbStatus::Ok);

        std::vector<IndexInfo> indexes;
        CHECK_DB(db->listIndexes(indexes), DbStatus::Ok);
        uint32_t primaryIndexId = 0;
        uint32_t codeIndexId = 0;
        for (size_t i = 0; i < indexes.size(); ++i) {
            if (indexes[i].tableName == "ChildRows") {
                CHECK(indexes[i].entryCount == 1);
                if (indexes[i].columnName == "Id") primaryIndexId = indexes[i].indexId;
                if (indexes[i].columnName == "Code") codeIndexId = indexes[i].indexId;
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
        }
        CHECK(runSql(engine, "COMMIT;"));
        CHECK(!engine.inTransaction());
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
            CHECK(countRows(engine, "ChildRows") == 1);
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

} // namespace

int main() {
    testHeapByteConstruction();
    testAllocationFailureAndRetry();
    testProductionMutationFailureAndRelocation();
    testInsertSavepointFailureAtomicity();
    std::printf("NDB1R4P11R2 heap page construction: %d checks, %d failures\n",
                gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
