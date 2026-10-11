// SQL9R acceptance tests: retained prepared plans, state isolation and a
// deterministic mixed prepared-DML workload.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <limits>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "database_engine.h"
#include "database_format.h"
#include "database_index.h"
#include "database_relational.h"
#include "database_sql.h"

using namespace gxos::db;

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

std::string testPath() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    const std::string dir = std::string(base) + "/gxos_gxdb_sql9r_tests";
#if defined(_WIN32)
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0700);
#endif
    return dir + "/prepared_reuse.gxdb";
}

void removeDatabase(const std::string& path) {
    std::remove(walPathFor(path).c_str());
    std::remove(path.c_str());
}

bool run(SqlEngine& engine, const std::string& sql) {
    const SqlExecutionResult result = engine.execute(sql);
    if (!result.ok) std::printf("  SQL failed: %s\n", result.error.describe().c_str());
    return result.ok;
}

struct OrderModel {
    int64_t id;
    int64_t userId;
    std::string state;
    std::string payload;
};

bool rowsEqual(const SqlResultSet& result, const std::vector<OrderModel>& model,
               int64_t userId) {
    std::vector<const OrderModel*> expected;
    for (size_t i = 0; i < model.size(); ++i)
        if (model[i].userId == userId) expected.push_back(&model[i]);
    if (result.rowCount() != expected.size() || result.columnCount() != 2) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        const std::string name = "User-" + std::to_string(userId);
        if (result.value(i, 0).textValue() != name ||
            result.value(i, 1).int64Value() != expected[i]->id) return false;
    }
    return true;
}

bool indexMatchesModel(Database& db) {
    std::vector<IndexInfo> indexes;
    if (!db.listIndexes(indexes).isOk()) return false;
    for (size_t i = 0; i < indexes.size(); ++i) {
        IndexValidation validation;
        if (!db.validateIndex(indexes[i].indexId, validation).isOk() ||
            !validation.ok) return false;
    }
    return true;
}

void testReuseAndMixedWorkload(const std::string& path,
                               std::vector<OrderModel>& model) {
    std::unique_ptr<Database> db;
    CHECK(Database::create(path, DatabaseCreateOptions(), db).isOk());
    if (!db) return;

    SqlPreparedStatement oldAfterClose;
    {
        SqlEngine engine(*db);
        CHECK(run(engine,
                  "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT NULL UNIQUE);"));
        CHECK(run(engine,
                  "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 NOT NULL, "
                  "State TEXT NOT NULL, Payload TEXT NOT NULL, "
                  "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(run(engine, "CREATE INDEX IX_Orders_State ON Orders (State);"));

        SqlPreparedStatement userInsert;
        CHECK(engine.prepare("INSERT INTO Users VALUES (?, ?);", userInsert).isOk());
        std::vector<std::string> users(65);
        for (int64_t id = 1; id <= 64; ++id) {
            users[static_cast<size_t>(id)] = "User-" + std::to_string(id);
            CHECK(userInsert.bindInt64(0, id).isOk());
            CHECK(userInsert.bindText(1, users[static_cast<size_t>(id)]).isOk());
            CHECK(userInsert.execute().ok);
        }

        SqlPreparedStatement orderInsert;
        CHECK(engine.prepare("INSERT INTO Orders VALUES (?, ?, ?, ?);", orderInsert).isOk());
        for (int64_t id = 1; id <= 64; ++id) {
            const int64_t userId = (id % 10) + 1;
            const std::string state = id % 2 ? "open" : "closed";
            CHECK(orderInsert.bindInt64(0, id).isOk());
            CHECK(orderInsert.bindInt64(1, userId).isOk());
            CHECK(orderInsert.bindText(2, state).isOk());
            CHECK(orderInsert.bindText(3, "seed").isOk());
            CHECK(orderInsert.execute().ok);
            OrderModel row = {id, userId, state, "seed"};
            model.push_back(row);
        }

        // Mandatory proof: one indexed physical plan serves 5,000 changing keys.
        SqlPreparedStatement point;
        CHECK(engine.prepare("SELECT Name FROM Users WHERE Id = ?;", point).isOk());
        std::string selectedIndex;
        for (int i = 0; i < 5000; ++i) {
            const int64_t id = (i % 64) + 1;
            CHECK(point.bindInt64(0, id).isOk());
            const SqlStatementResult result = point.executeQuery();
            CHECK(result.ok && result.accessPath == "IndexLookup");
            CHECK(result.resultSet.rowCount() == 1);
            if (result.ok && result.resultSet.rowCount() == 1)
                CHECK(result.resultSet.value(0, 0).textValue() == users[id]);
            if (i == 0) selectedIndex = result.accessIndexName;
            CHECK(result.accessIndexName == selectedIndex);
        }
        const SqlPreparedDiagnostics& pointDiag = point.diagnostics();
        CHECK(pointDiag.tokenizeCount == 1 && pointDiag.parseCount == 1);
        CHECK(pointDiag.semanticBindCount == 1 && pointDiag.physicalPlanBuildCount == 1);
        CHECK(pointDiag.executionCount == 5000 && pointDiag.physicalPlanReuseCount == 4999);
        CHECK(pointDiag.accessPath == "IndexLookup" && pointDiag.indexId != 0);

        SqlPreparedStatement range;
        CHECK(engine.prepare("SELECT Id FROM Users WHERE Id >= ? AND Id < ? ORDER BY Id;",
                             range).isOk());
        for (int64_t start = 1; start <= 64; ++start) {
            const int64_t end = start + 8;
            CHECK(range.bindInt64(0, start).isOk());
            CHECK(range.bindInt64(1, end).isOk());
            const SqlStatementResult result = range.executeQuery();
            CHECK(result.ok && result.accessPath == "IndexRange");
            std::vector<int64_t> expected;
            for (int64_t id = start; id < end && id <= 64; ++id) expected.push_back(id);
            CHECK(result.resultSet.rowCount() == expected.size());
            for (size_t i = 0; i < expected.size() && i < result.resultSet.rowCount(); ++i)
                CHECK(result.resultSet.value(i, 0).int64Value() == expected[i]);
        }
        CHECK(range.diagnostics().physicalPlanBuildCount == 1);
        CHECK(range.diagnostics().executionCount == 64);

        CHECK(range.bindInt64(0, std::numeric_limits<int64_t>::min()).isOk());
        CHECK(range.bindInt64(1, std::numeric_limits<int64_t>::max()).isOk());
        SqlStatementResult edgeRange = range.executeQuery();
        CHECK(edgeRange.ok && edgeRange.resultSet.rowCount() == 64);
        CHECK(range.bindInt64(0, std::numeric_limits<int64_t>::max()).isOk());
        CHECK(range.bindInt64(1, std::numeric_limits<int64_t>::max()).isOk());
        edgeRange = range.executeQuery();
        CHECK(edgeRange.ok && edgeRange.resultSet.rowCount() == 0);
        CHECK(range.bindNull(0).isOk());
        edgeRange = range.executeQuery();
        CHECK(edgeRange.ok && edgeRange.resultSet.rowCount() == 0);
        CHECK(range.diagnostics().physicalPlanBuildCount == 1);
        CHECK(range.diagnostics().executionCount == 67);

        SqlPreparedStatement textRange;
        CHECK(engine.prepare("SELECT Id FROM Orders WHERE State >= ? ORDER BY Id;",
                             textRange).isOk());
        CHECK(textRange.bindText(0, "").isOk());
        edgeRange = textRange.executeQuery();
        CHECK(edgeRange.ok && edgeRange.resultSet.rowCount() == 64);
        CHECK(textRange.bindText(0, "zzzz").isOk());
        edgeRange = textRange.executeQuery();
        CHECK(edgeRange.ok && edgeRange.resultSet.rowCount() == 0);
        CHECK(textRange.diagnostics().physicalPlanBuildCount == 1);

        SqlPreparedStatement lifetime;
        CHECK(engine.prepare("SELECT Name FROM Users WHERE Id = ?;", lifetime).isOk());
        CHECK(lifetime.bindInt64(0, 1).isOk());
        SqlStatementResult retainedResult = lifetime.executeQuery();
        CHECK(lifetime.bindInt64(0, 2).isOk());
        SqlStatementResult followingResult = lifetime.executeQuery();
        CHECK(retainedResult.ok && followingResult.ok);
        CHECK(retainedResult.resultSet.value(0, 0).textValue() == "User-1");
        CHECK(followingResult.resultSet.value(0, 0).textValue() == "User-2");

        const std::string joinSql =
            "SELECT u.Name, o.Id FROM Users AS u JOIN Orders AS o "
            "ON o.UserId = u.Id WHERE u.Id = ? ORDER BY o.Id;";
        SqlPreparedStatement join;
        CHECK(engine.prepare(joinSql, join).isOk());
        engine.setForceNestedLoop(true);
        SqlPreparedStatement nested;
        CHECK(engine.prepare(joinSql, nested).isOk());
        engine.setForceNestedLoop(false);
        CHECK(join.diagnostics().joinAccessPaths.size() == 1);
        CHECK(join.diagnostics().joinAccessPaths[0] == "IndexNestedLoop");
        CHECK(join.diagnostics().joinIndexIds[0] != 0);
        for (int64_t userId = 1; userId <= 64; ++userId) {
            CHECK(join.bindInt64(0, userId).isOk());
            CHECK(nested.bindInt64(0, userId).isOk());
            const SqlStatementResult indexed = join.executeQuery();
            const SqlStatementResult scanned = nested.executeQuery();
            CHECK(indexed.ok && scanned.ok);
            CHECK(indexed.joins.size() == 1 &&
                  indexed.joins[0].accessPath == "IndexNestedLoop");
            CHECK(scanned.joins.size() == 1 &&
                  scanned.joins[0].accessPath == "NestedLoop");
            CHECK(rowsEqual(indexed.resultSet, model, userId));
            CHECK(indexed.resultSet.rowCount() == scanned.resultSet.rowCount());
            for (size_t r = 0; r < indexed.resultSet.rowCount() &&
                               r < scanned.resultSet.rowCount(); ++r) {
                CHECK(indexed.resultSet.value(r, 0).textValue() ==
                      scanned.resultSet.value(r, 0).textValue());
                CHECK(indexed.resultSet.value(r, 1).int64Value() ==
                      scanned.resultSet.value(r, 1).int64Value());
            }
        }
        CHECK(join.diagnostics().physicalPlanBuildCount == 1);
        CHECK(join.diagnostics().executionCount == 64);
        CHECK(join.diagnostics().physicalPlanReuseCount == 63);

        SqlPreparedStatement aggregate;
        CHECK(engine.prepare(
                  "SELECT State, COUNT(*) AS N FROM Orders WHERE UserId = ? "
                  "GROUP BY State ORDER BY State;",
                  aggregate).isOk());
        for (int64_t userId = 1; userId <= 64; ++userId) {
            CHECK(aggregate.bindInt64(0, userId).isOk());
            const SqlStatementResult result = aggregate.executeQuery();
            CHECK(result.ok);
            std::map<std::string, int64_t> expected;
            for (size_t i = 0; i < model.size(); ++i)
                if (model[i].userId == userId) ++expected[model[i].state];
            CHECK(result.resultSet.rowCount() == expected.size());
            size_t row = 0;
            for (std::map<std::string, int64_t>::const_iterator it = expected.begin();
                 it != expected.end(); ++it, ++row) {
                CHECK(result.resultSet.value(row, 0).textValue() == it->first);
                CHECK(result.resultSet.value(row, 1).int64Value() == it->second);
            }
        }
        CHECK(aggregate.diagnostics().physicalPlanBuildCount == 1);
        CHECK(aggregate.diagnostics().executionCount == 64);

        SqlPreparedStatement orderPoint;
        SqlPreparedStatement orderRange;
        SqlPreparedStatement update;
        SqlPreparedStatement remove;
        CHECK(engine.prepare("SELECT Id, UserId, State, Payload FROM Orders WHERE Id = ?;",
                             orderPoint).isOk());
        CHECK(engine.prepare("SELECT Id FROM Orders WHERE Id >= ? AND Id < ? ORDER BY Id;",
                             orderRange).isOk());
        CHECK(engine.prepare("UPDATE Orders SET State = ?, Payload = ? WHERE Id = ?;",
                             update).isOk());
        CHECK(engine.prepare("DELETE FROM Orders WHERE Id = ?;", remove).isOk());
        CHECK(update.diagnostics().accessPath == "IndexLookup" &&
              update.diagnostics().indexId != 0);
        CHECK(remove.diagnostics().accessPath == "IndexLookup" &&
              remove.diagnostics().indexId != 0);

        // Six prepared statements are reused in a mixed workload. The model is
        // updated independently and checked after every query.
        const int kIterations = 850;
        for (int i = 0; i < kIterations; ++i) {
            const int64_t id = 10000 + i;
            const int64_t userId = (i % 10) + 1;
            const std::string initialState = i % 2 ? "open" : "closed";
            CHECK(orderInsert.bindInt64(0, id).isOk());
            CHECK(orderInsert.bindInt64(1, userId).isOk());
            CHECK(orderInsert.bindText(2, initialState).isOk());
            CHECK(orderInsert.bindText(3, "new").isOk());
            CHECK(orderInsert.execute().ok);
            OrderModel inserted = {id, userId, initialState, "new"};
            model.push_back(inserted);

            CHECK(orderPoint.bindInt64(0, id).isOk());
            SqlStatementResult result = orderPoint.executeQuery();
            CHECK(result.ok && result.accessPath == "IndexLookup");
            CHECK(result.resultSet.rowCount() == 1);
            CHECK(result.resultSet.value(0, 0).int64Value() == id);
            CHECK(result.resultSet.value(0, 1).int64Value() == userId);

            CHECK(orderRange.bindInt64(0, id - 5).isOk());
            CHECK(orderRange.bindInt64(1, id + 1).isOk());
            result = orderRange.executeQuery();
            CHECK(result.ok && result.accessPath == "IndexRange");
            std::vector<int64_t> expectedRange;
            for (size_t r = 0; r < model.size(); ++r)
                if (model[r].id >= id - 5 && model[r].id < id + 1)
                    expectedRange.push_back(model[r].id);
            CHECK(result.resultSet.rowCount() == expectedRange.size());
            for (size_t r = 0; r < expectedRange.size(); ++r)
                CHECK(result.resultSet.value(r, 0).int64Value() == expectedRange[r]);

            const std::string changedState = i % 3 ? "closed" : "open";
            const std::string changedPayload(64 + (i % 32), 'x');
            CHECK(update.bindText(0, changedState).isOk());
            CHECK(update.bindText(1, changedPayload).isOk());
            CHECK(update.bindInt64(2, id).isOk());
            result = update.execute();
            CHECK(result.ok && result.affectedRows == 1 &&
                  result.accessPath == "IndexLookup");
            model.back().state = changedState;
            model.back().payload = changedPayload;

            CHECK(join.bindInt64(0, userId).isOk());
            result = join.executeQuery();
            CHECK(result.ok && rowsEqual(result.resultSet, model, userId));

            CHECK(aggregate.bindInt64(0, userId).isOk());
            result = aggregate.executeQuery();
            CHECK(result.ok);
            std::map<std::string, int64_t> counts;
            for (size_t r = 0; r < model.size(); ++r)
                if (model[r].userId == userId) ++counts[model[r].state];
            CHECK(result.resultSet.rowCount() == counts.size());
            size_t outputRow = 0;
            for (std::map<std::string, int64_t>::const_iterator it = counts.begin();
                 it != counts.end(); ++it, ++outputRow) {
                CHECK(result.resultSet.value(outputRow, 0).textValue() == it->first);
                CHECK(result.resultSet.value(outputRow, 1).int64Value() == it->second);
            }

            if (i % 2 == 0) {
                CHECK(remove.bindInt64(0, id).isOk());
                result = remove.execute();
                CHECK(result.ok && result.affectedRows == 1 &&
                      result.accessPath == "IndexLookup");
                model.pop_back();
            }
            if ((i + 1) % 100 == 0) {
                std::string integrity;
                CHECK(db->validateIntegrity(integrity).isOk());
                CHECK(indexMatchesModel(*db));
            }
        }

        CHECK(orderInsert.diagnostics().physicalPlanBuildCount == 1);
        CHECK(orderPoint.diagnostics().physicalPlanBuildCount == 1);
        CHECK(orderRange.diagnostics().physicalPlanBuildCount == 1);
        CHECK(update.diagnostics().physicalPlanBuildCount == 1);
        CHECK(remove.diagnostics().physicalPlanBuildCount == 1);
        CHECK(update.diagnostics().executionCount == kIterations);
        CHECK(remove.diagnostics().executionCount == kIterations / 2);
        CHECK(orderInsert.diagnostics().executionCount +
                  orderPoint.diagnostics().executionCount +
                  orderRange.diagnostics().executionCount +
                  update.diagnostics().executionCount +
                  join.diagnostics().executionCount +
                  aggregate.diagnostics().executionCount +
                  remove.diagnostics().executionCount >= 5000);

        // A runtime FK/UNIQUE failure does not invalidate the retained plan;
        // a later valid execution still works.
        CHECK(orderInsert.bindInt64(0, 900000).isOk());
        CHECK(orderInsert.bindInt64(1, 999999).isOk());
        CHECK(orderInsert.bindText(2, "bad").isOk());
        CHECK(orderInsert.bindText(3, "bad").isOk());
        CHECK(!orderInsert.execute().ok);
        const uint64_t insertBuilds = orderInsert.diagnostics().physicalPlanBuildCount;
        CHECK(orderInsert.bindInt64(1, 1).isOk());
        CHECK(orderInsert.bindText(2, "open").isOk());
        CHECK(orderInsert.bindText(3, "reused").isOk());
        CHECK(orderInsert.execute().ok);
        model.push_back(OrderModel{900000, 1, "open", "reused"});
        CHECK(orderInsert.diagnostics().physicalPlanBuildCount == insertBuilds);

        // A duplicate primary key is another runtime failure; the same INSERT
        // plan still accepts a later row.
        CHECK(orderInsert.bindInt64(0, 900000).isOk());
        CHECK(orderInsert.bindInt64(1, 1).isOk());
        CHECK(orderInsert.bindText(2, "duplicate").isOk());
        CHECK(orderInsert.bindText(3, "duplicate").isOk());
        CHECK(!orderInsert.execute().ok);
        CHECK(orderInsert.diagnostics().physicalPlanBuildCount == insertBuilds);
        CHECK(orderInsert.bindInt64(0, 900004).isOk());
        CHECK(orderInsert.bindInt64(1, 1).isOk());
        CHECK(orderInsert.bindText(2, "after-duplicate").isOk());
        CHECK(orderInsert.bindText(3, "valid-again").isOk());
        CHECK(orderInsert.execute().ok);
        model.push_back(OrderModel{900004, 1, "after-duplicate", "valid-again"});
        CHECK(orderInsert.diagnostics().physicalPlanBuildCount == insertBuilds);

        // Failed statement inside an explicit transaction is isolated by the
        // regular statement savepoint; successful siblings commit together.
        CHECK(run(engine, "BEGIN;"));
        CHECK(orderInsert.bindInt64(0, 900001).isOk());
        CHECK(orderInsert.bindInt64(1, 2).isOk());
        CHECK(orderInsert.bindText(2, "open").isOk());
        CHECK(orderInsert.bindText(3, "tx-a").isOk());
        CHECK(orderInsert.execute().ok);
        CHECK(orderInsert.bindInt64(0, 900002).isOk());
        CHECK(orderInsert.bindInt64(1, 999999).isOk());
        CHECK(orderInsert.bindText(3, "tx-bad").isOk());
        CHECK(!orderInsert.execute().ok);
        CHECK(orderInsert.bindInt64(0, 900003).isOk());
        CHECK(orderInsert.bindInt64(1, 3).isOk());
        CHECK(orderInsert.bindText(3, "tx-c").isOk());
        CHECK(orderInsert.execute().ok);
        CHECK(run(engine, "COMMIT;"));
        model.push_back(OrderModel{900001, 2, "open", "tx-a"});
        model.push_back(OrderModel{900003, 3, "open", "tx-c"});
        CHECK(orderInsert.diagnostics().physicalPlanBuildCount == 1);

        CHECK(run(engine, "BEGIN;"));
        CHECK(orderInsert.bindInt64(0, 900005).isOk());
        CHECK(orderInsert.bindInt64(1, 4).isOk());
        CHECK(orderInsert.bindText(2, "rolled-back").isOk());
        CHECK(orderInsert.bindText(3, "rolled-back").isOk());
        CHECK(orderInsert.execute().ok);
        CHECK(run(engine, "ROLLBACK;"));
        CHECK(orderInsert.diagnostics().physicalPlanBuildCount == 1);

        SqlPreparedStatement nullQuery;
        CHECK(engine.prepare("SELECT Id FROM Users WHERE Name = ?;", nullQuery).isOk());
        CHECK(nullQuery.bindNull(0).isOk());
        SqlStatementResult nullResult = nullQuery.executeQuery();
        CHECK(nullResult.ok && nullResult.resultSet.rowCount() == 0);
        CHECK(nullQuery.diagnostics().accessPath == "IndexLookup");
        CHECK(nullQuery.diagnostics().physicalPlanBuildCount == 1);

        CHECK(db->close().isOk());
        CHECK(!point.executeQuery().ok);
        oldAfterClose = std::move(point);
    }
    db.reset();

    CHECK(!oldAfterClose.executeQuery().ok);
    CHECK(Database::open(path, DatabaseOpenOptions(), db).isOk());
    if (db) {
        SqlEngine reopened(*db);
        SqlPreparedStatement verify;
        std::sort(model.begin(), model.end(),
                  [](const OrderModel& a, const OrderModel& b) { return a.id < b.id; });
        CHECK(reopened.prepare("SELECT Id FROM Orders ORDER BY Id;", verify).isOk());
        const SqlStatementResult result = verify.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == model.size());
        for (size_t i = 0; i < model.size() && i < result.resultSet.rowCount(); ++i)
            CHECK(result.resultSet.value(i, 0).int64Value() == model[i].id);
        std::string integrity;
        CHECK(db->validateIntegrity(integrity).isOk());
        CHECK(indexMatchesModel(*db));
        CHECK(db->close().isOk());
    }
}

void testSchemaInvalidation(const std::string& path) {
    std::unique_ptr<Database> db;
    CHECK(Database::create(path, DatabaseCreateOptions(), db).isOk());
    if (!db) return;
    SqlEngine engine(*db);
    CHECK(run(engine,
              "CREATE TABLE T (Id INT64 PRIMARY KEY, Name TEXT NOT NULL);"));
    CHECK(run(engine, "INSERT INTO T VALUES (1, 'one');"));

    SqlPreparedStatement dropIndex;
    CHECK(run(engine, "CREATE INDEX IX_T_Name ON T (Name);"));
    CHECK(engine.prepare("SELECT Id FROM T WHERE Name = ?;", dropIndex).isOk());
    CHECK(dropIndex.diagnostics().accessPath == "IndexLookup");
    CHECK(dropIndex.bindText(0, "one").isOk());
    CHECK(dropIndex.executeQuery().ok);
    const uint64_t builds = dropIndex.diagnostics().physicalPlanBuildCount;
    CHECK(run(engine, "DROP INDEX IX_T_Name;"));
    SqlStatementResult stale = dropIndex.executeQuery();
    CHECK(!stale.ok && stale.error.code == SqlErrorCode::SchemaChanged);
    CHECK(dropIndex.diagnostics().physicalPlanBuildCount == builds);
    SqlPreparedStatement afterDrop;
    CHECK(engine.prepare("SELECT Id FROM T WHERE Name = ?;", afterDrop).isOk());
    CHECK(afterDrop.diagnostics().accessPath == "FullScan");

    SqlPreparedStatement beforeCreate;
    CHECK(engine.prepare("SELECT Id FROM T WHERE Name = ?;", beforeCreate).isOk());
    CHECK(beforeCreate.diagnostics().accessPath == "FullScan");
    CHECK(run(engine, "CREATE INDEX IX_T_Name2 ON T (Name);"));
    stale = beforeCreate.executeQuery();
    CHECK(!stale.ok && stale.error.code == SqlErrorCode::SchemaChanged);
    SqlPreparedStatement afterCreate;
    CHECK(engine.prepare("SELECT Id FROM T WHERE Name = ?;", afterCreate).isOk());
    CHECK(afterCreate.diagnostics().accessPath == "IndexLookup");

    SqlPreparedStatement beforeAlter;
    CHECK(engine.prepare("SELECT Name FROM T WHERE Id = ?;", beforeAlter).isOk());
    CHECK(run(engine, "ALTER TABLE T ADD COLUMN Added TEXT NULL;"));
    stale = beforeAlter.executeQuery();
    CHECK(!stale.ok && stale.error.code == SqlErrorCode::SchemaChanged);
    SqlPreparedStatement beforeDropTable;
    CHECK(engine.prepare("SELECT Id FROM T WHERE Id = ?;", beforeDropTable).isOk());
    CHECK(run(engine, "DROP TABLE T;"));
    stale = beforeDropTable.executeQuery();
    CHECK(!stale.ok && stale.error.code == SqlErrorCode::SchemaChanged);

    CHECK(run(engine,
              "CREATE TABLE TxSchema (Id INT64 PRIMARY KEY, Value TEXT NULL);"));
    SqlPreparedStatement transactionLocal;
    CHECK(engine.prepare("SELECT Value FROM TxSchema WHERE Id = ?;",
                         transactionLocal).isOk());
    CHECK(run(engine, "BEGIN;"));
    CHECK(run(engine, "ALTER TABLE TxSchema ADD COLUMN Later INT32 NULL;"));
    CHECK(run(engine, "ROLLBACK;"));
    stale = transactionLocal.executeQuery();
    CHECK(!stale.ok && stale.error.code == SqlErrorCode::SchemaChanged);
    CHECK(db->close().isOk());
}

} // namespace

int main() {
    std::printf("guideXOS SQL9R prepared-plan reuse and mixed workload\n");
    const std::string workloadPath = testPath();
    const std::string schemaPath = workloadPath + ".schema";
    removeDatabase(workloadPath);
    removeDatabase(schemaPath);
    std::vector<OrderModel> model;
    testReuseAndMixedWorkload(workloadPath, model);
    testSchemaInvalidation(schemaPath);
    removeDatabase(workloadPath);
    removeDatabase(schemaPath);
    std::printf("\n%d checks passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
