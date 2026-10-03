// guideXOS SQL -- Phase SQL4
// Hosted acceptance tests for the SQL language layer: tokenizer, parser, AST,
// semantic validation, execution, result sets and transaction integration.
//
// Same self-contained harness style as the SQL1-SQL3 suites. Every case writes
// real .gxdb/.gxwal files under the OS temp directory.
//
// Crash injection reuses the SQL3 byte-budgeted IDatabaseFile model so a SQL
// driven transaction can be interrupted around the commit durability point and
// still recover to exactly the pre-state or the post-state.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "database_diagnostics.h"
#include "database_engine.h"
#include "database_format.h"
#include "database_heap.h"
#include "database_io.h"
#include "database_relational.h"
#include "database_schema.h"
#include "database_sql.h"
#include "database_sql_expr.h"
#include "database_sql_parser.h"
#include "database_sql_token.h"
#include "database_sql_tokenizer.h"
#include "database_transaction.h"
#include "database_types.h"
#include "database_wal.h"

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

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)
#define CHECK_STATUS(expr, expected) checkStatus((expr), (expected), #expr, __LINE__)
#define CHECK_INT64(value, expected) checkInt64((value), (expected), #value, __LINE__)
#define CHECK_TEXT(value, expected) checkText((value), (expected), #value, __LINE__)
#define CHECK_BOOL(value, expected) checkBool((value), (expected), #value, __LINE__)

std::string testDir() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    std::string dir = std::string(base) + "/gxos_gxdb_sql4_tests";
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

void removeFile(const std::string& path) { std::remove(path.c_str()); }

std::string walPath(const std::string& dbPath) { return walPathFor(dbPath); }

// ---------------------------------------------------------------------------
// Byte-budgeted crash-injection backend (same model as the SQL3 suite).

struct BudgetState {
    uint64_t budget;
    uint64_t written;
    bool exhausted;
    bool recording;
    std::vector<uint64_t> boundaries;

    BudgetState() : budget(UINT64_MAX), written(0), exhausted(false), recording(false) {}
    void freeze() { budget = written; }
};

class BudgetFile : public IDatabaseFile {
public:
    explicit BudgetFile(std::shared_ptr<BudgetState> state)
        : _state(state), _inner(new HostDatabaseFile()) {}

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override {
        return _inner->open(path, mode, truncateExisting);
    }
    void close() override { _inner->close(); }
    bool isOpen() const override { return _inner->isOpen(); }
    bool isReadOnly() const override { return _inner->isReadOnly(); }
    uint64_t size() override { return _inner->size(); }

    bool readAt(uint64_t offset, void* buffer, size_t length) override {
        return _inner->readAt(offset, buffer, length);
    }

    bool writeAt(uint64_t offset, const void* buffer, size_t length) override {
        if (_state->recording) {
            _state->boundaries.push_back(_state->written);
        }
        const uint64_t remaining =
            (_state->written < _state->budget) ? (_state->budget - _state->written) : 0;
        if (length > remaining) {
            const size_t allowed = static_cast<size_t>(remaining);
            if (allowed > 0) {
                _inner->writeAt(offset, buffer, allowed);
            }
            _state->written += allowed;
            _state->exhausted = true;
            return false;
        }
        if (!_inner->writeAt(offset, buffer, length)) {
            return false;
        }
        _state->written += length;
        return true;
    }

    bool truncateTo(uint64_t size) override { return _inner->truncateTo(size); }
    bool flush() override { return _inner->flush(); }
    const std::string& path() const override { return _inner->path(); }

private:
    std::shared_ptr<BudgetState> _state;
    std::unique_ptr<HostDatabaseFile> _inner;
};

class BudgetFileSystem : public IDatabaseFileSystem {
public:
    explicit BudgetFileSystem(std::shared_ptr<BudgetState> state) : _state(state) {}
    std::unique_ptr<IDatabaseFile> createFile() override {
        return std::unique_ptr<IDatabaseFile>(new BudgetFile(_state));
    }
    bool exists(const std::string& path) override { return hostFileExists(path); }
    bool remove(const std::string& path) override {
        if (!hostFileExists(path)) return true;
        return std::remove(path.c_str()) == 0;
    }

private:
    std::shared_ptr<BudgetState> _state;
};

// ---------------------------------------------------------------------------
// SQL helpers.

bool tokenizeOk(const std::string& sql, std::vector<SqlToken>& out) {
    SqlError error;
    SqlTokenizer tokenizer;
    return tokenizer.tokenize(sql, out, error);
}

bool tokenizeFails(const std::string& sql, SqlErrorCode expected) {
    std::vector<SqlToken> tokens;
    SqlError error;
    SqlTokenizer tokenizer;
    if (tokenizer.tokenize(sql, tokens, error)) {
        return false;
    }
    return error.code == expected;
}

bool parseOne(const std::string& sql, SqlStatementAst& out, SqlError& error) {
    std::vector<SqlToken> tokens;
    SqlTokenizer tokenizer;
    if (!tokenizer.tokenize(sql, tokens, error)) {
        return false;
    }
    SqlParser parser(tokens);
    bool done = false;
    if (!parser.next(out, error, done)) {
        return false;
    }
    return !done;
}

bool parseFails(const std::string& sql, SqlErrorCode expected) {
    SqlStatementAst statement;
    SqlError error;
    if (parseOne(sql, statement, error)) {
        return false;
    }
    return error.code == expected;
}

std::unique_ptr<Database> newDb(const std::string& path) {
    std::unique_ptr<Database> db;
    if (!Database::create(path, DatabaseCreateOptions(), db).isOk()) {
        return nullptr;
    }
    return db;
}

std::string makeTag(uint64_t id, size_t length) {
    std::string tag = "tag" + std::to_string(static_cast<unsigned long long>(id));
    while (tag.size() < length) {
        tag.push_back('x');
    }
    return tag;
}

// Verifies a table with schema (Id INT64, Tag TEXT, Flag BOOLEAN) contains
// exactly rows 0..expectedCount-1 with consistent values.
bool verifySequential(const std::string& path, const std::string& table,
                      uint64_t expectedCount) {
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db).isOk() || !db) {
        return false;
    }
    std::unique_ptr<Table> t;
    if (!db->openTable(table, t).isOk()) {
        db->close();
        return false;
    }
    if (t->rowCount() != expectedCount) {
        db->close();
        return false;
    }
    std::unique_ptr<TableScan> scan;
    t->scanStart(scan);
    std::vector<DbValue> row;
    uint64_t index = 0;
    bool ok = true;
    while (scan->next(row)) {
        if (row.size() != 3 || row[0].int64Value() != static_cast<int64_t>(index) ||
            row[2].booleanValue() != (index % 2 == 0)) {
            ok = false;
            break;
        }
        ++index;
    }
    if (index != expectedCount || !scan->status().isOk()) {
        ok = false;
    }
    db->close();
    return ok;
}

// ---------------------------------------------------------------------------
// SQL5 helpers.

std::unique_ptr<Database> newDbWithPageSize(const std::string& path, uint32_t pageSize) {
    DatabaseCreateOptions options;
    options.pageSize = pageSize;
    std::unique_ptr<Database> db;
    if (!Database::create(path, options, db).isOk()) {
        return nullptr;
    }
    return db;
}

bool execOk(SqlEngine& engine, const std::string& sql) {
    SqlExecutionResult result = engine.execute(sql);
    return result.ok;
}

bool execAffected(SqlEngine& engine, const std::string& sql, uint64_t& affected) {
    SqlExecutionResult result = engine.execute(sql);
    if (!result.ok || result.statements.empty()) {
        return false;
    }
    affected = result.statements[0].affectedRows;
    return true;
}

// Returns the row count of a SELECT, or sets `ok` false on any failure.
uint64_t execRowCount(SqlEngine& engine, const std::string& sql, bool& ok) {
    SqlExecutionResult result = engine.execute(sql);
    ok = result.ok && !result.statements.empty();
    if (!ok) {
        return 0;
    }
    return result.statements[0].resultSet.rowCount();
}

// Builds a table Crash (Id INT64 NOT NULL, Tag TEXT NOT NULL, Flag BOOLEAN NOT
// NULL) with rows 0..count-1, every row carrying the same `tag`. Batches
// inserts into explicit transactions so a large table never hits the
// transaction page limit while being built.
bool buildTagTable(const std::string& path, uint64_t count, const std::string& tag) {
    std::unique_ptr<Database> db = newDb(path);
    if (!db) {
        return false;
    }
    SqlEngine engine(*db);
    if (!execOk(engine, "CREATE TABLE Crash (Id INT64 NOT NULL, Tag TEXT NOT NULL, "
                        "Flag BOOLEAN NOT NULL);")) {
        db->close();
        return false;
    }
    uint64_t inserted = 0;
    while (inserted < count) {
        std::string script = "BEGIN;\n";
        for (uint64_t i = 0; i < 250 && inserted < count; ++i, ++inserted) {
            script += "INSERT INTO Crash VALUES (";
            script += std::to_string(static_cast<unsigned long long>(inserted));
            script += ", '";
            script += tag;
            script += "', ";
            script += (inserted % 2 == 0) ? "TRUE" : "FALSE";
            script += ");\n";
        }
        script += "COMMIT;\n";
        if (!execOk(engine, script)) {
            db->close();
            return false;
        }
    }
    DbResult flushed = db->flush();
    DbResult closed = db->close();
    return flushed.isOk() && closed.isOk();
}

// Verifies every row of Crash has Tag == expectedTag, the expected count, and
// consistent Flag values.
bool verifyTagTable(const std::string& path, const std::string& expectedTag,
                    uint64_t expectedCount) {
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db).isOk() || !db) {
        return false;
    }
    std::unique_ptr<Table> table;
    if (!db->openTable("Crash", table).isOk()) {
        db->close();
        return false;
    }
    if (table->rowCount() != expectedCount) {
        db->close();
        return false;
    }
    std::unique_ptr<TableScan> scan;
    table->scanStart(scan);
    std::vector<DbValue> row;
    uint64_t seen = 0;
    bool ok = true;
    while (scan->next(row)) {
        if (row.size() != 3 || row[1].textValue() != expectedTag ||
            row[2].booleanValue() != (static_cast<uint64_t>(row[0].int64Value()) % 2 == 0)) {
            ok = false;
            break;
        }
        ++seen;
    }
    if (seen != expectedCount || !scan->status().isOk()) {
        ok = false;
    }
    db->close();
    return ok;
}

// ---------------------------------------------------------------------------
// Tokenizer tests (SQL4 section 39).

void testTokenizerKeywords() {
    std::printf("tokenizer: keywords and mixed case\n");
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("CREATE TABLE INSERT INTO VALUES SELECT FROM BEGIN COMMIT "
                         "ROLLBACK NOT NULL BOOLEAN BOOL INT INT32 INT64 BIGINT DOUBLE "
                         "FLOAT64 TEXT BLOB TRUE FALSE",
                         tokens));
        CHECK(tokens.size() == 25);
        if (tokens.size() == 25) {
            CHECK(tokens[0].kind == SqlTokenKind::Create);
            CHECK(tokens[1].kind == SqlTokenKind::Table);
            CHECK(tokens[2].kind == SqlTokenKind::Insert);
            CHECK(tokens[3].kind == SqlTokenKind::Into);
            CHECK(tokens[4].kind == SqlTokenKind::Values);
            CHECK(tokens[5].kind == SqlTokenKind::Select);
            CHECK(tokens[6].kind == SqlTokenKind::From);
            CHECK(tokens[7].kind == SqlTokenKind::Begin);
            CHECK(tokens[8].kind == SqlTokenKind::Commit);
            CHECK(tokens[9].kind == SqlTokenKind::Rollback);
            CHECK(tokens[10].kind == SqlTokenKind::Not);
            CHECK(tokens[11].kind == SqlTokenKind::Null);
            CHECK(tokens[12].kind == SqlTokenKind::Boolean);
            CHECK(tokens[13].kind == SqlTokenKind::Bool);
            CHECK(tokens[14].kind == SqlTokenKind::Int);
            CHECK(tokens[15].kind == SqlTokenKind::Int32);
            CHECK(tokens[16].kind == SqlTokenKind::Int64);
            CHECK(tokens[17].kind == SqlTokenKind::BigInt);
            CHECK(tokens[18].kind == SqlTokenKind::Double);
            CHECK(tokens[19].kind == SqlTokenKind::Float64);
            CHECK(tokens[20].kind == SqlTokenKind::Text);
            CHECK(tokens[21].kind == SqlTokenKind::Blob);
            CHECK(tokens[22].kind == SqlTokenKind::True);
            CHECK(tokens[23].kind == SqlTokenKind::False);
            CHECK(tokens[24].kind == SqlTokenKind::EndOfInput);
        }
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("select SeLeCt sElEcT tAbLe inTo", tokens));
        CHECK(tokens.size() == 6);
        CHECK(tokens[0].kind == SqlTokenKind::Select);
        CHECK(tokens[1].kind == SqlTokenKind::Select);
        CHECK(tokens[2].kind == SqlTokenKind::Select);
        CHECK(tokens[3].kind == SqlTokenKind::Table);
        CHECK(tokens[4].kind == SqlTokenKind::Into);
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("Users users USERS _id id2", tokens));
        CHECK(tokens.size() == 6);
        CHECK(tokens[0].kind == SqlTokenKind::Identifier);
        CHECK(tokens[0].text == "Users");
        CHECK(tokens[1].kind == SqlTokenKind::Identifier);
        CHECK(tokens[1].text == "users");
        CHECK(tokens[2].kind == SqlTokenKind::Identifier);
        CHECK(tokens[2].text == "USERS");
        CHECK(tokens[3].text == "_id");
        CHECK(tokens[4].text == "id2");
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("( ) , ; *", tokens));
        CHECK(tokens.size() == 6);
        CHECK(tokens[0].kind == SqlTokenKind::LeftParen);
        CHECK(tokens[1].kind == SqlTokenKind::RightParen);
        CHECK(tokens[2].kind == SqlTokenKind::Comma);
        CHECK(tokens[3].kind == SqlTokenKind::Semicolon);
        CHECK(tokens[4].kind == SqlTokenKind::Star);
        CHECK(tokens[5].kind == SqlTokenKind::EndOfInput);
    }
}

void testTokenizerLiterals() {
    std::printf("tokenizer: literals and locations\n");
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("0 42 -42 2147483647 9223372036854775807 "
                         "-9223372036854775808",
                         tokens));
        CHECK(tokens.size() == 7);
        CHECK(tokens[0].kind == SqlTokenKind::IntegerLiteral);
        CHECK(tokens[0].int64Value == 0);
        CHECK(tokens[1].int64Value == 42);
        CHECK(tokens[2].int64Value == -42);
        CHECK(tokens[3].int64Value == 2147483647);
        CHECK(tokens[4].int64Value == 9223372036854775807LL);
        CHECK(tokens[5].int64Value == (-9223372036854775807LL - 1));
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("3.14159 -0.5 1.0 .5 1e3 -2.5e-2", tokens));
        CHECK(tokens.size() == 7);
        CHECK(tokens[0].kind == SqlTokenKind::FloatLiteral);
        CHECK(std::fabs(tokens[0].float64Value - 3.14159) < 1e-9);
        CHECK(std::fabs(tokens[1].float64Value + 0.5) < 1e-9);
        CHECK(std::fabs(tokens[2].float64Value - 1.0) < 1e-9);
        CHECK(std::fabs(tokens[3].float64Value - 0.5) < 1e-9);
        CHECK(std::fabs(tokens[4].float64Value - 1000.0) < 1e-9);
        CHECK(std::fabs(tokens[5].float64Value + 0.025) < 1e-9);
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("'Alice' 'hello world' 'Leon''s database' ''", tokens));
        CHECK(tokens.size() == 5);
        CHECK(tokens[0].kind == SqlTokenKind::StringLiteral);
        CHECK(tokens[0].text == "Alice");
        CHECK(tokens[1].text == "hello world");
        CHECK(tokens[2].text == "Leon's database");
        CHECK(tokens[3].text == "");
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("X'001122AABBCC' x'' X'aaBB'", tokens));
        CHECK(tokens.size() == 4);
        CHECK(tokens[0].kind == SqlTokenKind::BlobLiteral);
        CHECK(tokens[0].bytes.size() == 6);
        CHECK(tokens[0].bytes[0] == 0x00);
        CHECK(tokens[0].bytes[1] == 0x11);
        CHECK(tokens[0].bytes[5] == 0xCC);
        CHECK(tokens[1].kind == SqlTokenKind::BlobLiteral);
        CHECK(tokens[1].bytes.empty());
        CHECK(tokens[2].bytes.size() == 2);
        CHECK(tokens[2].bytes[0] == 0xAA);
        CHECK(tokens[2].bytes[1] == 0xBB);
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("-- line comment\nSELECT /* block */ * FROM T;", tokens));
        CHECK(tokens.size() == 6);
        CHECK(tokens[0].kind == SqlTokenKind::Select);
        CHECK(tokens[1].kind == SqlTokenKind::Star);
        CHECK(tokens[2].kind == SqlTokenKind::From);
        CHECK(tokens[3].kind == SqlTokenKind::Identifier);
        CHECK(tokens[4].kind == SqlTokenKind::Semicolon);
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("SELECT\n  Id\nFROM Users", tokens));
        CHECK(tokens.size() == 5);
        CHECK(tokens[0].line == 1 && tokens[0].column == 1);
        CHECK(tokens[1].line == 2 && tokens[1].column == 3);
        CHECK(tokens[2].line == 3 && tokens[2].column == 1);
        CHECK(tokens[3].line == 3 && tokens[3].column == 6);
        CHECK(tokens[3].text == "Users");
    }
}

void testTokenizerMalformed() {
    std::printf("tokenizer: malformed and over-limit input\n");
    CHECK(tokenizeFails("'unterminated", SqlErrorCode::TokenizerError));
    CHECK(tokenizeFails("/* unterminated", SqlErrorCode::TokenizerError));
    CHECK(tokenizeFails("X'123'", SqlErrorCode::TokenizerError)); // odd hex count
    CHECK(tokenizeFails("X'12G4'", SqlErrorCode::TokenizerError)); // invalid hex
    CHECK(tokenizeFails("X'12", SqlErrorCode::TokenizerError));    // unterminated blob
    CHECK(tokenizeFails("@", SqlErrorCode::TokenizerError));
    CHECK(tokenizeFails("SELECT #", SqlErrorCode::TokenizerError));
    CHECK(tokenizeFails("9223372036854775808", SqlErrorCode::TokenizerError));
    CHECK(tokenizeFails("1e999", SqlErrorCode::TokenizerError));

    std::string longIdentifier(kSqlMaxIdentifierBytes + 1, 'a');
    CHECK(tokenizeFails(longIdentifier, SqlErrorCode::ResourceLimit));

    std::string longString(kSqlMaxStringLiteralBytes + 1, 'a');
    CHECK(tokenizeFails("'" + longString + "'", SqlErrorCode::ResourceLimit));

    std::string longBlob;
    longBlob.reserve(2 * (kSqlMaxBlobLiteralBytes + 1));
    for (size_t i = 0; i < kSqlMaxBlobLiteralBytes + 1; ++i) {
        longBlob += "ab";
    }
    CHECK(tokenizeFails("X'" + longBlob + "'", SqlErrorCode::ResourceLimit));

    std::string longNumber(kSqlMaxNumericLiteralBytes + 1, '9');
    CHECK(tokenizeFails(longNumber, SqlErrorCode::ResourceLimit));

    std::string huge(kSqlMaxInputBytes + 1, 'a');
    CHECK(tokenizeFails(huge, SqlErrorCode::ResourceLimit));

    std::string manyTokens;
    manyTokens.reserve(kSqlMaxTokens + 1);
    for (size_t i = 0; i < kSqlMaxTokens + 1; ++i) {
        manyTokens.push_back(',');
    }
    CHECK(tokenizeFails(manyTokens, SqlErrorCode::ResourceLimit));

    // Exactly at the identifier limit must be accepted.
    std::string maxIdentifier(kSqlMaxIdentifierBytes, 'a');
    std::vector<SqlToken> maxTokens;
    CHECK(tokenizeOk(maxIdentifier, maxTokens));
}

// ---------------------------------------------------------------------------
// Parser tests (SQL4 section 40).

void testParserSuccess() {
    std::printf("parser: successful statements\n");
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("CREATE TABLE Users (Id INT64 NOT NULL, Name TEXT NOT NULL, "
                       "Enabled BOOLEAN NOT NULL);",
                       statement, error));
        CHECK(statement.type == SqlStatementType::CreateTable);
        CHECK(statement.createTable.table.name == "Users");
        CHECK(statement.createTable.columns.size() == 3);
        CHECK(statement.createTable.columns[0].name.name == "Id");
        CHECK(statement.createTable.columns[0].type == DbType::Int64);
        CHECK(statement.createTable.columns[0].nullable == false);
        CHECK(statement.createTable.columns[1].type == DbType::Text);
        CHECK(statement.createTable.columns[2].type == DbType::Boolean);
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("CREATE TABLE Notes (Id INT64 NOT NULL, Body TEXT NULL);",
                       statement, error));
        CHECK(statement.createTable.columns[1].nullable == true);
    }
    {
        // Default nullability is nullable.
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("CREATE TABLE T (A INT);", statement, error));
        CHECK(statement.createTable.columns[0].type == DbType::Int32);
        CHECK(statement.createTable.columns[0].nullable == true);
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("INSERT INTO Users VALUES (1, 'Alice', TRUE);", statement,
                       error));
        CHECK(statement.type == SqlStatementType::Insert);
        CHECK(statement.insert.table.name == "Users");
        CHECK(statement.insert.values.size() == 3);
        CHECK(statement.insert.values[0].kind == SqlLiteralKind::Integer);
        CHECK(statement.insert.values[0].int64Value == 1);
        CHECK(statement.insert.values[1].kind == SqlLiteralKind::String);
        CHECK(statement.insert.values[1].textValue == "Alice");
        CHECK(statement.insert.values[2].kind == SqlLiteralKind::Boolean);
        CHECK(statement.insert.values[2].boolValue == true);
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT * FROM Users;", statement, error));
        CHECK(statement.type == SqlStatementType::Select);
        CHECK(statement.select.star);
        CHECK(statement.select.table.name == "Users");
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT Name, Id FROM Users;", statement, error));
        CHECK(!statement.select.star);
        CHECK(statement.select.columns.size() == 2);
        CHECK(statement.select.columns[0].name == "Name");
        CHECK(statement.select.columns[1].name == "Id");
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("BEGIN;", statement, error));
        CHECK(statement.type == SqlStatementType::Begin);
        CHECK(parseOne("COMMIT;", statement, error));
        CHECK(statement.type == SqlStatementType::Commit);
        CHECK(parseOne("ROLLBACK;", statement, error));
        CHECK(statement.type == SqlStatementType::Rollback);
    }
    {
        // Final statement may omit the semicolon.
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT * FROM Users", statement, error));
        CHECK(statement.type == SqlStatementType::Select);
    }
}

void testParserFailures() {
    std::printf("parser: syntax failures\n");
    CHECK(parseFails("CREATE;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("CREATE TABLE;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("CREATE TABLE T;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("INSERT;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("INSERT INTO T;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT FROM T;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("BEGIN extra;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * Users;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("CREATE TABLE T (A);", SqlErrorCode::SyntaxError));
    CHECK(parseFails("CREATE TABLE T (A VARCHAR(10));", SqlErrorCode::SyntaxError));
    CHECK(parseFails("CREATE TABLE T (A TEXT(10));", SqlErrorCode::Unsupported));
    CHECK(parseFails("INSERT INTO T VALUES ();", SqlErrorCode::SyntaxError));
    CHECK(parseFails("INSERT INTO T VALUES (1 2);", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("CREATE TABLE T (A INT64,);", SqlErrorCode::SyntaxError));

    // Over-limit column and value counts are rejected without crashing.
    std::string columns = "CREATE TABLE Big (";
    for (size_t i = 0; i < kSqlMaxColumnsPerCreate + 2; ++i) {
        if (i != 0) columns += ", ";
        columns += "C" + std::to_string(static_cast<unsigned long long>(i)) + " INT64";
    }
    columns += ");";
    CHECK(parseFails(columns, SqlErrorCode::ResourceLimit));

    std::string values = "INSERT INTO T VALUES (";
    for (size_t i = 0; i < kSqlMaxValuesPerInsert + 2; ++i) {
        if (i != 0) values += ", ";
        values += "1";
    }
    values += ");";
    CHECK(parseFails(values, SqlErrorCode::ResourceLimit));

    // Repeated semicolons are accepted as empty statements.
    SqlStatementAst statement;
    SqlError error;
    CHECK(parseOne(";; SELECT * FROM T;;", statement, error));
    CHECK(statement.type == SqlStatementType::Select);
}

// ---------------------------------------------------------------------------
// Semantic validation tests (SQL4 section 41).

void testSemanticValidation() {
    std::printf("semantic validation\n");
    const std::string path = uniquePath("sem");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result;

        result = engine.execute(
            "CREATE TABLE T (Id INT64 NOT NULL, Name TEXT NOT NULL, Small INT32 NOT "
            "NULL, Note TEXT NULL, Data BLOB NULL);");
        CHECK(result.ok);
        CHECK(result.statements.size() == 1);
        CHECK(result.statements[0].objectName == "T");

        result = engine.execute("CREATE TABLE T (Id INT64 NOT NULL);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute("CREATE TABLE D (Id INT64, Id INT64);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute("SELECT * FROM Missing;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute("SELECT Nope FROM T;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute("INSERT INTO Missing VALUES (1);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute("INSERT INTO T VALUES (1);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute("INSERT INTO T VALUES ('x', 'y', 1, NULL, NULL);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute("INSERT INTO T VALUES (1, NULL, 1, NULL, NULL);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute("INSERT INTO T VALUES (1, 'a', 2147483648, NULL, NULL);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        // Int64 literal overflow is caught during tokenization.
        result = engine.execute("INSERT INTO T VALUES (9223372036854775808, 'a', 1, "
                                "NULL, NULL);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::TokenizerError);

        // Valid row including NULL and BLOB.
        result = engine.execute(
            "INSERT INTO T VALUES (1, 'Alice', 7, NULL, X'00FF');");
        CHECK(result.ok);
        CHECK(result.statements[0].affectedRows == 1);

        result = engine.execute("SELECT * FROM T;");
        CHECK(result.ok);
        CHECK(result.statements[0].resultSet.rowCount() == 1);
        if (result.statements[0].resultSet.rowCount() == 1) {
            const std::vector<DbValue>& row = result.statements[0].resultSet.row(0);
            CHECK_INT64(row[0], 1);
            CHECK_TEXT(row[1], "Alice");
            CHECK(row[2].type() == DbType::Int32 && row[2].int32Value() == 7);
            CHECK(row[3].isNull());
            CHECK(row[4].type() == DbType::Blob && row[4].blobValue().size() == 2);
        }
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// End-to-end script (SQL4 section 42).

void testEndToEndScript() {
    std::printf("end-to-end script and reopen\n");
    const std::string path = uniquePath("e2e");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    const std::string script =
        "CREATE TABLE Users (\n"
        "    Id INT64 NOT NULL,\n"
        "    Name TEXT NOT NULL,\n"
        "    Enabled BOOLEAN NOT NULL,\n"
        "    Note TEXT NULL\n"
        ");\n"
        "INSERT INTO Users VALUES (1, 'Alice', TRUE, NULL);\n"
        "INSERT INTO Users VALUES (2, 'Bob', FALSE, 'hello');\n"
        "INSERT INTO Users VALUES (3, 'Carol', TRUE, 'Carol''s row');\n"
        "SELECT * FROM Users;\n"
        "SELECT Name, Id FROM Users;\n";

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(script);
        CHECK(result.ok);
        CHECK(result.statements.size() == 6);
        if (result.statements.size() == 6) {
            CHECK(result.statements[0].type == SqlStatementType::CreateTable);
            CHECK(result.statements[1].affectedRows == 1);
            CHECK(result.statements[2].affectedRows == 1);
            CHECK(result.statements[3].affectedRows == 1);

            const SqlResultSet& all = result.statements[4].resultSet;
            CHECK(all.columnCount() == 4);
            CHECK(all.rowCount() == 3);
            CHECK(all.column(0).name == "Id");
            CHECK(all.column(0).type == DbType::Int64);
            if (all.rowCount() == 3) {
                CHECK_INT64(all.row(0)[0], 1);
                CHECK_TEXT(all.row(0)[1], "Alice");
                CHECK_BOOL(all.row(0)[2], true);
                CHECK(all.row(0)[3].isNull());
                CHECK_TEXT(all.row(1)[3], "hello");
                CHECK_TEXT(all.row(2)[3], "Carol's row");
            }

            const SqlResultSet& projected = result.statements[5].resultSet;
            CHECK(projected.columnCount() == 2);
            CHECK(projected.column(0).name == "Name");
            CHECK(projected.column(1).name == "Id");
            if (projected.rowCount() == 3) {
                CHECK_TEXT(projected.row(0)[0], "Alice");
                CHECK_INT64(projected.row(0)[1], 1);
                CHECK_TEXT(projected.row(2)[0], "Carol");
                CHECK_INT64(projected.row(2)[1], 3);
            }
        }
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen and query again; the logical result must be identical.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            SqlExecutionResult result = engine.execute("SELECT * FROM Users;");
            CHECK(result.ok);
            CHECK(result.statements[0].resultSet.rowCount() == 3);
            if (result.statements[0].resultSet.rowCount() == 3) {
                const SqlResultSet& rs = result.statements[0].resultSet;
                CHECK_INT64(rs.row(2)[0], 3);
                CHECK_TEXT(rs.row(2)[3], "Carol's row");
            }
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Explicit transaction integration (SQL4 sections 26-29, 43).

void testExplicitTransactions() {
    std::printf("explicit transactions and rollback persistence\n");
    const std::string path = uniquePath("tx");

    {
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (!db) return;
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "BEGIN;\n"
            "CREATE TABLE TxRows (Id INT64 NOT NULL, Value TEXT NOT NULL);\n"
            "INSERT INTO TxRows VALUES (1, 'one');\n"
            "INSERT INTO TxRows VALUES (2, 'two');\n"
            "SELECT * FROM TxRows;\n"
            "COMMIT;\n");
        CHECK(result.ok);
        CHECK(result.statements.size() == 6);
        if (result.statements.size() == 6) {
            CHECK(result.statements[0].transactionActiveAfter == true);
            CHECK(result.statements[1].transactionActiveAfter == true);
            CHECK(result.statements[4].resultSet.rowCount() == 2);
            CHECK(result.statements[5].type == SqlStatementType::Commit);
            CHECK(result.statements[5].transactionActiveAfter == false);
        }
        CHECK(!engine.inTransaction());
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        if (db) {
            std::unique_ptr<Table> table;
            CHECK_STATUS(db->openTable("TxRows", table), DbStatus::Ok);
            CHECK(table->rowCount() == 2);
            CHECK_STATUS(db->close(), DbStatus::Ok);
        }
    }

    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        if (db) {
            SqlEngine engine(*db);
            SqlExecutionResult result = engine.execute(
                "BEGIN;\n"
                "INSERT INTO TxRows VALUES (3, 'three');\n"
                "INSERT INTO TxRows VALUES (4, 'four');\n"
                "ROLLBACK;\n");
            CHECK(result.ok);
            CHECK(result.statements.size() == 4);
            CHECK(!engine.inTransaction());
            CHECK_STATUS(db->close(), DbStatus::Ok);
        }
    }

    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        if (db) {
            std::unique_ptr<Table> table;
            CHECK_STATUS(db->openTable("TxRows", table), DbStatus::Ok);
            CHECK(table->rowCount() == 2);
            CHECK_STATUS(db->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

void testTransactionErrors() {
    std::printf("transaction error behavior\n");
    const std::string path = uniquePath("txerr");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute("BEGIN; BEGIN;");
        CHECK(!result.ok);
        CHECK(result.statements.size() == 2);
        if (result.statements.size() == 2) {
            CHECK(result.statements[0].ok);
            CHECK(!result.statements[1].ok);
            CHECK(result.statements[1].error.code == SqlErrorCode::TransactionError);
        }
        CHECK(engine.inTransaction());

        // A semantic error inside the active transaction keeps it active.
        result = engine.execute("CREATE TABLE Bad (A VARCHAR(10));");
        CHECK(!result.ok);
        CHECK(engine.inTransaction());

        result = engine.execute("ROLLBACK;");
        CHECK(result.ok);
        CHECK(!engine.inTransaction());
    }

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute("COMMIT;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::TransactionError);

        result = engine.execute("ROLLBACK;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::TransactionError);
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

void testReadYourWrites() {
    std::printf("read-your-writes through SQL\n");
    const std::string path = uniquePath("ryw");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "CREATE TABLE Users (Id INT64 NOT NULL, Name TEXT NOT NULL, Enabled "
            "BOOLEAN NOT NULL, Note TEXT NULL);");
        CHECK(result.ok);

        result = engine.execute(
            "BEGIN;\n"
            "INSERT INTO Users VALUES (200, 'Visible', TRUE, NULL);\n"
            "SELECT * FROM Users;\n"
            "ROLLBACK;\n");
        CHECK(result.ok);
        CHECK(result.statements.size() == 4);
        if (result.statements.size() == 4) {
            const SqlResultSet& rs = result.statements[2].resultSet;
            CHECK(rs.rowCount() == 1);
            if (rs.rowCount() == 1) {
                CHECK_INT64(rs.row(0)[0], 200);
                CHECK_TEXT(rs.row(0)[1], "Visible");
            }
        }
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);

    // After rollback and reopen the row must be absent.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            SqlExecutionResult result = engine.execute("SELECT * FROM Users;");
            CHECK(result.ok);
            CHECK(result.statements[0].resultSet.rowCount() == 0);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

void testCreateTableRollbackAndCommit() {
    std::printf("create table transaction integration\n");
    const std::string path = uniquePath("ddltx");

    {
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (!db) return;
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "BEGIN; CREATE TABLE Temp (Id INT64 NOT NULL); ROLLBACK;");
        CHECK(result.ok);
        result = engine.execute("SELECT * FROM Temp;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        result = engine.execute(
            "BEGIN; CREATE TABLE Kept (Id INT64 NOT NULL); COMMIT;");
        CHECK(result.ok);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            std::vector<TableInfo> tables;
            CHECK_STATUS(reopened->listTables(tables), DbStatus::Ok);
            CHECK(tables.size() == 1);
            if (tables.size() == 1) {
                CHECK(tables[0].name == "Kept");
            }
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Multi-statement behavior.

void testMultiStatementBehavior() {
    std::printf("multi-statement execution behavior\n");
    const std::string path = uniquePath("multi");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "CREATE TABLE A (Id INT64 NOT NULL);\n"
            "INSERT INTO Missing VALUES (1);\n"
            "CREATE TABLE B (Id INT64 NOT NULL);\n");
        CHECK(!result.ok);
        CHECK(result.stoppedEarly);
        CHECK(result.statements.size() == 2);
        CHECK(result.statements[0].ok);
        CHECK(!result.statements[1].ok);
        CHECK(result.failedStatementIndex == 1);

        // The first statement committed on its own; the third never ran.
        result = engine.execute("SELECT * FROM A;");
        CHECK(result.ok);
        result = engine.execute("SELECT * FROM B;");
        CHECK(!result.ok);

        // Empty input is valid and produces no statements.
        result = engine.execute("");
        CHECK(result.ok);
        CHECK(result.statements.empty());
        result = engine.execute("   \n\t  ");
        CHECK(result.ok);
        CHECK(result.statements.empty());
        result = engine.execute(";;;;;;;;;;");
        CHECK(result.ok);
        CHECK(result.statements.empty());
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Adversarial input (SQL4 section 30).

void testAdversarialInput() {
    std::printf("adversarial input safety\n");
    const std::string path = uniquePath("adv");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result;

        result = engine.execute(std::string("SELECT ") + std::string(5000, '('));
        CHECK(!result.ok);

        result = engine.execute("CREATE TABLE " + std::string(1000, 'a') + " (Id INT64);");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::ResourceLimit ||
              result.error.code == SqlErrorCode::TokenizerError);

        result = engine.execute("INSERT INTO T VALUES ('" + std::string(70000, 'a') + "');");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::ResourceLimit);

        result = engine.execute("SELECT " + std::string(70000, '9') + " FROM T;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::ResourceLimit);

        std::string punctuation = "!!!@@@###$$$%%%^^^&&&***";
        result = engine.execute(punctuation);
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::TokenizerError);

        // Invalid UTF-8 identifier byte reaches the catalog and is rejected.
        std::string invalidUtf8 = "CREATE TABLE ";
        invalidUtf8.push_back(static_cast<char>(0xFF));
        invalidUtf8 += " (Id INT64);";
        result = engine.execute(invalidUtf8);
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);

        // The database remains usable after every rejection.
        result = engine.execute("CREATE TABLE Ok (Id INT64 NOT NULL);");
        CHECK(result.ok);
        result = engine.execute("INSERT INTO Ok VALUES (1);");
        CHECK(result.ok);
        result = engine.execute("SELECT * FROM Ok;");
        CHECK(result.ok);
        CHECK(result.statements[0].resultSet.rowCount() == 1);
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Multi-page SQL dataset (SQL4 section 45).

void testMultiPageDataset() {
    std::printf("multi-page SQL dataset\n");
    const std::string path = uniquePath("multipage");
    const uint64_t kRows = 600;
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "CREATE TABLE Big (Id INT64 NOT NULL, Tag TEXT NOT NULL, Flag BOOLEAN NOT "
            "NULL);");
        CHECK(result.ok);

        std::string script = "BEGIN;\n";
        for (uint64_t i = 0; i < kRows; ++i) {
            script += "INSERT INTO Big VALUES (";
            script += std::to_string(static_cast<unsigned long long>(i));
            script += ", '";
            script += makeTag(i, 100);
            script += "', ";
            script += (i % 2 == 0) ? "TRUE" : "FALSE";
            script += ");\n";
            if ((i + 1) % 200 == 0) {
                script += "COMMIT;\nBEGIN;\n";
            }
        }
        script += "COMMIT;\n";

        result = engine.execute(script);
        CHECK(result.ok);

        result = engine.execute("SELECT * FROM Big;");
        CHECK(result.ok);
        CHECK(result.statements[0].resultSet.rowCount() == kRows);
        if (result.statements[0].resultSet.rowCount() == kRows) {
            const SqlResultSet& rs = result.statements[0].resultSet;
            bool allOk = true;
            for (uint64_t i = 0; i < kRows; ++i) {
                const std::vector<DbValue>& row = rs.row(static_cast<size_t>(i));
                if (row[0].int64Value() != static_cast<int64_t>(i) ||
                    row[1].textValue() != makeTag(i, 100) ||
                    row[2].booleanValue() != (i % 2 == 0)) {
                    allOk = false;
                    break;
                }
            }
            CHECK(allOk);
        }
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            std::unique_ptr<Table> table;
            CHECK_STATUS(reopened->openTable("Big", table), DbStatus::Ok);
            CHECK(table->rowCount() == kRows);
            CHECK(table->heapPageCount() > 1);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Deterministic SQL dataset (SQL4 section 46).

void testDeterministicDataset() {
    std::printf("deterministic SQL dataset (1000 rows)\n");
    const std::string path = uniquePath("deterministic");
    const uint64_t kRows = 1000;
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "CREATE TABLE Det (Id INT64 NOT NULL, Name TEXT NOT NULL, Enabled BOOLEAN "
            "NOT NULL);");
        CHECK(result.ok);

        std::string script;
        uint64_t inserted = 0;
        while (inserted < kRows) {
            script += "BEGIN;\n";
            for (uint64_t i = 0; i < 250 && inserted < kRows; ++i, ++inserted) {
                script += "INSERT INTO Det VALUES (";
                script += std::to_string(static_cast<unsigned long long>(inserted));
                script += ", 'name";
                script += std::to_string(static_cast<unsigned long long>(inserted));
                script += "', ";
                script += (inserted % 2 == 0) ? "TRUE" : "FALSE";
                script += ");\n";
            }
            script += "COMMIT;\n";
        }

        result = engine.execute(script);
        CHECK(result.ok);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            SqlExecutionResult result = engine.execute("SELECT * FROM Det;");
            CHECK(result.ok);
            const SqlResultSet& rs = result.statements[0].resultSet;
            CHECK(rs.rowCount() == kRows);
            if (rs.rowCount() == kRows) {
                uint64_t mismatches = 0;
                for (uint64_t i = 0; i < kRows; ++i) {
                    const std::vector<DbValue>& row = rs.row(static_cast<size_t>(i));
                    const std::string expectedName =
                        "name" + std::to_string(static_cast<unsigned long long>(i));
                    if (row[0].int64Value() != static_cast<int64_t>(i) ||
                        row[1].textValue() != expectedName ||
                        row[2].booleanValue() != (i % 2 == 0)) {
                        ++mismatches;
                    }
                }
                CHECK(mismatches == 0);
            }
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL/WAL crash integration (SQL4 section 44).

uint64_t measureSqlCommit(const std::string& path, const std::string& sql,
                          std::vector<uint64_t>& boundaries) {
    std::shared_ptr<BudgetState> state(new BudgetState());
    state->recording = true;
    BudgetFileSystem fs(state);
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db, &fs).isOk() || !db) {
        return 0;
    }
    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(sql);
        if (!result.ok) {
            db->close();
            return 0;
        }
    }
    const uint64_t commitEnd = state->written;
    boundaries = state->boundaries;
    db->close();
    return commitEnd;
}

void runCrashedSql(const std::string& path, const std::string& sql,
                   std::shared_ptr<BudgetState> state) {
    BudgetFileSystem fs(state);
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db, &fs).isOk() || !db) {
        return;
    }
    {
        SqlEngine engine(*db);
        engine.execute(sql);
    }
    db->close();
}

void testSqlCrashIntegration() {
    std::printf("SQL-driven crash/WAL integration\n");
    const std::string path = uniquePath("sqlcrash");
    const int kPreRows = 10;
    const int kInsert = 60;

    {
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (!db) return;
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "CREATE TABLE Crash (Id INT64 NOT NULL, Tag TEXT NOT NULL, Flag BOOLEAN "
            "NOT NULL);");
        CHECK(result.ok);
        for (int i = 0; i < kPreRows; ++i) {
            std::string sql = "INSERT INTO Crash VALUES (";
            sql += std::to_string(i);
            sql += ", '";
            sql += makeTag(static_cast<uint64_t>(i), 100);
            sql += "', ";
            sql += (i % 2 == 0) ? "TRUE" : "FALSE";
            sql += ");";
            result = engine.execute(sql);
            CHECK(result.ok);
        }
        CHECK_STATUS(db->flush(), DbStatus::Ok);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    CHECK(verifySequential(path, "Crash", kPreRows));
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    std::string txSql = "BEGIN;\n";
    for (int i = 0; i < kInsert; ++i) {
        const uint64_t id = static_cast<uint64_t>(kPreRows + i);
        txSql += "INSERT INTO Crash VALUES (";
        txSql += std::to_string(static_cast<unsigned long long>(id));
        txSql += ", '";
        txSql += makeTag(id, 100);
        txSql += "', ";
        txSql += (id % 2 == 0) ? "TRUE" : "FALSE";
        txSql += ");\n";
    }
    txSql += "COMMIT;\n";

    const std::string measure = uniquePath("sqlcrashmeasure");
    writeFileBytes(measure, baseBytes);
    removeFile(walPath(measure));
    std::vector<uint64_t> boundaries;
    const uint64_t commitEnd = measureSqlCommit(measure, txSql, boundaries);
    removeFile(walPath(measure));
    removeFile(measure);
    CHECK(commitEnd > kWalHeaderSize);
    CHECK(boundaries.size() > 4);

    std::set<uint64_t> points;
    points.insert(0);
    for (size_t i = 0; i < boundaries.size(); ++i) {
        points.insert(boundaries[i]);
        const uint64_t next =
            (i + 1 < boundaries.size()) ? boundaries[i + 1] : commitEnd;
        if (next > boundaries[i]) {
            points.insert(boundaries[i] + (next - boundaries[i]) / 2);
        }
    }
    points.insert(commitEnd);
    points.insert(commitEnd + 1);

    int tested = 0;
    for (std::set<uint64_t>::const_iterator it = points.begin(); it != points.end();
         ++it) {
        const uint64_t budget = *it;
        const std::string work = uniquePath("sqlcrashwork");
        writeFileBytes(work, baseBytes);
        removeFile(walPath(work));

        std::shared_ptr<BudgetState> state(new BudgetState());
        state->budget = budget;
        runCrashedSql(work, txSql, state);

        std::unique_ptr<Database> db;
        DbResult open = Database::open(work, DatabaseOpenOptions(), db);
        CHECK_STATUS(open, DbStatus::Ok);
        if (open.isOk() && db) {
            db->close();
        }

        const bool committed = budget >= commitEnd;
        const uint64_t expected =
            committed ? static_cast<uint64_t>(kPreRows + kInsert)
                      : static_cast<uint64_t>(kPreRows);
        if (!verifySequential(work, "Crash", expected)) {
            std::printf("  SQL crash boundary %llu failed (expected %llu rows)\n",
                        static_cast<unsigned long long>(budget),
                        static_cast<unsigned long long>(expected));
            CHECK(false);
        } else {
            CHECK(true);
        }
        ++tested;
        removeFile(walPath(work));
        removeFile(work);
    }
    CHECK(tested > 5);

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// CLI-style formatting helper exercised as part of the library test.

void testResultSetMetadata() {
    std::printf("result-set metadata and bounds\n");
    const std::string path = uniquePath("meta");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "CREATE TABLE M (A INT32 NOT NULL, B FLOAT64 NOT NULL, C BLOB NULL, D "
            "BOOL NOT NULL);");
        CHECK(result.ok);
        result = engine.execute("INSERT INTO M VALUES (5, 1.5, X'00', FALSE);");
        CHECK(result.ok);
        result = engine.execute("SELECT A, B, C, D FROM M;");
        CHECK(result.ok);
        const SqlResultSet& rs = result.statements[0].resultSet;
        CHECK(rs.columnCount() == 4);
        CHECK(rs.column(0).type == DbType::Int32);
        CHECK(rs.column(1).type == DbType::Float64);
        CHECK(rs.column(2).type == DbType::Blob);
        CHECK(rs.column(3).type == DbType::Boolean);
        if (rs.rowCount() == 1) {
            CHECK(rs.row(0)[0].int32Value() == 5);
            CHECK(std::fabs(rs.row(0)[1].float64Value() - 1.5) < 1e-9);
            CHECK(rs.row(0)[2].blobValue().size() == 1);
            CHECK_BOOL(rs.row(0)[3], false);
        }
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: tokenizer, parser, precedence (sections 2-6, 48).

std::vector<int64_t> collectIds(const SqlResultSet& rs) {
    std::vector<int64_t> ids;
    for (size_t i = 0; i < rs.rowCount(); ++i) {
        ids.push_back(rs.row(i)[0].int64Value());
    }
    return ids;
}

bool checkIds(SqlEngine& engine, const std::string& sql,
              const std::vector<int64_t>& expected) {
    SqlExecutionResult result = engine.execute(sql);
    if (!result.ok || result.statements.empty()) {
        return false;
    }
    return collectIds(result.statements[0].resultSet) == expected;
}

void testSql5Tokenizer() {
    std::printf("SQL5 tokenizer: predicate keywords and operators\n");
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("WHERE UPDATE SET DELETE ORDER BY ASC DESC LIMIT OFFSET "
                         "AND OR NOT IS NULL TRUE FALSE",
                         tokens));
        CHECK(tokens.size() == 18);
        CHECK(tokens[0].kind == SqlTokenKind::Where);
        CHECK(tokens[1].kind == SqlTokenKind::Update);
        CHECK(tokens[2].kind == SqlTokenKind::Set);
        CHECK(tokens[3].kind == SqlTokenKind::Delete);
        CHECK(tokens[4].kind == SqlTokenKind::Order);
        CHECK(tokens[5].kind == SqlTokenKind::By);
        CHECK(tokens[6].kind == SqlTokenKind::Asc);
        CHECK(tokens[7].kind == SqlTokenKind::Desc);
        CHECK(tokens[8].kind == SqlTokenKind::Limit);
        CHECK(tokens[9].kind == SqlTokenKind::Offset);
        CHECK(tokens[10].kind == SqlTokenKind::And);
        CHECK(tokens[11].kind == SqlTokenKind::Or);
        CHECK(tokens[12].kind == SqlTokenKind::Not);
        CHECK(tokens[13].kind == SqlTokenKind::Is);
        CHECK(tokens[14].kind == SqlTokenKind::Null);
        CHECK(tokens[15].kind == SqlTokenKind::True);
        CHECK(tokens[16].kind == SqlTokenKind::False);
    }
    {
        std::vector<SqlToken> tokens;
        CHECK(tokenizeOk("= <> != < <= > >=", tokens));
        CHECK(tokens.size() == 8);
        CHECK(tokens[0].kind == SqlTokenKind::Eq);
        CHECK(tokens[1].kind == SqlTokenKind::Ne);
        CHECK(tokens[2].kind == SqlTokenKind::Ne);
        CHECK(tokens[3].kind == SqlTokenKind::Lt);
        CHECK(tokens[4].kind == SqlTokenKind::Le);
        CHECK(tokens[5].kind == SqlTokenKind::Gt);
        CHECK(tokens[6].kind == SqlTokenKind::Ge);
    }
    CHECK(tokenizeFails("!", SqlErrorCode::TokenizerError));
    CHECK(tokenizeFails("! =", SqlErrorCode::TokenizerError));
}

void testSql5Parser() {
    std::printf("SQL5 parser: predicates, ORDER BY, LIMIT/OFFSET, UPDATE, DELETE\n");
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT * FROM Users WHERE Id >= 10 ORDER BY Name ASC, Id "
                       "DESC LIMIT 25 OFFSET 50;",
                       statement, error));
        CHECK(statement.type == SqlStatementType::Select);
        CHECK(statement.select.where.present);
        CHECK(statement.select.where.root >= 0);
        CHECK(statement.select.orderBy.size() == 2);
        CHECK(statement.select.orderBy[0].column.name == "Name");
        CHECK(!statement.select.orderBy[0].descending);
        CHECK(statement.select.orderBy[1].column.name == "Id");
        CHECK(statement.select.orderBy[1].descending);
        CHECK(statement.select.hasLimit && statement.select.limit == 25);
        CHECK(statement.select.hasOffset && statement.select.offset == 50);
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("UPDATE Users SET Name = 'Alice Smith', Enabled = TRUE WHERE "
                       "Id = 1;",
                       statement, error));
        CHECK(statement.type == SqlStatementType::Update);
        CHECK(statement.update.table.name == "Users");
        CHECK(statement.update.assignments.size() == 2);
        CHECK(statement.update.assignments[0].column.name == "Name");
        CHECK(statement.update.assignments[0].value.kind == SqlLiteralKind::String);
        CHECK(statement.update.assignments[1].column.name == "Enabled");
        CHECK(statement.update.where.present);
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("UPDATE Users SET Enabled = FALSE;", statement, error));
        CHECK(statement.type == SqlStatementType::Update);
        CHECK(!statement.update.where.present);
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("DELETE FROM Users WHERE Enabled = FALSE;", statement, error));
        CHECK(statement.type == SqlStatementType::Delete);
        CHECK(statement.deleteStatement.table.name == "Users");
        CHECK(statement.deleteStatement.where.present);
        CHECK(parseOne("DELETE FROM TempRows;", statement, error));
        CHECK(statement.type == SqlStatementType::Delete);
        CHECK(!statement.deleteStatement.where.present);
    }
    {
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT * FROM T OFFSET 100;", statement, error));
        CHECK(!statement.select.hasLimit);
        CHECK(statement.select.hasOffset && statement.select.offset == 100);
        CHECK(parseOne("SELECT * FROM T WHERE A IS NOT NULL;", statement, error));
        CHECK(parseOne("SELECT * FROM T WHERE A IS NULL;", statement, error));
    }
}

void testSql5Precedence() {
    std::printf("SQL5 precedence\n");
    {
        // A = 1 OR B = 2 AND C = 3  ==  A = 1 OR (B = 2 AND C = 3)
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT * FROM T WHERE A = 1 OR B = 2 AND C = 3;", statement,
                       error));
        const std::vector<SqlExprNode>& nodes = statement.select.where.nodes;
        const int32_t root = statement.select.where.root;
        CHECK(root >= 0 && nodes[root].kind == SqlExprKind::Or);
        if (root >= 0) {
            const SqlExprNode& orNode = nodes[static_cast<size_t>(root)];
            CHECK(orNode.left >= 0 && nodes[orNode.left].kind == SqlExprKind::Compare);
            CHECK(orNode.right >= 0 && nodes[orNode.right].kind == SqlExprKind::And);
        }
    }
    {
        // NOT A = 1 AND B = 2  ==  (NOT (A = 1)) AND (B = 2)
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT * FROM T WHERE NOT A = 1 AND B = 2;", statement, error));
        const std::vector<SqlExprNode>& nodes = statement.select.where.nodes;
        const int32_t root = statement.select.where.root;
        CHECK(root >= 0 && nodes[root].kind == SqlExprKind::And);
        if (root >= 0) {
            const SqlExprNode& andNode = nodes[static_cast<size_t>(root)];
            CHECK(andNode.left >= 0 && nodes[andNode.left].kind == SqlExprKind::Not);
            CHECK(andNode.right >= 0 && nodes[andNode.right].kind == SqlExprKind::Compare);
        }
    }
    {
        // Parenthesized grouping overrides precedence.
        SqlStatementAst statement;
        SqlError error;
        CHECK(parseOne("SELECT * FROM T WHERE (A = 1 OR B = 2) AND C = 3;", statement,
                       error));
        const std::vector<SqlExprNode>& nodes = statement.select.where.nodes;
        const int32_t root = statement.select.where.root;
        CHECK(root >= 0 && nodes[root].kind == SqlExprKind::And);
        if (root >= 0) {
            const SqlExprNode& andNode = nodes[static_cast<size_t>(root)];
            CHECK(andNode.left >= 0 && nodes[andNode.left].kind == SqlExprKind::Or);
        }
    }
}

void testSql5HostileInput() {
    std::printf("SQL5 hostile and malformed input\n");
    CHECK(parseFails("SELECT * FROM T WHERE;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T WHERE AND;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T WHERE (;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T WHERE A =;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T WHERE A IS;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T WHERE A IS NOT;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T ORDER;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T ORDER BY;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T ORDER BY A,;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T LIMIT;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T LIMIT -1;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T OFFSET;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("SELECT * FROM T OFFSET -5;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("UPDATE;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("UPDATE T;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("UPDATE T SET;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("UPDATE T SET A;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("UPDATE T SET A =;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("DELETE;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("DELETE T;", SqlErrorCode::SyntaxError));
    CHECK(parseFails("DELETE FROM;", SqlErrorCode::SyntaxError));

    // Excessive nesting fails with ResourceLimit, not a stack overflow.
    std::string deep = "SELECT * FROM T WHERE " + std::string(100, '(') + "A = 1" +
                       std::string(100, ')') + ";";
    CHECK(parseFails(deep, SqlErrorCode::ResourceLimit));

    // Excessive expression node count is bounded independently of depth.
    std::string many = "SELECT * FROM T WHERE A = 1";
    for (int i = 0; i < 3000; ++i) {
        many += " OR A = 1";
    }
    many += ";";
    CHECK(parseFails(many, SqlErrorCode::ResourceLimit));

    // Massive assignment list and ORDER BY list are bounded.
    std::string updates = "UPDATE T SET ";
    for (int i = 0; i < 80; ++i) {
        if (i != 0) updates += ", ";
        updates += "A = 1";
    }
    updates += ";";
    CHECK(parseFails(updates, SqlErrorCode::ResourceLimit));

    std::string orders = "SELECT * FROM T ORDER BY ";
    for (int i = 0; i < 80; ++i) {
        if (i != 0) orders += ", ";
        orders += "A";
    }
    orders += ";";
    CHECK(parseFails(orders, SqlErrorCode::ResourceLimit));
}

// ---------------------------------------------------------------------------
// SQL5: three-valued logic (sections 8-10, 37).

void testThreeValuedLogic() {
    std::printf("SQL5 three-valued logic truth tables\n");
    const SqlTruth T = SqlTruth::True;
    const SqlTruth F = SqlTruth::False;
    const SqlTruth U = SqlTruth::Unknown;

    CHECK(sqlTruthAnd(F, U) == F);
    CHECK(sqlTruthAnd(U, F) == F);
    CHECK(sqlTruthAnd(T, U) == U);
    CHECK(sqlTruthAnd(U, T) == U);
    CHECK(sqlTruthAnd(T, T) == T);
    CHECK(sqlTruthAnd(F, F) == F);
    CHECK(sqlTruthAnd(T, F) == F);
    CHECK(sqlTruthAnd(U, U) == U);

    CHECK(sqlTruthOr(T, U) == T);
    CHECK(sqlTruthOr(U, T) == T);
    CHECK(sqlTruthOr(F, U) == U);
    CHECK(sqlTruthOr(U, F) == U);
    CHECK(sqlTruthOr(T, F) == T);
    CHECK(sqlTruthOr(F, F) == F);
    CHECK(sqlTruthOr(U, U) == U);

    CHECK(sqlTruthNot(U) == U);
    CHECK(sqlTruthNot(T) == F);
    CHECK(sqlTruthNot(F) == T);
}

// ---------------------------------------------------------------------------
// SQL5: SELECT predicates (sections 7, 9, 11, 31, 32, 36).

void testSql5Predicates() {
    std::printf("SQL5 SELECT WHERE predicates\n");
    const std::string path = uniquePath("sql5pred");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 NOT NULL, Name TEXT NOT NULL, "
                             "Enabled BOOLEAN NOT NULL, Note TEXT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'Alice', TRUE, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'Bob', FALSE, 'x');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (3, 'Carol', TRUE, 'y');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (4, 'Dave', TRUE, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (5, 'Eve', FALSE, NULL);"));

        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Id = 2;", {2}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Id <> 2;", {1, 3, 4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Id != 2;", {1, 3, 4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Id < 3;", {1, 2}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Id <= 3;", {1, 2, 3}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Id > 3;", {4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Id >= 3;", {3, 4, 5}));

        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Name = 'Alice';", {1}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Name <> 'Alice';",
                       {2, 3, 4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Name < 'C';", {1, 2}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Name >= 'D';", {4, 5}));

        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Enabled = TRUE;", {1, 3, 4}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Enabled <> FALSE;",
                       {1, 3, 4}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Enabled;", {1, 3, 4}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE NOT Enabled;", {2, 5}));

        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Note IS NULL;", {1, 4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Note IS NOT NULL;", {2, 3}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Note = NULL;", {}));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Note <> NULL;", {}));

        CHECK(checkIds(engine,
                       "SELECT Id FROM Users WHERE (Enabled = TRUE AND Id >= 3) OR "
                       "Name = 'Alice';",
                       {1, 3, 4}));
        CHECK(checkIds(engine,
                       "SELECT Id FROM Users WHERE Enabled = TRUE AND (Id < 2 OR Id >= "
                       "4) AND Note IS NOT NULL;",
                       {}));

        // Unknown column and invalid comparison semantics fail before scanning.
        SqlExecutionResult result =
            engine.execute("SELECT * FROM Users WHERE DoesNotExist = 3;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);
        result = engine.execute("SELECT * FROM Users WHERE Enabled < TRUE;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);
        result = engine.execute("SELECT * FROM Users WHERE Name = 3;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

void testSql5NullAndBlobPredicates() {
    std::printf("SQL5 NULL and BLOB predicate semantics\n");
    const std::string path = uniquePath("sql5null");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Notes (Id INT64 NOT NULL, Body TEXT NULL, "
                             "Data BLOB NULL);"));
        CHECK(execOk(engine, "INSERT INTO Notes VALUES (1, NULL, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Notes VALUES (2, 'body', X'01');"));
        CHECK(execOk(engine, "INSERT INTO Notes VALUES (3, NULL, X'02');"));

        CHECK(checkIds(engine, "SELECT Id FROM Notes WHERE Body IS NULL;", {1, 3}));
        CHECK(checkIds(engine, "SELECT Id FROM Notes WHERE Body IS NOT NULL;", {2}));
        CHECK(checkIds(engine, "SELECT Id FROM Notes WHERE Data = X'01';", {2}));
        CHECK(checkIds(engine, "SELECT Id FROM Notes WHERE Data <> X'01';", {3}));
        CHECK(checkIds(engine, "SELECT Id FROM Notes WHERE Data = NULL;", {}));

        SqlExecutionResult result =
            engine.execute("SELECT * FROM Notes WHERE Data < X'01';");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: comparison typing and numeric promotion (section 11).

void testSql5ComparisonTyping() {
    std::printf("SQL5 comparison typing and promotion\n");
    const std::string path = uniquePath("sql5types");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Nums (Id INT64 NOT NULL, Small INT32 NOT NULL, "
                             "Big INT64 NOT NULL, Real FLOAT64 NOT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Nums VALUES (1, 10, 9007199254740992, 1.5);"));
        CHECK(execOk(engine, "INSERT INTO Nums VALUES (2, 20, 9007199254740993, 2.5);"));

        // Int32 vs Int64 promotion.
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Small = 10;", {1}));
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Small < 15;", {1}));
        // An out-of-range Int32 literal is a valid Int64 comparison, not an error.
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Small = 9999999999;", {}));
        // Int64 comparisons are exact.
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Big = 9007199254740992;", {1}));
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Big = 9007199254740993;", {2}));
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Big > 9007199254740992;", {2}));
        // Int64 -> Float64 promotion rounds; 2^53 and 2^53+1 are both 2^53 as
        // doubles, so both rows match an exact 2^53.0 literal.
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Big = 9007199254740992.0;",
                       {1, 2}));
        // Float64 ordering.
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Real > 2.0;", {2}));
        // Mixed Int64 vs Float64 column comparison.
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE Big = Real;", {}));

        // Boolean literal predicates.
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE TRUE;", {1, 2}));
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE FALSE;", {}));
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE NULL;", {}));
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE NULL IS NULL;", {1, 2}));
        CHECK(checkIds(engine, "SELECT Id FROM Nums WHERE NULL IS NOT NULL;", {}));

        // Assignment type rules.
        SqlExecutionResult result =
            engine.execute("UPDATE Nums SET Small = 9999999999 WHERE Id = 1;");
        CHECK(!result.ok && result.error.code == SqlErrorCode::SemanticError);
        result = engine.execute("UPDATE Nums SET Big = 1.5 WHERE Id = 1;");
        CHECK(!result.ok && result.error.code == SqlErrorCode::SemanticError);
        CHECK(execOk(engine, "UPDATE Nums SET Real = 1 WHERE Id = 1;"));
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: ORDER BY (sections 13, 14, 38).

void testSql5OrderBy() {
    std::printf("SQL5 ORDER BY semantics\n");
    const std::string path = uniquePath("sql5order");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Ord (Id INT64 NOT NULL, Name TEXT NOT NULL, "
                             "Score INT32 NULL, Enabled BOOLEAN NOT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Ord VALUES (1, 'b', 10, TRUE);"));
        CHECK(execOk(engine, "INSERT INTO Ord VALUES (2, 'a', 10, FALSE);"));
        CHECK(execOk(engine, "INSERT INTO Ord VALUES (3, 'c', NULL, TRUE);"));
        CHECK(execOk(engine, "INSERT INTO Ord VALUES (4, 'a', -5, TRUE);"));
        CHECK(execOk(engine, "INSERT INTO Ord VALUES (5, '', 10, FALSE);"));
        CHECK(execOk(engine, "INSERT INTO Ord VALUES (6, 'b', NULL, FALSE);"));
        CHECK(execOk(engine, "INSERT INTO Ord VALUES (7, 'a', 3, TRUE);"));

        CHECK(checkIds(engine, "SELECT Id FROM Ord ORDER BY Id;", {1, 2, 3, 4, 5, 6, 7}));
        CHECK(checkIds(engine, "SELECT Id FROM Ord ORDER BY Id ASC;",
                       {1, 2, 3, 4, 5, 6, 7}));
        CHECK(checkIds(engine, "SELECT Id FROM Ord ORDER BY Id DESC;",
                       {7, 6, 5, 4, 3, 2, 1}));
        CHECK(checkIds(engine, "SELECT Id FROM Ord ORDER BY Name ASC;",
                       {5, 2, 4, 7, 1, 6, 3}));
        CHECK(checkIds(engine, "SELECT Id FROM Ord ORDER BY Name ASC, Id DESC;",
                       {5, 7, 4, 2, 6, 1, 3}));
        // ASC: NULLs before non-NULL; DESC: NULLs after non-NULL.
        CHECK(checkIds(engine, "SELECT Id FROM Ord ORDER BY Score ASC;",
                       {3, 6, 4, 7, 1, 2, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM Ord ORDER BY Score DESC;",
                       {1, 2, 5, 7, 4, 3, 6}));
        CHECK(checkIds(engine, "SELECT Id FROM Ord ORDER BY Enabled DESC, Name ASC;",
                       {4, 7, 1, 3, 5, 2, 6}));
        // ORDER BY a column that is not projected.
        {
            SqlExecutionResult r =
                engine.execute("SELECT Name FROM Ord ORDER BY Id DESC;");
            CHECK(r.ok);
            if (r.ok) {
                const SqlResultSet& rs = r.statements[0].resultSet;
                CHECK(rs.columnCount() == 1);
                CHECK(rs.rowCount() == 7);
                if (rs.rowCount() == 7) {
                    CHECK_TEXT(rs.row(0)[0], "a"); // Id 7
                    CHECK_TEXT(rs.row(6)[0], "b"); // Id 1
                }
            }
        }
        // BLOB ordering is rejected.
        CHECK(execOk(engine, "CREATE TABLE Blobs (Id INT64 NOT NULL, Data BLOB NULL);"));
        CHECK(execOk(engine, "INSERT INTO Blobs VALUES (1, X'01');"));
        SqlExecutionResult result =
            engine.execute("SELECT Id FROM Blobs ORDER BY Data;");
        CHECK(!result.ok);
        CHECK(result.error.code == SqlErrorCode::SemanticError);
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: LIMIT / OFFSET (sections 15, 16, 39).

void testSql5LimitOffset() {
    std::printf("SQL5 LIMIT/OFFSET semantics\n");
    const std::string path = uniquePath("sql5limit");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE L (Id INT64 NOT NULL);"));
        for (int i = 1; i <= 5; ++i) {
            CHECK(execOk(engine, "INSERT INTO L VALUES (" + std::to_string(i) + ");"));
        }
        CHECK(checkIds(engine, "SELECT Id FROM L LIMIT 0;", {}));
        CHECK(checkIds(engine, "SELECT Id FROM L LIMIT 1;", {1}));
        CHECK(checkIds(engine, "SELECT Id FROM L LIMIT 10;", {1, 2, 3, 4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM L OFFSET 0;", {1, 2, 3, 4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM L OFFSET 3;", {4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM L LIMIT 2 OFFSET 1;", {2, 3}));
        CHECK(checkIds(engine, "SELECT Id FROM L LIMIT 5 OFFSET 0;", {1, 2, 3, 4, 5}));
        CHECK(checkIds(engine, "SELECT Id FROM L OFFSET 5;", {}));
        CHECK(checkIds(engine, "SELECT Id FROM L OFFSET 6;", {}));
        CHECK(checkIds(engine, "SELECT Id FROM L LIMIT 2 OFFSET 10;", {}));
        // WHERE and ORDER BY are applied before LIMIT/OFFSET.
        CHECK(checkIds(engine, "SELECT Id FROM L WHERE Id > 2 LIMIT 2;", {3, 4}));
        CHECK(checkIds(engine, "SELECT Id FROM L ORDER BY Id DESC LIMIT 2;", {5, 4}));
        CHECK(checkIds(engine, "SELECT Id FROM L ORDER BY Id DESC LIMIT 3 OFFSET 2;",
                       {3, 2, 1}));
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: UPDATE (sections 17, 18, 40).

void testSql5Update() {
    std::printf("SQL5 UPDATE behavior\n");
    const std::string path = uniquePath("sql5update");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 NOT NULL, Name TEXT NOT NULL, "
                             "Enabled BOOLEAN NOT NULL, Note TEXT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'Alice', TRUE, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'Bob', TRUE, 'b');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (3, 'Carol', FALSE, NULL);"));

        uint64_t affected = 0;
        CHECK(execAffected(engine, "UPDATE Users SET Enabled = FALSE WHERE Id = 42;",
                           affected));
        CHECK(affected == 0);

        CHECK(execAffected(engine, "UPDATE Users SET Enabled = FALSE WHERE Id = 1;",
                           affected));
        CHECK(affected == 1);
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Enabled = FALSE;", {1, 3}));

        // Multiple assigned columns.
        CHECK(execAffected(engine,
                           "UPDATE Users SET Name = 'Alice Smith', Note = 'n' WHERE Id "
                           "= 1;",
                           affected));
        CHECK(affected == 1);
        {
            SqlExecutionResult r = engine.execute(
                "SELECT Name, Note FROM Users WHERE Id = 1;");
            CHECK(r.ok);
            if (r.ok && r.statements[0].resultSet.rowCount() == 1) {
                CHECK_TEXT(r.statements[0].resultSet.row(0)[0], "Alice Smith");
                CHECK_TEXT(r.statements[0].resultSet.row(0)[1], "n");
            }
        }

        // Update all rows without WHERE.
        CHECK(execAffected(engine, "UPDATE Users SET Note = NULL;", affected));
        CHECK(affected == 3);
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Note IS NOT NULL;", {}));

        // Validation failures.
        SqlExecutionResult result =
            engine.execute("UPDATE Users SET Missing = 1 WHERE Id = 1;");
        CHECK(!result.ok && result.error.code == SqlErrorCode::SemanticError);
        result = engine.execute("UPDATE Users SET Name = 'x', Name = 'y' WHERE Id = 1;");
        CHECK(!result.ok && result.error.code == SqlErrorCode::SemanticError);
        result = engine.execute("UPDATE Users SET Name = NULL WHERE Id = 1;");
        CHECK(!result.ok && result.error.code == SqlErrorCode::SemanticError);
        result = engine.execute("UPDATE Users SET Id = 'x' WHERE Id = 1;");
        CHECK(!result.ok && result.error.code == SqlErrorCode::SemanticError);
        result = engine.execute("UPDATE Users SET Enabled = 1 WHERE Id = 1;");
        CHECK(!result.ok && result.error.code == SqlErrorCode::SemanticError);
        result = engine.execute("UPDATE Users SET Name = 'x' WHERE Missing = 1;");
        CHECK(!result.ok && result.error.code == SqlErrorCode::SemanticError);

        // Explicit transaction commit and rollback.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "UPDATE Users SET Name = 'Tx' WHERE Id = 2;"));
        {
            SqlExecutionResult r =
                engine.execute("SELECT Name FROM Users WHERE Id = 2;");
            CHECK(r.ok && r.statements[0].resultSet.rowCount() == 1);
            if (r.ok && r.statements[0].resultSet.rowCount() == 1) {
                CHECK_TEXT(r.statements[0].resultSet.row(0)[0], "Tx");
            }
        }
        CHECK(execOk(engine, "ROLLBACK;"));
        CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Name = 'Tx';", {}));

        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "UPDATE Users SET Name = 'Committed' WHERE Id = 2;"));
        CHECK(execOk(engine, "COMMIT;"));
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Name = 'Committed';",
                           {2}));
            std::unique_ptr<Table> table;
            CHECK_STATUS(reopened->openTable("Users", table), DbStatus::Ok);
            CHECK(table->rowCount() == 3);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: DELETE (sections 19, 41).

void testSql5Delete() {
    std::printf("SQL5 DELETE behavior\n");
    const std::string path = uniquePath("sql5delete");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 NOT NULL, Name TEXT NOT NULL, "
                             "Note TEXT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'Alice', NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'Bob', 'b');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (3, 'Carol', NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (4, 'Dave', 'd');"));

        uint64_t affected = 0;
        CHECK(execAffected(engine, "DELETE FROM Users WHERE Id = 99;", affected));
        CHECK(affected == 0);

        CHECK(execAffected(engine, "DELETE FROM Users WHERE Note IS NULL;", affected));
        CHECK(affected == 2);
        CHECK(checkIds(engine, "SELECT Id FROM Users;", {2, 4}));

        // Explicit rollback restores deleted rows.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execAffected(engine, "DELETE FROM Users WHERE Id = 2;", affected));
        CHECK(affected == 1);
        CHECK(checkIds(engine, "SELECT Id FROM Users;", {4}));
        CHECK(execOk(engine, "ROLLBACK;"));
        CHECK(checkIds(engine, "SELECT Id FROM Users;", {2, 4}));

        // Delete all without WHERE, then delete from an already-empty table.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execAffected(engine, "DELETE FROM Users;", affected));
        CHECK(affected == 2);
        CHECK(execOk(engine, "COMMIT;"));
        CHECK(execAffected(engine, "DELETE FROM Users;", affected));
        CHECK(affected == 0);
        CHECK(checkIds(engine, "SELECT Id FROM Users;", {}));
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            std::unique_ptr<Table> table;
            CHECK_STATUS(reopened->openTable("Users", table), DbStatus::Ok);
            CHECK(table->rowCount() == 0);
            // The schema survives deleting every row.
            CHECK(table->columnCount() == 3);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: read-your-writes after UPDATE/DELETE (sections 29, 30).

void testSql5ReadYourWrites() {
    std::printf("SQL5 read-your-writes after UPDATE/DELETE\n");
    const std::string path = uniquePath("sql5ryw");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 NOT NULL, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (7, 'Original');"));

        SqlExecutionResult result = engine.execute(
            "BEGIN;\n"
            "UPDATE Users SET Name = 'Changed' WHERE Id = 7;\n"
            "SELECT Name FROM Users WHERE Id = 7;\n"
            "ROLLBACK;\n");
        CHECK(result.ok);
        CHECK(result.statements.size() == 4);
        if (result.statements.size() == 4) {
            const SqlResultSet& rs = result.statements[2].resultSet;
            CHECK(rs.rowCount() == 1);
            if (rs.rowCount() == 1) {
                CHECK_TEXT(rs.row(0)[0], "Changed");
            }
        }

        result = engine.execute(
            "BEGIN;\n"
            "DELETE FROM Users WHERE Id = 7;\n"
            "SELECT * FROM Users WHERE Id = 7;\n"
            "ROLLBACK;\n");
        CHECK(result.ok);
        CHECK(result.statements.size() == 4);
        if (result.statements.size() == 4) {
            CHECK(result.statements[2].resultSet.rowCount() == 0);
        }
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            CHECK(checkIds(engine, "SELECT Id FROM Users WHERE Name = 'Original';",
                           {7}));
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: variable-width UPDATE relocation stress (sections 23, 33).

void testSql5UpdateRelocation() {
    std::printf("SQL5 variable-width UPDATE relocation\n");
    const std::string path = uniquePath("sql5reloc");

    std::string big;
    while (big.size() < 400) {
        big += "0123456789";
    }

    {
        std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
        CHECK(db != nullptr);
        if (!db) return;
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Reloc (Id INT64 NOT NULL, Body TEXT NOT NULL);"));

        std::string script = "BEGIN;\n";
        for (int i = 0; i < 40; ++i) {
            script += "INSERT INTO Reloc VALUES (";
            script += std::to_string(i);
            script += ", 'small";
            script += std::to_string(i);
            script += "');\n";
        }
        script += "COMMIT;\n";
        CHECK(execOk(engine, script));

        // Same-page rewrite: a small change that still fits the original page.
        uint64_t affected = 0;
        CHECK(execAffected(engine,
                           "UPDATE Reloc SET Body = 'rewritten' WHERE Id = 5;",
                           affected));
        CHECK(affected == 1);
        CHECK(checkIds(engine, "SELECT Id FROM Reloc WHERE Body = 'rewritten';", {5}));

        // Expand half the rows; each expanded row no longer fits its old page.
        affected = 0;
        CHECK(execAffected(engine,
                           "UPDATE Reloc SET Body = '" + big + "' WHERE Id < 20;",
                           affected));
        CHECK(affected == 20);

        // Commit path: exactly one version per row, count unchanged.
        std::unique_ptr<Table> table;
        CHECK_STATUS(db->openTable("Reloc", table), DbStatus::Ok);
        CHECK(table->rowCount() == 40);
        CHECK(table->heapPageCount() > 1);

        SqlExecutionResult r = engine.execute("SELECT Body FROM Reloc WHERE Id = 5;");
        CHECK(r.ok && r.statements[0].resultSet.rowCount() == 1);
        if (r.ok && r.statements[0].resultSet.rowCount() == 1) {
            CHECK_TEXT(r.statements[0].resultSet.row(0)[0], big);
        }
        CHECK(checkIds(engine, "SELECT Id FROM Reloc WHERE Body = 'small39';", {39}));
        CHECK(checkIds(engine,
                       "SELECT Id FROM Reloc WHERE Body = '" + big + "' ORDER BY Id;",
                       {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                        19}));

        // Rollback path: original values restored.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execAffected(engine,
                           "UPDATE Reloc SET Body = 'rolled' WHERE Id >= 20;",
                           affected));
        CHECK(affected == 20);
        CHECK(execOk(engine, "ROLLBACK;"));
        CHECK(checkIds(engine, "SELECT Id FROM Reloc WHERE Body = 'rolled';", {}));
        CHECK(checkIds(engine, "SELECT Id FROM Reloc WHERE Body = 'small39';", {39}));

        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            CHECK(checkIds(engine,
                           "SELECT Id FROM Reloc WHERE Body = '" + big +
                               "' ORDER BY Id;",
                           {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                            18, 19}));
            std::unique_ptr<Table> table;
            CHECK_STATUS(reopened->openTable("Reloc", table), DbStatus::Ok);
            CHECK(table->rowCount() == 40);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: DELETE compaction stress (sections 22, 34).

void testSql5DeleteCompaction() {
    std::printf("SQL5 multi-page DELETE compaction\n");
    const std::string path = uniquePath("sql5compact");
    const int kRows = 200;
    std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Del (Id INT64 NOT NULL, Tag TEXT NOT NULL);"));
        std::string script = "BEGIN;\n";
        for (int i = 0; i < kRows; ++i) {
            script += "INSERT INTO Del VALUES (";
            script += std::to_string(i);
            script += ", 't";
            script += std::to_string(i);
            script += "');\n";
        }
        script += "COMMIT;\n";
        CHECK(execOk(engine, script));

        std::unique_ptr<Table> table;
        CHECK_STATUS(db->openTable("Del", table), DbStatus::Ok);
        CHECK(table->heapPageCount() > 2);

        uint64_t affected = 0;
        CHECK(execAffected(engine, "DELETE FROM Del WHERE Id = 0;", affected));
        CHECK(affected == 1);
        CHECK(execAffected(engine, "DELETE FROM Del WHERE Id = 199;", affected));
        CHECK(affected == 1);

        // Delete every second row.
        std::string evens = "DELETE FROM Del WHERE ";
        bool first = true;
        for (int i = 2; i < kRows - 1; i += 2) {
            if (!first) evens += " OR ";
            first = false;
            evens += "Id = " + std::to_string(i);
        }
        evens += ";";
        CHECK(execAffected(engine, evens, affected));
        CHECK(affected == 99);

        // Delete a contiguous range spanning multiple pages.
        CHECK(execAffected(engine, "DELETE FROM Del WHERE Id >= 100 AND Id < 150;",
                           affected));
        CHECK(affected == 25);
        CHECK(checkIds(engine, "SELECT Id FROM Del WHERE Id = 100;", {}));
        CHECK(checkIds(engine, "SELECT Id FROM Del WHERE Id = 101;", {}));
        CHECK(checkIds(engine, "SELECT Id FROM Del WHERE Id = 99;", {99}));
        CHECK(checkIds(engine, "SELECT Id FROM Del WHERE Id = 151;", {151}));

        // Delete all remaining rows.
        CHECK(execAffected(engine, "DELETE FROM Del;", affected));
        CHECK(affected > 0);
        CHECK(checkIds(engine, "SELECT Id FROM Del;", {}));

        // Empty table remains queryable.
        CHECK(execAffected(engine, "DELETE FROM Del;", affected));
        CHECK(affected == 0);
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            std::unique_ptr<Table> table;
            CHECK_STATUS(reopened->openTable("Del", table), DbStatus::Ok);
            CHECK(table->rowCount() == 0);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: statement-failure atomicity inside an explicit transaction
// (sections 26, 42).

void testSql5StatementAtomicity() {
    std::printf("SQL5 statement-failure atomicity (explicit transaction)\n");
    const std::string path = uniquePath("sql5atomic");
    const uint64_t kRows = 4200;
    std::string big;
    while (big.size() < 400) {
        big += "abcdefghij";
    }
    std::string big2;
    while (big2.size() < 400) {
        big2 += "ZYXWVUTSRQ";
    }

    std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Atomic (Id INT64 NOT NULL, Tag TEXT NOT NULL);"));

        uint64_t inserted = 0;
        while (inserted < kRows) {
            std::string script = "BEGIN;\n";
            for (uint64_t i = 0; i < 250 && inserted < kRows; ++i, ++inserted) {
                script += "INSERT INTO Atomic VALUES (";
                script += std::to_string(static_cast<unsigned long long>(inserted));
                script += ", '" + big + "');\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }

        std::unique_ptr<Table> table;
        CHECK_STATUS(db->openTable("Atomic", table), DbStatus::Ok);
        CHECK(table->heapPageCount() > 4096);

        // Prior transaction work, then a mutation that must fail partway when
        // it exceeds the transaction page limit.
        SqlExecutionResult result = engine.execute(
            "BEGIN;\n"
            "INSERT INTO Atomic VALUES (999999, 'prior');\n"
            "UPDATE Atomic SET Tag = '" +
            big2 + "' WHERE Id >= 0;\n");
        CHECK(!result.ok);
        CHECK(result.statements.size() == 3);
        if (result.statements.size() == 3) {
            CHECK(result.statements[0].ok);  // BEGIN
            CHECK(result.statements[1].ok);  // INSERT
            CHECK(!result.statements[2].ok); // UPDATE
            CHECK(result.statements[2].error.code == SqlErrorCode::ResourceLimit);
        }
        CHECK(engine.inTransaction());

        // The prior INSERT is visible; the partial UPDATE is fully rolled back.
        {
            SqlExecutionResult r =
                engine.execute("SELECT Tag FROM Atomic WHERE Id = 999999;");
            CHECK(r.ok && r.statements[0].resultSet.rowCount() == 1);
            if (r.ok && r.statements[0].resultSet.rowCount() == 1) {
                CHECK_TEXT(r.statements[0].resultSet.row(0)[0], "prior");
            }
        }
        // The UPDATE would have written big2 into many pages before failing;
        // after the statement rollback no big2 value may survive, every
        // original value must be intact, and the row count must be exact.
        {
            bool ok = false;
            CHECK(execRowCount(engine, "SELECT Id FROM Atomic WHERE Tag = '" + big2 +
                                           "';",
                               ok) == 0);
            CHECK(ok);
        }
        {
            bool ok = false;
            CHECK(execRowCount(engine, "SELECT Id FROM Atomic WHERE Tag = '" + big +
                                           "';",
                               ok) == kRows);
            CHECK(ok);
        }
        std::unique_ptr<Table> afterFail;
        CHECK_STATUS(db->openTable("Atomic", afterFail), DbStatus::Ok);
        CHECK(afterFail->rowCount() == kRows + 1);

        CHECK(execOk(engine, "COMMIT;"));
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            std::unique_ptr<Table> table;
            CHECK_STATUS(reopened->openTable("Atomic", table), DbStatus::Ok);
            CHECK(table->rowCount() == kRows + 1);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: SQL-driven crash/WAL matrices (sections 43-45).

void testSql5CrashUpdate() {
    std::printf("SQL5 crash/WAL matrix: multi-page UPDATE\n");
    const std::string path = uniquePath("sql5crashupd");
    const uint64_t kRows = 120;
    const std::string baseTag = makeTag(1, 80);
    std::string newTag = makeTag(2, 200);

    CHECK(buildTagTable(path, kRows, baseTag));
    CHECK(verifyTagTable(path, baseTag, kRows));
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    const std::string txSql = "BEGIN;\nUPDATE Crash SET Tag = '" + newTag +
                              "' WHERE Id >= 0;\nCOMMIT;\n";

    const std::string measure = uniquePath("sql5crashupdmeasure");
    writeFileBytes(measure, baseBytes);
    removeFile(walPath(measure));
    std::vector<uint64_t> boundaries;
    const uint64_t commitEnd = measureSqlCommit(measure, txSql, boundaries);
    removeFile(walPath(measure));
    removeFile(measure);
    CHECK(commitEnd > kWalHeaderSize);
    CHECK(boundaries.size() > 4);

    std::set<uint64_t> points;
    points.insert(0);
    for (size_t i = 0; i < boundaries.size(); ++i) {
        points.insert(boundaries[i]);
        const uint64_t next =
            (i + 1 < boundaries.size()) ? boundaries[i + 1] : commitEnd;
        if (next > boundaries[i]) {
            points.insert(boundaries[i] + (next - boundaries[i]) / 2);
        }
    }
    points.insert(commitEnd);
    points.insert(commitEnd + 1);

    int tested = 0;
    for (std::set<uint64_t>::const_iterator it = points.begin(); it != points.end();
         ++it) {
        const uint64_t budget = *it;
        const std::string work = uniquePath("sql5crashupdwork");
        writeFileBytes(work, baseBytes);
        removeFile(walPath(work));

        std::shared_ptr<BudgetState> state(new BudgetState());
        state->budget = budget;
        runCrashedSql(work, txSql, state);

        std::unique_ptr<Database> db;
        DbResult open = Database::open(work, DatabaseOpenOptions(), db);
        CHECK_STATUS(open, DbStatus::Ok);
        if (open.isOk() && db) {
            db->close();
        }

        const bool committed = budget >= commitEnd;
        const std::string expected = committed ? newTag : baseTag;
        if (!verifyTagTable(work, expected, kRows)) {
            std::printf("  SQL5 UPDATE crash boundary %llu failed\n",
                        static_cast<unsigned long long>(budget));
            CHECK(false);
        } else {
            CHECK(true);
        }
        ++tested;
        removeFile(walPath(work));
        removeFile(work);
    }
    CHECK(tested > 5);

    removeFile(walPath(path));
    removeFile(path);
}

void testSql5CrashDelete() {
    std::printf("SQL5 crash/WAL matrix: multi-page DELETE\n");
    const std::string path = uniquePath("sql5crashdel");
    const uint64_t kRows = 120;
    const std::string baseTag = makeTag(3, 80);

    CHECK(buildTagTable(path, kRows, baseTag));
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    const std::string txSql = "BEGIN;\nDELETE FROM Crash WHERE Id >= 0;\nCOMMIT;\n";

    const std::string measure = uniquePath("sql5crashdelmeasure");
    writeFileBytes(measure, baseBytes);
    removeFile(walPath(measure));
    std::vector<uint64_t> boundaries;
    const uint64_t commitEnd = measureSqlCommit(measure, txSql, boundaries);
    removeFile(walPath(measure));
    removeFile(measure);
    CHECK(commitEnd > kWalHeaderSize);

    std::set<uint64_t> points;
    points.insert(0);
    for (size_t i = 0; i < boundaries.size(); ++i) {
        points.insert(boundaries[i]);
        const uint64_t next =
            (i + 1 < boundaries.size()) ? boundaries[i + 1] : commitEnd;
        if (next > boundaries[i]) {
            points.insert(boundaries[i] + (next - boundaries[i]) / 2);
        }
    }
    points.insert(commitEnd);
    points.insert(commitEnd + 1);

    int tested = 0;
    for (std::set<uint64_t>::const_iterator it = points.begin(); it != points.end();
         ++it) {
        const uint64_t budget = *it;
        const std::string work = uniquePath("sql5crashdelwork");
        writeFileBytes(work, baseBytes);
        removeFile(walPath(work));

        std::shared_ptr<BudgetState> state(new BudgetState());
        state->budget = budget;
        runCrashedSql(work, txSql, state);

        std::unique_ptr<Database> db;
        DbResult open = Database::open(work, DatabaseOpenOptions(), db);
        CHECK_STATUS(open, DbStatus::Ok);
        if (open.isOk() && db) {
            db->close();
        }

        const bool committed = budget >= commitEnd;
        const uint64_t expected = committed ? 0 : kRows;
        if (!verifyTagTable(work, baseTag, expected)) {
            std::printf("  SQL5 DELETE crash boundary %llu failed\n",
                        static_cast<unsigned long long>(budget));
            CHECK(false);
        } else {
            CHECK(true);
        }
        ++tested;
        removeFile(walPath(work));
        removeFile(work);
    }
    CHECK(tested > 5);

    removeFile(walPath(path));
    removeFile(path);
}

bool verifyCombinedCrashState(const std::string& path, bool post) {
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db).isOk() || !db) {
        return false;
    }
    std::unique_ptr<Table> table;
    if (!db->openTable("Crash", table).isOk()) {
        db->close();
        return false;
    }
    const uint64_t expected = post ? 60 : 120;
    if (table->rowCount() != expected) {
        db->close();
        return false;
    }
    std::unique_ptr<TableScan> scan;
    table->scanStart(scan);
    std::vector<DbValue> row;
    uint64_t seen = 0;
    bool ok = true;
    while (scan->next(row)) {
        const int64_t id = row[0].int64Value();
        const std::string& tag = row[1].textValue();
        if (post) {
            if (id % 2 != 0) {
                ok = false;
                break;
            }
            const std::string expectedTag = (id < 60) ? "live" : "inactive";
            if (tag != expectedTag) {
                ok = false;
                break;
            }
        } else if (tag != "live") {
            ok = false;
            break;
        }
        ++seen;
    }
    if (seen != expected || !scan->status().isOk()) {
        ok = false;
    }
    db->close();
    return ok;
}

void testSql5CrashCombined() {
    std::printf("SQL5 crash/WAL matrix: UPDATE + DELETE transaction\n");
    const std::string path = uniquePath("sql5crashcombo");
    const uint64_t kRows = 120;
    const std::string baseTag = "live";

    CHECK(buildTagTable(path, kRows, baseTag));
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    const std::string txSql =
        "BEGIN;\n"
        "UPDATE Crash SET Tag = 'inactive' WHERE Id >= 60;\n"
        "DELETE FROM Crash WHERE Flag = FALSE;\n"
        "COMMIT;\n";

    const std::string measure = uniquePath("sql5crashcombomeasure");
    writeFileBytes(measure, baseBytes);
    removeFile(walPath(measure));
    std::vector<uint64_t> boundaries;
    const uint64_t commitEnd = measureSqlCommit(measure, txSql, boundaries);
    removeFile(walPath(measure));
    removeFile(measure);
    CHECK(commitEnd > kWalHeaderSize);

    std::set<uint64_t> points;
    points.insert(0);
    for (size_t i = 0; i < boundaries.size(); ++i) {
        points.insert(boundaries[i]);
        const uint64_t next =
            (i + 1 < boundaries.size()) ? boundaries[i + 1] : commitEnd;
        if (next > boundaries[i]) {
            points.insert(boundaries[i] + (next - boundaries[i]) / 2);
        }
    }
    points.insert(commitEnd);
    points.insert(commitEnd + 1);

    int tested = 0;
    for (std::set<uint64_t>::const_iterator it = points.begin(); it != points.end();
         ++it) {
        const uint64_t budget = *it;
        const std::string work = uniquePath("sql5crashcombowork");
        writeFileBytes(work, baseBytes);
        removeFile(walPath(work));

        std::shared_ptr<BudgetState> state(new BudgetState());
        state->budget = budget;
        runCrashedSql(work, txSql, state);

        std::unique_ptr<Database> db;
        DbResult open = Database::open(work, DatabaseOpenOptions(), db);
        CHECK_STATUS(open, DbStatus::Ok);
        if (open.isOk() && db) {
            db->close();
        }

        const bool committed = budget >= commitEnd;
        if (!verifyCombinedCrashState(work, committed)) {
            std::printf("  SQL5 combined crash boundary %llu failed\n",
                        static_cast<unsigned long long>(budget));
            CHECK(false);
        } else {
            CHECK(true);
        }
        ++tested;
        removeFile(walPath(work));
        removeFile(work);
    }
    CHECK(tested > 5);

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: large deterministic dataset (section 46).

struct AppModelRow {
    int64_t id;
    std::string name;
    bool enabled;
    bool hasScore;
    int32_t score;
    bool alive;

    AppModelRow() : id(0), name(), enabled(false), hasScore(false), score(0), alive(true) {}
};

bool checkAppResult(const SqlResultSet& rs, const std::vector<AppModelRow>& model) {
    size_t index = 0;
    for (size_t i = 0; i < model.size(); ++i) {
        if (!model[i].alive) {
            continue;
        }
        if (index >= rs.rowCount()) {
            return false;
        }
        const std::vector<DbValue>& row = rs.row(index);
        if (row[0].int64Value() != model[i].id || row[1].textValue() != model[i].name ||
            row[2].booleanValue() != model[i].enabled) {
            return false;
        }
        if (model[i].hasScore) {
            if (row[3].isNull() || row[3].int32Value() != model[i].score) {
                return false;
            }
        } else if (!row[3].isNull()) {
            return false;
        }
        ++index;
    }
    return index == rs.rowCount();
}

void testSql5DeterministicDataset() {
    std::printf("SQL5 deterministic dataset (2000 rows)\n");
    const std::string path = uniquePath("sql5det");
    const int kRows = 2000;
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    std::vector<AppModelRow> model;
    model.reserve(kRows);

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE App (Id INT64 NOT NULL, Name TEXT NOT NULL, "
                             "Enabled BOOLEAN NOT NULL, Score INT32 NULL);"));

        uint64_t inserted = 0;
        while (inserted < static_cast<uint64_t>(kRows)) {
            std::string script = "BEGIN;\n";
            for (int i = 0; i < 250 && inserted < static_cast<uint64_t>(kRows);
                 ++i, ++inserted) {
                AppModelRow row;
                row.id = static_cast<int64_t>(inserted);
                row.name = "n" + std::to_string(static_cast<unsigned long long>(inserted));
                row.enabled = (inserted % 3 != 0);
                row.hasScore = (inserted % 5 != 0);
                row.score = static_cast<int32_t>(inserted);
                model.push_back(row);

                script += "INSERT INTO App VALUES (";
                script += std::to_string(static_cast<unsigned long long>(inserted));
                script += ", '" + row.name + "', ";
                script += row.enabled ? "TRUE" : "FALSE";
                script += ", ";
                script += row.hasScore
                              ? std::to_string(static_cast<long long>(row.score))
                              : "NULL";
                script += ");\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }

        // ORDER BY + LIMIT/OFFSET.
        {
            SqlExecutionResult r = engine.execute(
                "SELECT Id, Name, Enabled, Score FROM App WHERE Enabled = TRUE ORDER "
                "BY Name ASC LIMIT 10 OFFSET 5;");
            CHECK(r.ok);
            if (r.ok) {
                const SqlResultSet& rs = r.statements[0].resultSet;
                std::vector<AppModelRow> expected;
                for (size_t i = 0; i < model.size(); ++i) {
                    if (model[i].enabled) expected.push_back(model[i]);
                }
                std::stable_sort(expected.begin(), expected.end(),
                                 [](const AppModelRow& a, const AppModelRow& b) {
                                     return a.name < b.name;
                                 });
                CHECK(rs.rowCount() == 10);
                bool match = true;
                for (size_t i = 0; i < rs.rowCount() && i + 5 < expected.size(); ++i) {
                    if (rs.row(i)[0].int64Value() != expected[i + 5].id) {
                        match = false;
                        break;
                    }
                }
                CHECK(match);
            }
        }

        // UPDATE a range, then verify affected count and model.
        uint64_t affected = 0;
        CHECK(execAffected(engine,
                           "UPDATE App SET Enabled = FALSE WHERE Id >= 1000 AND Id < "
                           "1500;",
                           affected));
        CHECK(affected == 500);
        for (int i = 1000; i < 1500; ++i) {
            model[static_cast<size_t>(i)].enabled = false;
        }

        // DELETE rows with NULL score.
        CHECK(execAffected(engine, "DELETE FROM App WHERE Score IS NULL;", affected));
        uint64_t expectedDeleted = 0;
        for (size_t i = 0; i < model.size(); ++i) {
            if (model[i].alive && !model[i].hasScore) {
                model[i].alive = false;
                ++expectedDeleted;
            }
        }
        CHECK(affected == expectedDeleted);

        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            SqlExecutionResult r =
                engine.execute("SELECT Id, Name, Enabled, Score FROM App ORDER BY Id;");
            CHECK(r.ok);
            if (r.ok) {
                CHECK(checkAppResult(r.statements[0].resultSet, model));
            }
            std::unique_ptr<Table> table;
            CHECK_STATUS(reopened->openTable("App", table), DbStatus::Ok);
            uint64_t alive = 0;
            for (size_t i = 0; i < model.size(); ++i) {
                if (model[i].alive) ++alive;
            }
            CHECK(table->rowCount() == alive);
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: repeated transactional workload (section 47).

void testSql5RepeatedWorkload() {
    std::printf("SQL5 repeated transactional workload (250 lifecycles)\n");
    const std::string path = uniquePath("sql5repeat");
    const int kRows = 200;
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    std::vector<std::string> value(static_cast<size_t>(kRows));
    std::vector<bool> alive(static_cast<size_t>(kRows), true);

    {
        std::unique_ptr<SqlEngine> engine(new SqlEngine(*db));
        CHECK(execOk(*engine,
                     "CREATE TABLE Loop (Id INT64 NOT NULL, Value TEXT NOT NULL, "
                     "Flag BOOLEAN NOT NULL);"));
        std::string script = "BEGIN;\n";
        for (int i = 0; i < kRows; ++i) {
            value[static_cast<size_t>(i)] = "v" + std::to_string(i);
            script += "INSERT INTO Loop VALUES (";
            script += std::to_string(i);
            script += ", '";
            script += value[static_cast<size_t>(i)];
            script += "', ";
            script += (i % 2 == 0) ? "TRUE" : "FALSE";
            script += ");\n";
        }
        script += "COMMIT;\n";
        CHECK(execOk(*engine, script));

        uint64_t mismatches = 0;
        for (int k = 0; k < 250; ++k) {
            const int id = (k * 7) % kRows;
            const int mode = k % 4;
            uint64_t affected = 0;
            if (mode == 0) {
                const std::string newValue = "u" + std::to_string(k);
                CHECK(execOk(*engine, "BEGIN;"));
                CHECK(execAffected(*engine,
                                   "UPDATE Loop SET Value = '" + newValue +
                                       "' WHERE Id = " + std::to_string(id) + ";",
                                   affected));
                CHECK(execOk(*engine, "COMMIT;"));
                if (alive[static_cast<size_t>(id)]) {
                    if (affected != 1) ++mismatches;
                    value[static_cast<size_t>(id)] = newValue;
                } else if (affected != 0) {
                    ++mismatches;
                }
            } else if (mode == 1) {
                CHECK(execOk(*engine, "BEGIN;"));
                CHECK(execAffected(*engine,
                                   "UPDATE Loop SET Value = 'rollback' WHERE Id = " +
                                       std::to_string(id) + ";",
                                   affected));
                CHECK(execOk(*engine, "ROLLBACK;"));
                if (alive[static_cast<size_t>(id)] && affected != 1) {
                    ++mismatches;
                }
                if (!alive[static_cast<size_t>(id)] && affected != 0) {
                    ++mismatches;
                }
            } else if (mode == 2) {
                CHECK(execOk(*engine, "BEGIN;"));
                CHECK(execAffected(*engine,
                                   "DELETE FROM Loop WHERE Id = " +
                                       std::to_string(id) + ";",
                                   affected));
                CHECK(execOk(*engine, "COMMIT;"));
                if (alive[static_cast<size_t>(id)]) {
                    if (affected != 1) ++mismatches;
                    alive[static_cast<size_t>(id)] = false;
                } else if (affected != 0) {
                    ++mismatches;
                }
            } else {
                CHECK(execOk(*engine, "BEGIN;"));
                CHECK(execAffected(*engine,
                                   "DELETE FROM Loop WHERE Id = " +
                                       std::to_string(id) + ";",
                                   affected));
                CHECK(execOk(*engine, "ROLLBACK;"));
                if (alive[static_cast<size_t>(id)] && affected != 1) {
                    ++mismatches;
                }
            }

            // Read-your-writes check for this row.
            SqlExecutionResult r = engine->execute(
                "SELECT Value FROM Loop WHERE Id = " + std::to_string(id) + ";");
            if (!r.ok) {
                ++mismatches;
            } else if (alive[static_cast<size_t>(id)]) {
                if (r.statements[0].resultSet.rowCount() != 1 ||
                    r.statements[0].resultSet.row(0)[0].textValue() !=
                        value[static_cast<size_t>(id)]) {
                    ++mismatches;
                }
            } else if (r.statements[0].resultSet.rowCount() != 0) {
                ++mismatches;
            }

            if (k % 50 == 49) {
                engine.reset();
                CHECK_STATUS(db->close(), DbStatus::Ok);
                std::unique_ptr<Database> reopened;
                CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened),
                             DbStatus::Ok);
                if (!reopened) {
                    ++mismatches;
                    return;
                }
                db = std::move(reopened);
                engine.reset(new SqlEngine(*db));
            }
        }
        CHECK(mismatches == 0);
        engine.reset();
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// SQL5: CLI-style example workload (section 56).

void testSql5ExampleWorkload() {
    std::printf("SQL5 end-to-end example workload\n");
    const std::string path = uniquePath("sql5example");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(
            "CREATE TABLE Users (Id INT64 NOT NULL, Name TEXT NOT NULL, Enabled "
            "BOOLEAN NOT NULL, Note TEXT NULL);\n"
            "BEGIN;\n"
            "INSERT INTO Users VALUES (1, 'Alice', TRUE, NULL);\n"
            "INSERT INTO Users VALUES (2, 'Bob', TRUE, 'temporary');\n"
            "INSERT INTO Users VALUES (3, 'Carol', FALSE, NULL);\n"
            "INSERT INTO Users VALUES (4, 'Dave', TRUE, 'keep');\n"
            "COMMIT;\n"
            "SELECT Id, Name FROM Users WHERE Enabled = TRUE ORDER BY Name ASC;\n"
            "UPDATE Users SET Enabled = FALSE WHERE Name = 'Bob';\n"
            "DELETE FROM Users WHERE Enabled = FALSE;\n"
            "SELECT * FROM Users ORDER BY Id LIMIT 10 OFFSET 0;\n");
        CHECK(result.ok);
        CHECK(result.statements.size() == 11);
        if (result.statements.size() == 11) {
            CHECK(collectIds(result.statements[7].resultSet) ==
                  std::vector<int64_t>({1, 2, 4})); // SELECT Enabled=TRUE ORDER BY Name
            CHECK(result.statements[8].affectedRows == 1);  // UPDATE Bob
            CHECK(result.statements[9].affectedRows == 2);  // DELETE disabled (Bob, Carol)
            // Final SELECT after delete: Alice and Dave remain.
            const SqlResultSet& rs = result.statements[10].resultSet;
            CHECK(rs.rowCount() == 2);
            if (rs.rowCount() == 2) {
                CHECK_INT64(rs.row(0)[0], 1);
                CHECK_TEXT(rs.row(0)[1], "Alice");
                CHECK_INT64(rs.row(1)[0], 4);
                CHECK_TEXT(rs.row(1)[1], "Dave");
            }
        }
    }

    CHECK_STATUS(db->close(), DbStatus::Ok);

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            SqlEngine engine(*reopened);
            CHECK(checkIds(engine, "SELECT Id FROM Users ORDER BY Id;", {1, 4}));
            CHECK_STATUS(reopened->close(), DbStatus::Ok);
        }
    }

    removeFile(walPath(path));
    removeFile(path);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("guideXOS SQL4/SQL5 SQL language tests\n");
    std::string dir = testDir();
    std::printf("temp dir: %s\n", dir.c_str());

    std::string cmd = "del /q \"" + dir + "\\*.gxdb\" >nul 2>nul";
    std::system(cmd.c_str());
    cmd = "del /q \"" + dir + "\\*.gxwal\" >nul 2>nul";
    std::system(cmd.c_str());

    testTokenizerKeywords();
    testTokenizerLiterals();
    testTokenizerMalformed();
    testParserSuccess();
    testParserFailures();
    testSemanticValidation();
    testEndToEndScript();
    testExplicitTransactions();
    testTransactionErrors();
    testReadYourWrites();
    testCreateTableRollbackAndCommit();
    testMultiStatementBehavior();
    testAdversarialInput();
    testResultSetMetadata();
    testMultiPageDataset();
    testDeterministicDataset();
    testSqlCrashIntegration();

    // ---- SQL5 ----------------------------------------------------------
    testSql5Tokenizer();
    testSql5Parser();
    testSql5Precedence();
    testSql5HostileInput();
    testThreeValuedLogic();
    testSql5Predicates();
    testSql5NullAndBlobPredicates();
    testSql5ComparisonTyping();
    testSql5OrderBy();
    testSql5LimitOffset();
    testSql5Update();
    testSql5Delete();
    testSql5ReadYourWrites();
    testSql5UpdateRelocation();
    testSql5DeleteCompaction();
    testSql5StatementAtomicity();
    testSql5CrashUpdate();
    testSql5CrashDelete();
    testSql5CrashCombined();
    testSql5DeterministicDataset();
    testSql5RepeatedWorkload();
    testSql5ExampleWorkload();

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
