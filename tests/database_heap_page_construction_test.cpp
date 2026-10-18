#include "database_format.h"
#include "database_endian.h"
#include "database_heap.h"
#include "database_index.h"
#include "database_relational.h"
#include "database_sql.h"
#include "database_wal.h"
#include "gx_allocator_hosted.h"
#include "gx_heap_page_builder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

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

    AllocationState()
        : attempts(0), successes(0), frees(0), outstanding(0),
          matchingSize(0), matchingSuccesses(0), matchingFailures(0),
          failAtMatchingSuccess(0), failureEnabled(false) {}
};

bool allocateBytes(void* context, size_t size, size_t alignment,
                   void** outPointer) {
    AllocationState* state = static_cast<AllocationState*>(context);
    ++state->attempts;
    *outPointer = 0;
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

} // namespace

int main() {
    testHeapByteConstruction();
    testAllocationFailureAndRetry();
    testProductionMutationFailureAndRelocation();
    std::printf("NDB1R4P11R2 heap page construction: %d checks, %d failures\n",
                gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
