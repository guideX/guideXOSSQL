#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

#include "database_catalog.h"
#include "database_endian.h"
#include "database_heap.h"
#include "database_index.h"
#include "database_relational.h"
#include "database_schema.h"
#include "database_transaction.h"

namespace gxos {
namespace db {

class DatabaseCoordinatorTestAccess {
public:
    static bool tryAcquire(Database& database) {
        if (!database._operationMutex.try_lock()) return false;
        database._operationMutex.unlock();
        return true;
    }
};

} // namespace db
} // namespace gxos

using namespace gxos::db;

namespace {

int g_pass = 0;
int g_fail = 0;
int g_caseCounter = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::string testDirectory() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    const std::string path = std::string(base) + "/gxos_gxdb_r3ae_tests";
#if defined(_WIN32)
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0700);
#endif
    return path;
}

std::string uniquePath() {
    char suffix[48];
    std::snprintf(suffix, sizeof(suffix), "_%d.gxdb", g_caseCounter++);
    return testDirectory() + "/r3ae_read_view" + suffix;
}

std::string walPath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    return dot == std::string::npos ? path + ".gxwal"
                                     : path.substr(0, dot) + ".gxwal";
}

void removeDatabaseFiles(const std::string& path) {
    std::remove(path.c_str());
    const std::string wal = walPath(path);
    std::remove(wal.c_str());
}

std::vector<uint8_t> int64Key(int64_t value) {
    std::vector<uint8_t> key;
    encodeIndexKey(DbType::Int64, DbValue::int64(value), key);
    return key;
}

std::vector<DbValue> makeRow(int64_t id, int64_t key) {
    std::vector<DbValue> row;
    row.push_back(DbValue::int64(id));
    row.push_back(DbValue::int64(key));
    return row;
}

bool locatorsOrdered(const std::vector<RowLocator>& rows) {
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i - 1].pageId > rows[i].pageId) return false;
        if (rows[i - 1].pageId == rows[i].pageId &&
            rows[i - 1].slot >= rows[i].slot) return false;
    }
    return true;
}

bool snapshotPages(Database& database, std::vector<DatabasePage>& out) {
    out.clear();
    const uint64_t pageCount = database.pageCount();
    if (pageCount == 0) return false;
    out.reserve(static_cast<size_t>(pageCount - 1));
    for (uint64_t pageId = 1; pageId < pageCount; ++pageId) {
        DatabasePage page;
        if (!database.readPage(pageId, page).isOk()) return false;
        out.push_back(page);
    }
    return true;
}

bool samePages(const std::vector<DatabasePage>& left,
               const std::vector<DatabasePage>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].pageId != right[i].pageId ||
            left[i].type != right[i].type ||
            left[i].flags != right[i].flags ||
            left[i].generation != right[i].generation ||
            left[i].payloadSize != right[i].payloadSize ||
            left[i].payload != right[i].payload) return false;
    }
    return true;
}

void testProductionReadView() {
    std::printf("R3AE production operation read view and publication coordination\n");
    const std::string path = uniquePath();
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    std::unique_ptr<Database> database;
    DbResult result = Database::create(path, createOptions, database);
    CHECK(result.isOk() && database != nullptr);
    if (!database) return;

    TableDefinition tableDefinition("Rows");
    tableDefinition.columns.push_back(
        ColumnDefinition("Id", DbType::Int64, false));
    tableDefinition.columns.push_back(
        ColumnDefinition("K", DbType::Int64, false));
    uint32_t tableId = 0;
    CHECK(database->createTable(tableDefinition, tableId).isOk());

    IndexDefinition indexDefinition("IX_Rows_K", tableId, 1,
                                    IndexKind::Ordinary);
    uint32_t indexId = 0;
    CHECK(database->createIndex(indexDefinition, indexId).isOk());
    CHECK(indexId != 0);

    std::vector<RowLocator> locators(1, RowLocator(999, 999));
    CHECK(database->indexLookup(indexId, int64Key(7), locators).isOk());
    CHECK(locators.empty());
    IndexRangeBound unbounded;
    CHECK(database->indexRange(indexId, unbounded, locators).isOk());
    CHECK(locators.empty());

    std::unique_ptr<Table> table;
    CHECK(database->openTable("Rows", table).isOk() && table != nullptr);
    if (!table) return;
    CHECK(table->insert(makeRow(0, 7)).isOk());

    IndexValidation leafValidation;
    CHECK(database->validateIndex(indexId, leafValidation).isOk());
    CHECK(leafValidation.ok && leafValidation.height == 1 &&
          leafValidation.entryCount == 1);
    IndexInfo before;
    CHECK(database->describeIndex("IX_Rows_K", before).isOk());
    CHECK(before.rootPageId != 0);

    // Transaction-local overlay pages and the mutable catalog root are read
    // under the same coordinator and retain read-your-writes behavior.
    std::unique_ptr<Transaction> overlayTransaction;
    CHECK(database->beginTransaction(overlayTransaction).isOk());
    std::unique_ptr<Table> overlayTable;
    CHECK(overlayTransaction->openTable("Rows", overlayTable).isOk());
    CHECK(overlayTable->insert(makeRow(900, 9)).isOk());
    CHECK(database->indexLookup(indexId, int64Key(9), locators).isOk());
    CHECK(locators.size() == 1);
    CHECK(overlayTransaction->rollback().isOk());
    CHECK(database->indexLookup(indexId, int64Key(9), locators).isOk());
    CHECK(locators.empty());
    CHECK(database->indexLookup(indexId, int64Key(7), locators).isOk());
    CHECK(locators.size() == 1);
    result = database->indexLookup(UINT32_MAX, int64Key(7), locators);
    CHECK(!result.isOk() && locators.empty());

    // Hold the actual production read-view coordinator while a second thread
    // attempts a conflicting transaction that grows/splits the index.
    DatabaseReadView readView;
    CHECK(database->acquireReadView(readView).isOk());
    CHECK(readView.isActive() && readView.pageSize() == 512);
    const uint64_t capturedPageCount = readView.pageCount();
    uint64_t capturedRootPageId = 0;
    DbType capturedKeyType = DbType::Unknown;
    CHECK(readView.indexRoot(indexId, capturedRootPageId, capturedKeyType).isOk());
    CHECK(capturedRootPageId == before.rootPageId &&
          capturedKeyType == DbType::Int64);
    DatabasePage capturedRootPage;
    CHECK(readView.readPage(capturedRootPageId, capturedRootPage).isOk());
    CHECK(capturedRootPage.pageId == capturedRootPageId);
    IndexInfo capturedIndex;
    CHECK(database->describeIndex("IX_Rows_K", capturedIndex).isOk());
    CHECK(capturedIndex.rootPageId == before.rootPageId);
    std::unique_ptr<Transaction> reentrantWrite;
    result = database->beginTransaction(reentrantWrite);
    CHECK(!result.isOk() && result.status() == DbStatus::Internal &&
          reentrantWrite == nullptr);
    bool coordinatorAvailable = true;
    std::thread tryLockProbe([&]() {
        coordinatorAvailable =
            DatabaseCoordinatorTestAccess::tryAcquire(*database);
    });
    tryLockProbe.join();
    CHECK(!coordinatorAvailable);
    std::vector<DatabasePage> pagesBeforeReads;
    CHECK(snapshotPages(*database, pagesBeforeReads));

    bool writerReady = false;
    bool startWriter = false;
    DbResult writerResult = DbResult::ok();
    std::mutex barrierMutex;
    std::condition_variable barrierChanged;
    std::thread writer([&]() {
        {
            std::lock_guard<std::mutex> lock(barrierMutex);
            writerReady = true;
        }
        barrierChanged.notify_all();
        {
            std::unique_lock<std::mutex> lock(barrierMutex);
            barrierChanged.wait(lock, [&]() { return startWriter; });
        }

        std::unique_ptr<Transaction> transaction;
        writerResult = database->beginTransaction(transaction);
        if (!writerResult.isOk()) return;
        std::unique_ptr<Table> writerTable;
        writerResult = transaction->openTable("Rows", writerTable);
        if (!writerResult.isOk()) {
            transaction->rollback();
            return;
        }
        for (int64_t id = 1; id <= 80; ++id) {
            writerResult = writerTable->insert(makeRow(id, 7));
            if (!writerResult.isOk()) {
                transaction->rollback();
                return;
            }
        }
        writerResult = transaction->commit();
    });

    {
        std::unique_lock<std::mutex> lock(barrierMutex);
        barrierChanged.wait(lock, [&]() { return writerReady; });
        startWriter = true;
    }
    barrierChanged.notify_all();

    // Repeated point and range operations under this held view observe the
    // same root, page extent and duplicate set while the writer is pending.
    CHECK(readView.pageCount() == capturedPageCount);
    for (int repeat = 0; repeat < 3; ++repeat) {
        CHECK(database->indexLookup(indexId, int64Key(7), locators).isOk());
        CHECK(locators.size() == 1);
        IndexRangeBound duplicates;
        duplicates.hasLower = true;
        duplicates.lowerInclusive = true;
        duplicates.lower = DbValue::int64(7);
        duplicates.hasUpper = true;
        duplicates.upperInclusive = true;
        duplicates.upper = DbValue::int64(7);
        CHECK(database->indexRange(indexId, duplicates, locators).isOk());
        CHECK(locators.size() == 1 && locatorsOrdered(locators));
        CHECK(database->buffer().pinnedCount() == 0);
        IndexInfo stillA;
        CHECK(database->describeIndex("IX_Rows_K", stillA).isOk());
        CHECK(stillA.rootPageId == capturedIndex.rootPageId);
        CHECK(database->pageCount() == capturedPageCount);
    }
    std::vector<DatabasePage> pagesAfterReads;
    CHECK(snapshotPages(*database, pagesAfterReads));
    CHECK(samePages(pagesBeforeReads, pagesAfterReads));

    readView.release();
    CHECK(!readView.isActive());
    CHECK(readView.readPage(capturedRootPageId, capturedRootPage).status() ==
          DbStatus::NoActiveTransaction);
    writer.join();
    CHECK(writerResult.isOk());

    IndexInfo after;
    CHECK(database->describeIndex("IX_Rows_K", after).isOk());
    CHECK(after.rootPageId != capturedIndex.rootPageId);
    CHECK(database->pageCount() > capturedPageCount);
    IndexValidation multilevelValidation;
    CHECK(database->validateIndex(indexId, multilevelValidation).isOk());
    CHECK(multilevelValidation.ok && multilevelValidation.height >= 2);
    CHECK(database->indexLookup(indexId, int64Key(7), locators).isOk());
    CHECK(locators.size() == 81 && locatorsOrdered(locators));
    IndexRangeBound duplicates;
    duplicates.hasLower = true;
    duplicates.lowerInclusive = true;
    duplicates.lower = DbValue::int64(7);
    duplicates.hasUpper = true;
    duplicates.upperInclusive = true;
    duplicates.upper = DbValue::int64(7);
    CHECK(database->indexRange(indexId, duplicates, locators).isOk());
    CHECK(locators.size() == 81 && locatorsOrdered(locators));
    CHECK(database->indexRange(indexId, unbounded, locators).isOk());
    CHECK(locators.size() == 81 && locatorsOrdered(locators));
    IndexRangeBound lowerOnly;
    lowerOnly.hasLower = true;
    lowerOnly.lowerInclusive = true;
    lowerOnly.lower = DbValue::int64(7);
    CHECK(database->indexRange(indexId, lowerOnly, locators).isOk());
    CHECK(locators.size() == 81);
    IndexRangeBound upperOnly;
    upperOnly.hasUpper = true;
    upperOnly.upperInclusive = true;
    upperOnly.upper = DbValue::int64(7);
    CHECK(database->indexRange(indexId, upperOnly, locators).isOk());
    CHECK(locators.size() == 81);

    // Invalid root and late successor failures leave no published partial
    // output and release the production read-view lock on every error path.
    database->catalog().setIndexRoot(indexId, database->pageCount());
    locators.assign(1, RowLocator(999, 999));
    result = database->indexRange(indexId, unbounded, locators);
    CHECK(!result.isOk() && locators.empty());
    database->catalog().setIndexRoot(indexId, after.rootPageId);
    DatabaseReadView afterInvalidRoot;
    CHECK(database->acquireReadView(afterInvalidRoot).isOk());
    afterInvalidRoot.release();

    DatabasePage savedRoot;
    CHECK(database->readPage(after.rootPageId, savedRoot).isOk());
    DatabasePage corruptRoot = savedRoot;
    corruptRoot.payload[0] = 0;
    CHECK(database->writePage(corruptRoot).isOk());
    locators.assign(1, RowLocator(999, 999));
    result = database->indexLookup(indexId, int64Key(7), locators);
    CHECK(!result.isOk() && locators.empty());
    CHECK(database->writePage(savedRoot).isOk());
    DatabaseReadView afterInternalFailure;
    CHECK(database->acquireReadView(afterInternalFailure).isOk());
    afterInternalFailure.release();

    DatabasePage firstLeaf;
    CHECK(database->readPage(after.rootPageId, firstLeaf).isOk());
    uint64_t pageId = after.rootPageId;
    while (firstLeaf.type == PageType::IndexInternal) {
        const uint64_t childPageId = loadLe64(firstLeaf.payload.data() + 24);
        pageId = childPageId;
        CHECK(database->readPage(pageId, firstLeaf).isOk());
    }
    CHECK(firstLeaf.type == PageType::IndexLeaf);
    const DatabasePage savedFirstLeaf = firstLeaf;
    storeLe64(firstLeaf.payload.data() + 8, database->pageCount());
    CHECK(database->writePage(firstLeaf).isOk());
    locators.assign(1, RowLocator(999, 999));
    result = database->indexRange(indexId, duplicates, locators);
    CHECK(!result.isOk() && locators.empty());
    CHECK(database->writePage(savedFirstLeaf).isOk());
    DatabaseReadView afterTraversalFailure;
    CHECK(database->acquireReadView(afterTraversalFailure).isOk());
    afterTraversalFailure.release();

    CHECK(database->close().isOk());
    database.reset();
    removeDatabaseFiles(path);
}

} // namespace

int main() {
    testProductionReadView();
    std::printf("NDB1R4P11R3AE SQL publication read view: %d passed, %d failed\n",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
