// guideXOS SQL -- Phase SQL8
// Hosted acceptance tests for schema lifecycle and relational integrity:
// column DEFAULT metadata, INSERT column lists, the DEFAULT keyword,
// single-column FOREIGN KEY constraints (RESTRICT/NO ACTION), FK support
// indexes, DROP INDEX, DROP TABLE, ALTER TABLE ADD COLUMN, catalog v4,
// statement-final-state FK checking, crash consistency, corruption safety and
// a deterministic referential workload.
//
// Same self-contained harness style as the SQL1-SQL7 suites. Every case writes
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

void checkNull(const DbValue& value, const char* what, int line) {
    if (value.isNull()) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- expected NULL\n", line, what);
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

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)
#define CHECK_STATUS(expr, expected) checkStatus((expr), (expected), #expr, __LINE__)
#define CHECK_INT64(value, expected) checkInt64((value), (expected), #value, __LINE__)
#define CHECK_INTVAL(value, expected) checkIntValue((value), (expected), #value, __LINE__)
#define CHECK_TEXT(value, expected) checkText((value), (expected), #value, __LINE__)
#define CHECK_BOOL(value, expected) checkBool((value), (expected), #value, __LINE__)
#define CHECK_NULL(value) checkNull((value), #value, __LINE__)

std::string testDir() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    std::string dir = std::string(base) + "/gxos_gxdb_sql8_tests";
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

bool execOk(SqlEngine& engine, const std::string& sql) {
    SqlExecutionResult result = engine.execute(sql);
    if (!result.ok) {
        std::printf("    SQL failed: %s\n", result.error.describe().c_str());
    }
    return result.ok;
}

bool execFails(SqlEngine& engine, const std::string& sql) {
    return !engine.execute(sql).ok;
}

bool execFailsCode(SqlEngine& engine, const std::string& sql, SqlErrorCode code) {
    SqlExecutionResult result = engine.execute(sql);
    return !result.ok && result.error.code == code;
}

uint64_t execRowCount(SqlEngine& engine, const std::string& sql, bool& ok) {
    SqlExecutionResult result = engine.execute(sql);
    ok = result.ok && !result.statements.empty();
    if (!ok) return 0;
    return result.statements[0].resultSet.rowCount();
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

bool parseFails(const std::string& sql) {
    SqlStatementAst statement;
    SqlError error;
    return !parseOne(sql, statement, error);
}

// ---- Index / integrity validation -----------------------------------------

bool validateIndexAgainstHeap(Database& db, uint32_t indexId) {
    const Catalog::IndexRecord* rec = db.catalog().findIndex(indexId);
    if (rec == nullptr) return false;
    const Catalog::TableRecord* tableRec = db.catalog().findTable(rec->tableId);
    if (tableRec == nullptr) return false;
    if (rec->columnOrdinal >= tableRec->columns.size()) return false;

    IndexValidation validation;
    if (!db.validateIndex(indexId, validation).isOk() || !validation.ok) {
        return false;
    }

    std::unique_ptr<Table> table;
    if (!db.openTable(tableRec->name, table).isOk()) return false;
    if (validation.entryCount != table->rowCount()) return false;

    const DbType type = tableRec->columns[rec->columnOrdinal].type;
    std::unique_ptr<TableScan> scan;
    if (!table->scanStart(scan).isOk()) return false;
    std::vector<DbValue> row;
    while (scan->next(row)) {
        std::vector<uint8_t> key;
        if (!encodeIndexKey(type, row[rec->columnOrdinal], key)) return false;
        std::vector<RowLocator> locators;
        if (!db.indexLookup(indexId, key, locators).isOk()) return false;
        bool found = false;
        for (size_t i = 0; i < locators.size(); ++i) {
            if (locators[i].pageId == scan->currentPageId() &&
                locators[i].slot == scan->currentSlotIndex()) {
                found = true;
            }
        }
        if (!found) return false;
    }
    return scan->status().isOk();
}

bool validateAllIndexes(Database& db) {
    const std::vector<Catalog::IndexRecord> indexes = db.catalog().indexes();
    for (size_t i = 0; i < indexes.size(); ++i) {
        if (!validateIndexAgainstHeap(db, indexes[i].indexId)) return false;
    }
    return true;
}

bool validateIntegrity(Database& db) {
    std::string message;
    DbResult result = db.validateIntegrity(message);
    if (!result.isOk()) {
        std::printf("    integrity failure: %s\n", message.c_str());
        return false;
    }
    return true;
}

bool fullValidation(Database& db) {
    return validateAllIndexes(db) && validateIntegrity(db);
}

// ---------------------------------------------------------------------------
// Byte-budgeted crash-injection backend (same model as the SQL3-SQL7 suites).

struct BudgetState {
    uint64_t budget;
    uint64_t written;
    bool exhausted;
    bool recording;
    std::vector<uint64_t> boundaries;

    BudgetState() : budget(UINT64_MAX), written(0), exhausted(false), recording(false) {}
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

DatabaseOpenOptions crashOpenOptions() {
    DatabaseOpenOptions options;
    options.bufferCapacity = 8192;
    return options;
}

uint64_t measureSqlCommit(const std::string& path, const std::string& sql,
                          std::vector<uint64_t>& boundaries) {
    std::shared_ptr<BudgetState> state(new BudgetState());
    state->recording = true;
    BudgetFileSystem fs(state);
    std::unique_ptr<Database> db;
    if (!Database::open(path, crashOpenOptions(), db, &fs).isOk() || !db) {
        return 0;
    }
    {
        SqlEngine engine(*db);
        if (!engine.execute(sql).ok) {
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
    if (!Database::open(path, crashOpenOptions(), db, &fs).isOk() || !db) {
        return;
    }
    {
        SqlEngine engine(*db);
        engine.execute(sql);
    }
    db->close();
}

std::set<uint64_t> crashPoints(const std::vector<uint64_t>& boundaries,
                               uint64_t commitEnd) {
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
    return points;
}

typedef bool (*CrashVerify)(Database&, bool);

void runCrashMatrix(const std::string& tag, const std::string& path,
                    const std::string& txSql, CrashVerify verify) {
    std::vector<uint8_t> baseBytes;
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f != nullptr) {
            std::fseek(f, 0, SEEK_END);
            long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size > 0) {
                baseBytes.resize(static_cast<size_t>(size));
                size_t read = std::fread(baseBytes.data(), 1, baseBytes.size(), f);
                if (read != baseBytes.size()) baseBytes.clear();
            }
            std::fclose(f);
        }
    }
    const std::string measure = uniquePath(tag + "measure");
    {
        FILE* f = std::fopen(measure.c_str(), "wb");
        if (f != nullptr && !baseBytes.empty()) {
            std::fwrite(baseBytes.data(), 1, baseBytes.size(), f);
        }
        if (f != nullptr) std::fclose(f);
    }
    removeFile(walPath(measure));
    std::vector<uint64_t> boundaries;
    const uint64_t commitEnd = measureSqlCommit(measure, txSql, boundaries);
    removeFile(walPath(measure));
    removeFile(measure);
    CHECK(commitEnd > kWalHeaderSize);
    const std::set<uint64_t> points = crashPoints(boundaries, commitEnd);
    int tested = 0;
    for (std::set<uint64_t>::const_iterator it = points.begin(); it != points.end(); ++it) {
        const uint64_t budget = *it;
        const std::string work = uniquePath(tag + "work");
        {
            FILE* f = std::fopen(work.c_str(), "wb");
            if (f != nullptr && !baseBytes.empty()) {
                std::fwrite(baseBytes.data(), 1, baseBytes.size(), f);
            }
            if (f != nullptr) std::fclose(f);
        }
        removeFile(walPath(work));
        std::shared_ptr<BudgetState> state(new BudgetState());
        state->budget = budget;
        runCrashedSql(work, txSql, state);

        std::unique_ptr<Database> db;
        DbResult open = Database::open(work, DatabaseOpenOptions(), db);
        CHECK_STATUS(open, DbStatus::Ok);
        if (open.isOk() && db) {
            const bool committed = budget >= commitEnd;
            bool ok = verify(*db, committed);
            if (!ok) {
                std::printf("    crash boundary %llu (%s) produced an inconsistent state\n",
                            static_cast<unsigned long long>(budget), tag.c_str());
            }
            CHECK(ok);
            db->close();
        }
        ++tested;
        removeFile(walPath(work));
        removeFile(work);
    }
    CHECK(tested > 5);
}

// ---------------------------------------------------------------------------
// Parser / tokenizer.

void testTokenizerAndParser() {
    std::printf("SQL8 tokenizer and parser\n");
    std::vector<SqlToken> tokens;
    CHECK(tokenizeOk("DEFAULT FOREIGN REFERENCES DROP ALTER ADD COLUMN", tokens));
    CHECK(tokens.size() == 8); // 7 keywords plus end-of-input
    CHECK(tokens[0].kind == SqlTokenKind::Default);
    CHECK(tokens[1].kind == SqlTokenKind::Foreign);
    CHECK(tokens[2].kind == SqlTokenKind::References);
    CHECK(tokens[3].kind == SqlTokenKind::Drop);
    CHECK(tokens[4].kind == SqlTokenKind::Alter);
    CHECK(tokens[5].kind == SqlTokenKind::Add);
    CHECK(tokens[6].kind == SqlTokenKind::Column);

    SqlStatementAst stmt;
    SqlError error;
    CHECK(parseOne("DROP INDEX IX_A;", stmt, error));
    CHECK(stmt.type == SqlStatementType::DropIndex);
    CHECK(stmt.dropIndex.index.name == "IX_A");
    CHECK(parseOne("DROP TABLE Users;", stmt, error));
    CHECK(stmt.type == SqlStatementType::DropTable);
    CHECK(stmt.dropTable.table.name == "Users");
    CHECK(parseOne("ALTER TABLE T ADD COLUMN X INT64 NOT NULL DEFAULT 3;", stmt, error));
    CHECK(stmt.type == SqlStatementType::AlterTableAddColumn);
    CHECK(stmt.alterTableAddColumn.columnDef.hasDefault);
    CHECK(stmt.alterTableAddColumn.columnDef.defaultValue.kind == SqlLiteralKind::Integer);
    CHECK(parseOne("CREATE TABLE T (A INT64, FOREIGN KEY (A) REFERENCES P (Id));",
                   stmt, error));
    CHECK(stmt.createTable.foreignKeys.size() == 1);
    CHECK(stmt.createTable.foreignKeys[0].childColumn.name == "A");
    CHECK(stmt.createTable.foreignKeys[0].parentTable.name == "P");
    CHECK(stmt.createTable.foreignKeys[0].parentColumn.name == "Id");
    CHECK(parseOne("INSERT INTO T (A, B) VALUES (1, DEFAULT);", stmt, error));
    CHECK(stmt.insert.hasColumnList);
    CHECK(stmt.insert.columnList.size() == 2);
    CHECK(stmt.insert.values.size() == 2);
    CHECK(stmt.insert.values[1].isDefault);

    // Hostile / malformed input must fail to parse, not crash.
    CHECK(parseFails("CREATE TABLE T (A INT64, FOREIGN KEY () REFERENCES P(Id));"));
    CHECK(parseFails("CREATE TABLE T (A INT64, FOREIGN KEY (A, B) REFERENCES P(Id));"));
    CHECK(parseFails("CREATE TABLE T (A INT64, FOREIGN KEY (A) REFERENCES P());"));
    CHECK(parseFails("CREATE TABLE T (A INT64 DEFAULT);"));
    CHECK(parseFails("INSERT INTO T () VALUES ();"));
    CHECK(parseFails("DROP;"));
    CHECK(parseFails("DROP INDEX;"));
    CHECK(parseFails("DROP TABLE;"));
    CHECK(parseFails("ALTER;"));
    CHECK(parseFails("ALTER TABLE;"));
    CHECK(parseFails("ALTER TABLE T;"));
    CHECK(parseFails("ALTER TABLE T ADD;"));
    CHECK(parseFails("ALTER TABLE T ADD COLUMN;"));
    CHECK(parseFails("ALTER TABLE T ADD COLUMN X;"));
    CHECK(parseFails("CREATE TABLE T (A INT64, FOREIGN KEY (A) REFERENCES P (Id) "
                     "ON DELETE CASCADE);"));
    CHECK(parseFails("ALTER TABLE T DROP COLUMN X;"));
    CHECK(parseFails("ALTER TABLE T ADD FOREIGN KEY (A) REFERENCES P (Id);"));
    CHECK(parseFails("DROP CONSTRAINT C;"));
}

// ---------------------------------------------------------------------------
// DEFAULT metadata.

void testDefaultMetadata() {
    std::printf("column DEFAULT metadata, persistence and reopen\n");
    const std::string path = uniquePath("sql8defaults");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine,
                     "CREATE TABLE Settings (Id INT64 PRIMARY KEY, "
                     "Enabled BOOLEAN NOT NULL DEFAULT TRUE, "
                     "Label TEXT DEFAULT 'Untitled', "
                     "Retries INT32 DEFAULT 3, "
                     "Big INT64 DEFAULT 9223372036854775807, "
                     "Ratio FLOAT64 DEFAULT 1.5, "
                     "Payload BLOB DEFAULT X'0102', "
                     "Note TEXT DEFAULT NULL);"));
        CHECK(db->catalog().version() == kCatalogVersionV4);
        const Catalog::TableRecord* rec = db->catalog().findTable("Settings");
        CHECK(rec != nullptr);
        if (rec) {
            CHECK(rec->columns.size() == 8);
            CHECK(rec->columns[0].hasDefault == false);
            CHECK(rec->columns[1].hasDefault && rec->columns[1].defaultValue.booleanValue());
            CHECK(rec->columns[2].defaultValue.textValue() == "Untitled");
            CHECK(rec->columns[3].defaultValue.int32Value() == 3);
            CHECK(rec->columns[4].defaultValue.int64Value() == 9223372036854775807LL);
            CHECK(rec->columns[5].defaultValue.float64Value() == 1.5);
            CHECK(rec->columns[6].defaultValue.blobValue().size() == 2);
            CHECK(rec->columns[7].hasDefault && rec->columns[7].defaultValue.isNull());
        }
        CHECK(execOk(engine, "INSERT INTO Settings (Id) VALUES (1);"));
        CHECK(execOk(engine, "INSERT INTO Settings (Id, Note) VALUES (2, 'set');"));
        SqlResultSet rs;
        CHECK(execSelect(engine, "SELECT * FROM Settings ORDER BY Id;", rs));
        CHECK(rs.rowCount() == 2);
        if (rs.rowCount() == 2) {
            CHECK_BOOL(rs.row(0)[1], true);
            CHECK_TEXT(rs.row(0)[2], "Untitled");
            CHECK_NULL(rs.row(0)[7]);
            CHECK_TEXT(rs.row(1)[7], "set");
        }
        CHECK(fullValidation(*db));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen: defaults must survive exactly.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(reopened->catalog().version() == kCatalogVersionV4);
            const Catalog::TableRecord* rec = reopened->catalog().findTable("Settings");
            CHECK(rec != nullptr);
            if (rec) {
                CHECK(rec->columns.size() == 8);
                CHECK(rec->columns[2].hasDefault);
                CHECK(rec->columns[2].defaultValue.textValue() == "Untitled");
                CHECK(rec->columns[4].defaultValue.int64Value() == 9223372036854775807LL);
                CHECK(rec->columns[6].defaultValue.blobValue().size() == 2);
            }
            SqlEngine engine(*reopened);
            CHECK(execOk(engine, "INSERT INTO Settings (Id) VALUES (3);"));
            SqlResultSet rs;
            CHECK(execSelect(engine, "SELECT * FROM Settings WHERE Id = 3;", rs));
            CHECK(rs.rowCount() == 1);
            if (rs.rowCount() == 1) {
                CHECK_BOOL(rs.row(0)[1], true);
                CHECK_TEXT(rs.row(0)[2], "Untitled");
                CHECK_NULL(rs.row(0)[7]);
            }
            CHECK(fullValidation(*reopened));
            reopened->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testDefaultNullRules() {
    std::printf("DEFAULT / NULL rule enforcement\n");
    const std::string path = uniquePath("sql8defaultrules");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        // NOT NULL + DEFAULT NULL is rejected.
        CHECK(execFails(engine, "CREATE TABLE Bad (A INT64 NOT NULL DEFAULT NULL);"));
        CHECK(db->catalog().findTable("Bad") == nullptr);
        // NULL + DEFAULT NULL is accepted.
        CHECK(execOk(engine, "CREATE TABLE Good (A INT64 NULL DEFAULT NULL);"));
        // Duplicate DEFAULT rejected.
        CHECK(execFails(engine, "CREATE TABLE D (A INT64 DEFAULT 1 DEFAULT 2);"));
        // Conflicting NULL / NOT NULL rejected by the parser.
        CHECK(execFails(engine, "CREATE TABLE N (A INT64 NULL NOT NULL);"));
        // Type-mismatched default rejected.
        CHECK(execFails(engine, "CREATE TABLE M (A INT64 DEFAULT 'text');"));
        CHECK(db->catalog().findTable("M") == nullptr);
        // NOT NULL with no default: omitted INSERT must fail.
        CHECK(execOk(engine, "CREATE TABLE R (Id INT64 PRIMARY KEY, A INT64 NOT NULL);"));
        CHECK(execFails(engine, "INSERT INTO R (Id) VALUES (1);"));
        CHECK(execOk(engine, "INSERT INTO R VALUES (1, 5);"));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// INSERT column lists and DEFAULT keyword.

void testInsertColumnLists() {
    std::printf("INSERT column lists and DEFAULT keyword\n");
    const std::string path = uniquePath("sql8insertcols");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Name TEXT NOT NULL DEFAULT 'anon', "
                             "Score INT32 DEFAULT 7, "
                             "Tag TEXT DEFAULT 'x');"));
        CHECK(execOk(engine, "INSERT INTO Users (Name, Id) VALUES ('Alice', 1);"));
        CHECK(execOk(engine, "INSERT INTO Users (Id) VALUES (2);"));
        CHECK(execOk(engine, "INSERT INTO Users (Id, Score) VALUES (3, 99);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (4, DEFAULT, DEFAULT, 'z');"));
        CHECK(execOk(engine, "INSERT INTO Users (Id, Tag) VALUES (5, DEFAULT);"));

        SqlResultSet rs;
        CHECK(execSelect(engine, "SELECT * FROM Users ORDER BY Id;", rs));
        CHECK(rs.rowCount() == 5);
        if (rs.rowCount() == 5) {
            CHECK_INT64(rs.row(0)[0], 1);
            CHECK_TEXT(rs.row(0)[1], "Alice");
            CHECK_INTVAL(rs.row(0)[2], 7);
            CHECK_TEXT(rs.row(0)[3], "x");
            CHECK_INT64(rs.row(1)[0], 2);
            CHECK_TEXT(rs.row(1)[1], "anon");
            CHECK_INTVAL(rs.row(2)[2], 99);
            CHECK_INT64(rs.row(3)[0], 4);
            CHECK_TEXT(rs.row(3)[1], "anon");
            CHECK_TEXT(rs.row(3)[3], "z");
            CHECK_INT64(rs.row(4)[0], 5);
            CHECK_TEXT(rs.row(4)[3], "x");
        }

        // Validation failures.
        CHECK(execFails(engine, "INSERT INTO Users (Nope) VALUES (1);"));
        CHECK(execFails(engine, "INSERT INTO Users (Id, Id) VALUES (1, 2);"));
        CHECK(execFails(engine, "INSERT INTO Users (Id, Name) VALUES (1);"));
        CHECK(execFails(engine, "INSERT INTO Users (Id) VALUES (1, 2);"));
        CHECK(execFails(engine, "INSERT INTO Users (Id, Name) VALUES (1, 2);"));
        CHECK(fullValidation(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

void testInsertDefaultKeyword() {
    std::printf("DEFAULT keyword semantics and equivalence\n");
    const std::string path = uniquePath("sql8insertdefault");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, "
                             "A TEXT DEFAULT 'a', B INT64 DEFAULT 5);"));
        // Omitted columns and explicit DEFAULT must be equivalent.
        CHECK(execOk(engine, "INSERT INTO T (Id) VALUES (1);"));
        CHECK(execOk(engine, "INSERT INTO T VALUES (2, DEFAULT, DEFAULT);"));
        SqlResultSet rs;
        CHECK(execSelect(engine, "SELECT * FROM T ORDER BY Id;", rs));
        CHECK(rs.rowCount() == 2);
        if (rs.rowCount() == 2) {
            CHECK_TEXT(rs.row(0)[1], "a");
            CHECK_INT64(rs.row(0)[2], 5);
            CHECK_TEXT(rs.row(1)[1], "a");
            CHECK_INT64(rs.row(1)[2], 5);
        }
        // DEFAULT on a column with no declared default is an error.
        CHECK(execOk(engine, "CREATE TABLE U (Id INT64 PRIMARY KEY, X TEXT NOT NULL);"));
        CHECK(execFails(engine, "INSERT INTO U VALUES (1, DEFAULT);"));
        CHECK(execFailsCode(engine, "INSERT INTO U VALUES (1, DEFAULT);",
                            SqlErrorCode::SemanticError));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Foreign key creation and validation.

void testForeignKeyCreation() {
    std::printf("FOREIGN KEY metadata, referenced key requirements, support index\n");
    const std::string path = uniquePath("sql8fkcreate");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Email TEXT UNIQUE, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NOT NULL, "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(db->catalog().foreignKeyCount() == 1);
        std::vector<ForeignKeyInfo> fks;
        CHECK(db->listForeignKeys(fks).isOk());
        CHECK(fks.size() == 1);
        if (fks.size() == 1) {
            CHECK(fks[0].childTableName == "Orders");
            CHECK(fks[0].childColumnName == "UserId");
            CHECK(fks[0].parentTableName == "Users");
            CHECK(fks[0].parentColumnName == "Id");
            CHECK(fks[0].referencedIndexId != 0);
            CHECK(fks[0].supportIndexId != 0);
        }
        // The support index is system-owned and owned by the FK.
        const Catalog::IndexRecord* support =
            db->catalog().findIndex(fks[0].supportIndexId);
        CHECK(support != nullptr);
        if (support) {
            CHECK(support->systemOwned());
            CHECK(support->ownerForeignKeyId == fks[0].foreignKeyId);
            CHECK(!support->unique());
            CHECK(support->tableId == fks[0].childTableId);
        }
        // Referencing a non-unique column is rejected.
        CHECK(execOk(engine, "CREATE TABLE Plain (Id INT64 PRIMARY KEY, K INT64 NOT NULL);"));
        CHECK(execFails(engine, "CREATE TABLE Bad1 (Id INT64 PRIMARY KEY, "
                                "K INT64, FOREIGN KEY (K) REFERENCES Plain (K));"));
        CHECK(db->catalog().findTable("Bad1") == nullptr);
        // Unknown parent table / column rejected.
        CHECK(execFails(engine, "CREATE TABLE Bad2 (Id INT64 PRIMARY KEY, "
                                "K INT64, FOREIGN KEY (K) REFERENCES Nope (Id));"));
        CHECK(execFails(engine, "CREATE TABLE Bad3 (Id INT64 PRIMARY KEY, "
                                "K INT64, FOREIGN KEY (K) REFERENCES Users (Nope));"));
        // Referencing a UNIQUE column is allowed.
        CHECK(execOk(engine, "CREATE TABLE Links (Id INT64 PRIMARY KEY, "
                             "Email TEXT, FOREIGN KEY (Email) REFERENCES Users (Email));"));
        CHECK(fullValidation(*db));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen and confirm FK metadata persisted.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(reopened->catalog().version() == kCatalogVersionV4);
            CHECK(reopened->catalog().foreignKeyCount() == 2);
            CHECK(fullValidation(*reopened));
            reopened->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testForeignKeyTypeCompatibility() {
    std::printf("FOREIGN KEY type compatibility\n");
    const std::string path = uniquePath("sql8fktypes");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Parent32 (Id INT32 PRIMARY KEY);"));
        CHECK(execOk(engine, "CREATE TABLE Parent64 (Id INT64 PRIMARY KEY);"));
        CHECK(execOk(engine, "CREATE TABLE ParentF (Id FLOAT64 PRIMARY KEY);"));
        CHECK(execOk(engine, "CREATE TABLE ParentT (Id TEXT PRIMARY KEY);"));
        // Exact logical type required.
        CHECK(execFails(engine, "CREATE TABLE C1 (Id INT64 PRIMARY KEY, P INT32, "
                                "FOREIGN KEY (P) REFERENCES Parent64 (Id));"));
        CHECK(execFails(engine, "CREATE TABLE C2 (Id INT64 PRIMARY KEY, P INT64, "
                                "FOREIGN KEY (P) REFERENCES Parent32 (Id));"));
        CHECK(execFails(engine, "CREATE TABLE C3 (Id INT64 PRIMARY KEY, P INT64, "
                                "FOREIGN KEY (P) REFERENCES ParentF (Id));"));
        CHECK(execFails(engine, "CREATE TABLE C4 (Id INT64 PRIMARY KEY, P INT64, "
                                "FOREIGN KEY (P) REFERENCES ParentT (Id));"));
        CHECK(execOk(engine, "CREATE TABLE C5 (Id INT64 PRIMARY KEY, P INT32, "
                             "FOREIGN KEY (P) REFERENCES Parent32 (Id));"));
        CHECK(execOk(engine, "CREATE TABLE C6 (Id INT64 PRIMARY KEY, P TEXT, "
                             "FOREIGN KEY (P) REFERENCES ParentT (Id));"));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Foreign key enforcement.

void testFkInsertEnforcement() {
    std::printf("FOREIGN KEY INSERT enforcement\n");
    const std::string path = uniquePath("sql8fkinsert");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NULL, "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'A');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'B');"));
        // Valid parent.
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1);"));
        // Many children referencing one parent.
        for (int i = 0; i < 50; ++i) {
            CHECK(execOk(engine, "INSERT INTO Orders VALUES (" +
                                     std::to_string(100 + i) + ", 1);"));
        }
        // NULL child allowed (nullable FK).
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (200, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (201, NULL);"));
        // Missing parent rejected atomically.
        CHECK(execFailsCode(engine, "INSERT INTO Orders VALUES (300, 999);",
                            SqlErrorCode::ForeignKeyViolation));
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM Orders WHERE Id = 300;", ok) == 0);
        CHECK(ok);
        CHECK(fullValidation(*db));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Native relational API cannot bypass FK enforcement.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            std::unique_ptr<Table> orders;
            CHECK(reopened->openTable("Orders", orders).isOk());
            if (orders) {
                std::vector<DbValue> values;
                values.push_back(DbValue::int64(9999));
                values.push_back(DbValue::int64(123456));
                CHECK_STATUS(orders->insert(values), DbStatus::ForeignKeyViolation);
            }
            reopened->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testFkChildUpdate() {
    std::printf("FOREIGN KEY child UPDATE enforcement\n");
    const std::string path = uniquePath("sql8fkedit");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NULL, "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'A');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'B');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (11, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (12, 2);"));
        // valid -> valid
        CHECK(execOk(engine, "UPDATE Orders SET UserId = 2 WHERE Id = 10;"));
        // valid -> NULL
        CHECK(execOk(engine, "UPDATE Orders SET UserId = NULL WHERE Id = 10;"));
        // NULL -> valid
        CHECK(execOk(engine, "UPDATE Orders SET UserId = 1 WHERE Id = 11;"));
        // valid -> missing fails
        CHECK(execFailsCode(engine, "UPDATE Orders SET UserId = 999 WHERE Id = 10;",
                            SqlErrorCode::ForeignKeyViolation));
        // NULL -> missing fails
        CHECK(execFailsCode(engine, "UPDATE Orders SET UserId = 777 WHERE Id = 11;",
                            SqlErrorCode::ForeignKeyViolation));
        // Multi-row UPDATE where one target violates: whole statement fails.
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (13, 1);"));
        CHECK(execOk(engine, "UPDATE Orders SET UserId = 1 WHERE Id >= 10;"));
        CHECK(execFailsCode(engine, "UPDATE Orders SET UserId = 5 WHERE Id >= 10;",
                            SqlErrorCode::ForeignKeyViolation));
        bool ok = false;
        // No row received the invalid key; the previous valid state survives.
        CHECK(execRowCount(engine, "SELECT * FROM Orders WHERE UserId = 5;", ok) == 0);
        CHECK(ok);
        CHECK(execRowCount(engine, "SELECT * FROM Orders WHERE Id = 12 AND UserId = 1;",
                           ok) == 1);
        CHECK(ok);
        CHECK(fullValidation(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

void testFkParentRestrict() {
    std::printf("FOREIGN KEY parent DELETE / UPDATE RESTRICT\n");
    const std::string path = uniquePath("sql8fkparent");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NOT NULL, "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'A');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'B');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (3, 'C');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (11, 1);"));
        // Parent with no children can be deleted.
        CHECK(execOk(engine, "DELETE FROM Users WHERE Id = 2;"));
        // Parent with children is restricted.
        CHECK(execFailsCode(engine, "DELETE FROM Users WHERE Id = 1;",
                            SqlErrorCode::ForeignKeyViolation));
        // Non-key parent update is unaffected by child references.
        CHECK(execOk(engine, "UPDATE Users SET Name = 'Alice' WHERE Id = 1;"));
        // Referenced key change with children is restricted.
        CHECK(execFailsCode(engine, "UPDATE Users SET Id = 50 WHERE Id = 1;",
                            SqlErrorCode::ForeignKeyViolation));
        // Remove children, then parent key change succeeds.
        CHECK(execOk(engine, "DELETE FROM Orders WHERE UserId = 1;"));
        CHECK(execOk(engine, "UPDATE Users SET Id = 50 WHERE Id = 1;"));
        CHECK(execOk(engine, "DELETE FROM Users WHERE Id = 50;"));
        // Multi-row DELETE where one parent has a surviving child is restricted.
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (20, 3);"));
        CHECK(execFailsCode(engine, "DELETE FROM Users WHERE Id >= 3;",
                            SqlErrorCode::ForeignKeyViolation));
        CHECK(fullValidation(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

void testSelfReferentialFk() {
    std::printf("self-referential FOREIGN KEY and statement-final-state\n");
    const std::string path = uniquePath("sql8selfref");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Employees (Id INT64 PRIMARY KEY, "
                             "ManagerId INT64 NULL, "
                             "FOREIGN KEY (ManagerId) REFERENCES Employees (Id));"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (1, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (2, 1);"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (3, 2);"));
        // Missing parent rejected.
        CHECK(execFailsCode(engine, "INSERT INTO Employees VALUES (4, 99);",
                            SqlErrorCode::ForeignKeyViolation));
        // Deleting a referenced row is restricted while the child survives.
        CHECK(execFailsCode(engine, "DELETE FROM Employees WHERE Id = 1;",
                            SqlErrorCode::ForeignKeyViolation));
        // Changing the referenced key while the child survives is restricted.
        CHECK(execFailsCode(engine, "UPDATE Employees SET Id = 100 WHERE Id = 1;",
                            SqlErrorCode::ForeignKeyViolation));
        // Deleting every row in one statement is allowed: no child remains in
        // the statement's final state.
        CHECK(execOk(engine, "DELETE FROM Employees;"));
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM Employees;", ok) == 0);
        CHECK(ok);

        // Rebuild and test a multi-row final-state UPDATE: move a child and
        // delete its old parent in the same statement is not possible in one
        // statement, but re-pointing children first is.
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (1, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (2, 1);"));
        CHECK(execOk(engine, "UPDATE Employees SET ManagerId = NULL WHERE Id = 2;"));
        CHECK(execOk(engine, "DELETE FROM Employees WHERE Id = 1;"));
        CHECK(fullValidation(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

void testMultipleForeignKeys() {
    std::printf("multiple single-column foreign keys\n");
    const std::string path = uniquePath("sql8multifk");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Location (Id INT64 PRIMARY KEY, City TEXT);"));
        CHECK(execOk(engine, "CREATE TABLE Shipment (Id INT64 PRIMARY KEY, "
                             "OriginId INT64, DestinationId INT64, "
                             "FOREIGN KEY (OriginId) REFERENCES Location (Id), "
                             "FOREIGN KEY (DestinationId) REFERENCES Location (Id));"));
        CHECK(db->catalog().foreignKeyCount() == 2);
        // Each FK gets its own support index.
        std::vector<ForeignKeyInfo> fks;
        CHECK(db->listForeignKeys(fks).isOk());
        CHECK(fks.size() == 2);
        if (fks.size() == 2) {
            CHECK(fks[0].supportIndexId != fks[1].supportIndexId);
        }
        CHECK(execOk(engine, "INSERT INTO Location VALUES (1, 'A');"));
        CHECK(execOk(engine, "INSERT INTO Location VALUES (2, 'B');"));
        CHECK(execOk(engine, "INSERT INTO Shipment VALUES (10, 1, 2);"));
        CHECK(execFailsCode(engine, "INSERT INTO Shipment VALUES (11, 1, 99);",
                            SqlErrorCode::ForeignKeyViolation));
        // Each FK is enforced independently.
        CHECK(execFailsCode(engine, "DELETE FROM Location WHERE Id = 1;",
                            SqlErrorCode::ForeignKeyViolation));
        CHECK(execFailsCode(engine, "DELETE FROM Location WHERE Id = 2;",
                            SqlErrorCode::ForeignKeyViolation));
        CHECK(execOk(engine, "DELETE FROM Shipment WHERE Id = 10;"));
        CHECK(execOk(engine, "DELETE FROM Location WHERE Id = 1;"));
        CHECK(fullValidation(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

void testMultipleChildTables() {
    std::printf("multiple child tables referencing one parent\n");
    const std::string path = uniquePath("sql8multichild");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64, FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "CREATE TABLE Tickets (Id INT64 PRIMARY KEY, "
                             "UserId INT64, FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "CREATE TABLE Sessions (Id INT64 PRIMARY KEY, "
                             "UserId INT64, FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1);"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1);"));
        CHECK(execOk(engine, "INSERT INTO Tickets VALUES (20, 1);"));
        CHECK(execOk(engine, "INSERT INTO Sessions VALUES (30, 1);"));
        // Parent DELETE restricted if any incoming FK has a referencing row.
        CHECK(execFailsCode(engine, "DELETE FROM Users WHERE Id = 1;",
                            SqlErrorCode::ForeignKeyViolation));
        CHECK(execOk(engine, "DELETE FROM Orders WHERE UserId = 1;"));
        CHECK(execFailsCode(engine, "DELETE FROM Users WHERE Id = 1;",
                            SqlErrorCode::ForeignKeyViolation));
        CHECK(execOk(engine, "DELETE FROM Tickets WHERE UserId = 1;"));
        CHECK(execFailsCode(engine, "DELETE FROM Users WHERE Id = 1;",
                            SqlErrorCode::ForeignKeyViolation));
        CHECK(execOk(engine, "DELETE FROM Sessions WHERE UserId = 1;"));
        CHECK(execOk(engine, "DELETE FROM Users WHERE Id = 1;"));
        CHECK(fullValidation(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

void testFkTransactionVisibility() {
    std::printf("FOREIGN KEY transaction read-your-writes and savepoints\n");
    const std::string path = uniquePath("sql8fktx");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NOT NULL, "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        // Child sees a transaction-private parent.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (100, 'Tx');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (1, 100);"));
        CHECK(execOk(engine, "COMMIT;"));
        // A parent deleted earlier in the transaction is no longer visible.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (200, 'Tx2');"));
        CHECK(execOk(engine, "DELETE FROM Users WHERE Id = 200;"));
        CHECK(execFailsCode(engine, "INSERT INTO Orders VALUES (2, 200);",
                            SqlErrorCode::ForeignKeyViolation));
        // Prior work survives the failed statement; transaction stays usable.
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM Users WHERE Id = 100;", ok) == 1);
        CHECK(ok);
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (3, 100);"));
        CHECK(execOk(engine, "COMMIT;"));
        CHECK(execRowCount(engine, "SELECT * FROM Orders;", ok) == 2);
        CHECK(ok);
        CHECK(fullValidation(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// DROP INDEX.

void testDropIndex() {
    std::printf("DROP INDEX behavior and ownership protection\n");
    const std::string path = uniquePath("sql8dropindex");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, "
                             "K INT64 NOT NULL, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
        for (int i = 0; i < 40; ++i) {
            CHECK(execOk(engine, "INSERT INTO T VALUES (" + std::to_string(i) + ", " +
                                     std::to_string(i) + ", 'n" + std::to_string(i) +
                                     "');"));
        }
        CHECK(validateAllIndexes(*db));
        // Planner uses the index before the drop.
        {
            SqlExecutionResult r = engine.execute("SELECT * FROM T WHERE K = 7;");
            CHECK(r.ok && r.statements[0].accessPath == "IndexLookup");
        }
        // Unknown index rejected.
        CHECK(execFails(engine, "DROP INDEX Nope;"));
        // Constraint-backed and system-owned indexes cannot be dropped.
        CHECK(execFailsCode(engine, "DROP INDEX $PK_1;", SqlErrorCode::DependencyError));
        // Drop the user index inside a transaction; planner must not use it and
        // rollback restores it.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "DROP INDEX IX_T_K;"));
        CHECK(db->catalog().findIndex("IX_T_K") == nullptr);
        {
            SqlExecutionResult r = engine.execute("SELECT * FROM T WHERE K = 7;");
            CHECK(r.ok && r.statements[0].accessPath != "IndexLookup");
            bool ok = false;
            CHECK(execRowCount(engine, "SELECT * FROM T WHERE K = 7;", ok) == 1);
            CHECK(ok);
        }
        CHECK(execOk(engine, "ROLLBACK;"));
        CHECK(db->catalog().findIndex("IX_T_K") != nullptr);
        CHECK(validateAllIndexes(*db));
        // Commit removes it durably.
        CHECK(execOk(engine, "DROP INDEX IX_T_K;"));
        CHECK(db->catalog().findIndex("IX_T_K") == nullptr);
        CHECK(validateAllIndexes(*db));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(reopened->catalog().findIndex("IX_T_K") == nullptr);
            CHECK(validateAllIndexes(*reopened));
            reopened->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// DROP TABLE.

void testDropTable() {
    std::printf("DROP TABLE dependencies and transaction visibility\n");
    const std::string path = uniquePath("sql8droptable");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Email TEXT UNIQUE, Name TEXT);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Users_Name ON Users (Name);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NOT NULL, "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'a@x', 'A');"));
        CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 1);"));
        // Incoming FK blocks parent drop even with zero child rows.
        CHECK(execFailsCode(engine, "DROP TABLE Users;", SqlErrorCode::DependencyError));
        CHECK(db->catalog().findTable("Users") != nullptr);
        // Inside a transaction the table disappears from the catalog view and
        // queries fail to bind; rollback restores it.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "DROP TABLE Orders;"));
        CHECK(db->catalog().findTable("Orders") == nullptr);
        CHECK(execFails(engine, "SELECT * FROM Orders;"));
        CHECK(execOk(engine, "DROP TABLE Users;"));
        CHECK(execOk(engine, "ROLLBACK;"));
        CHECK(db->catalog().findTable("Users") != nullptr);
        CHECK(db->catalog().findTable("Orders") != nullptr);
        CHECK(db->catalog().foreignKeyCount() == 1);
        // Child then parent in one committed transaction.
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "DROP TABLE Orders;"));
        CHECK(execOk(engine, "DROP TABLE Users;"));
        CHECK(execOk(engine, "COMMIT;"));
        CHECK(db->catalog().findTable("Users") == nullptr);
        CHECK(db->catalog().findTable("Orders") == nullptr);
        CHECK(db->catalog().indexCount() == 0);
        CHECK(db->catalog().foreignKeyCount() == 0);
        CHECK(fullValidation(*db));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(reopened->catalog().tableCount() == 0);
            reopened->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testDropSelfReferencingTable() {
    std::printf("DROP self-referencing table\n");
    const std::string path = uniquePath("sql8dropself");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Employees (Id INT64 PRIMARY KEY, "
                             "ManagerId INT64 NULL, "
                             "FOREIGN KEY (ManagerId) REFERENCES Employees (Id));"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (1, NULL);"));
        CHECK(execOk(engine, "INSERT INTO Employees VALUES (2, 1);"));
        CHECK(execOk(engine, "DROP TABLE Employees;"));
        CHECK(db->catalog().findTable("Employees") == nullptr);
        CHECK(db->catalog().foreignKeyCount() == 0);
        CHECK(db->catalog().indexCount() == 0);
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// ALTER TABLE ADD COLUMN.

void testAlterAddColumn() {
    std::printf("ALTER TABLE ADD COLUMN backfill and persistence\n");
    const std::string path = uniquePath("sql8alter");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Users_Name ON Users (Name);"));
        for (int i = 0; i < 60; ++i) {
            CHECK(execOk(engine, "INSERT INTO Users VALUES (" + std::to_string(i) +
                                     ", 'n" + std::to_string(i) + "');"));
        }
        const uint32_t beforeVersion =
            db->catalog().findTable("Users")->schemaVersion;
        // Nullable column with default.
        CHECK(execOk(engine, "ALTER TABLE Users ADD COLUMN Note TEXT DEFAULT 'new';"));
        CHECK(db->catalog().findTable("Users")->schemaVersion == beforeVersion + 1);
        // NOT NULL with default.
        CHECK(execOk(engine, "ALTER TABLE Users ADD COLUMN Enabled BOOLEAN NOT NULL "
                             "DEFAULT TRUE;"));
        // Nullable no default.
        CHECK(execOk(engine, "ALTER TABLE Users ADD COLUMN Extra INT64 NULL;"));
        // Existing rows backfilled; indexes remapped.
        CHECK(validateAllIndexes(*db));
        SqlResultSet rs;
        CHECK(execSelect(engine, "SELECT Note, Enabled, Extra FROM Users WHERE Id = 42;",
                         rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            CHECK_TEXT(rs.row(0)[0], "new");
            CHECK_BOOL(rs.row(0)[1], true);
            CHECK_NULL(rs.row(0)[2]);
        }
        // Future inserts use the persisted default.
        CHECK(execOk(engine, "INSERT INTO Users (Id, Name) VALUES (1000, 'New');"));
        CHECK(execSelect(engine, "SELECT Note, Enabled FROM Users WHERE Id = 1000;", rs));
        CHECK(rs.rowCount() == 1);
        if (rs.rowCount() == 1) {
            CHECK_TEXT(rs.row(0)[0], "new");
            CHECK_BOOL(rs.row(0)[1], true);
        }
        // NOT NULL without default on a non-empty table is rejected before any
        // durable change.
        CHECK(execFails(engine, "ALTER TABLE Users ADD COLUMN Required TEXT NOT NULL;"));
        CHECK(db->catalog().findTable("Users")->columns.size() == 5);
        CHECK(validateAllIndexes(*db));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen: schema, rows and indexes all consistent.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            const Catalog::TableRecord* rec = reopened->catalog().findTable("Users");
            CHECK(rec != nullptr);
            if (rec) CHECK(rec->columns.size() == 5);
            CHECK(validateAllIndexes(*reopened));
            SqlEngine engine(*reopened);
            SqlResultSet rs;
            CHECK(execSelect(engine, "SELECT Note FROM Users WHERE Id = 7;", rs));
            CHECK(rs.rowCount() == 1 && rs.row(0)[0].textValue() == "new");
            reopened->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testAlterEmptyTableAndRestrictions() {
    std::printf("ALTER ADD COLUMN empty table and unsupported clauses\n");
    const std::string path = uniquePath("sql8alterrestrict");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        // Empty table may take a NOT NULL column without a default.
        CHECK(execOk(engine, "CREATE TABLE Empty (Id INT64 PRIMARY KEY);"));
        CHECK(execOk(engine, "ALTER TABLE Empty ADD COLUMN Required TEXT NOT NULL;"));
        // Future inserts that omit the column must fail.
        CHECK(execFails(engine, "INSERT INTO Empty (Id) VALUES (1);"));
        CHECK(execOk(engine, "INSERT INTO Empty VALUES (1, 'x');"));
        // Unsupported ALTER forms.
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY);"));
        CHECK(execFails(engine, "ALTER TABLE T ADD COLUMN P INT64 PRIMARY KEY;"));
        CHECK(execFails(engine, "ALTER TABLE T ADD COLUMN U INT64 UNIQUE;"));
        CHECK(execFails(engine, "ALTER TABLE T DROP COLUMN Id;"));
        CHECK(execFails(engine, "ALTER TABLE T ADD FOREIGN KEY (Id) REFERENCES T (Id);"));
        CHECK(execFails(engine, "ALTER TABLE Nope ADD COLUMN X INT64;"));
        // A failed ALTER must leave the schema unchanged.
        CHECK(db->catalog().findTable("T")->columns.size() == 1);
        CHECK(fullValidation(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

void testAlterStatementSavepoint() {
    std::printf("ALTER statement savepoint restores prior transaction work\n");
    const std::string path = uniquePath("sql8altersavepoint");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, Name TEXT);"));
        CHECK(execOk(engine, "INSERT INTO T VALUES (1, 'a');"));
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "INSERT INTO T VALUES (2, 'b');"));
        // This ALTER fails (NOT NULL without default, non-empty) after prior work.
        CHECK(execFails(engine, "ALTER TABLE T ADD COLUMN R TEXT NOT NULL;"));
        // Prior INSERT remains, schema unchanged, transaction still active.
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM T;", ok) == 2);
        CHECK(ok);
        CHECK(db->catalog().findTable("T")->columns.size() == 2);
        CHECK(execOk(engine, "COMMIT;"));
        CHECK(validateAllIndexes(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Schema generation / stale handles.

void testStaleHandles() {
    std::printf("stale table handles fail closed after DROP\n");
    const std::string path = uniquePath("sql8stale");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, K INT64);"));
        CHECK(execOk(engine, "INSERT INTO T VALUES (1, 2);"));
        std::unique_ptr<Table> handle;
        CHECK(db->openTable("T", handle).isOk());
        CHECK(handle != nullptr);
        CHECK(execOk(engine, "DROP TABLE T;"));
        if (handle) {
            std::vector<DbValue> values;
            values.push_back(DbValue::int64(9));
            values.push_back(DbValue::int64(9));
            // The dropped table's record no longer exists: the handle fails
            // closed rather than mutating a missing schema object.
            CHECK_STATUS(handle->insert(values), DbStatus::Internal);
        }
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Corruption safety.

void testCatalogCorruption() {
    std::printf("FK / DEFAULT catalog corruption fails safely\n");
    // Build a valid database with an FK, then corrupt catalog fields.
    const std::string path = uniquePath("sql8corrupt");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Name TEXT DEFAULT 'x');"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64, FOREIGN KEY (UserId) REFERENCES Users (Id));"));
    }
    db->close();

    std::vector<uint8_t> bytes;
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f != nullptr) {
            std::fseek(f, 0, SEEK_END);
            long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size > 0) {
                bytes.resize(static_cast<size_t>(size));
                size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
                if (read != bytes.size()) bytes.clear();
            }
            std::fclose(f);
        }
    }
    CHECK(!bytes.empty());
    // Flipping bytes inside the catalog root payload must never crash and must
    // either open cleanly or fail with a corruption status.
    const uint32_t pageSize = 4096;
    const size_t payloadStart = static_cast<size_t>(kPageHeaderSize);
    const size_t payloadEnd = pageSize;
    int opened = 0;
    int failed = 0;
    for (size_t off = payloadStart + 8; off < payloadEnd - 4; off += 7) {
        std::vector<uint8_t> copy = bytes;
        copy[off] ^= 0xFFu;
        const std::string work = uniquePath("sql8corruptwork");
        {
            FILE* f = std::fopen(work.c_str(), "wb");
            if (f != nullptr) {
                std::fwrite(copy.data(), 1, copy.size(), f);
                std::fclose(f);
            }
        }
        removeFile(walPath(work));
        std::unique_ptr<Database> cdb;
        DbResult open = Database::open(work, DatabaseOpenOptions(), cdb);
        if (open.isOk() && cdb) {
            ++opened;
            // If it opened, the integrity validator must not crash.
            std::string message;
            cdb->validateIntegrity(message);
            cdb->close();
        } else {
            ++failed;
        }
        removeFile(work);
    }
    CHECK(opened + failed > 100);
    CHECK(failed > 0); // at least some corruptions are detected
    removeFile(path);
    removeFile(walPath(path));
}

// ---------------------------------------------------------------------------
// Backward compatibility.

void testBackwardCompatibility() {
    std::printf("catalog v1/v2/v3 backward compatibility\n");
    // v1: a SQL1-created (legacy) database has no tables. Build one by hand is
    // out of scope; instead verify that a v2 and v3 database still open and can
    // be upgraded by a SQL8 feature.
    {
        const std::string path = uniquePath("sql8compatv2");
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE T (Id INT64 NOT NULL, K INT64 NOT NULL);"));
            CHECK(execOk(engine, "INSERT INTO T VALUES (1, 2);"));
            CHECK(db->catalog().version() == kCatalogVersion);
            db->close();
        }
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(reopened->catalog().version() == kCatalogVersion);
            SqlEngine engine(*reopened);
            // A SQL8 feature upgrades the catalog to v4 transactionally.
            CHECK(execOk(engine, "ALTER TABLE T ADD COLUMN Note TEXT DEFAULT 'n';"));
            CHECK(reopened->catalog().version() == kCatalogVersionV4);
            reopened->close();
        }
        std::unique_ptr<Database> again;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), again), DbStatus::Ok);
        if (again) {
            CHECK(again->catalog().version() == kCatalogVersionV4);
            CHECK(again->catalog().foreignKeyCount() == 0);
            again->close();
        }
        removeFile(walPath(path));
        removeFile(path);
    }
    {
        const std::string path = uniquePath("sql8compatv3");
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, K INT64 NOT NULL);"));
            CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
            CHECK(execOk(engine, "INSERT INTO T VALUES (1, 2);"));
            CHECK(db->catalog().version() == kCatalogVersionV3);
            db->close();
        }
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(reopened->catalog().version() == kCatalogVersionV3);
            CHECK(reopened->catalog().foreignKeyCount() == 0);
            CHECK(validateAllIndexes(*reopened));
            // No FK metadata is inferred from names.
            std::vector<ForeignKeyInfo> fks;
            CHECK(reopened->listForeignKeys(fks).isOk());
            CHECK(fks.empty());
            reopened->close();
        }
        removeFile(walPath(path));
        removeFile(path);
    }
}

// ---------------------------------------------------------------------------
// Query regressions after schema changes.

void testQueryRegressionsAfterSchemaChange() {
    std::printf("SQL7 queries remain correct after ALTER and DROP INDEX\n");
    const std::string path = uniquePath("sql8queryregress");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NOT NULL, Amount FLOAT64 NOT NULL, "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_Amount ON Orders (Amount);"));
        for (int u = 0; u < 10; ++u) {
            CHECK(execOk(engine, "INSERT INTO Users VALUES (" + std::to_string(u) +
                                     ", 'u" + std::to_string(u) + "');"));
        }
        for (int o = 0; o < 50; ++o) {
            CHECK(execOk(engine, "INSERT INTO Orders VALUES (" + std::to_string(o) +
                                     ", " + std::to_string(o % 10) + ", " +
                                     std::to_string(o) + ");"));
        }
        // Baseline join + aggregate.
        SqlResultSet rs;
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM Users AS u JOIN Orders AS o "
                                 "ON o.UserId = u.Id;", rs));
        CHECK(rs.rowCount() == 1 && rs.row(0)[0].int64Value() == 50);
        // ALTER then re-run.
        CHECK(execOk(engine, "ALTER TABLE Orders ADD COLUMN State TEXT DEFAULT 'new';"));
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM Users AS u JOIN Orders AS o "
                                 "ON o.UserId = u.Id WHERE o.State = 'new';", rs));
        CHECK(rs.rowCount() == 1 && rs.row(0)[0].int64Value() == 50);
        CHECK(execSelect(engine, "SELECT u.Id, COUNT(o.Id) AS C FROM Users AS u "
                                 "LEFT JOIN Orders AS o ON o.UserId = u.Id "
                                 "GROUP BY u.Id ORDER BY u.Id;", rs));
        CHECK(rs.rowCount() == 10);
        // DROP INDEX then re-run (falls back to full scan).
        CHECK(execOk(engine, "DROP INDEX IX_Orders_Amount;"));
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM Orders WHERE Amount >= 25;", rs));
        CHECK(rs.rowCount() == 1 && rs.row(0)[0].int64Value() == 25);
        CHECK(execSelect(engine, "SELECT DISTINCT State FROM Orders;", rs));
        CHECK(rs.rowCount() == 1 && rs.row(0)[0].textValue() == "new");
        CHECK(validateAllIndexes(*db));
    }
    db->close();
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Crash matrices.

bool verifyCreateTableFk(Database& db, bool committed) {
    const Catalog::TableRecord* users = db.catalog().findTable("Users");
    const Catalog::TableRecord* orders = db.catalog().findTable("Orders");
    if (users == nullptr) return false; // Users exists in both pre and post state
    if (!committed) {
        return orders == nullptr && db.catalog().foreignKeyCount() == 0;
    }
    if (orders == nullptr) return false;
    if (db.catalog().foreignKeyCount() != 1) return false;
    return fullValidation(db);
}

bool verifyParentChildCommit(Database& db, bool committed) {
    std::unique_ptr<Table> users;
    std::unique_ptr<Table> orders;
    if (!db.openTable("Users", users).isOk()) return false;
    if (!db.openTable("Orders", orders).isOk()) return false;
    const uint64_t expectedUsers = committed ? 2 : 1;
    const uint64_t expectedOrders = committed ? 1 : 0;
    if (users->rowCount() != expectedUsers) return false;
    if (orders->rowCount() != expectedOrders) return false;
    if (committed) {
        // The committed child must resolve to a committed parent.
        std::string message;
        if (!db.validateIntegrity(message).isOk()) return false;
    }
    return fullValidation(db);
}

bool verifyDropIndexCrash(Database& db, bool committed) {
    const bool exists = db.catalog().findIndex("IX_T_K") != nullptr;
    if (committed == exists) return false; // committed => absent
    if (!exists && db.catalog().findIndex("IX_T_K") != nullptr) return false;
    return validateAllIndexes(db);
}

bool verifyDropTableCrash(Database& db, bool committed) {
    const bool users = db.catalog().findTable("Users") != nullptr;
    const bool orders = db.catalog().findTable("Orders") != nullptr;
    if (!committed) {
        // Both present with the FK intact.
        return users && orders && db.catalog().foreignKeyCount() == 1;
    }
    return !users && !orders && db.catalog().foreignKeyCount() == 0;
}

bool verifyAlterCrash(Database& db, bool committed) {
    const Catalog::TableRecord* rec = db.catalog().findTable("T");
    if (rec == nullptr) return false;
    const bool hasNew = rec->columns.size() == 3;
    if (committed != hasNew) return false;
    if (!validateAllIndexes(db)) return false;
    // Row values: old schema has (Id, Payload); new schema appends Tag='t'.
    std::unique_ptr<Table> table;
    if (!db.openTable("T", table).isOk()) return false;
    std::unique_ptr<TableScan> scan;
    if (!table->scanStart(scan).isOk()) return false;
    std::vector<DbValue> row;
    uint64_t count = 0;
    while (scan->next(row)) {
        if (row.size() != rec->columns.size()) return false;
        if (committed) {
            if (!row[2].isNull() && row[2].textValue() != "t") return false;
        }
        ++count;
    }
    if (!scan->status().isOk()) return false;
    return count > 0;
}

void testCrashMatrices() {
    std::printf("SQL8 crash matrices (create FK / commit / drop / alter)\n");
    // CREATE TABLE + FK crash matrix.
    {
        const std::string path = uniquePath("sql8crashcreate");
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY);"));
            db->close();
        }
        runCrashMatrix("sql8crashcreate", path,
                       "BEGIN;\nCREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                       "UserId INT64, FOREIGN KEY (UserId) REFERENCES Users (Id));\n"
                       "COMMIT;\n",
                       verifyCreateTableFk);
        removeFile(path);
        removeFile(walPath(path));
    }
    // Parent + child commit crash matrix.
    {
        const std::string path = uniquePath("sql8crashcommit");
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT);"));
            CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                                 "UserId INT64 NOT NULL, "
                                 "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
            CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'A');"));
            db->close();
        }
        runCrashMatrix("sql8crashcommit", path,
                       "BEGIN;\nINSERT INTO Users VALUES (2, 'B');\n"
                       "INSERT INTO Orders VALUES (10, 2);\nCOMMIT;\n",
                       verifyParentChildCommit);
        removeFile(path);
        removeFile(walPath(path));
    }
    // DROP INDEX crash matrix.
    {
        const std::string path = uniquePath("sql8crashdropindex");
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, K INT64 NOT NULL);"));
            CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
            for (int i = 0; i < 120; ++i) {
                CHECK(execOk(engine, "INSERT INTO T VALUES (" + std::to_string(i) + ", " +
                                         std::to_string(i) + ");"));
            }
            db->close();
        }
        runCrashMatrix("sql8crashdropindex", path,
                       "BEGIN;\nDROP INDEX IX_T_K;\nCOMMIT;\n", verifyDropIndexCrash);
        removeFile(path);
        removeFile(walPath(path));
    }
    // DROP TABLE (child then parent) crash matrix.
    {
        const std::string path = uniquePath("sql8crashdroptable");
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                                 "Email TEXT UNIQUE, Name TEXT);"));
            CHECK(execOk(engine, "CREATE INDEX IX_Users_Name ON Users (Name);"));
            CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                                 "UserId INT64, FOREIGN KEY (UserId) REFERENCES Users (Id));"));
            for (int i = 0; i < 40; ++i) {
                CHECK(execOk(engine, "INSERT INTO Users VALUES (" + std::to_string(i) +
                                         ", 'e" + std::to_string(i) + "', 'n" +
                                         std::to_string(i) + "');"));
            }
            CHECK(execOk(engine, "INSERT INTO Orders VALUES (10, 0);"));
            db->close();
        }
        runCrashMatrix("sql8crashdroptable", path,
                       "BEGIN;\nDROP TABLE Orders;\nDROP TABLE Users;\nCOMMIT;\n",
                       verifyDropTableCrash);
        removeFile(path);
        removeFile(walPath(path));
    }
    // ALTER crash matrix with row rewrite + index locator remap.
    {
        const std::string path = uniquePath("sql8crashalter");
        std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
        CHECK(db != nullptr);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, "
                                 "Payload TEXT NOT NULL);"));
            CHECK(execOk(engine, "CREATE INDEX IX_T_Payload ON T (Payload);"));
            uint32_t inserted = 0;
            while (inserted < 120) {
                std::string script = "BEGIN;\n";
                for (uint32_t i = 0; i < 30 && inserted < 120; ++i, ++inserted) {
                    script += "INSERT INTO T VALUES (" + std::to_string(inserted) +
                              ", 'p" + std::to_string(inserted) + "');\n";
                }
                script += "COMMIT;\n";
                CHECK(execOk(engine, script));
            }
            db->close();
        }
        runCrashMatrix("sql8crashalter", path,
                       "BEGIN;\nALTER TABLE T ADD COLUMN Tag TEXT DEFAULT 't';\nCOMMIT;\n",
                       verifyAlterCrash);
        removeFile(path);
        removeFile(walPath(path));
    }
}

// ---------------------------------------------------------------------------
// Deterministic referential workload.

void testDeterministicDataset() {
    std::printf("deterministic SQL8 referential dataset\n");
    const std::string path = uniquePath("sql8dataset");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    const int kUsers = 200;
    const int kOrders = 600;
    const int kItems = 1200;
    std::set<int64_t> liveUsers;
    std::set<int64_t> liveOrders;
    std::set<int64_t> liveItems;
    std::map<int64_t, int64_t> orderOwner; // orderId -> userId
    std::map<int64_t, int64_t> itemOrder;  // itemId -> orderId

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT NULL, "
                             "Enabled BOOLEAN NOT NULL DEFAULT TRUE);"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NOT NULL, State TEXT NOT NULL DEFAULT 'new', "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "CREATE TABLE OrderItems (Id INT64 PRIMARY KEY, "
                             "OrderId INT64 NOT NULL, Sku TEXT NOT NULL, "
                             "Qty INT32 NOT NULL DEFAULT 1, "
                             "FOREIGN KEY (OrderId) REFERENCES Orders (Id));"));
        CHECK(execOk(engine, "CREATE INDEX IX_Orders_State ON Orders (State);"));

        for (int i = 0; i < kUsers; ++i) {
            CHECK(execOk(engine, "INSERT INTO Users (Id, Name) VALUES (" +
                                     std::to_string(i) + ", 'u" + std::to_string(i) +
                                     "');"));
            liveUsers.insert(i);
        }
        for (int i = 0; i < kOrders; ++i) {
            const int64_t owner = (i * 7) % kUsers;
            CHECK(execOk(engine, "INSERT INTO Orders (Id, UserId) VALUES (" +
                                     std::to_string(i) + ", " +
                                     std::to_string(owner) + ");"));
            liveOrders.insert(i);
            orderOwner[i] = owner;
        }
        for (int i = 0; i < kItems; ++i) {
            const int64_t order = (i * 13) % kOrders;
            CHECK(execOk(engine, "INSERT INTO OrderItems (Id, OrderId, Sku) VALUES (" +
                                     std::to_string(i) + ", " + std::to_string(order) +
                                     ", 'sku" + std::to_string(i % 50) + "');"));
            liveItems.insert(i);
            itemOrder[i] = order;
        }
        CHECK(fullValidation(*db));

        // Deterministic invalid inserts must be rejected.
        CHECK(execFailsCode(engine, "INSERT INTO Orders (Id, UserId) VALUES (9999, 123456);",
                            SqlErrorCode::ForeignKeyViolation));
        CHECK(execFailsCode(engine,
                            "INSERT INTO OrderItems (Id, OrderId, Sku) VALUES (9999, 123456, 's');",
                            SqlErrorCode::ForeignKeyViolation));
        // Parent RESTRICT: a referenced user cannot be deleted.
        CHECK(execFailsCode(engine, "DELETE FROM Users WHERE Id = 0;",
                            SqlErrorCode::ForeignKeyViolation));
        // Deterministic child removal then parent removal: delete every order
        // owned by user 0 (and their items), then the user.
        std::vector<int64_t> ordersOfUser0;
        for (std::map<int64_t, int64_t>::iterator it = orderOwner.begin();
             it != orderOwner.end(); ++it) {
            if (it->second == 0) ordersOfUser0.push_back(it->first);
        }
        for (size_t i = 0; i < ordersOfUser0.size(); ++i) {
            const int64_t oid = ordersOfUser0[i];
            CHECK(execOk(engine, "DELETE FROM OrderItems WHERE OrderId = " +
                                     std::to_string(oid) + ";"));
            std::vector<int64_t> removedItems;
            for (std::map<int64_t, int64_t>::iterator it = itemOrder.begin();
                 it != itemOrder.end(); ++it) {
                if (it->second == oid) removedItems.push_back(it->first);
            }
            for (size_t j = 0; j < removedItems.size(); ++j) {
                liveItems.erase(removedItems[j]);
                itemOrder.erase(removedItems[j]);
            }
        }
        for (size_t i = 0; i < ordersOfUser0.size(); ++i) {
            CHECK(execOk(engine, "DELETE FROM Orders WHERE Id = " +
                                     std::to_string(ordersOfUser0[i]) + ";"));
            liveOrders.erase(ordersOfUser0[i]);
            orderOwner.erase(ordersOfUser0[i]);
        }
        CHECK(execOk(engine, "DELETE FROM Users WHERE Id = 0;"));
        liveUsers.erase(0);

        // DROP a user index, then ALTER an existing table.
        CHECK(execOk(engine, "DROP INDEX IX_Orders_State;"));
        CHECK(execOk(engine, "ALTER TABLE OrderItems ADD COLUMN Note TEXT DEFAULT 'n';"));

        // JOIN + aggregate verification against the model.
        SqlResultSet rs;
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM Orders AS o JOIN Users AS u "
                                 "ON o.UserId = u.Id;", rs));
        CHECK(rs.rowCount() == 1 &&
              rs.row(0)[0].int64Value() == static_cast<int64_t>(liveOrders.size()));
        CHECK(execSelect(engine, "SELECT COUNT(*) FROM OrderItems AS i JOIN Orders AS o "
                                 "ON i.OrderId = o.Id;", rs));
        CHECK(rs.rowCount() == 1 &&
              rs.row(0)[0].int64Value() == static_cast<int64_t>(liveItems.size()));
        CHECK(fullValidation(*db));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen and re-verify.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(fullValidation(*reopened));
            std::unique_ptr<Table> orders;
            CHECK(reopened->openTable("Orders", orders).isOk());
            if (orders) CHECK(orders->rowCount() == liveOrders.size());
            std::unique_ptr<Table> items;
            CHECK(reopened->openTable("OrderItems", items).isOk());
            if (items) CHECK(items->rowCount() == liveItems.size());
            reopened->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testRepeatedLifecycle() {
    std::printf("repeated SQL8 schema/integrity lifecycles (250)\n");
    const std::string path = uniquePath("sql8repeat");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;

    std::set<int64_t> liveUsers;
    std::set<int64_t> liveOrders;
    int64_t nextUserId = 1;
    int64_t nextOrderId = 1;
    int mismatches = 0;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Name TEXT NOT NULL DEFAULT 'anon');"));
        CHECK(execOk(engine, "CREATE TABLE Orders (Id INT64 PRIMARY KEY, "
                             "UserId INT64 NOT NULL, "
                             "FOREIGN KEY (UserId) REFERENCES Users (Id));"));
        CHECK(execOk(engine, "CREATE TABLE Scratch (Id INT64 PRIMARY KEY, "
                             "V INT64 DEFAULT 0);"));
        for (int cycle = 0; cycle < 250; ++cycle) {
            const int mode = cycle % 10;
            if (mode == 0) {
                // Valid parent + child.
                const int64_t u = nextUserId++;
                const int64_t o = nextOrderId++;
                CHECK(execOk(engine, "BEGIN;"));
                CHECK(execOk(engine, "INSERT INTO Users VALUES (" + std::to_string(u) +
                                         ", 'u');"));
                CHECK(execOk(engine, "INSERT INTO Orders VALUES (" + std::to_string(o) +
                                         ", " + std::to_string(u) + ");"));
                CHECK(execOk(engine, "COMMIT;"));
                liveUsers.insert(u);
                liveOrders.insert(o);
            } else if (mode == 1) {
                // Invalid child INSERT rolled back.
                CHECK(execOk(engine, "BEGIN;"));
                CHECK(execFails(engine, "INSERT INTO Orders VALUES (" +
                                            std::to_string(nextOrderId) +
                                            ", 999999);"));
                CHECK(execOk(engine, "ROLLBACK;"));
                ++nextOrderId;
            } else if (mode == 2) {
                // Valid child UPDATE.
                if (!liveOrders.empty()) {
                    const int64_t o = *liveOrders.begin();
                    const int64_t u = *liveUsers.begin();
                    CHECK(execOk(engine, "UPDATE Orders SET UserId = " +
                                             std::to_string(u) + " WHERE Id = " +
                                             std::to_string(o) + ";"));
                }
            } else if (mode == 3) {
                // Invalid child UPDATE rolled back.
                if (!liveOrders.empty()) {
                    const int64_t o = *liveOrders.begin();
                    CHECK(execFails(engine, "UPDATE Orders SET UserId = 888888 WHERE Id = " +
                                                std::to_string(o) + ";"));
                }
            } else if (mode == 4) {
                // Parent RESTRICT while a child exists, then child removal.
                const int64_t u = nextUserId++;
                const int64_t o = nextOrderId++;
                CHECK(execOk(engine, "INSERT INTO Users VALUES (" + std::to_string(u) +
                                         ", 'f');"));
                CHECK(execOk(engine, "INSERT INTO Orders VALUES (" + std::to_string(o) +
                                         ", " + std::to_string(u) + ");"));
                CHECK(execFailsCode(engine, "DELETE FROM Users WHERE Id = " +
                                                std::to_string(u) + ";",
                                    SqlErrorCode::ForeignKeyViolation));
                CHECK(execOk(engine, "DELETE FROM Orders WHERE Id = " +
                                         std::to_string(o) + ";"));
                CHECK(execOk(engine, "DELETE FROM Users WHERE Id = " +
                                         std::to_string(u) + ";"));
            } else if (mode == 5) {
                // Remove a child then its parent.
                if (!liveOrders.empty()) {
                    const int64_t o = *liveOrders.begin();
                    const int64_t u = nextUserId++;
                    CHECK(execOk(engine, "INSERT INTO Users VALUES (" +
                                             std::to_string(u) + ", 'p');"));
                    CHECK(execOk(engine, "UPDATE Orders SET UserId = " +
                                             std::to_string(u) + " WHERE Id = " +
                                             std::to_string(o) + ";"));
                    CHECK(execOk(engine, "DELETE FROM Orders WHERE Id = " +
                                             std::to_string(o) + ";"));
                    CHECK(execOk(engine, "DELETE FROM Users WHERE Id = " +
                                             std::to_string(u) + ";"));
                    liveOrders.erase(o);
                }
            } else if (mode == 6) {
                // DEFAULT insertion.
                const int64_t u = nextUserId++;
                CHECK(execOk(engine, "INSERT INTO Users (Id) VALUES (" +
                                         std::to_string(u) + ");"));
                liveUsers.insert(u);
            } else if (mode == 7) {
                // Periodic ALTER on the small bounded Scratch table.
                CHECK(execOk(engine, "INSERT INTO Scratch (Id) VALUES (" +
                                         std::to_string(cycle) + ");"));
                CHECK(execOk(engine, "ALTER TABLE Scratch ADD COLUMN C" +
                                         std::to_string(cycle) + " INT64 DEFAULT " +
                                         std::to_string(cycle) + ";"));
            } else if (mode == 8) {
                // Explicit rollback of a schema change.
                CHECK(execOk(engine, "BEGIN;"));
                CHECK(execOk(engine, "CREATE TABLE Tmp" + std::to_string(cycle) +
                                         " (Id INT64 PRIMARY KEY);"));
                CHECK(execOk(engine, "ROLLBACK;"));
                if (db->catalog().findTable("Tmp" + std::to_string(cycle)) != nullptr) {
                    ++mismatches;
                }
            } else {
                // SQL7 JOIN verification.
                SqlResultSet rs;
                CHECK(execSelect(engine, "SELECT COUNT(*) FROM Orders AS o JOIN Users "
                                         "AS u ON o.UserId = u.Id;", rs));
                if (rs.rowCount() != 1 ||
                    rs.row(0)[0].int64Value() != static_cast<int64_t>(liveOrders.size())) {
                    ++mismatches;
                }
            }
        }
        CHECK(mismatches == 0);
        CHECK(fullValidation(*db));
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen, verify counts and integrity.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(fullValidation(*reopened));
            std::unique_ptr<Table> users;
            std::unique_ptr<Table> orders;
            CHECK(reopened->openTable("Users", users).isOk());
            CHECK(reopened->openTable("Orders", orders).isOk());
            if (users && orders) {
                CHECK(users->rowCount() == liveUsers.size());
                CHECK(orders->rowCount() == liveOrders.size());
            }
            reopened->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("guideXOS SQL8 schema lifecycle and relational integrity tests\n");
    std::string dir = testDir();
    std::printf("temp dir: %s\n", dir.c_str());

    std::string cmd = "del /q \"" + dir + "\\*.gxdb\" >nul 2>nul";
    std::system(cmd.c_str());
    cmd = "del /q \"" + dir + "\\*.gxwal\" >nul 2>nul";
    std::system(cmd.c_str());

    testTokenizerAndParser();
    testDefaultMetadata();
    testDefaultNullRules();
    testInsertColumnLists();
    testInsertDefaultKeyword();
    testForeignKeyCreation();
    testForeignKeyTypeCompatibility();
    testFkInsertEnforcement();
    testFkChildUpdate();
    testFkParentRestrict();
    testSelfReferentialFk();
    testMultipleForeignKeys();
    testMultipleChildTables();
    testFkTransactionVisibility();
    testDropIndex();
    testDropTable();
    testDropSelfReferencingTable();
    testAlterAddColumn();
    testAlterEmptyTableAndRestrictions();
    testAlterStatementSavepoint();
    testStaleHandles();
    testCatalogCorruption();
    testBackwardCompatibility();
    testQueryRegressionsAfterSchemaChange();
    testCrashMatrices();
    testDeterministicDataset();
    testRepeatedLifecycle();

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
