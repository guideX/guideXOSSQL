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

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("guideXOS SQL4 SQL language tests\n");
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

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
