#include "database_page.h"
#include "database_index.h"
#include "database_relational.h"
#include "database_sql.h"
#include "database_transaction.h"
#include "gx_array.h"
#include "gx_page_codec.h"
#include "gx_transaction_pages.h"
#include "gx_wal_page_image.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
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
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #condition);                                           \
        }                                                                       \
    } while (0)

#define CHECK_STATUS(expression, expected)                                     \
    do {                                                                        \
        const DbResult p7Result = (expression);                                 \
        ++gChecks;                                                              \
        if (p7Result.status() != (expected)) {                                  \
            ++gFailures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s returned %s, expected %s\n",\
                         __FILE__, __LINE__, #expression,                      \
                         dbStatusName(p7Result.status()),                      \
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
    *outPointer = nullptr;
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

bool pagePayloadIs(const uint8_t* image, uint32_t pageSize,
                   uint64_t pageId, const std::vector<uint8_t>& expected) {
    GxDatabasePageInfo info = {};
    if (gxDatabasePageInspect(image, pageSize, pageSize, pageId, &info) !=
            GxStatus::Ok ||
        info.payloadSize != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (image[kPageHeaderSize + i] != expected[i]) return false;
    }
    return true;
}

void testPortableTransactionPageOwner() {
    std::printf("portable transaction page owner, clone, move, and failures\n");
    FaultAllocatorState state;
    GxAllocator allocator = makeFaultAllocator(&state);
    GxTransactionPages pages = {};
    CHECK(gxTransactionPagesInit(&pages, &allocator, 512, 2) == GxStatus::Ok);
    CHECK(state.outstanding == 1);

    const std::vector<uint8_t> original = {1, 2, 3, 4, 5};
    const std::vector<uint8_t> replacement = {9, 8, 7};
    size_t index = 99;
    CHECK(gxTransactionPagesAppendPage(
              &pages, 7, static_cast<uint16_t>(PageType::Data), 3, 11,
              original.data(), original.size(), &index) == GxStatus::Ok);
    CHECK(index == 0 && gxTransactionPagesCount(&pages) == 1);
    const GxTransactionPageEntry* first = gxTransactionPagesAt(&pages, 0);
    CHECK(first && first->pageId == 7 && first->image);
    CHECK(pagePayloadIs(first->image, 512, 7, original));
    CHECK(pages.stats.currentBytes == 512);

    failAfter(&state, 0);
    CHECK(gxTransactionPagesReplacePage(
              &pages, 0, 7, static_cast<uint16_t>(PageType::Data), 4, 12,
              replacement.data(), replacement.size()) == GxStatus::OutOfMemory);
    disableFailure(&state);
    first = gxTransactionPagesAt(&pages, 0);
    CHECK(first && pagePayloadIs(first->image, 512, 7, original));
    CHECK(pages.stats.currentBytes == 512 && pages.stats.replacements == 0);

    CHECK(gxTransactionPagesReplacePage(
              &pages, 0, 7, static_cast<uint16_t>(PageType::Data), 4, 12,
              replacement.data(), replacement.size()) == GxStatus::Ok);
    first = gxTransactionPagesAt(&pages, 0);
    CHECK(first && pagePayloadIs(first->image, 512, 7, replacement));
    CHECK(pages.stats.currentBytes == 512 && pages.stats.replacements == 1);
    CHECK(pages.stats.peakBytes == 1024);

    GxArray encodedFromFields = {};
    GxArray encodedFromImage = {};
    CHECK(gxArrayInit(&encodedFromFields, &allocator, 1, 1024) == GxStatus::Ok);
    CHECK(gxArrayInit(&encodedFromImage, &allocator, 1, 1024) == GxStatus::Ok);
    CHECK(gxWalPageImageRecordEncode(
              &encodedFromFields, 42, 7,
              static_cast<uint16_t>(PageType::Data), 4, 12,
              replacement.data(), replacement.size(), 512, 4096) == GxStatus::Ok);
    CHECK(gxWalPageImageRecordEncodeImage(
              &encodedFromImage, 42, 7, first->image, 512, 512, 4096) ==
          GxStatus::Ok);
    CHECK(gxArraySize(&encodedFromFields) == gxArraySize(&encodedFromImage));
    bool equalRecords = gxArraySize(&encodedFromFields) ==
                        gxArraySize(&encodedFromImage);
    for (size_t i = 0; equalRecords && i < gxArraySize(&encodedFromFields); ++i) {
        equalRecords = encodedFromFields.data[i] == encodedFromImage.data[i];
    }
    CHECK(equalRecords);
    gxArrayDestroy(&encodedFromImage);
    gxArrayDestroy(&encodedFromFields);

    const size_t beforeFailedClone = state.outstanding;
    GxTransactionPages failedClone = {};
    failAfter(&state, 1); // Descriptor succeeds; the first cloned page fails.
    CHECK(gxTransactionPagesClone(&failedClone, &pages) == GxStatus::OutOfMemory);
    disableFailure(&state);
    CHECK(!failedClone.initialized);
    CHECK(state.outstanding == beforeFailedClone);
    first = gxTransactionPagesAt(&pages, 0);
    CHECK(first && pagePayloadIs(first->image, 512, 7, replacement));

    GxTransactionPages clone = {};
    CHECK(gxTransactionPagesClone(&clone, &pages) == GxStatus::Ok);
    CHECK(gxTransactionPagesCount(&clone) == 1);
    GxTransactionPages moved = {};
    CHECK(gxTransactionPagesMove(&moved, &clone) == GxStatus::Ok);
    CHECK(!clone.initialized && gxTransactionPagesCount(&moved) == 1);
    const GxTransactionPageEntry* movedEntry = gxTransactionPagesAt(&moved, 0);
    CHECK(movedEntry && pagePayloadIs(movedEntry->image, 512, 7, replacement));

    gxTransactionPagesDestroy(&moved);
    gxTransactionPagesDestroy(&clone);
    gxTransactionPagesDestroy(&failedClone);
    gxTransactionPagesDestroy(&pages);
    CHECK(state.outstanding == 0);
    CHECK(state.frees == state.successes);
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
    std::remove(walPath(path).c_str());
}

bool samePage(const DatabasePage& left, const DatabasePage& right) {
    return left.pageId == right.pageId && left.type == right.type &&
           left.flags == right.flags && left.generation == right.generation &&
           left.payload == right.payload;
}

void testProductionTransactionFailuresAndSavepoint() {
    std::printf("production transaction staging, visibility, savepoint, rollback\n");
    const std::string path = uniquePath("p7_owner");
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

    DatabasePage committed;
    CHECK_STATUS(db->readPage(1, committed), DbStatus::Ok);
    DatabasePage firstWrite = committed;
    firstWrite.payload[0] ^= 0x5a;
    DatabasePage secondWrite = firstWrite;
    secondWrite.payload[0] ^= 0x13;

    std::unique_ptr<Transaction> tx;
    failAfter(&state, 0);
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::NoSpace);
    CHECK(tx == nullptr && state.outstanding == 0);
    disableFailure(&state);
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    CHECK(tx != nullptr);

    // Fail the first page image allocation, then retry the same page.
    failAfter(&state, 0);
    CHECK_STATUS(tx->writePage(firstWrite), DbStatus::NoSpace);
    disableFailure(&state);
    CHECK(tx->modifiedPageCount() == 0);
    CHECK(tx->pageStats().currentBytes == 0);
    CHECK_STATUS(tx->writePage(firstWrite), DbStatus::Ok);
    CHECK(tx->modifiedPageCount() == 1);
    DatabasePage overlay;
    CHECK_STATUS(tx->readPage(1, overlay), DbStatus::Ok);
    CHECK(samePage(overlay, firstWrite));
    DatabasePage committedDuring;
    CHECK_STATUS(db->readPage(1, committedDuring), DbStatus::Ok);
    CHECK(samePage(committedDuring, committed));

    // A failed replacement leaves the previous staged image readable.
    failAfter(&state, 0);
    CHECK_STATUS(tx->writePage(secondWrite), DbStatus::NoSpace);
    disableFailure(&state);
    CHECK(tx->modifiedPageCount() == 1);
    CHECK_STATUS(tx->readPage(1, overlay), DbStatus::Ok);
    CHECK(samePage(overlay, firstWrite));
    CHECK_STATUS(tx->writePage(secondWrite), DbStatus::Ok);
    CHECK(tx->pageStats().replacements == 1);
    CHECK_STATUS(tx->readPage(1, overlay), DbStatus::Ok);
    CHECK(samePage(overlay, secondWrite));

    // A later image allocation failure does not advance the transaction page
    // count, and a successful retry preserves first-stage order.
    const uint64_t oldPageCount = tx->pageCount();
    uint64_t newPageId = 0;
    failAfter(&state, 0);
    CHECK_STATUS(tx->allocatePage(PageType::Data, newPageId), DbStatus::NoSpace);
    disableFailure(&state);
    CHECK(tx->pageCount() == oldPageCount && tx->modifiedPageCount() == 1);
    CHECK_STATUS(tx->allocatePage(PageType::Data, newPageId), DbStatus::Ok);
    CHECK(newPageId == oldPageCount && tx->modifiedPageCount() == 2);
    DatabasePage newPage;
    CHECK_STATUS(tx->readPage(newPageId, newPage), DbStatus::Ok);
    newPage.payload.assign(24, 0x4d);
    newPage.payloadSize = static_cast<uint32_t>(newPage.payload.size());
    CHECK_STATUS(tx->writePage(newPage), DbStatus::Ok);

    // Snapshot allocation failure leaves transaction pages intact. Successful
    // rollback restores the earlier owner set without allocating.
    const size_t outstandingBeforeSnapshot = state.outstanding;
    TransactionSavepoint failedSnapshot;
    failAfter(&state, 1); // Snapshot descriptor succeeds, cloned page fails.
    CHECK_STATUS(tx->beginStatement(failedSnapshot), DbStatus::NoSpace);
    disableFailure(&state);
    CHECK(state.outstanding == outstandingBeforeSnapshot);
    CHECK(tx->modifiedPageCount() == 2);
    CHECK_STATUS(tx->readPage(1, overlay), DbStatus::Ok);
    CHECK(samePage(overlay, secondWrite));

    TransactionSavepoint savepoint;
    CHECK_STATUS(tx->beginStatement(savepoint), DbStatus::Ok);
    DatabasePage thirdWrite = secondWrite;
    thirdWrite.payload[1] ^= 0x67;
    CHECK_STATUS(tx->writePage(thirdWrite), DbStatus::Ok);
    failAfter(&state, 0);
    CHECK_STATUS(tx->rollbackStatement(savepoint), DbStatus::Ok);
    disableFailure(&state);
    CHECK(tx->modifiedPageCount() == 2);
    CHECK_STATUS(tx->readPage(1, overlay), DbStatus::Ok);
    CHECK(samePage(overlay, secondWrite));

    const GxTransactionPageStats beforeRollback = tx->pageStats();
    CHECK(beforeRollback.currentBytes == 2 * db->pageSize());
    CHECK(beforeRollback.imageAllocations >= 4);
    CHECK(beforeRollback.replacements >= 2);
    CHECK(beforeRollback.allocationFailures >= 4);
    CHECK(beforeRollback.peakBytes >= 3 * db->pageSize());
    std::printf("P7 owner diagnostics: staged=%u image-allocations=%llu "
                "replacements=%llu failures=%llu peak-bytes=%llu\n",
                tx->modifiedPageCount(),
                static_cast<unsigned long long>(beforeRollback.imageAllocations),
                static_cast<unsigned long long>(beforeRollback.replacements),
                static_cast<unsigned long long>(beforeRollback.allocationFailures),
                static_cast<unsigned long long>(beforeRollback.peakBytes));

    CHECK_STATUS(tx->rollback(), DbStatus::Ok);
    CHECK(tx->modifiedPageCount() == 0 && tx->pageStats().currentBytes == 0);
    CHECK(db->pageCount() == oldPageCount);
    CHECK_STATUS(db->readPage(1, committedDuring), DbStatus::Ok);
    CHECK(samePage(committedDuring, committed));
    DatabasePage absent;
    CHECK_STATUS(db->readPage(newPageId, absent), DbStatus::OutOfBounds);
    tx.reset();
    CHECK(state.outstanding == 0 && state.frees == state.successes);
    CHECK_STATUS(db->close(), DbStatus::Ok);
    db.reset();

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    CHECK(reopened != nullptr);
    if (reopened) {
        DatabasePage afterReopen;
        CHECK_STATUS(reopened->readPage(1, afterReopen), DbStatus::Ok);
        CHECK(samePage(afterReopen, committed));
        CHECK(reopened->pageCount() == oldPageCount);
        CHECK_STATUS(reopened->close(), DbStatus::Ok);
    }
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

void testSql9TransactionOwnerWorkload() {
    std::printf("SQL9 prepared DML, FK, indexes, relocation, commit and reopen\n");
    const std::string path = uniquePath("p7_sql9");
    removeDatabaseFiles(path);
    FaultAllocatorState state;
    GxAllocator allocator = makeFaultAllocator(&state);
    DatabaseCreateOptions options;
    options.pageSize = 512;
    options.bufferCapacity = 3;
    options.transactionPageAllocator = &allocator;
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, options, db), DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE Accounts (Id INT64 PRIMARY KEY, "
                     "Name TEXT NOT NULL);").ok);
        CHECK(runSql(engine,
                     "CREATE TABLE Items (Id INT64 PRIMARY KEY, "
                     "AccountId INT64 NOT NULL, Tag TEXT NOT NULL, "
                     "Payload TEXT NOT NULL, FOREIGN KEY (AccountId) "
                     "REFERENCES Accounts (Id));").ok);
        CHECK(runSql(engine,
                     "CREATE INDEX IX_Items_Tag ON Items (Tag);").ok);

        SqlPreparedStatement insertAccount;
        SqlPreparedStatement insertItem;
        CHECK(engine.prepare("INSERT INTO Accounts VALUES (?, ?);",
                             insertAccount).isOk());
        CHECK(engine.prepare("INSERT INTO Items VALUES (?, ?, ?, ?);",
                             insertItem).isOk());
        CHECK(runSql(engine, "BEGIN;").ok);
        for (int64_t id = 1; id <= 12; ++id) {
            CHECK(insertAccount.bindInt64(0, id).isOk());
            CHECK(insertAccount.bindText(1, "account-" + std::to_string(id)).isOk());
            CHECK(insertAccount.execute().ok);
            CHECK(insertItem.bindInt64(0, id).isOk());
            CHECK(insertItem.bindInt64(1, id).isOk());
            CHECK(insertItem.bindText(2, "tag-" + std::to_string(id % 3)).isOk());
            CHECK(insertItem.bindText(3, "payload-" + std::to_string(id) +
                                          std::string(38 + id, 'x')).isOk());
            CHECK(insertItem.execute().ok);
        }
        CHECK(db->activeTransaction() != nullptr);
        const GxTransactionPageStats workloadStats =
            db->activeTransaction()->pageStats();
        std::printf("P7 SQL owner diagnostics: staged=%u image-allocations=%llu "
                    "replacements=%llu failures=%llu peak-bytes=%llu\n",
                    db->activeTransaction()->modifiedPageCount(),
                    static_cast<unsigned long long>(workloadStats.imageAllocations),
                    static_cast<unsigned long long>(workloadStats.replacements),
                    static_cast<unsigned long long>(workloadStats.allocationFailures),
                    static_cast<unsigned long long>(workloadStats.peakBytes));
        CHECK(db->activeTransaction()->modifiedPageCount() > 3);
        CHECK(workloadStats.currentBytes <= 4096u * db->pageSize());
        CHECK(workloadStats.peakBytes <= 4097u * db->pageSize());
        CHECK(runSql(engine, "COMMIT;").ok);

        SqlPreparedStatement updatePayload;
        SqlPreparedStatement invalidFkUpdate;
        SqlPreparedStatement insertExtra;
        SqlPreparedStatement deleteItem;
        CHECK(engine.prepare("UPDATE Items SET Payload = ? WHERE Id = ?;",
                             updatePayload).isOk());
        CHECK(engine.prepare("UPDATE Items SET AccountId = ? WHERE Id = ?;",
                             invalidFkUpdate).isOk());
        CHECK(engine.prepare("INSERT INTO Items VALUES (?, ?, ?, ?);",
                             insertExtra).isOk());
        CHECK(engine.prepare("DELETE FROM Items WHERE Id = ?;",
                             deleteItem).isOk());

        std::vector<IndexInfo> beforeUpdateIndexes;
        CHECK_STATUS(db->listIndexes(beforeUpdateIndexes), DbStatus::Ok);
        uint32_t itemPrimaryIndex = 0;
        for (size_t i = 0; i < beforeUpdateIndexes.size(); ++i) {
            if (beforeUpdateIndexes[i].tableName == "Items" &&
                beforeUpdateIndexes[i].primaryKey()) {
                itemPrimaryIndex = beforeUpdateIndexes[i].indexId;
            }
        }
        CHECK(itemPrimaryIndex != 0);
        std::vector<uint8_t> itemThreeKey;
        CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(3), itemThreeKey));
        std::vector<RowLocator> locatorBeforeUpdate;
        CHECK_STATUS(db->indexLookup(itemPrimaryIndex, itemThreeKey,
                                     locatorBeforeUpdate), DbStatus::Ok);
        CHECK(locatorBeforeUpdate.size() == 1);

        CHECK(runSql(engine, "BEGIN;").ok);
        const std::string relocatedPayload = "relocated-" + std::string(350, 'r');
        CHECK(updatePayload.bindText(0, relocatedPayload).isOk());
        CHECK(updatePayload.bindInt64(1, 3).isOk());
        CHECK(updatePayload.execute().ok);
        CHECK(invalidFkUpdate.bindInt64(0, 999).isOk());
        CHECK(invalidFkUpdate.bindInt64(1, 3).isOk());
        SqlStatementResult failedUpdate = invalidFkUpdate.execute();
        CHECK(!failedUpdate.ok && failedUpdate.transactionActiveAfter);
        CHECK(insertExtra.bindInt64(0, 13).isOk());
        CHECK(insertExtra.bindInt64(1, 1).isOk());
        CHECK(insertExtra.bindText(2, "tag-extra").isOk());
        CHECK(insertExtra.bindText(3, "extra-row").isOk());
        CHECK(insertExtra.execute().ok);
        CHECK(deleteItem.bindInt64(0, 12).isOk());
        CHECK(deleteItem.execute().ok);

        SqlPreparedStatement ownRead;
        CHECK(engine.prepare("SELECT Payload FROM Items WHERE Id = ?;",
                             ownRead).isOk());
        CHECK(ownRead.bindInt64(0, 3).isOk());
        SqlStatementResult ownReadResult = ownRead.executeQuery();
        CHECK(ownReadResult.ok && ownReadResult.resultSet.rowCount() == 1);
        if (ownReadResult.ok && ownReadResult.resultSet.rowCount() == 1) {
            CHECK(ownReadResult.resultSet.value(0, 0).textValue() ==
                  relocatedPayload);
        }
        CHECK(runSql(engine, "COMMIT;").ok);

        std::vector<RowLocator> locatorAfterUpdate;
        CHECK_STATUS(db->indexLookup(itemPrimaryIndex, itemThreeKey,
                                     locatorAfterUpdate), DbStatus::Ok);
        CHECK(locatorAfterUpdate.size() == 1);
        if (locatorBeforeUpdate.size() == 1 && locatorAfterUpdate.size() == 1) {
            CHECK(locatorBeforeUpdate[0].pageId != locatorAfterUpdate[0].pageId ||
                  locatorBeforeUpdate[0].slot != locatorAfterUpdate[0].slot);
        }

        std::vector<TableInfo> tables;
        CHECK_STATUS(db->listTables(tables), DbStatus::Ok);
        bool multipleHeapPages = false;
        for (size_t i = 0; i < tables.size(); ++i) {
            if (tables[i].name == "Items" && tables[i].heapPageCount >= 2) {
                multipleHeapPages = true;
            }
        }
        CHECK(multipleHeapPages);
        std::string integrity;
        CHECK_STATUS(db->validateIntegrity(integrity), DbStatus::Ok);
        std::vector<IndexInfo> indexes;
        CHECK_STATUS(db->listIndexes(indexes), DbStatus::Ok);
        CHECK(indexes.size() >= 4); // Two primary keys, FK support, secondary.
        for (size_t i = 0; i < indexes.size(); ++i) {
            IndexValidation validation;
            CHECK_STATUS(db->validateIndex(indexes[i].indexId, validation),
                         DbStatus::Ok);
            CHECK(validation.ok);
        }

        CHECK(runSql(engine, "BEGIN;").ok);
        CHECK(updatePayload.bindText(0, "will-rollback").isOk());
        CHECK(updatePayload.bindInt64(1, 3).isOk());
        CHECK(updatePayload.execute().ok);
        CHECK(runSql(engine, "ROLLBACK;").ok);
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    db.reset();
    CHECK(state.outstanding == 0 && state.frees == state.successes);

    // Reopening twice checks both durable row/index state and repeat recovery
    // after the WAL checkpoint performed by close.
    for (int reopen = 0; reopen < 2; ++reopen) {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened),
                     DbStatus::Ok);
        CHECK(reopened != nullptr);
        if (!reopened) break;
        SqlEngine engine(*reopened);
        SqlPreparedStatement query;
        CHECK(engine.prepare("SELECT Id, Payload FROM Items WHERE Id = ?;",
                             query).isOk());
        CHECK(query.bindInt64(0, 3).isOk());
        SqlStatementResult row = query.executeQuery();
        CHECK(row.ok && row.resultSet.rowCount() == 1);
        if (row.ok && row.resultSet.rowCount() == 1) {
            CHECK(row.resultSet.value(0, 1).textValue() ==
                  "relocated-" + std::string(350, 'r'));
        }
        SqlExecutionResult allItems = runSql(
            engine, "SELECT Id FROM Items ORDER BY Id;");
        CHECK(allItems.ok && allItems.statements.size() == 1);
        if (allItems.ok && allItems.statements.size() == 1) {
            const SqlResultSet& result = allItems.statements[0].resultSet;
            CHECK(result.rowCount() == 12);
            std::vector<int64_t> ids;
            for (size_t i = 0; i < result.rowCount(); ++i) {
                ids.push_back(result.value(i, 0).int64Value());
            }
            CHECK(ids.size() == 12 && ids.front() == 1 && ids.back() == 13);
            for (size_t i = 0; i < ids.size(); ++i) CHECK(ids[i] != 12);
        }
        std::string integrity;
        CHECK_STATUS(reopened->validateIntegrity(integrity), DbStatus::Ok);
        CHECK_STATUS(reopened->close(), DbStatus::Ok);
    }
    removeDatabaseFiles(path);
}

} // namespace

int main() {
    testPortableTransactionPageOwner();
    testProductionTransactionFailuresAndSavepoint();
    testSql9TransactionOwnerWorkload();
    std::printf("NDB1R4P7: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
