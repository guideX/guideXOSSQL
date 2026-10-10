// guideXOS SQL -- Phase SQL9
// Hosted prepared-statement, typed-binding, reuse and stale-schema tests.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
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
#include "database_sql_tokenizer.h"

using namespace gxos::db;

namespace {

int g_pass = 0;
int g_fail = 0;
int g_case = 0;

void check(bool ok, const char* expression, int line) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(x) check((x), #x, __LINE__)

std::string testDir() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    const std::string dir = std::string(base) + "/gxos_gxdb_sql9_tests";
#if defined(_WIN32)
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0700);
#endif
    return dir;
}

std::string pathFor(const char* prefix) {
    return testDir() + "/" + prefix + std::to_string(g_case++) + ".gxdb";
}

void removeDatabase(const std::string& path) {
    std::remove(walPathFor(path).c_str());
    std::remove(path.c_str());
}

std::unique_ptr<Database> newDatabase(const std::string& path) {
    std::unique_ptr<Database> db;
    if (!Database::create(path, DatabaseCreateOptions(), db).isOk()) return nullptr;
    return db;
}

bool run(SqlEngine& engine, const std::string& sql) {
    SqlExecutionResult result = engine.execute(sql);
    if (!result.ok) std::printf("  SQL failed: %s\n", result.error.describe().c_str());
    return result.ok;
}

void tokenizerAndPrepareErrors() {
    std::printf("SQL9 tokenizer and prepare limits\n");
    SqlTokenizer tokenizer;
    std::vector<SqlToken> tokens;
    SqlError error;
    CHECK(tokenizer.tokenize("SELECT '?' FROM T WHERE Id=?; -- ?\n", tokens,
                             error));
    size_t parameters = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind == SqlTokenKind::Parameter) {
            CHECK(tokens[i].parameterOrdinal == 0);
            ++parameters;
        }
    }
    CHECK(parameters == 1);

    const std::string path = pathFor("prepare_");
    std::unique_ptr<Database> db = newDatabase(path);
    CHECK(db != nullptr);
    if (db) {
        SqlEngine engine(*db);
        SqlPreparedStatement prepared;
        error = engine.prepare("; SELECT 1;", prepared);
        CHECK(error.code == SqlErrorCode::SyntaxError);
        error = engine.prepare("SELECT * FROM A; SELECT * FROM A;", prepared);
        CHECK(error.code == SqlErrorCode::Unsupported);
        error = engine.prepare("SELECT * FROM ?;", prepared);
        CHECK(!error.isOk());
        error = engine.prepare("CREATE TABLE DdlParam (Id INT64 DEFAULT ?);", prepared);
        CHECK(error.code == SqlErrorCode::ParameterError);
        std::string tooMany = "SELECT * FROM A WHERE ";
        for (size_t i = 0; i < kSqlMaxParameters + 1; ++i) {
            if (i) tooMany += " AND ";
            tooMany += "A = ?";
        }
        error = engine.prepare(tooMany, prepared);
        CHECK(error.code == SqlErrorCode::ResourceLimit);
        db->close();
    }
    removeDatabase(path);
}

void typedBindingsAndReuse() {
    std::printf("SQL9 typed bindings, ownership, injection and reuse\n");
    const std::string path = pathFor("bind_");
    std::unique_ptr<Database> db = newDatabase(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(run(engine,
                  "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT NULL, "
                  "Email TEXT UNIQUE, Enabled BOOLEAN NOT NULL DEFAULT TRUE, "
                  "Note TEXT NULL);"));
        CHECK(run(engine, "CREATE TABLE BinaryData (Id INT64 PRIMARY KEY, Payload BLOB NULL);"));
        CHECK(run(engine, "CREATE TABLE Metrics (Id INT64 PRIMARY KEY, Value FLOAT64);"));

        SqlPreparedStatement insert;
        SqlError error = engine.prepare(
            "INSERT INTO Users (Id, Name, Enabled, Note) VALUES (?, ?, ?, ?);",
            insert);
        CHECK(error.isOk());
        CHECK(insert.parameterCount() == 4);
        CHECK(insert.parameters()[0].type == DbType::Int64);
        CHECK(insert.parameters()[1].type == DbType::Text);
        CHECK(insert.parameters()[2].type == DbType::Boolean);
        CHECK(insert.parameters()[3].type == DbType::Text);
        CHECK(!insert.parameters()[0].nullable && insert.parameters()[3].nullable);

        SqlStatementResult result = insert.execute();
        CHECK(!result.ok && result.error.code == SqlErrorCode::ParameterNotBound);
        CHECK(insert.bindText(0, "1").code == SqlErrorCode::ParameterError);
        CHECK(insert.bindInt64(0, 1).isOk());
        CHECK(insert.bindText(1, "Alice").isOk());
        CHECK(insert.bindBoolean(2, true).isOk());
        CHECK(insert.bindNull(3).isOk());
        result = insert.execute();
        CHECK(result.ok && result.affectedRows == 1);

        SqlPreparedStatement binaryInsert;
        CHECK(engine.prepare("INSERT INTO BinaryData VALUES (?, ?);", binaryInsert).isOk());
        CHECK(binaryInsert.parameters()[1].type == DbType::Blob);
        std::vector<uint8_t> binary;
        binary.push_back(0x00);
        binary.push_back(0x3f);
        binary.push_back(0xff);
        CHECK(binaryInsert.bindInt64(0, 1).isOk());
        CHECK(binaryInsert.bindBlob(1, binary.data(), binary.size()).isOk());
        binary[0] = 0x7e; // Binding owns a copy, not this caller buffer.
        CHECK(binaryInsert.execute().ok);
        CHECK(binaryInsert.bindInt64(0, 2).isOk());
        CHECK(binaryInsert.bindBlob(1, nullptr, 0).isOk());
        CHECK(binaryInsert.execute().ok);
        SqlPreparedStatement binaryQuery;
        CHECK(engine.prepare("SELECT Payload FROM BinaryData WHERE Id = ?;",
                             binaryQuery).isOk());
        CHECK(binaryQuery.bindInt64(0, 1).isOk());
        SqlStatementResult binaryResult = binaryQuery.executeQuery();
        CHECK(binaryResult.ok && binaryResult.resultSet.rowCount() == 1);
        if (binaryResult.ok && binaryResult.resultSet.rowCount() == 1) {
            const std::vector<uint8_t>& stored =
                binaryResult.resultSet.value(0, 0).blobValue();
            CHECK(stored.size() == 3 && stored[0] == 0x00 && stored[1] == 0x3f &&
                  stored[2] == 0xff);
        }
        CHECK(binaryQuery.bindInt64(0, 2).isOk());
        binaryResult = binaryQuery.executeQuery();
        CHECK(binaryResult.ok && binaryResult.resultSet.value(0, 0).blobValue().empty());

        const std::string injection = "'; DROP TABLE Users; --";
        CHECK(insert.bindInt64(0, 2).isOk());
        CHECK(insert.bindText(1, injection).isOk());
        CHECK(insert.bindBoolean(2, false).isOk());
        CHECK(insert.bindText(3, std::string("x\0y", 3)).isOk());
        result = insert.execute();
        CHECK(result.ok && result.affectedRows == 1);

        CHECK(insert.bindInt64(0, 3).isOk());
        CHECK(insert.bindNull(1).code == SqlErrorCode::ParameterError);
        CHECK(insert.bindText(1, "Carol").isOk());
        CHECK(insert.bindBoolean(2, true).isOk());
        CHECK(insert.bindText(3, "third").isOk());
        result = insert.execute();
        CHECK(result.ok && result.affectedRows == 1);

        CHECK(run(engine, "CREATE INDEX IX_Users_Name ON Users (Name);"));
        CHECK(run(engine, "CREATE INDEX IX_Metrics_Value ON Metrics (Value);"));
        CHECK(run(engine, "INSERT INTO Metrics VALUES (1, 1.5);"));
        CHECK(run(engine, "INSERT INTO Metrics VALUES (2, 2.5);"));

        SqlPreparedStatement textQuery;
        CHECK(engine.prepare("SELECT Id FROM Users WHERE Name = ?;", textQuery).isOk());
        CHECK(textQuery.parameters()[0].type == DbType::Text);
        CHECK(textQuery.bindText(0, "Alice").isOk());
        SqlStatementResult typedResult = textQuery.executeQuery();
        CHECK(typedResult.ok && typedResult.accessPath == "IndexLookup");
        CHECK(typedResult.resultSet.rowCount() == 1);
        CHECK(textQuery.bindInt64(1, 1).code == SqlErrorCode::ParameterError);

        SqlPreparedStatement indexedInsert;
        CHECK(engine.prepare("INSERT INTO Users (Id, Name) VALUES (?, ?);",
                             indexedInsert).isOk());
        CHECK(indexedInsert.bindInt64(0, 4).isOk());
        CHECK(indexedInsert.bindText(1, std::string(2000, 'k')).isOk());
        CHECK(!indexedInsert.execute().ok); // SQL6 indexed-key bound remains active.
        CHECK(indexedInsert.bindText(1, "Dave").isOk());
        CHECK(indexedInsert.execute().ok);

        SqlPreparedStatement indexedUpdate;
        CHECK(engine.prepare("UPDATE Users SET Name = ? WHERE Id = ?;",
                             indexedUpdate).isOk());
        const std::string relocatedName(300, 'd');
        CHECK(indexedUpdate.bindText(0, relocatedName).isOk());
        CHECK(indexedUpdate.bindInt64(1, 4).isOk());
        SqlStatementResult updateResult = indexedUpdate.execute();
        CHECK(updateResult.ok && updateResult.affectedRows == 1);
        CHECK(textQuery.bindText(0, relocatedName).isOk());
        typedResult = textQuery.executeQuery();
        CHECK(typedResult.ok && typedResult.accessPath == "IndexLookup");
        CHECK(typedResult.resultSet.rowCount() == 1);

        SqlPreparedStatement uniqueUpdate;
        CHECK(engine.prepare("UPDATE Users SET Email = ? WHERE Id = ?;",
                             uniqueUpdate).isOk());
        CHECK(uniqueUpdate.bindText(0, "same@example.test").isOk());
        CHECK(uniqueUpdate.bindInt64(1, 1).isOk());
        CHECK(uniqueUpdate.execute().ok);
        CHECK(uniqueUpdate.bindInt64(1, 2).isOk());
        CHECK(!uniqueUpdate.execute().ok);
        CHECK(uniqueUpdate.bindText(0, "other@example.test").isOk());
        CHECK(uniqueUpdate.execute().ok);

        SqlPreparedStatement booleanQuery;
        CHECK(engine.prepare("SELECT Id FROM Users WHERE Enabled = ?;",
                             booleanQuery).isOk());
        CHECK(booleanQuery.parameters()[0].type == DbType::Boolean);
        CHECK(booleanQuery.bindBoolean(0, false).isOk());
        typedResult = booleanQuery.executeQuery();
        CHECK(typedResult.ok && typedResult.resultSet.rowCount() == 1);

        SqlPreparedStatement floatQuery;
        CHECK(engine.prepare("SELECT Id FROM Metrics WHERE Value >= ?;",
                             floatQuery).isOk());
        CHECK(floatQuery.parameters()[0].type == DbType::Float64);
        CHECK(floatQuery.bindFloat64(0, 2.0).isOk());
        typedResult = floatQuery.executeQuery();
        CHECK(typedResult.ok && typedResult.accessPath == "IndexRange");
        CHECK(typedResult.resultSet.rowCount() == 1);
        CHECK(floatQuery.bindFloat64(0, std::numeric_limits<double>::infinity()).code ==
              SqlErrorCode::ParameterError);

        SqlPreparedStatement query;
        const uint64_t parseCount = engine.sqlParseCount();
        error = engine.prepare("SELECT Name, Note FROM Users WHERE Id = ?;", query);
        CHECK(error.isOk());
        CHECK(query.resultColumns().size() == 2);
        CHECK(query.resultColumns()[0].type == DbType::Text);
        CHECK(query.resultColumns()[1].nullable);
        const uint64_t preparedParseCount = engine.sqlParseCount();
        CHECK(preparedParseCount == parseCount + 1);
        CHECK(query.bindInt64(0, 2).isOk());
        result = query.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 1);
        if (result.ok && result.resultSet.rowCount() == 1) {
            CHECK(result.resultSet.value(0, 0).textValue() == injection);
            CHECK(result.resultSet.value(0, 1).textValue() == std::string("x\0y", 3));
        }
        CHECK(query.bindInt32(0, 1).isOk()); // Safe Int32 -> Int64 promotion.
        result = query.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 1);
        if (result.ok && result.resultSet.rowCount() == 1) {
            CHECK(result.resultSet.value(0, 0).textValue() == "Alice");
        }
        CHECK(engine.sqlParseCount() == preparedParseCount);
        CHECK(engine.preparedExecutionCount() >= 5);

        const uint64_t beforeRepeatedReads = engine.sqlParseCount();
        for (size_t i = 0; i < 5000; ++i) {
            const int64_t id = (i & 1u) == 0 ? 1 : 2;
            CHECK(query.bindInt64(0, id).isOk());
            result = query.executeQuery();
            CHECK(result.ok && result.resultSet.rowCount() == 1);
            if (result.ok && result.resultSet.rowCount() == 1) {
                CHECK(result.resultSet.value(0, 0).textValue() ==
                      (id == 1 ? "Alice" : injection));
            }
        }
        CHECK(engine.sqlParseCount() == beforeRepeatedReads);
        query.clearBindings();
        result = query.executeQuery();
        CHECK(!result.ok && result.error.code == SqlErrorCode::ParameterNotBound);

        SqlPreparedStatement nullableQuery;
        CHECK(engine.prepare("SELECT Id FROM Users WHERE Note = ?;", nullableQuery).isOk());
        CHECK(nullableQuery.bindNull(0).isOk());
        result = nullableQuery.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 0);

        const std::string oversized(kMaxTextBytes + 1, 'z');
        CHECK(insert.bindText(1, oversized).code == SqlErrorCode::ParameterError);
        std::vector<uint8_t> oversizedBlob(kMaxBlobBytes + 1, 0x7f);
        CHECK(insert.bindBlob(3, oversizedBlob.data(), oversizedBlob.size()).code ==
              SqlErrorCode::ParameterError);

        SqlPreparedStatement ambiguous;
        CHECK(engine.prepare("SELECT Id FROM Users WHERE ? = ?;", ambiguous).code ==
              SqlErrorCode::ParameterError);
        CHECK(engine.prepare("SELECT Id FROM Users WHERE Missing = ?;", ambiguous).code ==
              SqlErrorCode::SemanticError);
    }
    CHECK(db->close().isOk());
    std::unique_ptr<Database> reopened;
    CHECK(Database::open(path, DatabaseOpenOptions(), reopened).isOk());
    if (reopened) {
        SqlEngine engine(*reopened);
        SqlPreparedStatement verify;
        CHECK(engine.prepare("SELECT Name FROM Users WHERE Id = ?;", verify).isOk());
        CHECK(verify.bindInt64(0, 2).isOk());
        SqlStatementResult result = verify.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 1);
        if (result.ok && result.resultSet.rowCount() == 1) {
            CHECK(result.resultSet.value(0, 0).textValue() == "'; DROP TABLE Users; --");
        }
        std::string message;
        CHECK(reopened->validateIntegrity(message).isOk());
        CHECK(reopened->close().isOk());
    }
    removeDatabase(path);
}

void preparedQueriesAndDml() {
    std::printf("SQL9 indexed/range/join/aggregate/LIMIT and DML\n");
    const std::string path = pathFor("query_");
    std::unique_ptr<Database> db = newDatabase(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(run(engine,
                  "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT NULL, "
                  "Enabled BOOLEAN NOT NULL DEFAULT TRUE);"));
        CHECK(run(engine,
                  "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 NOT NULL, "
                  "State TEXT NOT NULL, Score INT32 NOT NULL, "
                  "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(run(engine, "CREATE INDEX IX_Orders_Score ON Orders (Score);"));
        CHECK(run(engine, "INSERT INTO Users VALUES (1, 'A', TRUE);"));
        CHECK(run(engine, "INSERT INTO Users VALUES (2, 'B', FALSE);"));
        CHECK(run(engine, "INSERT INTO Orders VALUES (10, 1, 'open', 10);"));
        CHECK(run(engine, "INSERT INTO Orders VALUES (11, 1, 'closed', 20);"));
        CHECK(run(engine, "INSERT INTO Orders VALUES (12, 2, 'open', 30);"));

        SqlPreparedStatement point;
        CHECK(engine.prepare("SELECT Name FROM Users WHERE Id = ?;", point).isOk());
        CHECK(point.bindInt64(0, 1).isOk());
        SqlStatementResult result = point.executeQuery();
        CHECK(result.ok && result.accessPath == "IndexLookup");
        CHECK(point.bindInt64(0, 2).isOk());
        result = point.executeQuery();
        CHECK(result.ok && result.resultSet.value(0, 0).textValue() == "B");

        SqlPreparedStatement range;
        CHECK(engine.prepare("SELECT Id FROM Orders WHERE Score >= ? ORDER BY Id;",
                             range).isOk());
        CHECK(range.parameters()[0].type == DbType::Int32);
        CHECK(range.bindInt32(0, 20).isOk());
        result = range.executeQuery();
        CHECK(result.ok && result.accessPath == "IndexRange");
        CHECK(result.resultSet.rowCount() == 2);
        CHECK(range.bindInt32(0, 31).isOk());
        result = range.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 0);

        SqlPreparedStatement page;
        CHECK(engine.prepare("SELECT Id FROM Users ORDER BY Id LIMIT ? OFFSET ?;",
                             page).isOk());
        CHECK(page.parameters()[0].type == DbType::Int64 &&
              page.parameters()[0].nonNegative);
        CHECK(page.bindInt64(0, 1).isOk());
        CHECK(page.bindInt64(1, 1).isOk());
        result = page.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 1);
        CHECK(result.resultSet.value(0, 0).int64Value() == 2);
        CHECK(page.bindInt32(0, -1).code == SqlErrorCode::ParameterError);
        CHECK(page.bindInt64(0, -1).code == SqlErrorCode::ParameterError);
        result = page.executeQuery(); // Rejected binding preserved the prior good one.
        CHECK(result.ok && result.resultSet.rowCount() == 1);

        SqlPreparedStatement join;
        CHECK(engine.prepare(
                  "SELECT u.Name, o.Id FROM Users AS u JOIN Orders AS o "
                  "ON o.UserId = u.Id AND o.Score >= ? WHERE u.Id = ?;",
                  join).isOk());
        CHECK(join.bindInt32(0, 10).isOk());
        CHECK(join.bindInt64(1, 1).isOk());
        result = join.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 2);
        CHECK(result.joins.size() == 1);
        CHECK(result.joins[0].accessPath == "IndexNestedLoop");

        SqlPreparedStatement grouped;
        CHECK(engine.prepare(
                  "SELECT State, COUNT(*) AS CountRows FROM Orders WHERE UserId = ? "
                  "GROUP BY State ORDER BY State;",
                  grouped).isOk());
        CHECK(grouped.bindInt64(0, 1).isOk());
        result = grouped.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 2);
        CHECK(result.resultSet.columnCount() == 2);
        CHECK(grouped.bindInt64(0, 2).isOk());
        result = grouped.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 1);

        SqlPreparedStatement update;
        CHECK(engine.prepare("UPDATE Users SET Name = ? WHERE Id = ?;", update).isOk());
        CHECK(update.bindText(0, "A variable width name").isOk());
        CHECK(update.bindInt64(1, 1).isOk());
        result = update.execute();
        CHECK(result.ok && result.affectedRows == 1);
        CHECK(update.bindInt64(1, 999).isOk());
        result = update.execute();
        CHECK(result.ok && result.affectedRows == 0);

        SqlPreparedStatement deleteStatement;
        CHECK(engine.prepare("DELETE FROM Orders WHERE Id = ?;", deleteStatement).isOk());
        CHECK(deleteStatement.bindInt64(0, 10).isOk());
        result = deleteStatement.execute();
        CHECK(result.ok && result.affectedRows == 1);
        CHECK(deleteStatement.bindInt64(0, 999).isOk());
        result = deleteStatement.execute();
        CHECK(result.ok && result.affectedRows == 0);

        SqlPreparedStatement childInsert;
        CHECK(engine.prepare("INSERT INTO Orders VALUES (?, ?, ?, ?);", childInsert).isOk());
        CHECK(childInsert.bindInt64(0, 13).isOk());
        CHECK(childInsert.bindInt64(1, 999).isOk());
        CHECK(childInsert.bindText(2, "bad-parent").isOk());
        CHECK(childInsert.bindInt32(3, 40).isOk());
        CHECK(!childInsert.execute().ok); // Existing FK enforcement still applies.
        CHECK(childInsert.bindInt64(1, 1).isOk());
        CHECK(childInsert.bindText(2, "after-fk-failure").isOk());
        CHECK(childInsert.execute().ok); // Same statement is reusable after failure.

        SqlPreparedStatement deleteParent;
        CHECK(engine.prepare("DELETE FROM Users WHERE Id = ?;", deleteParent).isOk());
        CHECK(deleteParent.bindInt64(0, 1).isOk());
        CHECK(!deleteParent.execute().ok); // Prepared parent RESTRICT.

        SqlPreparedStatement transactionInsert;
        CHECK(engine.prepare("INSERT INTO Users (Id, Name) VALUES (?, ?);",
                             transactionInsert).isOk());
        CHECK(run(engine, "BEGIN;"));
        CHECK(transactionInsert.bindInt64(0, 9).isOk());
        CHECK(transactionInsert.bindText(1, "rolled back").isOk());
        CHECK(transactionInsert.execute().ok);
        CHECK(run(engine, "ROLLBACK;"));
        SqlPreparedStatement count;
        CHECK(engine.prepare("SELECT COUNT(*) FROM Users WHERE Id = ?;", count).isOk());
        CHECK(count.bindInt64(0, 9).isOk());
        result = count.executeQuery();
        CHECK(result.ok && result.resultSet.value(0, 0).int64Value() == 0);

        // A failed constraint execution leaves the reusable statement valid.
        CHECK(transactionInsert.bindInt64(0, 1).isOk());
        CHECK(transactionInsert.bindText(1, "duplicate").isOk());
        CHECK(!transactionInsert.execute().ok);
        CHECK(transactionInsert.bindInt64(0, 3).isOk());
        CHECK(transactionInsert.bindText(1, "after failure").isOk());
        CHECK(transactionInsert.execute().ok);

        CHECK(run(engine, "BEGIN;"));
        CHECK(transactionInsert.bindInt64(0, 8).isOk());
        CHECK(transactionInsert.bindText(1, "transaction parent").isOk());
        CHECK(transactionInsert.execute().ok);
        CHECK(childInsert.bindInt64(0, 14).isOk());
        CHECK(childInsert.bindInt64(1, 999).isOk());
        CHECK(childInsert.bindText(2, "failed child").isOk());
        CHECK(childInsert.bindInt32(3, 50).isOk());
        result = childInsert.execute();
        CHECK(!result.ok && result.transactionActiveAfter);
        CHECK(count.bindInt64(0, 8).isOk());
        result = count.executeQuery();
        CHECK(result.ok && result.resultSet.value(0, 0).int64Value() == 1);
        CHECK(childInsert.bindInt64(1, 8).isOk());
        CHECK(childInsert.bindText(2, "valid child").isOk());
        CHECK(childInsert.execute().ok);
        CHECK(run(engine, "COMMIT;"));

        std::string integrityMessage;
        CHECK(db->validateIntegrity(integrityMessage).isOk());
        std::vector<IndexInfo> indexes;
        CHECK(db->listIndexes(indexes).isOk());
        for (size_t i = 0; i < indexes.size(); ++i) {
            IndexValidation validation;
            CHECK(db->validateIndex(indexes[i].indexId, validation).isOk());
            CHECK(validation.ok);
        }
    }
    CHECK(db->close().isOk());
    std::unique_ptr<Database> reopened;
    CHECK(Database::open(path, DatabaseOpenOptions(), reopened).isOk());
    if (reopened) {
        std::string message;
        CHECK(reopened->validateIntegrity(message).isOk());
        SqlEngine engine(*reopened);
        SqlPreparedStatement query;
        CHECK(engine.prepare("SELECT Name FROM Users WHERE Id = ?;", query).isOk());
        CHECK(query.bindInt64(0, 8).isOk());
        SqlStatementResult result = query.executeQuery();
        CHECK(result.ok && result.resultSet.rowCount() == 1);
        CHECK(reopened->close().isOk());
    }
    removeDatabase(path);
}

void stalePlansAndClose() {
    std::printf("SQL9 schema generation and close invalidation\n");
    const std::string path = pathFor("stale_");
    std::unique_ptr<Database> db = newDatabase(path);
    CHECK(db != nullptr);
    if (!db) return;
    SqlEngine engine(*db);
    CHECK(run(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, Name TEXT);"));
    CHECK(run(engine, "INSERT INTO T VALUES (1, 'a');"));
    SqlPreparedStatement query;
    CHECK(engine.prepare("SELECT Name FROM T WHERE Id = ?;", query).isOk());
    CHECK(query.bindInt64(0, 1).isOk());
    CHECK(query.executeQuery().ok);
    CHECK(run(engine, "ALTER TABLE T ADD COLUMN Note TEXT NULL;"));
    SqlStatementResult result = query.executeQuery();
    CHECK(!result.ok && result.error.code == SqlErrorCode::SchemaChanged);

    SqlPreparedStatement afterAlter;
    CHECK(engine.prepare("SELECT Name FROM T WHERE Id = ?;", afterAlter).isOk());
    CHECK(afterAlter.bindInt64(0, 1).isOk());
    CHECK(run(engine, "CREATE INDEX IX_T_Name ON T (Name);"));
    result = afterAlter.executeQuery();
    CHECK(!result.ok && result.error.code == SqlErrorCode::SchemaChanged);

    SqlPreparedStatement afterCreateIndex;
    CHECK(engine.prepare("SELECT Name FROM T WHERE Name = ?;", afterCreateIndex).isOk());
    CHECK(afterCreateIndex.bindText(0, "a").isOk());
    CHECK(run(engine, "DROP INDEX IX_T_Name;"));
    result = afterCreateIndex.executeQuery();
    CHECK(!result.ok && result.error.code == SqlErrorCode::SchemaChanged);

    SqlPreparedStatement transactionLocal;
    CHECK(engine.prepare("SELECT Name FROM T WHERE Id = ?;", transactionLocal).isOk());
    CHECK(transactionLocal.bindInt64(0, 1).isOk());
    CHECK(run(engine, "BEGIN;"));
    CHECK(run(engine, "CREATE INDEX IX_T_Tx ON T (Name);"));
    CHECK(run(engine, "ROLLBACK;"));
    result = transactionLocal.executeQuery();
    CHECK(!result.ok && result.error.code == SqlErrorCode::SchemaChanged);

    SqlPreparedStatement dropped;
    CHECK(engine.prepare("SELECT * FROM T WHERE Id = ?;", dropped).isOk());
    CHECK(dropped.bindInt64(0, 1).isOk());
    CHECK(run(engine, "DROP TABLE T;"));
    result = dropped.executeQuery();
    CHECK(!result.ok && result.error.code == SqlErrorCode::SchemaChanged);
    CHECK(db->close().isOk());
    result = dropped.executeQuery();
    CHECK(!result.ok && result.error.code == SqlErrorCode::DatabaseClosed);
    removeDatabase(path);
}

void preparedDdlPolicy() {
    std::printf("SQL9 parameter-free prepared DDL policy\n");
    const std::string path = pathFor("ddl_");
    std::unique_ptr<Database> db = newDatabase(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        SqlPreparedStatement ddl;
        CHECK(engine.prepare("CREATE TABLE CreatedByPrepared (Id INT64 PRIMARY KEY);",
                             ddl).isOk());
        SqlStatementResult result = ddl.execute();
        CHECK(result.ok && result.type == SqlStatementType::CreateTable);
        result = ddl.execute();
        CHECK(!result.ok && result.error.code == SqlErrorCode::SchemaChanged);
    }
    CHECK(db->close().isOk());
    removeDatabase(path);
}

void statementOutlivesEngineSafely() {
    std::printf("SQL9 statement handle after connection destruction\n");
    const std::string path = pathFor("lifetime_");
    std::unique_ptr<Database> db = newDatabase(path);
    CHECK(db != nullptr);
    if (!db) return;
    SqlPreparedStatement orphan;
    {
        SqlEngine engine(*db);
        CHECK(run(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY);"));
        CHECK(engine.prepare("SELECT Id FROM T WHERE Id = ?;", orphan).isOk());
    }
    SqlStatementResult result = orphan.executeQuery();
    CHECK(!result.ok && result.error.code == SqlErrorCode::DatabaseClosed);
    CHECK(db->close().isOk());
    removeDatabase(path);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("guideXOS SQL9 prepared statement tests\n");
    tokenizerAndPrepareErrors();
    typedBindingsAndReuse();
    preparedQueriesAndDml();
    stalePlansAndClose();
    preparedDdlPolicy();
    statementOutlivesEngineSafely();
    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
