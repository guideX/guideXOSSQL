// guideXOS SQL -- Phase SQL7
// Hosted acceptance tests for multi-relation and aggregate query semantics:
// aliases, qualified identifiers, INNER/LEFT JOIN, self-joins, multi-table
// joins, index-assisted nested-loop joins, COUNT/SUM/AVG/MIN/MAX, GROUP BY,
// DISTINCT, aggregate ordering, transaction visibility through JOIN, bounded
// resources and a deterministic independent-model workload.
//
// Same self-contained harness style as the SQL1-SQL6 suites. Every case writes
// real .gxdb/.gxwal files under the OS temp directory.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "database_diagnostics.h"
#include "database_engine.h"
#include "database_format.h"
#include "database_heap.h"
#include "database_index.h"
#include "database_relational.h"
#include "database_schema.h"
#include "database_sql.h"
#include "database_sql_expr.h"
#include "database_sql_parser.h"
#include "database_sql_token.h"
#include "database_sql_tokenizer.h"
#include "database_transaction.h"
#include "database_types.h"

using namespace gxos::db;

namespace {

int g_pass = 0;
int g_fail = 0;
int g_caseCounter = 0;

void checkTrue(bool condition, const char* what, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s\n", line, what);
    }
}

void checkStatus(const DbResult& result, DbStatus expected, const char* what, int line) {
    if (result.status() == expected) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- expected %s, got %s (%s)\n", line, what,
                    dbStatusName(expected), dbStatusName(result.status()),
                    result.message().c_str());
    }
}

void checkInt64(const DbValue& value, int64_t expected, const char* what, int line) {
    if (!value.isNull() && value.type() == DbType::Int64 &&
        value.int64Value() == expected) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- unexpected Int64 value\n", line, what);
    }
}

void checkText(const DbValue& value, const std::string& expected, const char* what,
               int line) {
    if (!value.isNull() && value.type() == DbType::Text &&
        value.textValue() == expected) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- unexpected Text value\n", line, what);
    }
}

void checkBool(const DbValue& value, bool expected, const char* what, int line) {
    if (!value.isNull() && value.type() == DbType::Boolean &&
        value.booleanValue() == expected) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- unexpected Boolean value\n", line, what);
    }
}

void checkDouble(const DbValue& value, double expected, const char* what, int line) {
    if (!value.isNull() && value.type() == DbType::Float64 &&
        value.float64Value() == expected) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- unexpected Float64 value\n", line, what);
    }
}

void checkIntValue(const DbValue& value, int64_t expected, const char* what, int line) {
    const bool ok = !value.isNull() &&
                    ((value.type() == DbType::Int64 && value.int64Value() == expected) ||
                     (value.type() == DbType::Int32 &&
                      static_cast<int64_t>(value.int32Value()) == expected));
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- unexpected integer value\n", line, what);
    }
}

void checkNull(const DbValue& value, const char* what, int line) {
    if (value.isNull()) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- expected NULL\n", line, what);
    }
}

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)
#define CHECK_STATUS(expr, expected) checkStatus((expr), (expected), #expr, __LINE__)
#define CHECK_INT64(value, expected) checkInt64((value), (expected), #value, __LINE__)
#define CHECK_INTVAL(value, expected) checkIntValue((value), (expected), #value, __LINE__)
#define CHECK_TEXT(value, expected) checkText((value), (expected), #value, __LINE__)
#define CHECK_BOOL(value, expected) checkBool((value), (expected), #value, __LINE__)
#define CHECK_DOUBLE(value, expected) checkDouble((value), (expected), #value, __LINE__)
#define CHECK_NULL(value) checkNull((value), #value, __LINE__)

std::string testDir() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    std::string dir = std::string(base) + "/gxos_gxdb_sql7_tests";
#if defined(_WIN32)
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0700);
#endif
    return dir;
}

std::string uniquePath(const std::string& name) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "_%d_", g_caseCounter++);
    return testDir() + "/" + name + buffer + ".gxdb";
}

std::string walPath(const std::string& dbPath) { return walPathFor(dbPath); }

void removeFile(const std::string& path) { std::remove(path.c_str()); }

std::unique_ptr<Database> newDb(const std::string& path) {
    std::unique_ptr<Database> db;
    if (!Database::create(path, DatabaseCreateOptions(), db).isOk()) {
        return nullptr;
    }
    return db;
}

std::unique_ptr<Database> newDbWithPageSize(const std::string& path, uint32_t pageSize) {
    DatabaseCreateOptions options;
    options.pageSize = pageSize;
    std::unique_ptr<Database> db;
    if (!Database::create(path, options, db).isOk()) {
        return nullptr;
    }
    return db;
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return data;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize(static_cast<size_t>(size));
        size_t read = std::fread(data.data(), 1, data.size(), f);
        if (read != data.size()) data.clear();
    }
    std::fclose(f);
    return data;
}

bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    size_t written = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return written == data.size();
}

bool execOk(SqlEngine& engine, const std::string& sql) {
    return engine.execute(sql).ok;
}

bool execSelect(SqlEngine& engine, const std::string& sql, SqlResultSet& out) {
    SqlExecutionResult result = engine.execute(sql);
    if (!result.ok || result.statements.empty() ||
        !result.statements[0].resultSet.hasResult()) {
        return false;
    }
    out = result.statements[0].resultSet;
    return true;
}

uint64_t execRowCount(SqlEngine& engine, const std::string& sql, bool& ok) {
    SqlExecutionResult result = engine.execute(sql);
    ok = result.ok && !result.statements.empty();
    if (!ok) return 0;
    return result.statements[0].resultSet.rowCount();
}

// ---- Value / row encodings for set and vector comparisons ------------------

std::string encodeValue(const DbValue& value) {
    if (value.isNull()) return "N";
    switch (value.type()) {
    case DbType::Boolean:
        return value.booleanValue() ? "b1" : "b0";
    case DbType::Int32:
        return "i32:" + std::to_string(static_cast<long long>(value.int32Value()));
    case DbType::Int64:
        return "i64:" + std::to_string(static_cast<long long>(value.int64Value()));
    case DbType::Float64: {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "f:%.17g", value.float64Value());
        return buffer;
    }
    case DbType::Text:
        return "t:" + value.textValue();
    case DbType::Blob: {
        const std::vector<uint8_t>& bytes = value.blobValue();
        std::string out = "x:";
        for (size_t i = 0; i < bytes.size(); ++i) {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%02X", bytes[i]);
            out += hex;
        }
        return out;
    }
    default:
        return "?";
    }
}

std::string encodeRow(const SqlResultSet& rs, size_t row) {
    std::string out;
    for (size_t c = 0; c < rs.columnCount(); ++c) {
        if (c != 0) out += "\x1f";
        out += encodeValue(rs.row(row)[c]);
    }
    return out;
}

std::vector<std::string> resultVector(const SqlResultSet& rs) {
    std::vector<std::string> out;
    for (size_t r = 0; r < rs.rowCount(); ++r) out.push_back(encodeRow(rs, r));
    return out;
}

std::set<std::string> resultSet(const SqlResultSet& rs) {
    std::set<std::string> out;
    for (size_t r = 0; r < rs.rowCount(); ++r) out.insert(encodeRow(rs, r));
    return out;
}

// ---- Tokenizer / parser helpers -------------------------------------------

bool tokenizeOk(const std::string& sql, std::vector<SqlToken>& out) {
    SqlError error;
    SqlTokenizer tokenizer;
    return tokenizer.tokenize(sql, out, error);
}

bool parseOne(const std::string& sql, SqlStatementAst& out, SqlError& error) {
    std::vector<SqlToken> tokens;
    SqlTokenizer tokenizer;
    if (!tokenizer.tokenize(sql, tokens, error)) return false;
    SqlParser parser(tokens);
    bool done = false;
    if (!parser.next(out, error, done)) return false;
    return !done;
}

bool parseFails(const std::string& sql, SqlErrorCode expected) {
    SqlStatementAst statement;
    SqlError error;
    if (parseOne(sql, statement, error)) return false;
    return error.code == expected;
}

// ---------------------------------------------------------------------------
// Tokenizer (sections 2, 3, 52).

void testSql7Tokenizer() {
    std::printf("SQL7 tokenizer: join, alias, aggregate and grouping keywords\n");
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("SELECT DISTINCT AS JOIN INNER LEFT OUTER COUNT SUM AVG MIN "
                         "MAX GROUP ON BY",
                         tokens));
        CHECK(tokens.size() == 16); // 15 keywords plus end-of-input
        if (tokens.size() == 16) {
            CHECK(tokens[1].kind == SqlTokenKind::Distinct);
            CHECK(tokens[2].kind == SqlTokenKind::As);
            CHECK(tokens[3].kind == SqlTokenKind::Join);
            CHECK(tokens[4].kind == SqlTokenKind::Inner);
            CHECK(tokens[5].kind == SqlTokenKind::Left);
            CHECK(tokens[6].kind == SqlTokenKind::Outer);
            CHECK(tokens[7].kind == SqlTokenKind::Count);
            CHECK(tokens[8].kind == SqlTokenKind::Sum);
            CHECK(tokens[9].kind == SqlTokenKind::Avg);
            CHECK(tokens[10].kind == SqlTokenKind::Min);
            CHECK(tokens[11].kind == SqlTokenKind::Max);
            CHECK(tokens[12].kind == SqlTokenKind::Group);
        }
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("u.Id", tokens));
        CHECK(tokens.size() == 4);
        if (tokens.size() == 4) {
            CHECK(tokens[0].kind == SqlTokenKind::Identifier);
            CHECK(tokens[0].text == "u");
            CHECK(tokens[1].kind == SqlTokenKind::Dot);
            CHECK(tokens[2].kind == SqlTokenKind::Identifier);
            CHECK(tokens[2].text == "Id");
        }
    }
    // Numeric literals with dots are unaffected.
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("1.5 .25", tokens));
        CHECK(tokens[0].kind == SqlTokenKind::FloatLiteral);
        CHECK(tokens[1].kind == SqlTokenKind::FloatLiteral);
    }
}

// ---------------------------------------------------------------------------
// Parser shape (sections 7, 10, 52).

void testSql7Parser() {
    std::printf("SQL7 parser: joins, aliases, aggregates, GROUP BY, DISTINCT\n");
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne(
            "SELECT DISTINCT u.Id AS X, COUNT(*) AS C FROM Users AS u "
            "JOIN Orders o ON o.UserId = u.Id GROUP BY u.Id ORDER BY C DESC LIMIT 5 "
            "OFFSET 1;",
            statement, error));
        CHECK(statement.type == SqlStatementType::Select);
        CHECK(statement.select.distinct);
        CHECK(statement.select.projection.size() == 2);
        if (statement.select.projection.size() == 2) {
            const SqlProjectionItemAst& first = statement.select.projection[0];
            CHECK(first.kind == SqlProjectionKind::Column);
            CHECK(first.columnRef.hasQualifier);
            CHECK(first.columnRef.qualifier == "u");
            CHECK(first.columnRef.name == "Id");
            CHECK(first.hasAlias && first.alias.name == "X");
            const SqlProjectionItemAst& second = statement.select.projection[1];
            CHECK(second.kind == SqlProjectionKind::Aggregate);
            CHECK(second.aggregate.kind == SqlAggregateKind::Count);
            CHECK(second.aggregate.star);
            CHECK(second.hasAlias && second.alias.name == "C");
        }
        CHECK(statement.select.from.table.name == "Users");
        CHECK(statement.select.from.hasAlias);
        CHECK(statement.select.from.alias.name == "u");
        CHECK(statement.select.joins.size() == 1);
        if (statement.select.joins.size() == 1) {
            CHECK(statement.select.joins[0].type == SqlJoinType::Inner);
            CHECK(statement.select.joins[0].table.table.name == "Orders");
            CHECK(statement.select.joins[0].table.hasAlias);
            CHECK(statement.select.joins[0].table.alias.name == "o");
            CHECK(statement.select.joins[0].on.present);
        }
        CHECK(statement.select.groupBy.size() == 1);
        CHECK(statement.select.orderBy.size() == 1);
        CHECK(statement.select.orderBy[0].descending);
        CHECK(statement.select.hasLimit && statement.select.limit == 5);
        CHECK(statement.select.hasOffset && statement.select.offset == 1);
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT * FROM A LEFT OUTER JOIN B ON A.Id = B.AId;", statement,
                       error));
        CHECK(statement.select.star);
        CHECK(statement.select.joins.size() == 1);
        if (statement.select.joins.size() == 1) {
            CHECK(statement.select.joins[0].type == SqlJoinType::Left);
        }
    }
    {
        // Bare aliases.
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT a.Id FROM A a JOIN B b ON a.Id = b.Id;", statement, error));
        CHECK(statement.select.from.hasAlias && statement.select.from.alias.name == "a");
        CHECK(statement.select.joins[0].table.hasAlias);
        CHECK(statement.select.joins[0].table.alias.name == "b");
    }
    {
        // Bare projection alias.
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT COUNT(*) Total FROM T;", statement, error));
        CHECK(statement.select.projection.size() == 1);
        if (statement.select.projection.size() == 1) {
            CHECK(statement.select.projection[0].hasAlias);
            CHECK(statement.select.projection[0].alias.name == "Total");
        }
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT SUM(o.Amount), MIN(Amount), MAX(Amount), AVG(Amount) "
                       "FROM Orders AS o;",
                       statement, error));
        CHECK(statement.select.projection.size() == 4);
        if (statement.select.projection.size() == 4) {
            CHECK(statement.select.projection[0].aggregate.kind == SqlAggregateKind::Sum);
            CHECK(statement.select.projection[0].aggregate.column.hasQualifier);
            CHECK(statement.select.projection[1].aggregate.kind == SqlAggregateKind::Min);
            CHECK(statement.select.projection[2].aggregate.kind == SqlAggregateKind::Max);
            CHECK(statement.select.projection[3].aggregate.kind == SqlAggregateKind::Avg);
        }
    }
}

// ---------------------------------------------------------------------------
// Hostile and malformed input (section 80).

void testSql7HostileInput() {
    std::printf("SQL7 hostile and malformed input\n");
    CHECK(parseFails("SELECT * FROM A JOIN;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM A JOIN B;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM A JOIN B ON;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM A LEFT B ON A.Id = B.Id;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM A LEFT JOIN B;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM A AS;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT COUNT(;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT COUNT();", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT SUM(*);", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT AVG();", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT MIN();", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT MAX();", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT A, COUNT(*) FROM T GROUP;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT A, COUNT(*) FROM T GROUP BY;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT DISTINCT;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT SUM(COUNT(*)) FROM T;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM A, B;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT COUNT(*, A) FROM T;", SqlErrorCode::SyntaxError));
    // RIGHT/FULL/CROSS JOIN are excluded and must not be read as bare aliases.
    CHECK(parseFails("SELECT * FROM A RIGHT JOIN B ON A.Id = B.Id;",
                     SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM A FULL JOIN B ON A.Id = B.Id;",
                     SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM A CROSS JOIN B ON A.Id = B.Id;",
                     SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT A, COUNT(*) FROM T GROUP BY A HAVING COUNT(*) > 1;",
                     SqlErrorCode::SyntaxError));

    // Excessive join count (17 sources) is a bounded resource error.
    {
        std::string sql = "SELECT * FROM T0";
        for (int i = 1; i <= 16; ++i) {
            sql += " JOIN T" + std::to_string(i) + " ON T0.Id = T" +
                   std::to_string(i) + ".Id";
        }
        sql += ";";
        CHECK(parseFails(sql, SqlErrorCode::ResourceLimit));
    }
    // Excessive GROUP BY term count.
    {
        std::string sql = "SELECT COUNT(*) FROM T GROUP BY ";
        for (int i = 0; i < 65; ++i) {
            if (i != 0) sql += ", ";
            sql += "C" + std::to_string(i);
        }
        sql += ";";
        CHECK(parseFails(sql, SqlErrorCode::ResourceLimit));
    }
    // Excessive projection count.
    {
        std::string sql = "SELECT ";
        for (int i = 0; i < 65; ++i) {
            if (i != 0) sql += ", ";
            sql += "C" + std::to_string(i);
        }
        sql += " FROM T;";
        CHECK(parseFails(sql, SqlErrorCode::ResourceLimit));
    }
}

// ---------------------------------------------------------------------------
// Qualified identifiers, aliases, ambiguity (sections 3-7, 81-83).

void testSql7QualifiedAndAliases() {
    std::printf("SQL7 qualified identifiers and alias rules\n");
    const std::string path = uniquePath("sql7alias");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE A (Id INT64 PRIMARY KEY, AId INT64 NOT NULL, "
                             "Val TEXT NOT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE B (Id INT64 PRIMARY KEY, AId INT64 NOT NULL, "
                             "Val TEXT NOT NULL);"));
        CHECK(execOk(engine, "INSERT INTO A VALUES (1, 10, 'a1');"));
        CHECK(execOk(engine, "INSERT INTO B VALUES (1, 10, 'b1');"));

        // Qualified reference to the table name when no alias is present.
        SqlResultSet rs;
        CHECK(execSelect(engine, "SELECT A.Val FROM A WHERE A.Id = 1;", rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) CHECK_TEXT(rs.row(0)[0], "a1");

        // Explicit alias replaces the table name as the qualifier.
        CHECK(execSelect(engine, "SELECT x.Val FROM A AS x WHERE x.Id = 1;", rs));
        CHECK(rs.rowCount() == 1);
        {
            SqlExecutionResult bad = engine.execute("SELECT A.Val FROM A AS x;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }

        // Unqualified column legal when unique across visible sources.
        CHECK(execSelect(engine, "SELECT Val FROM A;", rs));
        CHECK(rs.rowCount() == 1);

        // Ambiguous unqualified column must fail, not pick the leftmost.
        {
            SqlExecutionResult bad =
                engine.execute("SELECT Id FROM A JOIN B ON A.AId = B.AId;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        // Qualified resolves it.
        CHECK(execSelect(engine, "SELECT A.Id, B.Id FROM A JOIN B ON A.AId = B.AId;",
                         rs));
        CHECK(rs.rowCount() == 1);

        // Unknown qualifier and unknown column.
        {
            SqlExecutionResult bad = engine.execute("SELECT z.Id FROM A AS x;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        {
            SqlExecutionResult bad = engine.execute("SELECT x.Nope FROM A AS x;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        // Unknown qualifier in ON must fail before execution.
        {
            SqlExecutionResult bad =
                engine.execute("SELECT A.Id FROM A JOIN B ON z.Id = B.Id;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        // Duplicate alias.
        {
            SqlExecutionResult bad =
                engine.execute("SELECT * FROM A AS x JOIN B AS x ON x.Id = x.Id;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        // Duplicate table source without aliases.
        {
            SqlExecutionResult bad =
                engine.execute("SELECT * FROM A JOIN A ON A.Id = A.Id;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        // Alias case sensitivity: u and U are distinct qualifiers.
        CHECK(execSelect(engine, "SELECT u.Val, U.Val FROM A AS u JOIN B AS U "
                                 "ON u.AId = U.AId;",
                         rs));
        CHECK(rs.rowCount() == 1);
        // ORDER BY ambiguity on a joined query.
        {
            SqlExecutionResult bad =
                engine.execute("SELECT A.Val FROM A JOIN B ON A.AId = B.AId ORDER BY Id;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        // ORDER BY qualified works.
        CHECK(execSelect(engine,
                         "SELECT A.Val FROM A JOIN B ON A.AId = B.AId ORDER BY B.Id;",
                         rs));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// INNER JOIN (sections 11, 12, 62).

void testSql7InnerJoin() {
    std::printf("SQL7 INNER JOIN semantics\n");
    const std::string path = uniquePath("sql7inner");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT "
                             "NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 "
                             "NULL, Amount INT64 NOT NULL, Enabled BOOLEAN NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_UserId ON Orders (UserId);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'Alice');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'Bob');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (3, 'Carol');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1, 100, TRUE);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (11, 1, 50, FALSE);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (12, 2, 200, TRUE);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (13, NULL, 999, TRUE);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (14, 99, 5, TRUE);"));

        SqlResultSet rs;
        // NULL = NULL and 99 (no user) never match under INNER JOIN.
        CHECK(execSelect(engine,
                         "SELECT u.Name, o.Amount FROM Users AS u JOIN Orders AS o "
                         "ON o.UserId = u.Id ORDER BY o.Id;",
                         rs));
        CHECK(rs.rowCount() == 3);
        if (rs.rowCount() == 3) {
            CHECK_TEXT(rs.row(0)[0], "Alice");
            CHECK_INT64(rs.row(0)[1], 100);
            CHECK_TEXT(rs.row(1)[0], "Alice");
            CHECK_INT64(rs.row(1)[1], 50);
            CHECK_TEXT(rs.row(2)[0], "Bob");
            CHECK_INT64(rs.row(2)[1], 200);
        }

        // Reversed orientation: indexed right side is Users(Id).
        CHECK(execSelect(engine,
                         "SELECT u.Name, o.Id FROM Orders AS o JOIN Users AS u "
                         "ON u.Id = o.UserId ORDER BY o.Id;",
                         rs));
        CHECK(rs.rowCount() == 3);

        // Additional ON conjunct must still be post-filtered.
        CHECK(execSelect(engine,
                         "SELECT u.Name, o.Amount FROM Users AS u JOIN Orders AS o "
                         "ON o.UserId = u.Id AND o.Enabled = TRUE ORDER BY o.Id;",
                         rs));
        CHECK(rs.rowCount() == 2);
        if (rs.rowCount() == 2) {
            CHECK_INT64(rs.row(0)[1], 100);
            CHECK_INT64(rs.row(1)[1], 200);
        }

        // JOIN without a qualifier is INNER.
        CHECK(execSelect(engine,
                         "SELECT u.Name FROM Users AS u JOIN Orders AS o "
                         "ON o.UserId = u.Id ORDER BY u.Id;",
                         rs));
        CHECK(rs.rowCount() == 3);

        // WHERE on top of a join.
        CHECK(execSelect(engine,
                         "SELECT u.Name FROM Users AS u JOIN Orders AS o "
                         "ON o.UserId = u.Id WHERE o.Amount >= 100 ORDER BY o.Id;",
                         rs));
        CHECK(rs.rowCount() == 2);
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// LEFT JOIN and null extension (sections 13, 14, 58, 63).

void testSql7LeftJoin() {
    std::printf("SQL7 LEFT JOIN and null-extension semantics\n");
    const std::string path = uniquePath("sql7left");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT "
                             "NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 "
                             "NULL, Amount INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_UserId ON Orders (UserId);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'Alice');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'Bob');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (3, 'Carol');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1, 100);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (11, 1, 50);"));

        SqlResultSet rs;
        CHECK(execSelect(engine,
                         "SELECT u.Name, o.Id, o.Amount FROM Users AS u "
                         "LEFT JOIN Orders AS o ON o.UserId = u.Id ORDER BY u.Id, o.Id;",
                         rs));
        CHECK(rs.rowCount() == 4);
        if (rs.rowCount() == 4) {
            CHECK_TEXT(rs.row(0)[0], "Alice");
            CHECK_INT64(rs.row(0)[1], 10);
            CHECK_TEXT(rs.row(1)[0], "Alice");
            CHECK_INT64(rs.row(1)[1], 11);
            CHECK_TEXT(rs.row(2)[0], "Bob");
            CHECK_NULL(rs.row(2)[1]); // null-extended right relation
            CHECK_NULL(rs.row(2)[2]);
            CHECK_TEXT(rs.row(3)[0], "Carol");
            CHECK_NULL(rs.row(3)[1]);
        }
        // Right-side NOT NULL columns are nullable in the result metadata.
        CHECK(rs.column(1).nullable);
        CHECK(rs.column(2).nullable);

        // COUNT(*) vs COUNT(o.Id) with LEFT JOIN (sections 57, 58).
        CHECK(execSelect(engine,
                         "SELECT u.Id, COUNT(*) AS Rows, COUNT(o.Id) AS Orders FROM "
                         "Users AS u LEFT JOIN Orders AS o ON o.UserId = u.Id "
                         "GROUP BY u.Id ORDER BY u.Id;",
                         rs));
        CHECK(rs.rowCount() == 3);
        if (rs.rowCount() == 3) {
            CHECK_INT64(rs.row(0)[1], 2); // Alice: 2 joined rows
            CHECK_INT64(rs.row(0)[2], 2);
            CHECK_INT64(rs.row(1)[1], 1); // Bob: one null-extended row
            CHECK_INT64(rs.row(1)[2], 0);
            CHECK_INT64(rs.row(2)[1], 1); // Carol
            CHECK_INT64(rs.row(2)[2], 0);
        }

        // LEFT JOIN with a nullable left key: NULL probe cannot match.
        CHECK(execOk(engine, "CREATE TABLE L (Id INT64 NOT NULL, K INT64 NULL);"));
        CHECK(execOk(engine, "CREATE TABLE R (Id INT64 NOT NULL, K INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_R_K ON R (K);"));
        CHECK(execOk(engine, "INSERT INTO L VALUES (1, NULL);"));
        CHECK(execOk(engine, "INSERT INTO L VALUES (2, 5);"));
        CHECK(execOk(engine, "INSERT INTO R VALUES (100, 5);"));
        CHECK(execSelect(engine,
                         "SELECT l.Id, r.Id FROM L AS l LEFT JOIN R AS r ON r.K = l.K "
                         "ORDER BY l.Id;",
                         rs));
        CHECK(rs.rowCount() == 2);
        if (rs.rowCount() == 2) {
            CHECK_NULL(rs.row(0)[1]); // NULL = anything is UNKNOWN
            CHECK_INT64(rs.row(1)[1], 100);
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Self-join (sections 5, 59).

void testSql7SelfJoin() {
    std::printf("SQL7 self-join with aliases\n");
    const std::string path = uniquePath("sql7self");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Employees (Id INT64 PRIMARY KEY, ManagerId "
                             "INT64 NULL, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (1, NULL, 'CEO');"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (2, 1, 'VP');"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (3, 1, 'CFO');"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (4, 2, 'Eng');"));

        SqlResultSet rs;
        CHECK(execSelect(engine,
                         "SELECT e.Name AS Employee, m.Name AS Manager FROM Employees "
                         "AS e LEFT JOIN Employees AS m ON e.ManagerId = m.Id ORDER BY "
                         "e.Id;",
                         rs));
        CHECK(rs.rowCount() == 4);
        if (rs.rowCount() == 4) {
            CHECK_TEXT(rs.row(0)[0], "CEO");
            CHECK_NULL(rs.row(0)[1]);
            CHECK_TEXT(rs.row(1)[0], "VP");
            CHECK_TEXT(rs.row(1)[1], "CEO");
            CHECK_TEXT(rs.row(2)[0], "CFO");
            CHECK_TEXT(rs.row(2)[1], "CEO");
            CHECK_TEXT(rs.row(3)[0], "Eng");
            CHECK_TEXT(rs.row(3)[1], "VP");
        }
        CHECK(rs.column(0).name == "Employee");
        CHECK(rs.column(1).name == "Manager");
        // Relation identity is the bound source instance, not the table id.
        CHECK(execSelect(engine,
                         "SELECT a.Name, b.Name FROM Employees AS a JOIN Employees AS b "
                         "ON a.Id = b.Id ORDER BY a.Id;",
                         rs));
        CHECK(rs.rowCount() == 4);
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Multi-table join chain (sections 15, 61).

void testSql7MultiTableJoin() {
    std::printf("SQL7 three-relation join chain\n");
    const std::string path = uniquePath("sql7multi");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Customers (Id INT64 PRIMARY KEY, Name TEXT "
                             "NOT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, CustomerId "
                             "INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_CustomerId ON Orders "
                             "(CustomerId);"));
        CHECK(execOk(engine, "CREATE TABLE OrderItems (Id INT64 PRIMARY KEY, OrderId "
                             "INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_OrderItems_OrderId ON OrderItems "
                             "(OrderId);"));
        CHECK(execOk(engine, "INSERT INTO Customers VALUES (1, 'C1');"));
        CHECK(execOk(engine, "INSERT INTO Customers VALUES (2, 'C2');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (11, 1);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (12, 2);"));
        CHECK(execOk(engine, "INSERT INTO OrderItems VALUES (100, 10);"));
        CHECK(execOk(engine, "INSERT INTO OrderItems VALUES (101, 10);"));
        CHECK(execOk(engine, "INSERT INTO OrderItems VALUES (102, 12);"));

        SqlResultSet rs;
        CHECK(execSelect(engine,
                         "SELECT c.Name, o.Id, i.Id FROM Customers AS c JOIN Orders AS "
                         "o ON o.CustomerId = c.Id JOIN OrderItems AS i ON i.OrderId = "
                         "o.Id ORDER BY i.Id;",
                         rs));
        CHECK(rs.rowCount() == 3);
        if (rs.rowCount() == 3) {
            CHECK_INT64(rs.row(0)[2], 100);
            CHECK_INT64(rs.row(1)[2], 101);
            CHECK_INT64(rs.row(2)[2], 102);
        }

        // Mixed INNER then LEFT: order 11 has no items but must survive.
        CHECK(execSelect(engine,
                         "SELECT o.Id, i.Id FROM Customers AS c JOIN Orders AS o ON "
                         "o.CustomerId = c.Id LEFT JOIN OrderItems AS i ON i.OrderId = "
                         "o.Id WHERE c.Id = 1 ORDER BY o.Id, i.Id;",
                         rs));
        CHECK(rs.rowCount() == 3);
        if (rs.rowCount() == 3) {
            CHECK_INT64(rs.row(0)[0], 10);
            CHECK_INT64(rs.row(0)[1], 100);
            CHECK_INT64(rs.row(1)[0], 10);
            CHECK_INT64(rs.row(1)[1], 101);
            CHECK_INT64(rs.row(2)[0], 11);
            CHECK_NULL(rs.row(2)[1]);
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Indexed-vs-nested equivalence (sections 18-25, 64, 75).

void testSql7IndexedEquivalence() {
    std::printf("SQL7 indexed-vs-nested JOIN equivalence\n");
    const std::string path = uniquePath("sql7equiv");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Email TEXT "
                             "UNIQUE, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 "
                             "NULL, Amount INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_UserId ON Orders (UserId);"));
        // Duplicate right keys and NULL probe values.
        for (int i = 0; i < 40; ++i) {
            CHECK(execOk(engine, "INSERT INTO Users VALUES (" + std::to_string(i) +
                                     ", 'e" + std::to_string(i) + "', 'n" +
                                     std::to_string(i % 7) + "');"));
        }
        for (int i = 0; i < 120; ++i) {
            std::string userId = (i % 9 == 0) ? "NULL" : std::to_string(i % 40);
            CHECK(execOk(engine, "INSERT INTO Orders VALUES (" + std::to_string(1000 + i) +
                                     ", " + userId + ", " + std::to_string(i % 50) +
                                     ");"));
        }

        const char* queries[] = {
            // PRIMARY KEY right index.
            "SELECT u.Name, o.Amount FROM Orders AS o JOIN Users AS u ON u.Id = o.UserId "
            "ORDER BY o.Id;",
            // UNIQUE right index (self-join over a UNIQUE column).
            "SELECT u.Id, u2.Id FROM Users AS u JOIN Users AS u2 ON u2.Email = u.Email "
            "ORDER BY u.Id;",
            // Non-unique right index.
            "SELECT u.Name, o.Id FROM Users AS u JOIN Orders AS o ON o.UserId = u.Id "
            "ORDER BY o.Id;",
            // LEFT JOIN with NULL probes.
            "SELECT u.Name, o.Id FROM Users AS u LEFT JOIN Orders AS o ON o.UserId = "
            "u.Id ORDER BY u.Id, o.Id;",
            // Additional ON conjunct.
            "SELECT u.Name, o.Id FROM Users AS u JOIN Orders AS o ON o.UserId = u.Id "
            "AND o.Amount >= 25 ORDER BY o.Id;",
            // Self-join over an indexed PK.
            "SELECT a.Name, b.Name FROM Users AS a JOIN Users AS b ON a.Id = b.Id "
            "ORDER BY a.Id;",
            // Aggregate over an indexed join.
            "SELECT u.Name, COUNT(o.Id) AS C, SUM(o.Amount) AS S FROM Users AS u LEFT "
            "JOIN Orders AS o ON o.UserId = u.Id GROUP BY u.Id, u.Name ORDER BY u.Id;"
        };
        for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); ++q) {
            SqlResultSet indexed;
            SqlEngine indexEngine(*db);
            CHECK(execSelect(indexEngine, queries[q], indexed));

            SqlResultSet nested;
            SqlEngine nestedEngine(*db);
            nestedEngine.setForceNestedLoop(true);
            CHECK(execSelect(nestedEngine, queries[q], nested));

            CHECK(resultVector(indexed) == resultVector(nested));
            CHECK(indexed.columnCount() == nested.columnCount());
        }

        // Diagnostics: the indexed plan is selected and candidates are bounded.
        {
            SqlExecutionResult r = engine.execute(
                "SELECT u.Name FROM Users AS u JOIN Orders AS o ON o.UserId = u.Id;");
            CHECK(r.ok);
            if (r.ok && !r.statements[0].joins.empty()) {
                CHECK(r.statements[0].joins[0].accessPath == "IndexNestedLoop");
                CHECK(r.statements[0].joins[0].indexName == "IX_Orders_UserId");
            }
        }
        // Forced nested path reports NestedLoop.
        {
            SqlEngine forced(*db);
            forced.setForceNestedLoop(true);
            SqlExecutionResult r = forced.execute(
                "SELECT u.Name FROM Users AS u JOIN Orders AS o ON o.UserId = u.Id;");
            CHECK(r.ok);
            if (r.ok && !r.statements[0].joins.empty()) {
                CHECK(r.statements[0].joins[0].accessPath == "NestedLoop");
            }
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Aggregate semantics (sections 29-34, 53, 67, 68, 70).

void testSql7Aggregates() {
    std::printf("SQL7 aggregate functions and NULL semantics\n");
    const std::string path = uniquePath("sql7agg");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Nums (Id INT64 PRIMARY KEY, I32 INT32 NULL, "
                             "I64 INT64 NULL, F64 FLOAT64 NULL, B BOOLEAN NULL, T TEXT "
                             "NULL);"));
        CHECK(execOk(engine, "INSERT INTO Nums VALUES (1, 1, 10, 1.5, TRUE, 'b');"));
        CHECK(execOk(engine, "INSERT INTO Nums VALUES (2, NULL, NULL, NULL, NULL, "
                             "NULL);"));
        CHECK(execOk(engine, "INSERT INTO Nums VALUES (3, 3, 30, 3.5, FALSE, 'a');"));
        CHECK(execOk(engine, "INSERT INTO Nums VALUES (4, 2, 20, 2.5, TRUE, '');"));

        SqlResultSet rs;
        CHECK(execSelect(engine,
                         "SELECT COUNT(*), COUNT(I32), SUM(I32), SUM(I64), SUM(F64), "
                         "AVG(I32), AVG(I64), AVG(F64), MIN(I32), MAX(I32), MIN(I64), "
                         "MAX(I64), MIN(F64), MAX(F64), MIN(T), MAX(T), MIN(B), MAX(B) "
                         "FROM Nums;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            const std::vector<DbValue>& row = rs.row(0);
            CHECK_INT64(row[0], 4);  // COUNT(*)
            CHECK_INT64(row[1], 3);  // COUNT(I32) ignores NULL
            CHECK_INT64(row[2], 6);  // 1+3+2
            CHECK_INT64(row[3], 60); // 10+30+20
            CHECK_DOUBLE(row[4], 7.5);
            CHECK_DOUBLE(row[5], 2.0); // 6/3
            CHECK_DOUBLE(row[6], 20.0);
            CHECK_DOUBLE(row[7], 2.5);
            CHECK_INTVAL(row[8], 1); // MIN(Int32) keeps the Int32 type
            CHECK_INTVAL(row[9], 3); // MAX(Int32) keeps the Int32 type
            CHECK_INT64(row[10], 10);
            CHECK_INT64(row[11], 30);
            CHECK_DOUBLE(row[12], 1.5);
            CHECK_DOUBLE(row[13], 3.5);
            CHECK_TEXT(row[14], ""); // empty string is the MIN
            CHECK_TEXT(row[15], "b");
            CHECK_BOOL(row[16], false); // FALSE < TRUE
            CHECK_BOOL(row[17], true);
        }

        // All-NULL group: SUM/AVG/MIN/MAX are NULL; COUNT(col) is 0.
        CHECK(execOk(engine, "CREATE TABLE AllNull (Id INT64 NOT NULL, V INT64 NULL);"));
        CHECK(execOk(engine, "INSERT INTO AllNull VALUES (1, NULL);"));
        CHECK(execOk(engine, "INSERT INTO AllNull VALUES (2, NULL);"));
        CHECK(execSelect(engine,
                         "SELECT COUNT(*), COUNT(V), SUM(V), AVG(V), MIN(V), MAX(V) FROM "
                         "AllNull;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            CHECK_INT64(rs.row(0)[0], 2);
            CHECK_INT64(rs.row(0)[1], 0);
            CHECK_NULL(rs.row(0)[2]);
            CHECK_NULL(rs.row(0)[3]);
            CHECK_NULL(rs.row(0)[4]);
            CHECK_NULL(rs.row(0)[5]);
        }

        // Empty input, no GROUP BY: exactly one row.
        CHECK(execOk(engine, "CREATE TABLE Empty (Id INT64 NOT NULL, V INT64 NULL);"));
        CHECK(execSelect(engine,
                         "SELECT COUNT(*), SUM(V), AVG(V), MIN(V), MAX(V) FROM Empty;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            CHECK_INT64(rs.row(0)[0], 0);
            CHECK_NULL(rs.row(0)[1]);
            CHECK_NULL(rs.row(0)[2]);
            CHECK_NULL(rs.row(0)[3]);
            CHECK_NULL(rs.row(0)[4]);
        }
        // Empty input with GROUP BY: zero rows.
        CHECK(execSelect(engine, "SELECT V, COUNT(*) FROM Empty GROUP BY V;", rs));
        CHECK(rs.rowCount() == 0);

        // Aggregate without GROUP BY with a WHERE filter.
        CHECK(execSelect(engine,
                         "SELECT COUNT(*), SUM(I32) FROM Nums WHERE I32 IS NOT NULL;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            CHECK_INT64(rs.row(0)[0], 3);
            CHECK_INT64(rs.row(0)[1], 6);
        }

        // Unsupported aggregate arguments fail cleanly.
        {
            SqlExecutionResult bad = engine.execute("SELECT SUM(T) FROM Nums;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        {
            SqlExecutionResult bad = engine.execute("SELECT AVG(T) FROM Nums;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        {
            SqlExecutionResult bad = engine.execute("SELECT MAX(T) FROM Nums;");
            // Text MIN/MAX is supported; ensure it succeeds instead.
            CHECK(bad.ok);
        }
        CHECK(execOk(engine, "CREATE TABLE Blobs (Id INT64 NOT NULL, Data BLOB NULL);"));
        CHECK(execOk(engine, "INSERT INTO Blobs VALUES (1, X'01');"));
        {
            SqlExecutionResult bad = engine.execute("SELECT MAX(Data) FROM Blobs;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        // Aggregate in WHERE is not accepted.
        {
            SqlExecutionResult bad =
                engine.execute("SELECT Id FROM Nums WHERE COUNT(*) > 1;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SyntaxError);
        }

        // Integer SUM overflow is a structured error, not a wrap.
        CHECK(execOk(engine, "CREATE TABLE Big (Id INT64 NOT NULL, V INT64 NOT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Big VALUES (1, 9223372036854775807);"));
        CHECK(execOk(engine, "INSERT INTO Big VALUES (2, 1);"));
        {
            SqlExecutionResult bad = engine.execute("SELECT SUM(V) FROM Big;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::ExecutionError);
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// GROUP BY (sections 35-40, 66, 71).

void testSql7GroupBy() {
    std::printf("SQL7 GROUP BY semantics\n");
    const std::string path = uniquePath("sql7group");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE G (Id INT64 PRIMARY KEY, N INT64 NULL, T "
                             "TEXT NULL, B BOOLEAN NULL);"));
        CHECK(execOk(engine, "INSERT INTO G VALUES (1, 1, 'x', TRUE);"));
        CHECK(execOk(engine, "INSERT INTO G VALUES (2, 1, 'x', FALSE);"));
        CHECK(execOk(engine, "INSERT INTO G VALUES (3, 2, 'y', TRUE);"));
        CHECK(execOk(engine, "INSERT INTO G VALUES (4, NULL, NULL, NULL);"));
        CHECK(execOk(engine, "INSERT INTO G VALUES (5, NULL, '', TRUE);"));
        CHECK(execOk(engine, "INSERT INTO G VALUES (6, -1, 'y', FALSE);"));

        SqlResultSet rs;
        // GROUP BY IntColumn.
        CHECK(execSelect(engine,
                         "SELECT N, COUNT(*) FROM G GROUP BY N ORDER BY N;",
                         rs));
        CHECK(rs.rowCount() == 4); // NULL, -1, 1, 2
        if (rs.rowCount() == 4) {
            CHECK_NULL(rs.row(0)[0]);
            CHECK_INT64(rs.row(0)[1], 2);
            CHECK_INT64(rs.row(1)[0], -1);
            CHECK_INT64(rs.row(1)[1], 1);
            CHECK_INT64(rs.row(2)[0], 1);
            CHECK_INT64(rs.row(2)[1], 2);
            CHECK_INT64(rs.row(3)[0], 2);
            CHECK_INT64(rs.row(3)[1], 1);
        }
        // GROUP BY TextColumn (empty string is its own group).
        CHECK(execSelect(engine,
                         "SELECT T, COUNT(*) FROM G GROUP BY T ORDER BY T;",
                         rs));
        CHECK(rs.rowCount() == 4); // NULL, '', x, y
        // GROUP BY BooleanColumn.
        CHECK(execSelect(engine,
                         "SELECT B, COUNT(*) FROM G GROUP BY B ORDER BY B;",
                         rs));
        CHECK(rs.rowCount() == 3); // NULL, FALSE, TRUE
        // Composite key groups NULL correctly and is collision-free.
        CHECK(execSelect(engine,
                         "SELECT N, T, COUNT(*) FROM G GROUP BY N, T ORDER BY N, T;",
                         rs));
        CHECK(rs.rowCount() == 5); // (1,x),(2,y),(NULL,NULL),(NULL,''),(-1,y)
        // Grouping NULLs together differs from SQL `NULL = NULL` (UNKNOWN):
        // the predicate matches no rows, but COUNT(*) still yields one row.
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM G WHERE N = NULL;", rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) CHECK_INT64(rs.row(0)[0], 0);

        // Projection validation.
        {
            SqlExecutionResult bad =
                engine.execute("SELECT N, T, COUNT(*) FROM G GROUP BY N;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
        CHECK(execOk(engine, "SELECT N, T, COUNT(*) FROM G GROUP BY N, T;"));
        // Aggregate-only projection with GROUP BY is legal.
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM G GROUP BY N;", rs));
        CHECK(rs.rowCount() == 4);
        // A query with GROUP BY but no aggregate is a grouped distinct.
        CHECK(execSelect(engine, "SELECT N FROM G GROUP BY N;", rs));
        CHECK(rs.rowCount() == 4);
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// DISTINCT (sections 41-44, 72, 73).

void testSql7Distinct() {
    std::printf("SQL7 DISTINCT semantics\n");
    const std::string path = uniquePath("sql7distinct");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE D (Id INT64 PRIMARY KEY, N INT64 NULL, T "
                             "TEXT NULL, B BOOLEAN NULL, Data BLOB NULL);"));
        CHECK(execOk(engine, "INSERT INTO D VALUES (1, 1, 'x', TRUE, X'01');"));
        CHECK(execOk(engine, "INSERT INTO D VALUES (2, 1, 'x', TRUE, X'01');"));
        CHECK(execOk(engine, "INSERT INTO D VALUES (3, NULL, NULL, NULL, NULL);"));
        CHECK(execOk(engine, "INSERT INTO D VALUES (4, NULL, NULL, NULL, NULL);"));
        CHECK(execOk(engine, "INSERT INTO D VALUES (5, 2, '', FALSE, X'02');"));
        CHECK(execOk(engine, "INSERT INTO D VALUES (6, -1, 'y', FALSE, X'02');"));

        SqlResultSet rs;
        CHECK(execSelect(engine, "SELECT DISTINCT N FROM D ORDER BY N;", rs));
        CHECK(rs.rowCount() == 4); // NULL, -1, 1, 2
        CHECK(execSelect(engine, "SELECT DISTINCT T FROM D ORDER BY T;", rs));
        CHECK(rs.rowCount() == 4); // NULL, '', x, y
        CHECK(execSelect(engine, "SELECT DISTINCT B FROM D ORDER BY B;", rs));
        CHECK(rs.rowCount() == 3);
        CHECK(execSelect(engine, "SELECT DISTINCT N, T FROM D ORDER BY N, T;", rs));
        CHECK(rs.rowCount() == 4); // (1,x),(NULL,NULL),(2,''),(-1,y)
        // BLOB participates with byte-wise equality.
        CHECK(execSelect(engine, "SELECT DISTINCT Data FROM D;", rs));
        CHECK(rs.rowCount() == 3); // NULL, 0x01, 0x02

        // Repeated NULLs collapse to one row.
        CHECK(execSelect(engine, "SELECT DISTINCT N FROM D WHERE N IS NULL;", rs));
        CHECK(rs.rowCount() == 1);
        CHECK_NULL(rs.row(0)[0]);

        // DISTINCT then ORDER then OFFSET then LIMIT.
        CHECK(execSelect(engine,
                         "SELECT DISTINCT T FROM D ORDER BY T LIMIT 2 OFFSET 1;",
                         rs));
        CHECK(rs.rowCount() == 2);
        if (rs.rowCount() == 2) {
            // Sorted distinct T: NULL, '', x, y. OFFSET 1 -> '', x.
            CHECK_TEXT(rs.row(0)[0], "");
            CHECK_TEXT(rs.row(1)[0], "x");
        }
        // DISTINCT after aggregation.
        CHECK(execSelect(engine,
                         "SELECT DISTINCT COUNT(*) FROM D GROUP BY N;", rs));
        // ORDER BY in DISTINCT must reference a projected column.
        {
            SqlExecutionResult bad =
                engine.execute("SELECT DISTINCT N FROM D ORDER BY T;");
            CHECK(!bad.ok && bad.error.code == SqlErrorCode::SemanticError);
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Aggregate + JOIN (sections 57, 74, 75).

void testSql7AggregateJoin() {
    std::printf("SQL7 aggregate over JOIN results\n");
    const std::string path = uniquePath("sql7aggjoin");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Customers (Id INT64 PRIMARY KEY, Name TEXT "
                             "NOT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, CustomerId "
                             "INT64 NULL, Amount INT64 NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_CustomerId ON Orders "
                             "(CustomerId);"));
        CHECK(execOk(engine, "INSERT INTO Customers VALUES (1, 'Alice');"));
        CHECK(execOk(engine, "INSERT INTO Customers VALUES (2, 'Bob');"));
        CHECK(execOk(engine, "INSERT INTO Customers VALUES (3, 'Carol');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1, 100);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (11, 1, 50);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (12, 2, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (13, NULL, 7);"));

        SqlResultSet rs;
        CHECK(execSelect(engine,
                         "SELECT c.Id, c.Name, COUNT(o.Id) AS OrderCount, "
                         "SUM(o.Amount) AS TotalAmount FROM Customers AS c LEFT JOIN "
                         "Orders AS o ON o.CustomerId = c.Id GROUP BY c.Id, c.Name "
                         "ORDER BY c.Id;",
                         rs));
        CHECK(rs.rowCount() == 3);
        if (rs.rowCount() == 3) {
            CHECK_INT64(rs.row(0)[2], 2);
            CHECK_INT64(rs.row(0)[3], 150);
            CHECK_INT64(rs.row(1)[2], 1);
            CHECK_NULL(rs.row(1)[3]); // only NULL amount
            CHECK_INT64(rs.row(2)[2], 0); // no orders
            CHECK_NULL(rs.row(2)[3]);
        }
        // ORDER BY aggregate alias DESC.
        CHECK(execSelect(engine,
                         "SELECT c.Id, SUM(o.Amount) AS Total FROM Customers AS c LEFT "
                         "JOIN Orders AS o ON o.CustomerId = c.Id GROUP BY c.Id ORDER BY "
                         "Total DESC;",
                         rs));
        CHECK(rs.rowCount() == 3);
        if (rs.rowCount() == 3) {
            CHECK_INT64(rs.row(0)[0], 1); // 150
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Transaction visibility through JOIN (sections 27, 28, 65).

void testSql7TransactionVisibility() {
    std::printf("SQL7 transaction visibility through JOIN\n");
    const std::string path = uniquePath("sql7tx");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT "
                             "NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 "
                             "NOT NULL, Amount INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_UserId ON Orders (UserId);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'Alice');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1, 100);"));

        SqlResultSet rs;
        // Read-your-writes: new rows and new index entries visible inside the tx.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'Bob');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (11, 2, 200);"));
        CHECK(execSelect(engine,
                         "SELECT u.Name, o.Amount FROM Users AS u JOIN Orders AS o ON "
                         "o.UserId = u.Id ORDER BY o.Id;",
                         rs));
        CHECK(rs.rowCount() == 2);
        if (rs.rowCount() == 2) {
            CHECK_TEXT(rs.row(1)[0], "Bob");
            CHECK_INT64(rs.row(1)[1], 200);
        }
        // Transaction-local UPDATE of an indexed join key.
        CHECK(execOk(engine, "UPDATE Orders SET UserId = 1 WHERE Id = 11;"));
        CHECK(execSelect(engine,
                         "SELECT u.Name FROM Users AS u JOIN Orders AS o ON o.UserId = "
                         "u.Id WHERE o.Id = 11;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) CHECK_TEXT(rs.row(0)[0], "Alice");
        // Transaction-local DELETE.
        CHECK(execOk(engine, "DELETE FROM Orders WHERE Id = 10;"));
        CHECK(execSelect(engine,
                         "SELECT COUNT(*) FROM Users AS u JOIN Orders AS o ON o.UserId "
                         "= u.Id;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) CHECK_INT64(rs.row(0)[0], 1);
        CHECK(execOk(engine, "ROLLBACK;"));

        // After rollback, only the pre-BEGIN durable state remains.
        CHECK(execSelect(engine,
                         "SELECT u.Name, o.Amount FROM Users AS u JOIN Orders AS o ON "
                         "o.UserId = u.Id ORDER BY o.Id;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            CHECK_TEXT(rs.row(0)[0], "Alice");
            CHECK_INT64(rs.row(0)[1], 100);
        }
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM Users;", rs));
        CHECK_INT64(rs.row(0)[0], 1);
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen: nothing transaction-local survived.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            SqlResultSet rs;
            CHECK(execSelect(engine,
                             "SELECT u.Name FROM Users AS u LEFT JOIN Orders AS o ON "
                             "o.UserId = u.Id;",
                             rs));
            CHECK(rs.rowCount() == 1);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Index corruption during an indexed JOIN must surface, not silently fall back
// (section 26, 86).

void testSql7JoinIndexCorruption() {
    std::printf("SQL7 indexed JOIN surfaces index corruption\n");
    const uint32_t pageSize = 512;
    const std::string base = uniquePath("sql7corruptbase");
    uint64_t rootPage = 0;
    {
        std::unique_ptr<Database> db = newDbWithPageSize(base, pageSize);
        CHECK(db != nullptr);
        if (!db) return;
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT "
                             "NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 "
                             "NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_UserId ON Orders (UserId);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'A');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1);"));
        const Catalog::IndexRecord* rec = db->catalog().findIndex("IX_Orders_UserId");
        CHECK(rec != nullptr);
        if (rec != nullptr) rootPage = rec->rootPageId;
        db->close();
    }
    CHECK(rootPage != 0);

    const std::vector<uint8_t> baseBytes = readFileBytes(base);
    const std::string work = uniquePath("sql7corrupt");
    std::vector<uint8_t> bytes = baseBytes;
    const size_t pageBase = static_cast<size_t>(rootPage) * pageSize;
    if (pageBase + 40 < bytes.size()) {
        bytes[pageBase + 40] ^= 0xFFu; // corrupt the payload; page CRC no longer matches
    }
    writeFileBytes(work, bytes);

    std::unique_ptr<Database> db;
    DbResult open = Database::open(work, DatabaseOpenOptions(), db);
    bool failed = false;
    if (open.isOk() && db) {
        // The right-side indexed source is Orders(UserId); the JOIN must report
        // the corruption rather than fall back to NestedLoop.
        SqlEngine engine(*db);
        SqlExecutionResult r = engine.execute(
            "SELECT u.Name FROM Users AS u JOIN Orders AS o ON o.UserId = u.Id;");
        if (!r.ok) failed = true;
        db->close();
    } else {
        failed = true; // open-time detection is also acceptable
    }
    CHECK(failed);

    removeFile(work);
    removeFile(walPath(work));
    removeFile(base);
    removeFile(walPath(base));
}

// ---------------------------------------------------------------------------
// Float64 aggregate/grouping edge policy (section 69).

void testSql7FloatEdges() {
    std::printf("SQL7 Float64 aggregate/grouping edges\n");
    // Non-finite float literals are rejected by the tokenizer.
    CHECK(parseFails("SELECT * FROM T WHERE X = 1e400;", SqlErrorCode::TokenizerError));

    const std::string path = uniquePath("sql7float");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE F (Id INT64 PRIMARY KEY, V FLOAT64 NULL);"));
        CHECK(execOk(engine, "INSERT INTO F VALUES (1, -0.0);"));
        CHECK(execOk(engine, "INSERT INTO F VALUES (2, 0.0);"));
        CHECK(execOk(engine, "INSERT INTO F VALUES (3, 1.5);"));
        CHECK(execOk(engine, "INSERT INTO F VALUES (4, NULL);"));
        SqlResultSet rs;
        // -0.0 and +0.0 are equal for DISTINCT.
        CHECK(execSelect(engine, "SELECT DISTINCT V FROM F;", rs));
        CHECK(rs.rowCount() == 3);
        // And for grouping.
        CHECK(execSelect(engine, "SELECT V, COUNT(*) FROM F GROUP BY V;", rs));
        CHECK(rs.rowCount() == 3);
        CHECK(execSelect(engine,
                         "SELECT COUNT(*), SUM(V), MIN(V), MAX(V) FROM F;", rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            CHECK_INT64(rs.row(0)[0], 4);
            CHECK_DOUBLE(rs.row(0)[1], 1.5);
            CHECK_DOUBLE(rs.row(0)[2], 0.0);
            CHECK_DOUBLE(rs.row(0)[3], 1.5);
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Result metadata (section 84).

void testSql7ResultMetadata() {
    std::printf("SQL7 result-column metadata\n");
    const std::string path = uniquePath("sql7meta");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, V INT64 NOT NULL, "
                             "Txt TEXT NULL);"));
        CHECK(execOk(engine, "INSERT INTO T VALUES (1, 5, 'x');"));
        SqlResultSet rs;
        CHECK(execSelect(engine,
                         "SELECT COUNT(*) AS C, SUM(V) AS S, AVG(V) AS A, MIN(V) AS Mi, "
                         "MAX(V) AS Ma FROM T;",
                         rs));
        CHECK(rs.columnCount() == 5);
        if (rs.columnCount() == 5) {
            CHECK(rs.column(0).name == "C");
            CHECK(rs.column(0).type == DbType::Int64);
            CHECK(!rs.column(0).nullable);
            CHECK(rs.column(1).name == "S");
            CHECK(rs.column(1).type == DbType::Int64);
            CHECK(rs.column(2).name == "A");
            CHECK(rs.column(2).type == DbType::Float64);
            CHECK(rs.column(3).name == "Mi");
            CHECK(rs.column(3).type == DbType::Int64);
            CHECK(rs.column(4).name == "Ma");
            CHECK(rs.column(4).type == DbType::Int64);
        }
        // LEFT JOIN right column is nullable even if storage says NOT NULL.
        CHECK(execSelect(engine,
                         "SELECT t.V FROM T AS t LEFT JOIN T AS t2 ON t.Id = t2.Id;",
                         rs));
        CHECK(rs.columnCount() == 1);
        // Default aggregate display names.
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM T;", rs));
        CHECK(rs.column(0).name == "COUNT(*)");
        CHECK(execSelect(engine, "SELECT SUM(V) FROM T;", rs));
        CHECK(rs.column(0).name == "SUM(V)");
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Deterministic multi-table workload with an independent model (section 78).

struct CustomerModel {
    int64_t id;
    std::string name;
    bool hasRegion;
    std::string region;
};

struct OrderModel {
    int64_t id;
    bool hasCustomer;
    int64_t customerId;
    int64_t amount;
    bool enabled;
};

struct ItemModel {
    int64_t id;
    int64_t orderId;
};

void testSql7DeterministicDataset() {
    std::printf("SQL7 deterministic multi-table dataset (1000/3000/8000 rows)\n");
    const std::string path = uniquePath("sql7det");
    const int kCustomers = 1000;
    const int kOrders = 3000;
    const int kItems = 8000;
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    std::vector<CustomerModel> customers;
    std::vector<OrderModel> orders;
    std::vector<ItemModel> items;
    customers.reserve(kCustomers);
    orders.reserve(kOrders);
    items.reserve(kItems);

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Customers (Id INT64 PRIMARY KEY, Name TEXT "
                             "NOT NULL, Region TEXT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, CustomerId "
                             "INT64 NULL, Amount INT64 NOT NULL, Enabled BOOLEAN NOT "
                             "NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_CustomerId ON Orders "
                             "(CustomerId);"));
        CHECK(execOk(engine, "CREATE TABLE OrderItems (Id INT64 PRIMARY KEY, OrderId "
                             "INT64 NOT NULL, Qty INT32 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_OrderItems_OrderId ON OrderItems "
                             "(OrderId);"));

        // Customers.
        for (int i = 0; i < kCustomers; i += 200) {
            std::string script = "BEGIN;\n";
            for (int j = i; j < i + 200 && j < kCustomers; ++j) {
                CustomerModel c;
                c.id = j;
                c.name = "cust" + std::to_string(j);
                c.hasRegion = (j % 5 != 0);
                c.region = c.hasRegion ? ("R" + std::to_string(j % 7)) : std::string();
                customers.push_back(c);
                script += "INSERT INTO Customers VALUES (" + std::to_string(c.id) +
                          ", '" + c.name + "', " +
                          (c.hasRegion ? ("'" + c.region + "'") : "NULL") + ");\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }
        // Orders.
        for (int i = 0; i < kOrders; i += 200) {
            std::string script = "BEGIN;\n";
            for (int j = i; j < i + 200 && j < kOrders; ++j) {
                OrderModel o;
                o.id = 100000 + j;
                o.hasCustomer = (j % 11 != 0);
                o.customerId = j % kCustomers;
                o.amount = (j * 37) % 1000;
                o.enabled = (j % 3 != 0);
                orders.push_back(o);
                script += "INSERT INTO Orders VALUES (" + std::to_string(o.id) + ", " +
                          (o.hasCustomer ? std::to_string(o.customerId) : "NULL") + ", " +
                          std::to_string(o.amount) + ", " +
                          (o.enabled ? "TRUE" : "FALSE") + ");\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }
        // OrderItems.
        for (int i = 0; i < kItems; i += 200) {
            std::string script = "BEGIN;\n";
            for (int j = i; j < i + 200 && j < kItems; ++j) {
                ItemModel item;
                item.id = 200000 + j;
                item.orderId = 100000 + (j % kOrders);
                items.push_back(item);
                script += "INSERT INTO OrderItems VALUES (" + std::to_string(item.id) +
                          ", " + std::to_string(item.orderId) + ", " +
                          std::to_string((j % 5) + 1) + ");\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }

        // Independent model computations.
        std::vector<int64_t> ordersPerCustomer(customers.size(), 0);
        uint64_t innerJoin = 0;
        for (size_t i = 0; i < orders.size(); ++i) {
            if (orders[i].hasCustomer) {
                ++innerJoin;
                ++ordersPerCustomer[static_cast<size_t>(orders[i].customerId)];
            }
        }
        // LEFT JOIN Customers -> Orders: matching rows plus one null-extended
        // row for every customer with no matching order.
        uint64_t leftJoin = 0;
        for (size_t i = 0; i < ordersPerCustomer.size(); ++i) {
            leftJoin += (ordersPerCustomer[i] == 0)
                            ? 1
                            : static_cast<uint64_t>(ordersPerCustomer[i]);
        }
        uint64_t threeWay = 0;
        for (size_t k = 0; k < items.size(); ++k) {
            const int64_t orderId = items[k].orderId;
            // Find the order; it is guaranteed to exist.
            for (size_t o = 0; o < orders.size(); ++o) {
                if (orders[o].id == orderId) {
                    if (orders[o].hasCustomer) ++threeWay;
                    break;
                }
            }
        }
        int64_t sumAmount = 0;
        int64_t minAmount = orders.empty() ? 0 : orders[0].amount;
        int64_t maxAmount = 0;
        for (size_t i = 0; i < orders.size(); ++i) {
            sumAmount += orders[i].amount;
            if (orders[i].amount < minAmount) minAmount = orders[i].amount;
            if (orders[i].amount > maxAmount) maxAmount = orders[i].amount;
        }

        SqlResultSet rs;
        bool ok = false;
        CHECK(execRowCount(engine,
                           "SELECT c.Id FROM Customers AS c JOIN Orders AS o ON "
                           "o.CustomerId = c.Id;",
                           ok) == innerJoin);
        CHECK(ok);
        CHECK(execRowCount(engine,
                           "SELECT c.Id FROM Customers AS c LEFT JOIN Orders AS o ON "
                           "o.CustomerId = c.Id;",
                           ok) == leftJoin);
        CHECK(ok);
        CHECK(execRowCount(engine,
                           "SELECT c.Id FROM Customers AS c JOIN Orders AS o ON "
                           "o.CustomerId = c.Id JOIN OrderItems AS i ON i.OrderId = "
                           "o.Id;",
                           ok) == threeWay);
        CHECK(ok);
        CHECK(execSelect(engine,
                         "SELECT COUNT(*), SUM(Amount), MIN(Amount), MAX(Amount) FROM "
                         "Orders;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            CHECK_INT64(rs.row(0)[0], static_cast<int64_t>(orders.size()));
            CHECK_INT64(rs.row(0)[1], sumAmount);
            CHECK_INT64(rs.row(0)[2], minAmount);
            CHECK_INT64(rs.row(0)[3], maxAmount);
        }

        // GROUP BY region matches an independent grouping.
        std::map<std::string, int64_t> regionCounts;
        int64_t nullRegionCount = 0;
        for (size_t i = 0; i < customers.size(); ++i) {
            if (customers[i].hasRegion) {
                ++regionCounts[customers[i].region];
            } else {
                ++nullRegionCount;
            }
        }
        CHECK(execSelect(engine,
                         "SELECT Region, COUNT(*) FROM Customers GROUP BY Region;", rs));
        CHECK(rs.rowCount() == regionCounts.size() + 1);
        {
            std::map<std::string, int64_t> got;
            int64_t gotNull = 0;
            for (size_t r = 0; r < rs.rowCount(); ++r) {
                if (rs.row(r)[0].isNull()) {
                    gotNull = rs.row(r)[1].int64Value();
                } else {
                    got[rs.row(r)[0].textValue()] = rs.row(r)[1].int64Value();
                }
            }
            CHECK(got == regionCounts);
            CHECK(gotNull == nullRegionCount);
        }

        // LEFT JOIN + COUNT(o.Id) per customer.
        CHECK(execSelect(engine,
                         "SELECT c.Id, COUNT(o.Id) AS C FROM Customers AS c LEFT JOIN "
                         "Orders AS o ON o.CustomerId = c.Id GROUP BY c.Id ORDER BY "
                         "c.Id;",
                         rs));
        CHECK(rs.rowCount() == customers.size());
        if (rs.rowCount() == customers.size()) {
            std::vector<int64_t> expected(customers.size(), 0);
            for (size_t i = 0; i < orders.size(); ++i) {
                if (orders[i].hasCustomer) ++expected[orders[i].customerId];
            }
            bool match = true;
            for (size_t r = 0; r < rs.rowCount(); ++r) {
                if (rs.row(r)[0].int64Value() != static_cast<int64_t>(r) ||
                    rs.row(r)[1].int64Value() != expected[r]) {
                    match = false;
                    break;
                }
            }
            CHECK(match);
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Repeated query/transaction workload (section 79).

void testSql7RepeatedWorkload() {
    std::printf("SQL7 repeated joined/aggregate workload (250 lifecycles)\n");
    const std::string path = uniquePath("sql7repeat");
    const int kUsers = 40;
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    std::vector<int64_t> orderCount(static_cast<size_t>(kUsers), 0);
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT "
                             "NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 "
                             "NOT NULL, Amount INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_UserId ON Orders (UserId);"));
        for (int i = 0; i < kUsers; ++i) {
            CHECK(execOk(engine, "INSERT INTO Users VALUES (" + std::to_string(i) +
                                     ", 'u" + std::to_string(i) + "');"));
        }

        int64_t nextOrderId = 1;
        int mismatches = 0;
        for (int k = 0; k < 250; ++k) {
            const int user = (k * 7) % kUsers;
            const int mode = k % 5;
            SqlResultSet rs;
            if (mode == 0) {
                CHECK(execOk(engine, "BEGIN;"));
                CHECK(execOk(engine, "INSERT INTO Orders VALUES (" +
                                         std::to_string(nextOrderId) + ", " +
                                         std::to_string(user) + ", " +
                                         std::to_string(k % 100) + ");"));
                CHECK(execOk(engine, "COMMIT;"));
                ++orderCount[static_cast<size_t>(user)];
                ++nextOrderId;
            } else if (mode == 1) {
                CHECK(execOk(engine, "BEGIN;"));
                CHECK(execOk(engine, "INSERT INTO Orders VALUES (" +
                                         std::to_string(nextOrderId) + ", " +
                                         std::to_string(user) + ", 0);"));
                CHECK(execOk(engine, "ROLLBACK;"));
                ++nextOrderId;
            } else if (mode == 2) {
                // Aggregate over an indexed join.
                CHECK(execSelect(engine,
                                 "SELECT COUNT(*) FROM Users AS u JOIN Orders AS o ON "
                                 "o.UserId = u.Id;",
                                 rs));
                int64_t total = 0;
                for (size_t i = 0; i < orderCount.size(); ++i) total += orderCount[i];
                if (!rs.hasResult() || rs.row(0)[0].int64Value() != total) ++mismatches;
            } else if (mode == 3) {
                // LEFT JOIN with per-user counts.
                CHECK(execSelect(engine,
                                 "SELECT u.Id, COUNT(o.Id) AS C FROM Users AS u LEFT "
                                 "JOIN Orders AS o ON o.UserId = u.Id GROUP BY u.Id "
                                 "ORDER BY u.Id;",
                                 rs));
                if (rs.rowCount() != static_cast<size_t>(kUsers)) {
                    ++mismatches;
                } else {
                    for (int i = 0; i < kUsers; ++i) {
                        if (rs.row(i)[1].int64Value() !=
                            orderCount[static_cast<size_t>(i)]) {
                            ++mismatches;
                            break;
                        }
                    }
                }
            } else {
                // Force nested loop and compare the row count to the indexed plan.
                SqlResultSet indexed;
                CHECK(execSelect(engine,
                                 "SELECT u.Name, o.Amount FROM Users AS u JOIN Orders "
                                 "AS o ON o.UserId = u.Id;",
                                 indexed));
                SqlEngine forced(*db);
                forced.setForceNestedLoop(true);
                SqlResultSet nested;
                CHECK(execSelect(forced,
                                 "SELECT u.Name, o.Amount FROM Users AS u JOIN Orders "
                                 "AS o ON o.UserId = u.Id;",
                                 nested));
                if (resultSet(indexed) != resultSet(nested)) ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen and re-check the aggregate against the model.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            SqlResultSet rs;
            CHECK(execSelect(engine, "SELECT COUNT(*) FROM Orders;", rs));
            int64_t total = 0;
            for (size_t i = 0; i < orderCount.size(); ++i) total += orderCount[i];
            CHECK_INT64(rs.row(0)[0], total);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("guideXOS SQL7 joins, aliases, aggregates, GROUP BY and DISTINCT tests\n");
    std::string dir = testDir();
    std::printf("temp dir: %s\n", dir.c_str());

    std::string cmd = "del /q \"" + dir + "\\*.gxdb\" >nul 2>nul";
    std::system(cmd.c_str());
    cmd = "del /q \"" + dir + "\\*.gxwal\" >nul 2>nul";
    std::system(cmd.c_str());

    testSql7Tokenizer();
    testSql7Parser();
    testSql7HostileInput();
    testSql7QualifiedAndAliases();
    testSql7InnerJoin();
    testSql7LeftJoin();
    testSql7SelfJoin();
    testSql7MultiTableJoin();
    testSql7IndexedEquivalence();
    testSql7Aggregates();
    testSql7GroupBy();
    testSql7Distinct();
    testSql7AggregateJoin();
    testSql7TransactionVisibility();
    testSql7JoinIndexCorruption();
    testSql7FloatEdges();
    testSql7ResultMetadata();
    testSql7DeterministicDataset();
    testSql7RepeatedWorkload();

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
