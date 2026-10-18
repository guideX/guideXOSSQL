#include "database_buffer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "database_index.h"
#include "database_relational.h"
#include "database_sql.h"
#include "database_transaction.h"
#include "gx_test_allocator.h"

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

#define CHECK_STATUS(expression, expected)                                     \
    do {                                                                        \
        const DbResult p6Result = (expression);                                 \
        ++gChecks;                                                              \
        if (p6Result.status() != (expected)) {                                  \
            ++gFailures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s returned %s, expected %s\n",\
                         __FILE__, __LINE__, #expression,                      \
                         dbStatusName(p6Result.status()),                      \
                         dbStatusName(expected));                              \
        }                                                                       \
    } while (0)

struct MemoryFileState {
    std::vector<uint8_t> bytes;
    bool failNextRead;
    bool failNextWrite;
    bool failNextFlush;

    MemoryFileState() : bytes(), failNextRead(false), failNextWrite(false),
                        failNextFlush(false) {}
};

class FaultingMemoryFile : public IDatabaseFile {
public:
    explicit FaultingMemoryFile(const std::shared_ptr<MemoryFileState>& state)
        : _state(state), _open(false), _readOnly(false), _path() {}

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override {
        if (mode == FileOpenMode::Create && !_state->bytes.empty() &&
            !truncateExisting) {
            return DbResult::error(DbStatus::AlreadyExists, "memory file exists");
        }
        if (truncateExisting) _state->bytes.clear();
        _path = path;
        _readOnly = mode == FileOpenMode::ReadOnly;
        _open = true;
        return DbResult::ok();
    }

    DbResult close() override {
        _open = false;
        return DbResult::ok();
    }

    bool isOpen() const override { return _open; }
    bool isReadOnly() const override { return _readOnly; }

    DbResult getSize(uint64_t& outSize) override {
        if (!_open) return DbResult::error(DbStatus::NotOpen, "closed");
        outSize = _state->bytes.size();
        return DbResult::ok();
    }

    bool readAt(uint64_t offset, void* buffer, size_t length) override {
        if (_state->failNextRead) {
            _state->failNextRead = false;
            return false;
        }
        if (!_open || offset > _state->bytes.size() ||
            length > _state->bytes.size() - static_cast<size_t>(offset) ||
            (length != 0 && buffer == nullptr)) {
            return false;
        }
        if (length != 0) {
            const uint8_t* source =
                &_state->bytes[static_cast<size_t>(offset)];
            uint8_t* destination = static_cast<uint8_t*>(buffer);
            for (size_t i = 0; i < length; ++i) destination[i] = source[i];
        }
        return true;
    }

    bool writeAt(uint64_t offset, const void* buffer, size_t length) override {
        if (_state->failNextWrite) {
            _state->failNextWrite = false;
            return false;
        }
        if (!_open || _readOnly || offset > static_cast<uint64_t>(-1) - length ||
            offset + length > static_cast<uint64_t>(static_cast<size_t>(-1)) ||
            (length != 0 && buffer == nullptr)) {
            return false;
        }
        const size_t end = static_cast<size_t>(offset + length);
        if (end > _state->bytes.size()) _state->bytes.resize(end, 0);
        if (length != 0) {
            const uint8_t* source = static_cast<const uint8_t*>(buffer);
            uint8_t* destination = &_state->bytes[static_cast<size_t>(offset)];
            for (size_t i = 0; i < length; ++i) destination[i] = source[i];
        }
        return true;
    }

    bool truncateTo(uint64_t size) override {
        if (!_open || _readOnly ||
            size > static_cast<uint64_t>(static_cast<size_t>(-1))) {
            return false;
        }
        _state->bytes.resize(static_cast<size_t>(size));
        return true;
    }

    bool flush() override {
        if (_state->failNextFlush) {
            _state->failNextFlush = false;
            return false;
        }
        return _open;
    }

    const std::string& path() const override { return _path; }

private:
    std::shared_ptr<MemoryFileState> _state;
    bool _open;
    bool _readOnly;
    std::string _path;
};

std::unique_ptr<DatabaseFile> makeMemoryDatabase(
    const std::shared_ptr<MemoryFileState>& state) {
    std::unique_ptr<IDatabaseFile> raw(new FaultingMemoryFile(state));
    if (!raw->open("p6-memory.gxdb", FileOpenMode::Create, true).isOk()) {
        return std::unique_ptr<DatabaseFile>();
    }
    DatabaseCreateOptions options;
    options.pageSize = 512;
    options.creationTimeUnixNanos = 1;
    std::unique_ptr<DatabaseFile> file;
    if (!DatabaseFile::create(std::move(raw), options, file).isOk()) {
        return std::unique_ptr<DatabaseFile>();
    }
    return file;
}

DatabasePage makePage(uint64_t pageId, uint8_t seed, size_t payloadLength) {
    DatabasePage page;
    page.pageId = pageId;
    page.type = PageType::Data;
    page.flags = static_cast<uint16_t>(seed * 7u);
    page.generation = static_cast<uint32_t>(seed) + 3;
    page.payload.resize(payloadLength);
    page.payloadSize = static_cast<uint32_t>(payloadLength);
    for (size_t i = 0; i < payloadLength; ++i) {
        page.payload[i] = static_cast<uint8_t>(seed + i * 13u);
    }
    return page;
}

uint64_t appendPage(DatabaseFile& file, uint8_t seed, size_t payloadLength) {
    uint64_t pageId = 0;
    CHECK_STATUS(file.allocatePage(PageType::Data, pageId), DbStatus::Ok);
    DatabasePage page = makePage(pageId, seed, payloadLength);
    CHECK_STATUS(file.writePage(page), DbStatus::Ok);
    return pageId;
}

bool samePage(const DatabasePage& left, const DatabasePage& right) {
    return left.pageId == right.pageId && left.type == right.type &&
           left.flags == right.flags && left.generation == right.generation &&
           left.payload == right.payload;
}

std::string uniquePath(const char* prefix) {
    const uint64_t stamp = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    char name[128];
    std::snprintf(name, sizeof(name), "%s_%llu.gxdb", prefix,
                  static_cast<unsigned long long>(stamp));
    return name;
}

std::string walPath(const std::string& path) {
    return walPathFor(path);
}

void removeDatabaseFiles(const std::string& path) {
    std::remove(path.c_str());
    const std::string wal = walPath(path);
    std::remove(wal.c_str());
}

std::vector<uint8_t> readWholeFile(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
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

void testPortableEntryInitializationFailures() {
    std::printf("portable cache allocation and bounded initialization\n");
    std::shared_ptr<MemoryFileState> state(new MemoryFileState());
    std::unique_ptr<DatabaseFile> file = makeMemoryDatabase(state);
    CHECK(file != nullptr);
    if (!file) return;
    const uint64_t pageId = appendPage(*file, 0x22, 19);
    CHECK_STATUS(file->flush(), DbStatus::Ok);

    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);

    gxArenaFailAfter(&arena, 0);
    {
        BufferManager cache(*file, 2, &allocator);
        CHECK_STATUS(cache.init(), DbStatus::NoSpace);
    }
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);

    gxArenaFailAfter(&arena, 1);
    {
        BufferManager cache(*file, 2, &allocator);
        CHECK_STATUS(cache.init(), DbStatus::NoSpace);
    }
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);

    gxArenaDisableFailure(&arena);
    {
        BufferManager cache(*file, UINT32_MAX, &allocator);
        CHECK_STATUS(cache.init(), DbStatus::TransactionTooLarge);
    }
    CHECK(arena.outstanding == 0);

    GxPageCache portable = {};
    gxArenaFailAfter(&arena, 0);
    CHECK(gxPageCacheInit(&portable, &allocator, 2, 512) ==
          GxStatus::OutOfMemory);
    CHECK(portable.initialized == false);
    CHECK(arena.outstanding == 0);
    gxArenaDisableFailure(&arena);

    // An explicitly supplied but incomplete allocator is a configuration
    // error. It must not be replaced by the hosted allocator.
    GxAllocator incompleteAllocator = {};
    incompleteAllocator.maxAllocation = static_cast<size_t>(-1);
    incompleteAllocator.maxAlignment = 8;
    {
        BufferManager cache(*file, 2, &incompleteAllocator);
        CHECK_STATUS(cache.init(), DbStatus::InvalidArgument);
        CHECK(cache.residentCount() == 0);
    }

    // Resource limits from the selected production allocator reach the SQL
    // caller and leave no partially initialized cache allocation behind.
    GxAllocator limitedAllocator = allocator;
    limitedAllocator.maxAllocation = 1;
    {
        BufferManager cache(*file, 2, &limitedAllocator);
        CHECK_STATUS(cache.init(), DbStatus::TransactionTooLarge);
    }
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);

    CHECK_STATUS(file->close(), DbStatus::Ok);
    (void)pageId;
}

void testBufferManagerOwnershipEvictionAndIoFailures() {
    std::printf("production buffer manager ownership, eviction, and I/O faults\n");
    std::shared_ptr<MemoryFileState> state(new MemoryFileState());
    std::unique_ptr<DatabaseFile> file = makeMemoryDatabase(state);
    CHECK(file != nullptr);
    if (!file) return;
    const uint64_t pageA = appendPage(*file, 0x12, 33);
    const uint64_t pageB = appendPage(*file, 0x45, 51);
    const uint64_t pageC = appendPage(*file, 0x76, 27);
    const uint64_t pageD = appendPage(*file, 0x29, 45);
    CHECK_STATUS(file->flush(), DbStatus::Ok);

    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);
    {
        BufferManager cache(*file, 2, &allocator);
        CHECK_STATUS(cache.init(), DbStatus::Ok);
        CHECK(cache.capacity() == 2);
        const size_t allocationsAfterInit = arena.outstanding;
        CHECK_STATUS(cache.init(), DbStatus::InvalidArgument);
        CHECK(arena.outstanding == allocationsAfterInit);

        // Repeated allocation failure never publishes a partial miss.
        for (int attempt = 0; attempt < 2; ++attempt) {
            gxArenaFailAfter(&arena, 0);
            BufferSlot* failed = nullptr;
            CHECK_STATUS(cache.pinPage(pageA, failed), DbStatus::NoSpace);
            CHECK(failed == nullptr);
            CHECK(cache.residentCount() == 0);
        }
        gxArenaDisableFailure(&arena);

        BufferSlot* a = nullptr;
        CHECK_STATUS(cache.pinPage(pageA, a), DbStatus::Ok);
        CHECK(a != nullptr && samePage(a->page, makePage(pageA, 0x12, 33)));
        BufferSlot* firstAView = a;
        CHECK_STATUS(cache.unpin(*a), DbStatus::Ok);

        BufferSlot* b = nullptr;
        CHECK_STATUS(cache.pinPage(pageB, b), DbStatus::Ok);
        CHECK(b != nullptr && samePage(b->page, makePage(pageB, 0x45, 51)));
        CHECK_STATUS(cache.unpin(*b), DbStatus::Ok);

        BufferSlot* aHit = nullptr;
        CHECK_STATUS(cache.pinPage(pageA, aHit), DbStatus::Ok);
        CHECK(aHit == firstAView);
        CHECK(samePage(aHit->page, makePage(pageA, 0x12, 33)));
        CHECK_STATUS(cache.unpin(*aHit), DbStatus::Ok);

        // Failed replacement preserves both existing entries and their bytes.
        gxArenaFailAfter(&arena, 0);
        BufferSlot* failedReplacement = nullptr;
        CHECK_STATUS(cache.pinPage(pageC, failedReplacement), DbStatus::NoSpace);
        CHECK(failedReplacement == nullptr);
        CHECK(cache.residentCount() == 2);
        CHECK(cache.dirtyCount() == 0);
        gxArenaDisableFailure(&arena);
        BufferSlot* retainedA = nullptr;
        CHECK_STATUS(cache.pinPage(pageA, retainedA), DbStatus::Ok);
        CHECK(samePage(retainedA->page, makePage(pageA, 0x12, 33)));
        CHECK_STATUS(cache.unpin(*retainedA), DbStatus::Ok);

        BufferSlot* c = nullptr;
        CHECK_STATUS(cache.pinPage(pageC, c), DbStatus::Ok);
        CHECK(samePage(c->page, makePage(pageC, 0x76, 27)));
        CHECK_STATUS(cache.unpin(*c), DbStatus::Ok);
        BufferSlot* reloadedB = nullptr;
        CHECK_STATUS(cache.pinPage(pageB, reloadedB), DbStatus::Ok);
        CHECK(samePage(reloadedB->page, makePage(pageB, 0x45, 51)));
        CHECK_STATUS(cache.unpin(*reloadedB), DbStatus::Ok);

        // A failed read and failed dirty eviction leave the old image owned.
        const uint32_t residentBeforeReadFailure = cache.residentCount();
        state->failNextRead = true;
        BufferSlot* failedRead = nullptr;
        CHECK_STATUS(cache.pinPage(pageD, failedRead), DbStatus::IoError);
        CHECK(failedRead == nullptr);
        CHECK(cache.residentCount() == residentBeforeReadFailure);

        BufferSlot* dirtyA = nullptr;
        CHECK_STATUS(cache.pinPage(pageA, dirtyA), DbStatus::Ok);
        dirtyA->page.payload[0] = 0xE1;
        dirtyA->page.generation += 100;
        CHECK_STATUS(cache.markDirty(*dirtyA), DbStatus::Ok);
        CHECK_STATUS(cache.unpin(*dirtyA), DbStatus::Ok);
        BufferSlot* makeCRecent = nullptr;
        CHECK_STATUS(cache.pinPage(pageC, makeCRecent), DbStatus::Ok);
        CHECK_STATUS(cache.unpin(*makeCRecent), DbStatus::Ok);

        state->failNextWrite = true;
        BufferSlot* failedEviction = nullptr;
        CHECK_STATUS(cache.pinPage(pageD, failedEviction), DbStatus::IoError);
        CHECK(failedEviction == nullptr);
        CHECK(cache.residentCount() == 2);
        CHECK(cache.dirtyCount() == 1);

        // Retry writes the dirty victim and reload returns the exact modified bytes.
        BufferSlot* d = nullptr;
        CHECK_STATUS(cache.pinPage(pageD, d), DbStatus::Ok);
        CHECK_STATUS(cache.unpin(*d), DbStatus::Ok);
        BufferSlot* reloadedA = nullptr;
        CHECK_STATUS(cache.pinPage(pageA, reloadedA), DbStatus::Ok);
        CHECK(reloadedA->page.payload[0] == 0xE1);
        CHECK(reloadedA->page.generation ==
              static_cast<uint32_t>(0x12) + 3 + 100);
        CHECK_STATUS(cache.unpin(*reloadedA), DbStatus::Ok);

        // Page write failure retains dirty state. A later retry clears it only
        // after writePageImage succeeds.
        BufferSlot* dirtyD = nullptr;
        CHECK_STATUS(cache.pinPage(pageD, dirtyD), DbStatus::Ok);
        dirtyD->page.payload[1] ^= 0x5A;
        CHECK_STATUS(cache.markDirty(*dirtyD), DbStatus::Ok);
        CHECK_STATUS(cache.unpin(*dirtyD), DbStatus::Ok);
        state->failNextWrite = true;
        CHECK_STATUS(cache.flush(), DbStatus::IoError);
        CHECK(cache.dirtyCount() == 1);
        CHECK_STATUS(cache.flush(), DbStatus::Ok);
        CHECK(cache.dirtyCount() == 0);

        // The established barrier policy clears page dirty state after writes;
        // retrying a failed file flush still retries the barrier.
        BufferSlot* dirtyAgain = nullptr;
        CHECK_STATUS(cache.pinPage(pageD, dirtyAgain), DbStatus::Ok);
        dirtyAgain->page.payload[2] ^= 0x33;
        CHECK_STATUS(cache.markDirty(*dirtyAgain), DbStatus::Ok);
        CHECK_STATUS(cache.unpin(*dirtyAgain), DbStatus::Ok);
        state->failNextFlush = true;
        CHECK_STATUS(cache.flush(), DbStatus::IoError);
        CHECK(cache.dirtyCount() == 0);
        CHECK_STATUS(cache.flush(), DbStatus::Ok);

        const uint64_t comparisonsBefore = cache.cacheStats().lookupComparisons;
        const uint64_t imageAllocationsBefore =
            cache.cacheStats().imageAllocations;
        const auto hitStart = std::chrono::steady_clock::now();
        bool repeatedHitsSucceeded = true;
        for (uint32_t i = 0; i < 10000; ++i) {
            BufferSlot* repeated = nullptr;
            if (!cache.pinPage(pageD, repeated).isOk() || repeated == nullptr ||
                !cache.unpin(*repeated).isOk()) {
                repeatedHitsSucceeded = false;
                break;
            }
        }
        const auto hitEnd = std::chrono::steady_clock::now();
        const uint64_t repeatedHitMicros = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                hitEnd - hitStart).count());
        const uint64_t comparisonsForRepeatedHits =
            cache.cacheStats().lookupComparisons - comparisonsBefore;
        CHECK(repeatedHitsSucceeded);
        CHECK(cache.cacheStats().imageAllocations == imageAllocationsBefore);
        std::printf("P6 repeated page hits: ops=10000 elapsed-us=%llu "
                    "lookup-comparisons=%llu peak-bytes=%llu\n",
                    static_cast<unsigned long long>(repeatedHitMicros),
                    static_cast<unsigned long long>(comparisonsForRepeatedHits),
                    static_cast<unsigned long long>(cache.cacheMemoryPeakBytes()));

        const GxPageCacheStats& stats = cache.cacheStats();
        CHECK(stats.hits > 0);
        CHECK(stats.misses >= 5);
        CHECK(stats.evictions >= 2);
        CHECK(stats.imageAllocations >= 5);
        CHECK(stats.lookupComparisons >= stats.hits + stats.misses);
        const size_t fixedBytes =
            2 * (sizeof(GxPageCacheEntry) + sizeof(BufferSlot));
        CHECK(cache.cacheMemoryBytes() <= fixedBytes + 2 * 512);
        CHECK(cache.cacheMemoryPeakBytes() <= fixedBytes + 3 * 512);
        CHECK(cache.pinnedCount() == 0);
    }
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK_STATUS(file->close(), DbStatus::Ok);
}

void testDatabaseScopedPageIdentity() {
    std::printf("database-scoped page identity\n");
    std::shared_ptr<MemoryFileState> stateA(new MemoryFileState());
    std::shared_ptr<MemoryFileState> stateB(new MemoryFileState());
    std::unique_ptr<DatabaseFile> fileA = makeMemoryDatabase(stateA);
    std::unique_ptr<DatabaseFile> fileB = makeMemoryDatabase(stateB);
    CHECK(fileA != nullptr && fileB != nullptr);
    if (!fileA || !fileB) return;
    const uint64_t pageA = appendPage(*fileA, 0x10, 25);
    const uint64_t pageB = appendPage(*fileB, 0x90, 25);
    CHECK(pageA == pageB);

    GxArena arenaA;
    GxArena arenaB;
    GxAllocator allocatorA = {};
    GxAllocator allocatorB = {};
    gxArenaInit(&arenaA, sizeof(arenaA.bytes));
    gxArenaInit(&arenaB, sizeof(arenaB.bytes));
    gxArenaMakeAllocator(&arenaA, &allocatorA);
    gxArenaMakeAllocator(&arenaB, &allocatorB);
    {
        BufferManager cacheA(*fileA, 1, &allocatorA);
        BufferManager cacheB(*fileB, 1, &allocatorB);
        CHECK_STATUS(cacheA.init(), DbStatus::Ok);
        CHECK_STATUS(cacheB.init(), DbStatus::Ok);
        BufferSlot* a = nullptr;
        BufferSlot* b = nullptr;
        CHECK_STATUS(cacheA.pinPage(pageA, a), DbStatus::Ok);
        CHECK_STATUS(cacheB.pinPage(pageB, b), DbStatus::Ok);
        CHECK(a->page.pageId == b->page.pageId);
        CHECK(a->page.payload != b->page.payload);
        CHECK(a->page.payload[0] != b->page.payload[0]);
        CHECK_STATUS(cacheA.unpin(*a), DbStatus::Ok);
        CHECK_STATUS(cacheB.unpin(*b), DbStatus::Ok);
    }
    CHECK(arenaA.outstanding == 0 && arenaB.outstanding == 0);
    CHECK_STATUS(fileA->close(), DbStatus::Ok);
    CHECK_STATUS(fileB->close(), DbStatus::Ok);
}

uint32_t primaryKeyIndex(Database& db) {
    std::vector<IndexInfo> indexes;
    if (!db.listIndexes(indexes).isOk()) return 0;
    for (size_t i = 0; i < indexes.size(); ++i) {
        if (indexes[i].tableName == "CacheRows" && indexes[i].primaryKey()) {
            return indexes[i].indexId;
        }
    }
    return 0;
}

std::string originalPayload(uint32_t rowId) {
    const size_t size = 80 + (rowId * 37u) % 150;
    return std::string(size, static_cast<char>('a' + rowId % 26));
}

void testProductionSqlWorkloadAndIsolation() {
    std::printf("production SQL workload, transaction isolation, and reopen\n");
    const std::string path = uniquePath("p6_sql");
    removeDatabaseFiles(path);

    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);

    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    createOptions.bufferCapacity = 2;
    createOptions.pageCacheAllocator = &allocator;
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) {
        removeDatabaseFiles(path);
        return;
    }

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE CacheRows (Id INT64 PRIMARY KEY, "
                     "GroupKey TEXT NOT NULL, Payload TEXT NOT NULL);").ok);
        CHECK(runSql(engine,
                     "CREATE INDEX IX_CacheRows_GroupKey "
                     "ON CacheRows (GroupKey);").ok);
        CHECK(runSql(engine, "BEGIN;").ok);
        for (uint32_t i = 0; i < 36; ++i) {
            const std::string sql =
                "INSERT INTO CacheRows VALUES (" + std::to_string(i) + ", 'g" +
                std::to_string(i % 4) + "', '" + originalPayload(i) + "');";
            CHECK(runSql(engine, sql).ok);
        }
        CHECK(runSql(engine, "COMMIT;").ok);

        std::vector<TableInfo> tables;
        CHECK_STATUS(db->listTables(tables), DbStatus::Ok);
        bool multipleHeapPages = false;
        for (size_t i = 0; i < tables.size(); ++i) {
            if (tables[i].name == "CacheRows" && tables[i].heapPageCount >= 2) {
                multipleHeapPages = true;
            }
        }
        CHECK(multipleHeapPages);

        const uint32_t pkIndex = primaryKeyIndex(*db);
        CHECK(pkIndex != 0);
        std::vector<uint8_t> key;
        CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(5), key));
        std::vector<RowLocator> locators;
        CHECK_STATUS(db->indexLookup(pkIndex, key, locators), DbStatus::Ok);
        CHECK(locators.size() == 1);
        if (locators.size() == 1) {
            DatabasePage committedBefore;
            CHECK_STATUS(db->readPage(locators[0].pageId, committedBefore),
                         DbStatus::Ok);
            const uint32_t dirtyBefore = db->buffer().dirtyCount();
            CHECK(runSql(engine, "BEGIN;").ok);
            CHECK(runSql(engine,
                         "UPDATE CacheRows SET Payload='uncommitted' WHERE Id=5;").ok);
            DatabasePage overlayPage;
            CHECK_STATUS(db->activeTransaction()->readPage(locators[0].pageId,
                                                           overlayPage),
                         DbStatus::Ok);
            DatabasePage committedDuring;
            CHECK_STATUS(db->readPage(locators[0].pageId, committedDuring),
                         DbStatus::Ok);
            CHECK(!samePage(overlayPage, committedBefore));
            CHECK(samePage(committedDuring, committedBefore));
            CHECK(db->buffer().dirtyCount() == dirtyBefore);
            CHECK(runSql(engine, "ROLLBACK;").ok);
            DatabasePage committedAfterRollback;
            CHECK_STATUS(db->readPage(locators[0].pageId,
                                      committedAfterRollback), DbStatus::Ok);
            CHECK(samePage(committedAfterRollback, committedBefore));
        }

        CHECK(runSql(engine, "BEGIN;").ok);
        CHECK(runSql(engine,
                     "UPDATE CacheRows SET Payload='durable-update' WHERE Id=5;").ok);
        CHECK(runSql(engine, "DELETE FROM CacheRows WHERE Id=6;").ok);
        CHECK(runSql(engine, "COMMIT;").ok);

        CHECK(runSql(engine, "BEGIN;").ok);
        CHECK(runSql(engine,
                     "UPDATE CacheRows SET Payload='rolled-back' WHERE Id=7;").ok);
        CHECK(runSql(engine, "DELETE FROM CacheRows WHERE Id=8;").ok);
        CHECK(runSql(engine, "ROLLBACK;").ok);

        for (int repeat = 0; repeat < 4; ++repeat) {
            SqlExecutionResult bySecondary = runSql(
                engine,
                "SELECT Id, Payload FROM CacheRows WHERE GroupKey='g3' ORDER BY Id;");
            CHECK(bySecondary.ok && bySecondary.statements.size() == 1);
            if (bySecondary.ok && bySecondary.statements.size() == 1) {
                const SqlStatementResult& statement = bySecondary.statements[0];
                CHECK(statement.accessPath == "IndexLookup");
                CHECK(statement.resultSet.rowCount() == 9);
                for (size_t row = 0; row < statement.resultSet.rowCount(); ++row) {
                    const uint32_t id = static_cast<uint32_t>(
                        statement.resultSet.value(row, 0).int64Value());
                    CHECK(id % 4 == 3);
                    CHECK(statement.resultSet.value(row, 1).textValue() ==
                          originalPayload(id));
                }
            }
            SqlExecutionResult byPrimary = runSql(
                engine, "SELECT Payload FROM CacheRows WHERE Id=5;");
            CHECK(byPrimary.ok && byPrimary.statements.size() == 1);
            if (byPrimary.ok && byPrimary.statements.size() == 1) {
                CHECK(byPrimary.statements[0].accessPath == "IndexLookup");
                CHECK(byPrimary.statements[0].resultSet.rowCount() == 1);
                CHECK(byPrimary.statements[0].resultSet.value(0, 0).textValue() ==
                      "durable-update");
            }
        }

        std::string integrity;
        CHECK_STATUS(db->validateIntegrity(integrity), DbStatus::Ok);
        std::vector<IndexInfo> indexes;
        CHECK_STATUS(db->listIndexes(indexes), DbStatus::Ok);
        CHECK(indexes.size() >= 2);
        for (size_t i = 0; i < indexes.size(); ++i) {
            IndexValidation validation;
            CHECK_STATUS(db->validateIndex(indexes[i].indexId, validation),
                         DbStatus::Ok);
            CHECK(validation.ok);
        }
    }

    const GxPageCacheStats stats = db->buffer().cacheStats();
    CHECK(stats.hits > 0);
    CHECK(stats.misses > 0);
    CHECK(stats.evictions > 0);
    CHECK(stats.imageAllocations > 0);
    CHECK(db->buffer().pinnedCount() == 0);
    CHECK(db->buffer().cacheMemoryPeakBytes() <=
          2 * (sizeof(GxPageCacheEntry) + sizeof(BufferSlot)) + 3 * 512);
    std::printf("P6 cache diagnostics: hits=%llu misses=%llu evictions=%llu "
                "image-allocations=%llu peak-bytes=%llu\n",
                static_cast<unsigned long long>(stats.hits),
                static_cast<unsigned long long>(stats.misses),
                static_cast<unsigned long long>(stats.evictions),
                static_cast<unsigned long long>(stats.imageAllocations),
                static_cast<unsigned long long>(
                    db->buffer().cacheMemoryPeakBytes()));

    CHECK_STATUS(db->close(), DbStatus::Ok);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    db.reset();

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened),
                 DbStatus::Ok);
    CHECK(reopened != nullptr);
    if (reopened) {
        SqlEngine engine(*reopened);
        SqlExecutionResult allRows = runSql(
            engine, "SELECT Id, Payload FROM CacheRows ORDER BY Id;");
        CHECK(allRows.ok && allRows.statements.size() == 1);
        if (allRows.ok && allRows.statements.size() == 1) {
            const SqlResultSet& rows = allRows.statements[0].resultSet;
            CHECK(rows.rowCount() == 35);
            uint32_t rowIndex = 0;
            for (uint32_t id = 0; id < 36; ++id) {
                if (id == 6) continue;
                CHECK(static_cast<uint32_t>(rows.value(rowIndex, 0).int64Value()) == id);
                const std::string expected =
                    id == 5 ? "durable-update" : originalPayload(id);
                CHECK(rows.value(rowIndex, 1).textValue() == expected);
                ++rowIndex;
            }
        }
        std::string integrity;
        CHECK_STATUS(reopened->validateIntegrity(integrity), DbStatus::Ok);
        CHECK_STATUS(reopened->close(), DbStatus::Ok);
    }
    removeDatabaseFiles(path);
}

void testDatabaseOpenAllocationFailuresPreserveFiles() {
    std::printf("cache allocation failure during production open\n");
    const std::string path = uniquePath("p6_open_fail");
    removeDatabaseFiles(path);
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;
    CHECK_STATUS(db->close(), DbStatus::Ok);
    db.reset();
    const std::vector<uint8_t> originalDatabase = readWholeFile(path);
    const std::vector<uint8_t> originalWal = readWholeFile(walPath(path));

    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);
    DatabaseOpenOptions options;
    options.pageCacheAllocator = &allocator;

    gxArenaFailAfter(&arena, 0);
    std::unique_ptr<Database> failedOpen;
    CHECK_STATUS(Database::open(path, options, failedOpen), DbStatus::NoSpace);
    CHECK(failedOpen == nullptr);
    CHECK(arena.outstanding == 0);
    CHECK(readWholeFile(path) == originalDatabase);
    CHECK(readWholeFile(walPath(path)) == originalWal);

    gxArenaFailAfter(&arena, 2);
    CHECK_STATUS(Database::open(path, options, failedOpen), DbStatus::NoSpace);
    CHECK(failedOpen == nullptr);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(readWholeFile(path) == originalDatabase);
    CHECK(readWholeFile(walPath(path)) == originalWal);

    gxArenaDisableFailure(&arena);
    removeDatabaseFiles(path);
}

void testDurableWalAfterCacheAllocationFailure() {
    std::printf("durable WAL recovery after cache publication allocation failure\n");
    const std::string path = uniquePath("p6_recovery");
    removeDatabaseFiles(path);

    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    createOptions.bufferCapacity = 1;
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, createOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE CacheRecovery (Id INT64 PRIMARY KEY, "
                     "Payload TEXT NOT NULL);").ok);
        CHECK(runSql(engine, "BEGIN;").ok);
        for (uint32_t i = 0; i < 32; ++i) {
            const std::string sql =
                "INSERT INTO CacheRecovery VALUES (" + std::to_string(i) +
                ", '" + originalPayload(i) + "');";
            CHECK(runSql(engine, sql).ok);
        }
        CHECK(runSql(engine, "COMMIT;").ok);
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    db.reset();

    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);
    DatabaseOpenOptions openOptions;
    openOptions.bufferCapacity = 1;
    openOptions.pageCacheAllocator = &allocator;
    CHECK_STATUS(Database::open(path, openOptions, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) {
        removeDatabaseFiles(path);
        return;
    }

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine, "BEGIN;").ok);
        CHECK(runSql(engine,
                     "UPDATE CacheRecovery SET Payload='cache-committed' "
                     "WHERE Id >= 0;").ok);
        // Catalog serialization at commit first reads its committed root via
        // the cache. Let that single miss succeed, then fail the subsequent
        // committed-page publication after the WAL durability point.
        gxArenaFailAfter(&arena, 1);
        SqlExecutionResult commit = runSql(engine, "COMMIT;", false);
        CHECK(!commit.ok);
        CHECK(db->diagnostics().recoveryRequired);
        DatabasePage blocked;
        CHECK_STATUS(db->readPage(1, blocked), DbStatus::RecoveryRequired);
        CHECK_STATUS(db->flush(), DbStatus::RecoveryRequired);
    }

    CHECK_STATUS(db->close(), DbStatus::RecoveryRequired);
    CHECK(!db->isOpen());
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(readWholeFile(walPath(path)).size() > 32);
    gxArenaDisableFailure(&arena);
    db.reset();

    std::unique_ptr<Database> recovered;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), recovered),
                 DbStatus::Ok);
    CHECK(recovered != nullptr);
    if (recovered) {
        SqlEngine engine(*recovered);
        SqlExecutionResult rows = runSql(
            engine,
            "SELECT Id, Payload FROM CacheRecovery ORDER BY Id;");
        CHECK(rows.ok && rows.statements.size() == 1);
        if (rows.ok && rows.statements.size() == 1) {
            const SqlResultSet& result = rows.statements[0].resultSet;
            CHECK(result.rowCount() == 32);
            for (size_t i = 0; i < result.rowCount(); ++i) {
                CHECK(result.value(i, 0).int64Value() ==
                      static_cast<int64_t>(i));
                CHECK(result.value(i, 1).textValue() == "cache-committed");
            }
        }
        std::string integrity;
        CHECK_STATUS(recovered->validateIntegrity(integrity), DbStatus::Ok);
        CHECK_STATUS(recovered->close(), DbStatus::Ok);
    }
    removeDatabaseFiles(path);
}

} // namespace

int main() {
    testPortableEntryInitializationFailures();
    testBufferManagerOwnershipEvictionAndIoFailures();
    testDatabaseScopedPageIdentity();
    testDatabaseOpenAllocationFailuresPreserveFiles();
    testProductionSqlWorkloadAndIsolation();
    testDurableWalAfterCacheAllocationFailure();
    std::printf("NDB1R4P6: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
