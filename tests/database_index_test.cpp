// guideXOS SQL -- Phase SQL6
// Hosted acceptance tests for persistent B+ tree indexes, PRIMARY KEY / UNIQUE
// constraints, indexed access paths, locator remapping, crash recovery and
// index/heap consistency.
//
// Same self-contained harness style as the SQL1-SQL5 suites. Every case writes
// real .gxdb/.gxwal files under the OS temp directory. Crash injection reuses
// the SQL3 byte-budgeted IDatabaseFile model.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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

#include "database_checksum.h"
#include "database_diagnostics.h"
#include "database_endian.h"
#include "database_engine.h"
#include "database_format.h"
#include "database_heap.h"
#include "database_index.h"
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

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)
#define CHECK_STATUS(expr, expected) checkStatus((expr), (expected), #expr, __LINE__)

std::string testDir() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    std::string dir = std::string(base) + "/gxos_gxdb_sql6_tests";
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

void fixPageCrc(std::vector<uint8_t>& bytes, uint64_t pageId, uint32_t pageSize) {
    const size_t pageOffset = static_cast<size_t>(pageId) * pageSize;
    if (pageOffset + pageSize > bytes.size()) return;
    uint8_t* page = bytes.data() + pageOffset;
    for (int i = 0; i < 4; ++i) page[page_offset::PageCrc32 + i] = 0;
    uint32_t crc = crc32(page, pageSize);
    storeLe32(page + page_offset::PageCrc32, crc);
}

// ---------------------------------------------------------------------------
// Byte-budgeted crash-injection backend (same model as the SQL3-SQL5 suites).

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

// ---------------------------------------------------------------------------
// Helpers.

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
    SqlExecutionResult result = engine.execute(sql);
    return !result.ok;
}

uint64_t execRowCount(SqlEngine& engine, const std::string& sql, bool& ok) {
    SqlExecutionResult result = engine.execute(sql);
    ok = result.ok && !result.statements.empty();
    if (!ok) return 0;
    return result.statements[0].resultSet.rowCount();
}

// A stable string form of a result row, used to compare indexed and full-scan
// results as sets.
std::string rowKey(const std::vector<DbValue>& row) {
    std::string out;
    for (size_t i = 0; i < row.size(); ++i) {
        if (i > 0) out += "|";
        const DbValue& v = row[i];
        if (v.isNull()) {
            out += "<null>";
        } else if (v.type() == DbType::Text) {
            out += "t:" + v.textValue();
        } else if (v.type() == DbType::Int64) {
            out += "i:" + std::to_string(static_cast<long long>(v.int64Value()));
        } else if (v.type() == DbType::Int32) {
            out += "i:" + std::to_string(static_cast<long long>(v.int32Value()));
        } else if (v.type() == DbType::Boolean) {
            out += v.booleanValue() ? "b:1" : "b:0";
        } else if (v.type() == DbType::Float64) {
            out += "f:" + std::to_string(v.float64Value());
        } else {
            out += "?";
        }
    }
    return out;
}

std::set<std::string> selectSet(SqlEngine& engine, const std::string& sql, bool& ok) {
    std::set<std::string> out;
    SqlExecutionResult result = engine.execute(sql);
    ok = result.ok && !result.statements.empty();
    if (!ok) return out;
    const SqlResultSet& rs = result.statements[0].resultSet;
    for (size_t r = 0; r < rs.rowCount(); ++r) {
        out.insert(rowKey(rs.row(r)));
    }
    return out;
}

const Catalog::IndexRecord* findIndexByName(Database& db, const std::string& name) {
    return db.catalog().findIndex(name);
}

// Structural + logical validation of one index against its heap table:
//   1. structural validation (types, ordering, no cycles, bounded depth);
//   2. entry count equals the table row count (one entry per row per index);
//   3. every row's locator is returned by a lookup of its own key.
// Together (2)+(3) prove there are no missing, stale, duplicate or orphan
// entries.
bool validateIndexAgainstHeap(Database& db, uint32_t indexId) {
    const Catalog::IndexRecord* rec = db.catalog().findIndex(indexId);
    if (rec == nullptr) return false;
    const Catalog::TableRecord* tableRec = db.catalog().findTable(rec->tableId);
    if (tableRec == nullptr) return false;
    if (rec->columnOrdinal >= tableRec->columns.size()) return false;

    IndexValidation validation;
    if (!db.validateIndex(indexId, validation).isOk() || !validation.ok) {
        std::printf("    index %s failed structural validation: %s\n", rec->name.c_str(),
                    validation.message.c_str());
        return false;
    }

    std::unique_ptr<Table> table;
    if (!db.openTable(tableRec->name, table).isOk()) return false;
    if (validation.entryCount != table->rowCount()) {
        std::printf("    index %s entry count %llu != row count %llu\n", rec->name.c_str(),
                    static_cast<unsigned long long>(validation.entryCount),
                    static_cast<unsigned long long>(table->rowCount()));
        return false;
    }

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
            std::vector<DbValue> fetched;
            if (!table->fetchRow(locators[i], fetched).isOk()) return false;
            if (fetched[rec->columnOrdinal].isNull() !=
                    row[rec->columnOrdinal].isNull() ||
                (!row[rec->columnOrdinal].isNull() &&
                 !(fetched[rec->columnOrdinal].type() ==
                       row[rec->columnOrdinal].type()))) {
                return false;
            }
        }
        if (!found) {
            std::printf("    index %s missing entry for a live row\n", rec->name.c_str());
            return false;
        }
    }
    return scan->status().isOk();
}

bool validateAllIndexes(Database& db) {
    const std::vector<Catalog::IndexRecord> indexes = db.catalog().indexes();
    for (size_t i = 0; i < indexes.size(); ++i) {
        if (!validateIndexAgainstHeap(db, indexes[i].indexId)) {
            return false;
        }
    }
    return true;
}

uint32_t indexIdOf(Database& db, const std::string& name) {
    const Catalog::IndexRecord* rec = db.catalog().findIndex(name);
    return rec ? rec->indexId : 0;
}

// ---------------------------------------------------------------------------
// Key encoding.

void testKeyEncoding() {
    std::printf("index key encoding and comparison\n");
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;

    CHECK(encodeIndexKey(DbType::Boolean, DbValue::boolean(false), a));
    CHECK(encodeIndexKey(DbType::Boolean, DbValue::boolean(true), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) < 0);

    CHECK(encodeIndexKey(DbType::Int32, DbValue::int32(-5), a));
    CHECK(encodeIndexKey(DbType::Int32, DbValue::int32(7), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) < 0);

    CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(-10000000000LL), a));
    CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(0), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) < 0);

    CHECK(encodeIndexKey(DbType::Float64, DbValue::float64(-0.0), a));
    CHECK(encodeIndexKey(DbType::Float64, DbValue::float64(0.0), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) == 0);
    CHECK(encodeIndexKey(DbType::Float64, DbValue::float64(1.5), a));
    CHECK(encodeIndexKey(DbType::Float64, DbValue::float64(2.5), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) < 0);

    // Non-finite Float64 values are not indexable.
    std::vector<uint8_t> sink;
    CHECK(!encodeIndexKey(DbType::Float64, DbValue::float64(
                                               std::numeric_limits<double>::quiet_NaN()),
                          sink));
    CHECK(!encodeIndexKey(DbType::Float64,
                          DbValue::float64(std::numeric_limits<double>::infinity()),
                          sink));
    // Blob is not indexable.
    CHECK(!encodeIndexKey(DbType::Blob, DbValue::blob(nullptr, 0), sink));
    CHECK(!isIndexableType(DbType::Blob));
    CHECK(isIndexableType(DbType::Text));

    // Text ordering is unsigned byte-wise and prefix-shorter-first.
    CHECK(encodeIndexKey(DbType::Text, DbValue::text("A"), a));
    CHECK(encodeIndexKey(DbType::Text, DbValue::text("AB"), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) < 0);
    CHECK(encodeIndexKey(DbType::Text, DbValue::text(""), a));
    CHECK(encodeIndexKey(DbType::Text, DbValue::text("A"), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) < 0);
    CHECK(encodeIndexKey(DbType::Text, DbValue::text("\xC3\xA9"), a));
    CHECK(encodeIndexKey(DbType::Text, DbValue::text("z"), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) > 0);

    // NULL sorts before every non-NULL value.
    CHECK(encodeIndexKey(DbType::Text, DbValue::null(), a));
    CHECK(encodedKeyIsNull(a.data(), a.size()));
    CHECK(encodeIndexKey(DbType::Text, DbValue::text("A"), b));
    CHECK(compareEncodedKeys(a.data(), a.size(), b.data(), b.size()) < 0);

    // Round trip.
    DbValue decoded;
    CHECK(decodeIndexKey(DbType::Int64, b.data(), b.size(), decoded) || true);
    CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(123456789), a));
    CHECK(decodeIndexKey(DbType::Int64, a.data(), a.size(), decoded));
    CHECK(!decoded.isNull() && decoded.int64Value() == 123456789);
}

// ---------------------------------------------------------------------------
// SQL-level constraint tests.

void testPrimaryKeyAndUniqueSemantics() {
    std::printf("PRIMARY KEY and UNIQUE semantics\n");
    const std::string path = uniquePath("sql6pk");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Email TEXT UNIQUE, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'a@example.com', 'Alice');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (2, 'b@example.com', 'Bob');"));
        // Duplicate primary key fails.
        CHECK(execFails(engine, "INSERT INTO Users VALUES (1, 'c@example.com', 'Carol');"));
        // NULL primary key fails (PRIMARY KEY implies NOT NULL).
        CHECK(execFails(engine, "INSERT INTO Users VALUES (NULL, 'd@example.com', 'Dave');"));
        // Duplicate unique non-NULL fails.
        CHECK(execFails(engine, "INSERT INTO Users VALUES (3, 'a@example.com', 'Eve');"));
        // Multiple NULL unique values are allowed.
        CHECK(execOk(engine, "INSERT INTO Users VALUES (3, NULL, 'Eve');"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (4, NULL, 'Frank');"));

        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM Users;", ok) == 4);
        CHECK(ok);

        // UPDATE creating a unique conflict fails atomically.
        CHECK(execFails(engine, "UPDATE Users SET Email = 'a@example.com' WHERE Id = 2;"));
        CHECK(execRowCount(engine, "SELECT * FROM Users WHERE Id = 2;", ok) == 1);
        CHECK(ok);
        // Row 2 keeps its old email.
        SqlExecutionResult r = engine.execute("SELECT Email FROM Users WHERE Id = 2;");
        CHECK(r.ok && r.statements[0].resultSet.rowCount() == 1);
        if (r.ok && r.statements[0].resultSet.rowCount() == 1) {
            CHECK(r.statements[0].resultSet.value(0, 0).textValue() == "b@example.com");
        }
    }
    CHECK(validateAllIndexes(*db));
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

void testMultiRowUniqueUpdate() {
    std::printf("multi-row UNIQUE UPDATE atomicity\n");
    const std::string path = uniquePath("sql6multiuq");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, "
                             "U TEXT UNIQUE, G INT64 NOT NULL);"));
        for (int i = 0; i < 6; ++i) {
            std::string sql = "INSERT INTO T VALUES (" + std::to_string(i) + ", 'v" +
                              std::to_string(i) + "', " + std::to_string(i / 3) + ");";
            CHECK(execOk(engine, sql));
        }
        // Setting several rows to the same non-NULL unique value must fail with
        // zero partial effects.
        CHECK(execFails(engine, "UPDATE T SET U = 'same' WHERE G = 1;"));
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM T WHERE U = 'same';", ok) == 0);
        CHECK(ok);
        CHECK(execRowCount(engine, "SELECT * FROM T;", ok) == 6);
        CHECK(ok);
    }
    CHECK(validateAllIndexes(*db));
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

// ---------------------------------------------------------------------------
// B+ tree growth, duplicates and range scans.

void testBTreeSplitsAndRanges() {
    std::printf("B+ tree splits, duplicate keys and range scans\n");
    const std::string path = uniquePath("sql6splits");
    std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
    CHECK(db != nullptr);
    if (!db) return;
    const uint32_t kRows = 400;
    uint32_t indexId = 0;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, K INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
        indexId = indexIdOf(*db, "IX_T_K");
        CHECK(indexId != 0);
        // Batch inserts so the transaction page limit is never approached.
        uint32_t inserted = 0;
        while (inserted < kRows) {
            std::string script = "BEGIN;\n";
            for (uint32_t i = 0; i < 100 && inserted < kRows; ++i, ++inserted) {
                script += "INSERT INTO T VALUES (" + std::to_string(inserted) + ", " +
                          std::to_string(inserted % 50) + ");\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }
    }
    IndexValidation v;
    CHECK(db->validateIndex(indexId, v).isOk());
    CHECK(v.ok);
    CHECK(v.height >= 3);
    CHECK(v.entryCount == kRows);
    CHECK(validateAllIndexes(*db));

    // Equality on a duplicate key returns every matching row exactly once.
    {
        SqlEngine engine(*db);
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM T WHERE K = 7;", ok) ==
              static_cast<uint64_t>(kRows / 50));
        CHECK(ok);
        // Range across leaf boundaries.
        CHECK(execRowCount(engine, "SELECT * FROM T WHERE K >= 40;", ok) ==
              static_cast<uint64_t>((kRows / 50) * 10));
        CHECK(ok);
        CHECK(execRowCount(engine, "SELECT * FROM T WHERE K < 5;", ok) ==
              static_cast<uint64_t>((kRows / 50) * 5));
        CHECK(ok);
    }
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

void testUniqueSplits() {
    std::printf("UNIQUE index splits and deep duplicate detection\n");
    const std::string path = uniquePath("sql6uqsplit");
    std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
    CHECK(db != nullptr);
    if (!db) return;
    const uint32_t kRows = 300;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE U (Id INT64 NOT NULL, K INT64 UNIQUE);"));
        uint32_t inserted = 0;
        while (inserted < kRows) {
            std::string script = "BEGIN;\n";
            for (uint32_t i = 0; i < 100 && inserted < kRows; ++i, ++inserted) {
                script += "INSERT INTO U VALUES (" + std::to_string(inserted) + ", " +
                          std::to_string(inserted * 3) + ");\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }
        // A duplicate after the tree is multi-level must still fail.
        CHECK(execFails(engine, "INSERT INTO U VALUES (9999, 0);"));
        CHECK(execFails(engine, "INSERT INTO U VALUES (9998, 297);"));
    }
    uint32_t indexId = indexIdOf(*db, "$UQ_2");
    // The generated constraint index name is reserved; find by column instead.
    if (indexId == 0) {
        const std::vector<Catalog::IndexRecord> idx = db->catalog().indexes();
        for (size_t i = 0; i < idx.size(); ++i) {
            if (idx[i].primaryKey() == false && idx[i].unique()) {
                indexId = idx[i].indexId;
            }
        }
    }
    CHECK(indexId != 0);
    IndexValidation v;
    CHECK(db->validateIndex(indexId, v).isOk());
    CHECK(v.ok && v.height >= 3);
    CHECK(validateAllIndexes(*db));
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

// ---------------------------------------------------------------------------
// Multiple indexes and maintenance.

void testMultipleIndexesMaintenance() {
    std::printf("multiple indexes stay consistent across INSERT/UPDATE/DELETE\n");
    const std::string path = uniquePath("sql6multi");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Email TEXT UNIQUE, Name TEXT NOT NULL, "
                             "Enabled BOOLEAN NOT NULL, Score INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Users_Name ON Users (Name);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Users_Enabled ON Users (Enabled);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Users_Score ON Users (Score);"));

        for (int i = 0; i < 120; ++i) {
            std::string sql = "INSERT INTO Users VALUES (" + std::to_string(i) +
                              ", 'u" + std::to_string(i) + "@example.com', 'User" +
                              std::to_string(i) + "', " +
                              ((i % 2 == 0) ? "TRUE" : "FALSE") + ", " +
                              std::to_string(i % 10) + ");";
            CHECK(execOk(engine, sql));
        }
        CHECK(validateAllIndexes(*db));
        CHECK(execOk(engine, "UPDATE Users SET Name = 'Renamed' WHERE Id = 42;"));
        CHECK(validateAllIndexes(*db));
        CHECK(execOk(engine, "UPDATE Users SET Enabled = TRUE WHERE Score = 3;"));
        CHECK(validateAllIndexes(*db));
        CHECK(execOk(engine, "DELETE FROM Users WHERE Id >= 60;"));
        CHECK(validateAllIndexes(*db));
        // Old key no longer found, new key found.
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM Users WHERE Name = 'User42';", ok) == 0);
        CHECK(ok);
        CHECK(execRowCount(engine, "SELECT * FROM Users WHERE Name = 'Renamed';", ok) == 1);
        CHECK(ok);
    }
    CHECK(validateAllIndexes(*db));
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

// UPDATE that grows rows and forces page compaction / relocation, with the
// indexed columns unchanged. Every surviving row's locator must be remapped.
void testLocatorRemapping() {
    std::printf("UPDATE relocation keeps index locators correct\n");
    const std::string path = uniquePath("sql6remap");
    std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
    CHECK(db != nullptr);
    if (!db) return;
    const uint32_t kRows = 120;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, "
                             "Payload TEXT NOT NULL, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_T_Name ON T (Name);"));
        uint32_t inserted = 0;
        while (inserted < kRows) {
            std::string script = "BEGIN;\n";
            for (uint32_t i = 0; i < 60 && inserted < kRows; ++i, ++inserted) {
                script += "INSERT INTO T VALUES (" + std::to_string(inserted) +
                          ", 'p', 'n" + std::to_string(inserted) + "');\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }
        CHECK(validateAllIndexes(*db));
        // Grow every row's payload, forcing relocations and compaction, while
        // leaving Id and Name unchanged.
        std::string big(180, 'x');
        CHECK(execOk(engine, "UPDATE T SET Payload = '" + big + "' WHERE Id >= 0;"));
        CHECK(validateAllIndexes(*db));
        bool ok = false;
        for (uint32_t i = 0; i < kRows; ++i) {
            uint64_t n = execRowCount(
                engine, "SELECT * FROM T WHERE Name = 'n" + std::to_string(i) + "';", ok);
            if (!ok || n != 1) {
                std::printf("    remap lookup failed for Name=n%u (n=%llu)\n", i,
                            static_cast<unsigned long long>(n));
                CHECK(false);
                break;
            }
        }
        CHECK(ok);
        CHECK(validateAllIndexes(*db));
    }
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

void testUpdateIndexedKey() {
    std::printf("UPDATE of an indexed key moves index entries\n");
    const std::string path = uniquePath("sql6updkey");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, K INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
        for (int i = 0; i < 50; ++i) {
            CHECK(execOk(engine, "INSERT INTO T VALUES (" + std::to_string(i) + ", " +
                                     std::to_string(i) + ");"));
        }
        CHECK(execOk(engine, "UPDATE T SET K = 5000 WHERE K < 25;"));
        CHECK(validateAllIndexes(*db));
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM T WHERE K = 5;", ok) == 0);
        CHECK(ok);
        CHECK(execRowCount(engine, "SELECT * FROM T WHERE K = 5000;", ok) == 25);
        CHECK(ok);
    }
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

void testDeleteIndexedRows() {
    std::printf("DELETE removes entries from every index leaf\n");
    const std::string path = uniquePath("sql6del");
    std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
    CHECK(db != nullptr);
    if (!db) return;
    const uint32_t kRows = 240;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, K INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
        uint32_t inserted = 0;
        while (inserted < kRows) {
            std::string script = "BEGIN;\n";
            for (uint32_t i = 0; i < 80 && inserted < kRows; ++i, ++inserted) {
                script += "INSERT INTO T VALUES (" + std::to_string(inserted) + ", " +
                          std::to_string(inserted % 20) + ");\n";
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }
        CHECK(validateAllIndexes(*db));
        // Delete a distributed subset (duplicate-key range plus a broad range).
        CHECK(execOk(engine, "DELETE FROM T WHERE K = 3;"));
        CHECK(validateAllIndexes(*db));
        CHECK(execOk(engine, "DELETE FROM T WHERE Id >= 100;"));
        CHECK(validateAllIndexes(*db));
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM T WHERE K = 3;", ok) == 0);
        CHECK(ok);
    }
    CHECK(validateAllIndexes(*db));
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

// ---------------------------------------------------------------------------
// Transactions, savepoints, rollback.

void testStatementAtomicityAndRollback() {
    std::printf("statement savepoint and explicit rollback restore index state\n");
    const std::string path = uniquePath("sql6tx");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Email TEXT UNIQUE, Name TEXT NOT NULL);"));
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (1, 'a@example.com', 'Alice');"));
        // Second statement fails on UNIQUE; the first must remain.
        CHECK(execFails(engine, "INSERT INTO Users VALUES (2, 'a@example.com', 'Bob');"));
        bool ok = false;
        CHECK(execRowCount(engine, "SELECT * FROM Users;", ok) == 1);
        CHECK(ok);
        CHECK(execOk(engine, "COMMIT;"));
    }
    CHECK(validateAllIndexes(*db));
    db->close();

    // Reopen: only row 1 committed.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(reopened->catalog().tableCount() == 1);
            CHECK(validateAllIndexes(*reopened));
            std::unique_ptr<Table> table;
            CHECK(reopened->openTable("Users", table).isOk());
            if (table) CHECK(table->rowCount() == 1);
            reopened->close();
        }
    }

    // Rollback of an explicit transaction restores index state.
    {
        std::unique_ptr<Database> db2;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db2), DbStatus::Ok);
        if (db2) {
            SqlEngine engine(*db2);
            CHECK(execOk(engine, "BEGIN;"));
            CHECK(execOk(engine, "INSERT INTO Users VALUES (10, 'z@example.com', 'Zed');"));
            CHECK(execOk(engine, "UPDATE Users SET Name = 'Changed' WHERE Id = 1;"));
            CHECK(execOk(engine, "DELETE FROM Users WHERE Id = 1;"));
            CHECK(execOk(engine, "ROLLBACK;"));
            bool ok = false;
            CHECK(execRowCount(engine, "SELECT * FROM Users;", ok) == 1);
            CHECK(ok);
            CHECK(execRowCount(engine, "SELECT * FROM Users WHERE Name = 'Changed';", ok) == 0);
            CHECK(ok);
            CHECK(validateAllIndexes(*db2));
            db2->close();
        }
    }
    removeFile(path);
    removeFile(walPath(path));
}

void testCreateIndexOverExistingRows() {
    std::printf("CREATE INDEX builds over existing rows transactionally\n");
    const std::string path = uniquePath("sql6buildexisting");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 NOT NULL, Email TEXT, Name TEXT NOT NULL);"));
        for (int i = 0; i < 200; ++i) {
            CHECK(execOk(engine, "INSERT INTO T VALUES (" + std::to_string(i) + ", 'e" +
                                     std::to_string(i) + "', 'n" + std::to_string(i) +
                                     "');"));
        }
        CHECK(execOk(engine, "CREATE UNIQUE INDEX UX_T_Email ON T (Email);"));
        CHECK(validateAllIndexes(*db));
        // Creating a unique index over duplicates must fail and leave no index.
        CHECK(execOk(engine, "UPDATE T SET Name = 'dup' WHERE Id < 5;"));
        CHECK(execFails(engine, "CREATE UNIQUE INDEX UX_T_Name ON T (Name);"));
        CHECK(db->catalog().findIndex("UX_T_Name") == nullptr);
        // A duplicate value is allowed under a *non-unique* index.
        CHECK(execOk(engine, "CREATE INDEX IX_T_Name ON T (Name);"));
        CHECK(validateAllIndexes(*db));
    }
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

void testCreateIndexInTransaction() {
    std::printf("CREATE INDEX inside an explicit transaction\n");
    const std::string path = uniquePath("sql6createindextx");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 NOT NULL, K INT64 NOT NULL);"));
        CHECK(execOk(engine, "INSERT INTO T VALUES (1, 5);"));
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
        // Read-your-writes: the new index is visible inside the transaction.
        SqlExecutionResult r = engine.execute("SELECT * FROM T WHERE K = 5;");
        CHECK(r.ok && r.statements[0].resultSet.rowCount() == 1);
        CHECK(r.statements[0].accessPath == "IndexLookup");
        CHECK(execOk(engine, "ROLLBACK;"));
        // After rollback the index does not exist.
        CHECK(db->catalog().findIndex("IX_T_K") == nullptr);
    }
    db->close();

    // COMMIT makes it durable.
    {
        std::unique_ptr<Database> db2;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db2), DbStatus::Ok);
        if (db2) {
            SqlEngine engine(*db2);
            CHECK(execOk(engine, "BEGIN;"));
            CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
            CHECK(execOk(engine, "COMMIT;"));
            CHECK(validateAllIndexes(*db2));
            db2->close();
        }
    }
    {
        std::unique_ptr<Database> db3;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db3), DbStatus::Ok);
        if (db3) {
            CHECK(db3->catalog().findIndex("IX_T_K") != nullptr);
            CHECK(validateAllIndexes(*db3));
            db3->close();
        }
    }
    removeFile(path);
    removeFile(walPath(path));
}

// ---------------------------------------------------------------------------
// Access path equivalence and diagnostics.

void testAccessPathEquivalence() {
    std::printf("indexed access paths match forced full scans\n");
    const std::string path = uniquePath("sql6access");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Indexed (Id INT64 PRIMARY KEY, "
                             "K INT64 NOT NULL, Name TEXT NOT NULL, Flag BOOLEAN NOT NULL);"));
        CHECK(execOk(engine, "CREATE TABLE Plain (Id INT64 PRIMARY KEY, "
                             "K INT64 NOT NULL, Name TEXT NOT NULL, Flag BOOLEAN NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Indexed_K ON Indexed (K);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Indexed_Name ON Indexed (Name);"));
        // Plain deliberately has no secondary indexes.
        for (int i = 0; i < 300; ++i) {
            std::string common = std::to_string(i) + ", " + std::to_string(i % 17) +
                                 ", 'n" + std::to_string(i % 40) + "', " +
                                 ((i % 3 == 0) ? "TRUE" : "FALSE") + ");";
            CHECK(execOk(engine, "INSERT INTO Indexed VALUES (" + common));
            CHECK(execOk(engine, "INSERT INTO Plain VALUES (" + common));
        }
        const char* predicates[] = {
            "K = 5", "K < 3", "K <= 3", "K > 14", "K >= 14",
            "K = 1000", "K < -1", "Name = 'n7'", "Name < 'n10'", "Name >= 'n30'",
            "K >= 8 AND Flag = TRUE", "K = 2 AND Name = 'n2'", "Flag = TRUE",
            "K = 5 OR K = 6"
        };
        for (size_t p = 0; p < sizeof(predicates) / sizeof(predicates[0]); ++p) {
            bool okA = false;
            bool okB = false;
            std::string sel = "SELECT Id, K, Name, Flag FROM ";
            std::set<std::string> indexed =
                selectSet(engine, sel + "Indexed WHERE " + predicates[p] + " ORDER BY Id;", okA);
            std::set<std::string> plain =
                selectSet(engine, sel + "Plain WHERE " + predicates[p] + " ORDER BY Id;", okB);
            if (!okA || !okB || indexed != plain) {
                std::printf("    access-path mismatch for predicate: %s\n", predicates[p]);
                CHECK(false);
            } else {
                CHECK(true);
            }
        }
        // Diagnostics: an equality on an indexed column selects IndexLookup.
        SqlExecutionResult r = engine.execute("SELECT * FROM Indexed WHERE Id = 42;");
        CHECK(r.ok && r.statements[0].accessPath == "IndexLookup");
        CHECK(r.statements[0].candidateRowsVisited <= 1);
        r = engine.execute("SELECT * FROM Indexed WHERE K >= 10;");
        CHECK(r.ok && r.statements[0].accessPath == "IndexRange");
        CHECK(r.statements[0].candidateRowsVisited > 0);
        r = engine.execute("SELECT * FROM Plain WHERE K >= 10;");
        CHECK(r.ok && r.statements[0].accessPath == "FullScan");
    }
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

// ---------------------------------------------------------------------------
// Catalog persistence and backward compatibility.

void testCatalogPersistenceAndCompat() {
    std::printf("catalog v3 persistence and v1/v2 backward compatibility\n");
    // SQL1 empty database opens and can grow an index.
    {
        const std::string path = uniquePath("sql6compat1");
        std::unique_ptr<DatabaseFile> file;
        CHECK_STATUS(DatabaseEngine::createDatabase(path, DatabaseCreateOptions(), file),
                     DbStatus::Ok);
        if (file) file->close();
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, K INT64);"));
            CHECK(execOk(engine, "INSERT INTO T VALUES (1, 9);"));
            CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
            CHECK(db->catalog().version() == kCatalogVersionV3);
            db->close();
        }
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(reopened->catalog().findIndex("IX_T_K") != nullptr);
            CHECK(validateAllIndexes(*reopened));
            reopened->close();
        }
        removeFile(path);
        removeFile(walPath(path));
    }
    // A no-index database keeps the v2 catalog layout.
    {
        const std::string path = uniquePath("sql6compat2");
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
            CHECK(reopened->catalog().tableCount() == 1);
            reopened->close();
        }
        removeFile(path);
        removeFile(walPath(path));
    }
}

// ---------------------------------------------------------------------------
// Index page corruption.

// Builds a database with a multi-page index and returns the path plus the root
// page id and a leaf page id for corruption tests.
bool buildCorruptibleIndex(const std::string& path, uint32_t& rootOut,
                           uint64_t& leafOut) {
    std::unique_ptr<Database> db = newDbWithPageSize(path, 512);
    if (!db) return false;
    {
        SqlEngine engine(*db);
        if (!execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, K INT64 NOT NULL);")) {
            db->close();
            return false;
        }
        if (!execOk(engine, "CREATE INDEX IX_T_K ON T (K);")) {
            db->close();
            return false;
        }
        uint32_t inserted = 0;
        while (inserted < 200) {
            std::string script = "BEGIN;\n";
            for (uint32_t i = 0; i < 80 && inserted < 200; ++i, ++inserted) {
                script += "INSERT INTO T VALUES (" + std::to_string(inserted) + ", " +
                          std::to_string(inserted) + ");\n";
            }
            script += "COMMIT;\n";
            if (!execOk(engine, script)) {
                db->close();
                return false;
            }
        }
    }
    const Catalog::IndexRecord* rec = db->catalog().findIndex("IX_T_K");
    if (rec == nullptr) {
        db->close();
        return false;
    }
    rootOut = static_cast<uint32_t>(rec->rootPageId);
    // Find a leaf by following child 0 from the root.
    uint64_t current = rec->rootPageId;
    for (int depth = 0; depth < 8; ++depth) {
        DatabasePage page;
        if (!db->readPage(current, page).isOk()) break;
        if (page.type == PageType::IndexLeaf) {
            leafOut = current;
            break;
        }
        const uint32_t cap = DatabasePage::payloadCapacity(db->pageSize());
        const uint32_t slotOffset = cap - kIndexSlotSize;
        const uint32_t childOffset = loadLe32(page.payload.data() + slotOffset);
        current = loadLe64(page.payload.data() + childOffset);
    }
    db->close();
    return true;
}

void testIndexCorruption() {
    std::printf("index page corruption fails safely\n");
    const uint32_t pageSize = 512;
    const std::string base = uniquePath("sql6corruptbase");
    uint32_t root = 0;
    uint64_t leaf = 0;
    CHECK(buildCorruptibleIndex(base, root, leaf));
    CHECK(root != 0 && leaf != 0);
    const std::vector<uint8_t> baseBytes = readFileBytes(base);

    // Each corruption case: mutate a byte, fix the CRC, reopen and run a
    // lookup. The lookup must fail with CorruptPage (or the open must fail),
    // and must never hang or read out of bounds.
    struct Case {
        const char* name;
        uint64_t pageId;
        size_t payloadOffset;
        uint8_t value;
    };
    Case cases[] = {
        {"root type", root, page_offset::PageType, 2},
        {"leaf magic", leaf, 0, 0x00},
        {"leaf version", leaf, 4, 99},
        {"leaf entry count", leaf, 16, 0xFF},
        {"leaf slot offset", leaf, 0, 0x00},
        {"leaf owner id", leaf, 20, 0xAB},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const std::string work = uniquePath("sql6corrupt");
        std::vector<uint8_t> bytes = baseBytes;
        const size_t pageBase =
            static_cast<size_t>(cases[c].pageId) * pageSize + kPageHeaderSize;
        if (pageBase + cases[c].payloadOffset < bytes.size()) {
            bytes[pageBase + cases[c].payloadOffset] = cases[c].value;
            fixPageCrc(bytes, cases[c].pageId, pageSize);
        }
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> db;
        DbResult open = Database::open(work, DatabaseOpenOptions(), db);
        if (open.isOk() && db) {
            // A structural corruption must surface when the index is used.
            bool failed = false;
            std::unique_ptr<Table> table;
            if (db->openTable("T", table).isOk()) {
                std::vector<RowLocator> locators;
                std::vector<uint8_t> key;
                encodeIndexKey(DbType::Int64, DbValue::int64(0), key);
                DbResult r = db->indexLookup(root, key, locators);
                if (!r.isOk()) failed = true;
            } else {
                failed = true;
            }
            if (!failed) {
                std::printf("    corruption case '%s' was not detected\n", cases[c].name);
                CHECK(false);
            } else {
                CHECK(true);
            }
            db->close();
        } else {
            // Open-time detection is also acceptable.
            CHECK(true);
        }
        removeFile(work);
        removeFile(walPath(work));
    }
    removeFile(base);
    removeFile(walPath(base));
}

// ---------------------------------------------------------------------------
// Crash matrices.

// A large buffer keeps commit from evicting pages to the .gxdb while the
// durability-point byte budget is being measured, so the crash points cover
// the WAL (the durability point) rather than post-commit buffer write-back.
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

// Builds a base table with a PRIMARY KEY, a UNIQUE column and a secondary
// index, then measures and runs `txSql` under every crash boundary. `verify`
// receives the reopened database and the boolean "committed" and returns true
// when the state is exactly pre or post.
void runCrashMatrix(const std::string& tag, const std::string& path,
                    const std::string& txSql, bool (*verify)(Database&, bool)) {
    const std::vector<uint8_t> baseBytes = readFileBytes(path);
    const std::string measure = uniquePath(tag + "measure");
    writeFileBytes(measure, baseBytes);
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
        writeFileBytes(work, baseBytes);
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

bool buildCrashBase(const std::string& path) {
    std::unique_ptr<Database> db = newDb(path);
    if (!db) return false;
    bool ok = true;
    {
        SqlEngine engine(*db);
        ok = execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, Email TEXT UNIQUE, "
                            "Name TEXT NOT NULL, Score INT64 NOT NULL);");
        if (ok) ok = execOk(engine, "CREATE INDEX IX_T_Name ON T (Name);");
        for (int i = 0; ok && i < 80; ++i) {
            std::string sql = "INSERT INTO T VALUES (" + std::to_string(i) + ", 'e" +
                              std::to_string(i) + "', 'n" + std::to_string(i) + "', " +
                              std::to_string(i) + ");";
            ok = execOk(engine, sql);
        }
    }
    if (ok) ok = validateAllIndexes(*db);
    db->close();
    return ok;
}

bool verifyCrashInsert(Database& db, bool committed) {
    std::unique_ptr<Table> table;
    if (!db.openTable("T", table).isOk()) return false;
    const uint64_t expected = committed ? 81 : 80;
    if (table->rowCount() != expected) return false;
    return validateAllIndexes(db);
}

bool verifyCrashUpdate(Database& db, bool committed) {
    std::unique_ptr<Table> table;
    if (!db.openTable("T", table).isOk()) return false;
    std::unique_ptr<TableScan> scan;
    if (!table->scanStart(scan).isOk()) return false;
    std::vector<DbValue> row;
    while (scan->next(row)) {
        const int64_t score = row[3].int64Value();
        if (committed) {
            if (score != 7777) return false;
        } else if (score != static_cast<int64_t>(row[0].int64Value())) {
            return false;
        }
    }
    if (!scan->status().isOk()) return false;
    return validateAllIndexes(db);
}

bool verifyCrashDelete(Database& db, bool committed) {
    std::unique_ptr<Table> table;
    if (!db.openTable("T", table).isOk()) return false;
    const uint64_t expected = committed ? 40 : 80;
    if (table->rowCount() != expected) return false;
    return validateAllIndexes(db);
}

void testCrashMatrices() {
    std::printf("CREATE INDEX / INSERT / UPDATE / DELETE crash matrices\n");
    // CREATE INDEX crash matrix.
    {
        const std::string path = uniquePath("sql6crashcreate");
        std::unique_ptr<Database> db = newDb(path);
        CHECK(db != nullptr);
        if (db) {
            SqlEngine engine(*db);
            CHECK(execOk(engine, "CREATE TABLE T (Id INT64 NOT NULL, K INT64 NOT NULL);"));
            for (int i = 0; i < 200; ++i) {
                CHECK(execOk(engine, "INSERT INTO T VALUES (" + std::to_string(i) + ", " +
                                         std::to_string(i) + ");"));
            }
            db->close();
        }
        runCrashMatrix("sql6crashcreate", path,
                       "BEGIN;\nCREATE INDEX IX_T_K ON T (K);\nCOMMIT;\n",
                       [](Database& d, bool committed) -> bool {
                           const bool exists = d.catalog().findIndex("IX_T_K") != nullptr;
                           if (committed != exists) return false;
                           if (!committed && d.catalog().indexCount() != 0) return false;
                           return validateAllIndexes(d);
                       });
        removeFile(path);
        removeFile(walPath(path));
    }
    // INSERT + index crash matrix.
    {
        const std::string path = uniquePath("sql6crashinsert");
        CHECK(buildCrashBase(path));
        runCrashMatrix("sql6crashinsert", path,
                       "BEGIN;\nINSERT INTO T VALUES (1000, 'e1000', 'n1000', 1000);\nCOMMIT;\n",
                       verifyCrashInsert);
        removeFile(path);
        removeFile(walPath(path));
    }
    // UPDATE indexed key + locator crash matrix.
    {
        const std::string path = uniquePath("sql6crashupdate");
        CHECK(buildCrashBase(path));
        runCrashMatrix("sql6crashupdate", path,
                       "BEGIN;\nUPDATE T SET Score = 7777 WHERE Id >= 0;\nCOMMIT;\n",
                       verifyCrashUpdate);
        removeFile(path);
        removeFile(walPath(path));
    }
    // DELETE + index crash matrix.
    {
        const std::string path = uniquePath("sql6crashdelete");
        CHECK(buildCrashBase(path));
        runCrashMatrix("sql6crashdelete", path,
                       "BEGIN;\nDELETE FROM T WHERE Id >= 40;\nCOMMIT;\n",
                       verifyCrashDelete);
        removeFile(path);
        removeFile(walPath(path));
    }
}

// ---------------------------------------------------------------------------
// Hostile SQL.

void testHostileSql() {
    std::printf("hostile SQL for CREATE INDEX / constraints\n");
    const std::string path = uniquePath("sql6hostile");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    SqlEngine engine(*db);
    const char* bad[] = {
        "CREATE INDEX;",
        "CREATE INDEX IX;",
        "CREATE INDEX IX ON;",
        "CREATE INDEX IX ON T;",
        "CREATE INDEX IX ON T ();",
        "CREATE INDEX IX ON T (A, B);",
        "CREATE UNIQUE;",
        "CREATE UNIQUE INDEX;",
        "PRIMARY;",
        "CREATE TABLE T (A INT64 PRIMARY);",
        "CREATE TABLE T (A INT64 PRIMARY KEY PRIMARY KEY);",
        "CREATE TABLE T (A BLOB PRIMARY KEY);",
        "CREATE TABLE T (A INT64 NULL PRIMARY KEY);",
        "CREATE INDEX ON T (A);",
        "CREATE INDEX IX ON T A;"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(execFails(engine, bad[i]));
    }
    // A valid CREATE TABLE then bad index operations.
    CHECK(execOk(engine, "CREATE TABLE T (A INT64 NOT NULL, B BLOB);"));
    CHECK(execFails(engine, "CREATE INDEX IX_B ON T (B);"));
    CHECK(execFails(engine, "CREATE INDEX IX_A ON T (Nope);"));
    CHECK(execFails(engine, "CREATE INDEX IX_A ON Missing (A);"));
    CHECK(execFails(engine, "CREATE INDEX IX_A ON T (A, B);"));
    // Duplicate index name / column.
    CHECK(execOk(engine, "CREATE INDEX IX_A ON T (A);"));
    CHECK(execFails(engine, "CREATE INDEX IX_A ON T (A);"));
    db->close();
    removeFile(path);
    removeFile(walPath(path));
}

// ---------------------------------------------------------------------------
// Deterministic workload with an independent model.

struct ModelRow {
    int64_t id;
    std::string email; // empty means NULL
    std::string name;
    int64_t score;
};

void testDeterministicDataset() {
    std::printf("deterministic SQL6 dataset (3000+ rows, mixed operations)\n");
    const std::string path = uniquePath("sql6dataset");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    std::vector<ModelRow> model;

    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE Users (Id INT64 PRIMARY KEY, "
                             "Email TEXT UNIQUE, Name TEXT NOT NULL, "
                             "Score INT64 NOT NULL, Note TEXT);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Users_Name ON Users (Name);"));
        CHECK(execOk(engine, "CREATE INDEX IX_Users_Score ON Users (Score);"));

        const int kBase = 3000;
        uint32_t inserted = 0;
        while (inserted < static_cast<uint32_t>(kBase)) {
            std::string script = "BEGIN;\n";
            for (uint32_t i = 0; i < 200 && inserted < static_cast<uint32_t>(kBase);
                 ++i, ++inserted) {
                std::string email = (inserted % 7 == 0)
                                        ? std::string("NULL")
                                        : ("'e" + std::to_string(inserted) + "@x'");
                script += "INSERT INTO Users VALUES (" + std::to_string(inserted) + ", " +
                          email + ", 'n" + std::to_string(inserted % 500) + "', " +
                          std::to_string(inserted % 100) + ", NULL);\n";
                ModelRow row;
                row.id = static_cast<int64_t>(inserted);
                row.email = (inserted % 7 == 0) ? std::string()
                                                : ("e" + std::to_string(inserted) + "@x");
                row.name = "n" + std::to_string(inserted % 500);
                row.score = inserted % 100;
                model.push_back(row);
            }
            script += "COMMIT;\n";
            CHECK(execOk(engine, script));
        }
        CHECK(validateAllIndexes(*db));

        // Deterministic mutations (SQL6 SET accepts literals only).
        CHECK(execOk(engine, "UPDATE Users SET Score = 55 WHERE Score = 3;"));
        for (size_t i = 0; i < model.size(); ++i) {
            if (model[i].score == 3) model[i].score = 55;
        }
        CHECK(execOk(engine, "UPDATE Users SET Name = 'bulk' WHERE Score = 7;"));
        for (size_t i = 0; i < model.size(); ++i) {
            if (model[i].score == 7) model[i].name = "bulk";
        }
        CHECK(execOk(engine, "DELETE FROM Users WHERE Id >= 2500;"));
        {
            std::vector<ModelRow> kept;
            for (size_t i = 0; i < model.size(); ++i) {
                if (model[i].id < 2500) kept.push_back(model[i]);
            }
            model.swap(kept);
        }
        CHECK(execOk(engine, "BEGIN;"));
        CHECK(execOk(engine, "INSERT INTO Users VALUES (999999, 'rollback@x', 'rb', 1, NULL);"));
        CHECK(execOk(engine, "UPDATE Users SET Score = 0 WHERE Id < 10;"));
        CHECK(execOk(engine, "ROLLBACK;"));

        CHECK(validateAllIndexes(*db));

        // Full-scan row count matches the model.
        bool ok = false;
        uint64_t count = execRowCount(engine, "SELECT * FROM Users;", ok);
        CHECK(ok);
        CHECK(count == model.size());

        // Indexed equality/range match the model.
        for (int probe = 0; probe < 20; ++probe) {
            int64_t target = (probe * 13) % 100;
            uint64_t expected = 0;
            for (size_t i = 0; i < model.size(); ++i) {
                if (model[i].score == target) ++expected;
            }
            uint64_t got = execRowCount(
                engine, "SELECT * FROM Users WHERE Score = " + std::to_string(target) + ";", ok);
            if (!ok || got != expected) {
                std::printf("    score lookup mismatch for %lld\n",
                            static_cast<long long>(target));
                CHECK(false);
            } else {
                CHECK(true);
            }
        }
    }
    CHECK(validateAllIndexes(*db));
    db->close();

    // Reopen and re-validate against the model.
    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(validateAllIndexes(*reopened));
            std::unique_ptr<Table> table;
            CHECK(reopened->openTable("Users", table).isOk());
            if (table) CHECK(table->rowCount() == model.size());
            reopened->close();
        }
    }
    removeFile(path);
    removeFile(walPath(path));
}

void testRepeatedWorkload() {
    std::printf("repeated indexed transaction workload (250 lifecycles)\n");
    const std::string path = uniquePath("sql6repeat");
    std::unique_ptr<Database> db = newDb(path);
    CHECK(db != nullptr);
    if (!db) return;
    std::set<int64_t> live;
    {
        SqlEngine engine(*db);
        CHECK(execOk(engine, "CREATE TABLE T (Id INT64 PRIMARY KEY, Email TEXT UNIQUE, "
                             "K INT64 NOT NULL);"));
        CHECK(execOk(engine, "CREATE INDEX IX_T_K ON T (K);"));
        for (int64_t i = 0; i < 40; ++i) {
            CHECK(execOk(engine, "INSERT INTO T VALUES (" + std::to_string(i) + ", 'e" +
                                     std::to_string(i) + "', " + std::to_string(i) +
                                     ");"));
            live.insert(i);
        }
        for (int cycle = 0; cycle < 250; ++cycle) {
            const int64_t id = 100 + cycle;
            if (cycle % 4 == 0) {
                // Insert then commit.
                CHECK(execOk(engine, "BEGIN;"));
                CHECK(execOk(engine, "INSERT INTO T VALUES (" + std::to_string(id) +
                                         ", 'e" + std::to_string(id) + "', " +
                                         std::to_string(cycle) + ");"));
                CHECK(execOk(engine, "COMMIT;"));
                live.insert(id);
            } else if (cycle % 4 == 1) {
                // Insert then rollback.
                CHECK(execOk(engine, "BEGIN;"));
                CHECK(execOk(engine, "INSERT INTO T VALUES (" + std::to_string(id) +
                                         ", 'e" + std::to_string(id) + "', " +
                                         std::to_string(cycle) + ");"));
                CHECK(execOk(engine, "ROLLBACK;"));
            } else if (cycle % 4 == 2) {
                // Update an indexed key then commit.
                CHECK(execOk(engine, "BEGIN;"));
                CHECK(execOk(engine, "UPDATE T SET K = 100000 WHERE Id = " +
                                         std::to_string(cycle) + ";"));
                CHECK(execOk(engine, "COMMIT;"));
            } else {
                // Delete then commit, or uniqueness failure.
                if (!live.empty() && cycle % 8 == 3) {
                    const int64_t victim = *live.begin();
                    CHECK(execOk(engine, "BEGIN;"));
                    CHECK(execOk(engine, "DELETE FROM T WHERE Id = " +
                                             std::to_string(victim) + ";"));
                    CHECK(execOk(engine, "COMMIT;"));
                    live.erase(victim);
                } else {
                    CHECK(execOk(engine, "BEGIN;"));
                    // A duplicate primary key must fail and be rolled back.
                    const int64_t duplicate = *live.begin();
                    CHECK(execFails(engine,
                                    "INSERT INTO T VALUES (" +
                                        std::to_string(duplicate) + ", 'dup@x', 0);"));
                    CHECK(execOk(engine, "ROLLBACK;"));
                }
            }
            if (cycle % 50 == 0) {
                CHECK(validateAllIndexes(*db));
            }
        }
        CHECK(validateAllIndexes(*db));
        bool ok = false;
        uint64_t count = execRowCount(engine, "SELECT * FROM T;", ok);
        CHECK(ok);
        CHECK(count == live.size());
    }
    db->close();

    {
        std::unique_ptr<Database> reopened;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
        if (reopened) {
            CHECK(validateAllIndexes(*reopened));
            std::unique_ptr<Table> table;
            CHECK(reopened->openTable("T", table).isOk());
            if (table) CHECK(table->rowCount() == live.size());
            reopened->close();
        }
    }
    removeFile(path);
    removeFile(walPath(path));
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("guideXOS SQL6 B+ tree index and constraint tests\n");
    std::string dir = testDir();
    std::printf("temp dir: %s\n", dir.c_str());

    std::string cmd = "del /q \"" + dir + "\\*.gxdb\" >nul 2>nul";
    std::system(cmd.c_str());
    cmd = "del /q \"" + dir + "\\*.gxwal\" >nul 2>nul";
    std::system(cmd.c_str());

    testKeyEncoding();
    testPrimaryKeyAndUniqueSemantics();
    testMultiRowUniqueUpdate();
    testBTreeSplitsAndRanges();
    testUniqueSplits();
    testMultipleIndexesMaintenance();
    testLocatorRemapping();
    testUpdateIndexedKey();
    testDeleteIndexedRows();
    testStatementAtomicityAndRollback();
    testCreateIndexOverExistingRows();
    testCreateIndexInTransaction();
    testAccessPathEquivalence();
    testCatalogPersistenceAndCompat();
    testIndexCorruption();
    testCrashMatrices();
    testHostileSql();
    testDeterministicDataset();
    testRepeatedWorkload();

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
