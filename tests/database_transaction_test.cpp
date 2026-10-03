// guideXOS SQL -- Phase SQL3
// Hosted acceptance tests for crash-atomic transactions and the write-ahead log.
//
// No external test framework; same approach as the SQL1/SQL2 suites. Every case
// writes real .gxdb/.gxwal files under the OS temp directory.
//
// Crash injection uses a byte-budgeted IDatabaseFile backend that preserves
// exactly the bytes written before the injected boundary (a process-crash
// model). A "freeze" operation stops further writes at the current point,
// which lets a test crash after a durable WAL COMMIT but before any database
// page reaches the .gxdb.

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

#include "database_checksum.h"
#include "database_diagnostics.h"
#include "database_endian.h"
#include "database_engine.h"
#include "database_format.h"
#include "database_heap.h"
#include "database_io.h"
#include "database_relational.h"
#include "database_schema.h"
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
    std::string dir = std::string(base) + "/gxos_gxdb_sql3_tests";
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

bool copyFileBytes(const std::string& src, const std::string& dst) {
    return writeFileBytes(dst, readFileBytes(src));
}

bool fileExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

void removeFile(const std::string& path) {
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Byte-budgeted crash-injection backend.

struct BudgetState {
    uint64_t budget;    // max cumulative bytes written; UINT64_MAX = unlimited
    uint64_t written;   // cumulative bytes written so far
    bool exhausted;     // true once a write was cut short
    bool recording;     // record a boundary offset at the start of every write
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
// Helpers.

TableDefinition makeTable(const std::string& name) {
    TableDefinition def(name);
    def.columns.push_back(ColumnDefinition("Id", DbType::Int64, false));
    def.columns.push_back(ColumnDefinition("Tag", DbType::Text, false));
    def.columns.push_back(ColumnDefinition("Flag", DbType::Boolean, false));
    return def;
}

std::vector<DbValue> makeRow(uint64_t id) {
    std::vector<DbValue> row;
    row.push_back(DbValue::int64(static_cast<int64_t>(id)));
    row.push_back(DbValue::text("r" + std::to_string(id)));
    row.push_back(DbValue::boolean(id % 2 == 0));
    return row;
}

void buildBase(const std::string& path, const std::string& table, int rows) {
    std::unique_ptr<Database> db;
    if (!Database::create(path, DatabaseCreateOptions(), db).isOk() || !db) {
        return;
    }
    uint32_t id = 0;
    db->createTable(makeTable(table), id);
    std::unique_ptr<Table> t;
    db->openTable(table, t);
    for (int i = 0; i < rows; ++i) {
        t->insert(makeRow(static_cast<uint64_t>(i)));
    }
    db->flush();
    db->close();
}

uint64_t countRows(const std::string& path, const std::string& table) {
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db).isOk() || !db) {
        return UINT64_MAX;
    }
    std::unique_ptr<Table> t;
    if (!db->openTable(table, t).isOk()) {
        db->close();
        return UINT64_MAX;
    }
    std::unique_ptr<TableScan> scan;
    t->scanStart(scan);
    std::vector<DbValue> row;
    uint64_t count = 0;
    while (scan->next(row)) {
        ++count;
    }
    db->close();
    return count;
}

// Verifies rows are exactly 0..expectedCount-1 in order.
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
            row[1].textValue() != "r" + std::to_string(index) ||
            row[2].booleanValue() != (index % 2 == 0)) {
            ok = false;
            break;
        }
        ++index;
    }
    if (index != expectedCount) {
        ok = false;
    }
    if (!scan->status().isOk()) {
        ok = false;
    }
    db->close();
    return ok;
}

std::string walPath(const std::string& dbPath) {
    return walPathFor(dbPath);
}

// Runs an explicit transaction inserting `insertCount` rows starting at `startId`.
// `state` controls crash injection. Returns the commit result (or the first
// error). The Database is destroyed before returning (simulating process exit).
DbResult runCrashedInsert(const std::string& path, const std::string& table,
                          uint64_t startId, int insertCount,
                          std::shared_ptr<BudgetState> state) {
    BudgetFileSystem fs(state);
    std::unique_ptr<Database> db;
    DbResult open = Database::open(path, DatabaseOpenOptions(), db, &fs);
    if (!open.isOk()) {
        return open;
    }
    std::unique_ptr<Transaction> tx;
    DbResult result = db->beginTransaction(tx);
    if (!result.isOk()) {
        db->close();
        return result;
    }
    std::unique_ptr<Table> t;
    result = tx->openTable(table, t);
    if (!result.isOk()) {
        tx->rollback();
        db->close();
        return result;
    }
    for (int i = 0; i < insertCount; ++i) {
        result = t->insert(makeRow(startId + static_cast<uint64_t>(i)));
        if (!result.isOk()) {
            tx->rollback();
            db->close();
            return result;
        }
    }
    result = tx->commit();
    db->close();
    return result;
}

// Runs a transaction with a recording backend and returns the cumulative byte
// offset at the durability point (end of the COMMIT record) and the list of
// write boundaries. The database is left at post-state.
uint64_t measureCommit(const std::string& path, const std::string& table,
                       uint64_t startId, int insertCount,
                       std::vector<uint64_t>& boundaries) {
    std::shared_ptr<BudgetState> state(new BudgetState());
    state->recording = true;
    BudgetFileSystem fs(state);
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db, &fs).isOk() || !db) {
        return 0;
    }
    std::unique_ptr<Transaction> tx;
    if (!db->beginTransaction(tx).isOk()) {
        db->close();
        return 0;
    }
    std::unique_ptr<Table> t;
    if (!tx->openTable(table, t).isOk()) {
        tx->rollback();
        db->close();
        return 0;
    }
    for (int i = 0; i < insertCount; ++i) {
        if (!t->insert(makeRow(startId + static_cast<uint64_t>(i))).isOk()) {
            tx->rollback();
            db->close();
            return 0;
        }
    }
    if (!tx->commit().isOk()) {
        db->close();
        return 0;
    }
    const uint64_t commitEnd = state->written;
    boundaries = state->boundaries;
    db->close();
    return commitEnd;
}

DbResult runCrashedCreateTables(const std::string& path, int startIndex, int count,
                                std::shared_ptr<BudgetState> state) {
    BudgetFileSystem fs(state);
    std::unique_ptr<Database> db;
    DbResult open = Database::open(path, DatabaseOpenOptions(), db, &fs);
    if (!open.isOk()) {
        return open;
    }
    std::unique_ptr<Transaction> tx;
    DbResult result = db->beginTransaction(tx);
    if (!result.isOk()) {
        db->close();
        return result;
    }
    for (int i = 0; i < count; ++i) {
        uint32_t id = 0;
        result = tx->createTable(makeTable("G" + std::to_string(startIndex + i)), id);
        if (!result.isOk()) {
            tx->rollback();
            db->close();
            return result;
        }
    }
    result = tx->commit();
    db->close();
    return result;
}

uint64_t measureCreateTables(const std::string& path, int startIndex, int count,
                             std::vector<uint64_t>& boundaries) {
    std::shared_ptr<BudgetState> state(new BudgetState());
    state->recording = true;
    BudgetFileSystem fs(state);
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db, &fs).isOk() || !db) {
        return 0;
    }
    std::unique_ptr<Transaction> tx;
    if (!db->beginTransaction(tx).isOk()) {
        db->close();
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        uint32_t id = 0;
        if (!tx->createTable(makeTable("G" + std::to_string(startIndex + i)), id).isOk()) {
            tx->rollback();
            db->close();
            return 0;
        }
    }
    if (!tx->commit().isOk()) {
        db->close();
        return 0;
    }
    const uint64_t commitEnd = state->written;
    boundaries = state->boundaries;
    db->close();
    return commitEnd;
}

bool verifyTableCount(const std::string& path, int expected) {
    std::unique_ptr<Database> db;
    if (!Database::open(path, DatabaseOpenOptions(), db).isOk() || !db) {
        return false;
    }
    std::vector<TableInfo> tables;
    if (!db->listTables(tables).isOk()) {
        db->close();
        return false;
    }
    bool ok = (tables.size() == static_cast<size_t>(expected));
    for (size_t i = 0; i < tables.size() && ok; ++i) {
        TableDefinition def;
        if (!db->describeTable(tables[i].name, def).isOk() || def.columns.size() != 3) {
            ok = false;
        }
    }
    db->close();
    return ok;
}

// ---------------------------------------------------------------------------
// Basic transaction semantics.

void testBeginCommitRollback() {
    std::printf("begin/commit and begin/rollback\n");
    const std::string path = uniquePath("basic");
    buildBase(path, "T", 0);

    // Commit path.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        CHECK(tx != nullptr);
        CHECK(db->diagnostics().transactionActive);
        CHECK(db->diagnostics().transactionId >= 1);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        CHECK_STATUS(t->insert(makeRow(0)), DbStatus::Ok);
        CHECK_STATUS(t->insert(makeRow(1)), DbStatus::Ok);
        CHECK(tx->modifiedPageCount() >= 1);
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        CHECK(!db->diagnostics().transactionActive);
        CHECK(db->diagnostics().transactionModifiedPages == 0);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    CHECK(verifySequential(path, "T", 2));

    // Rollback path.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        CHECK_STATUS(t->insert(makeRow(2)), DbStatus::Ok);
        CHECK_STATUS(t->insert(makeRow(3)), DbStatus::Ok);
        CHECK_STATUS(tx->rollback(), DbStatus::Ok);
        CHECK(!db->diagnostics().transactionActive);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    CHECK(verifySequential(path, "T", 2));
    removeFile(walPath(path));
    removeFile(path);
}

void testDuplicateAndNoActive() {
    std::printf("duplicate begin and no-active rejection\n");
    const std::string path = uniquePath("dupbegin");
    buildBase(path, "T", 0);

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);

    // Commit with no active transaction.
    // (Construct a transaction directly is not public; instead exercise via a
    // second begin and explicit API misuse through the implicit path.)
    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    std::unique_ptr<Transaction> tx2;
    CHECK_STATUS(db->beginTransaction(tx2), DbStatus::TransactionAlreadyActive);
    CHECK(tx2 == nullptr);
    CHECK_STATUS(tx->rollback(), DbStatus::Ok);
    CHECK_STATUS(tx->rollback(), DbStatus::NoActiveTransaction);
    CHECK_STATUS(tx->commit(), DbStatus::NoActiveTransaction);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    removeFile(walPath(path));
    removeFile(path);
}

void testReadYourWrites() {
    std::printf("read-your-writes inside a transaction\n");
    const std::string path = uniquePath("ryw");
    buildBase(path, "T", 1);

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    std::unique_ptr<Table> t;
    CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
    CHECK(t->rowCount() == 1);
    CHECK_STATUS(t->insert(makeRow(1)), DbStatus::Ok);
    CHECK(t->rowCount() == 2);

    // Scan through the active transaction sees the uncommitted row.
    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(t->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    uint64_t count = 0;
    while (scan->next(row)) {
        CHECK(row[0].int64Value() == static_cast<int64_t>(count));
        ++count;
    }
    CHECK(count == 2);
    CHECK_STATUS(scan->status(), DbStatus::Ok);

    CHECK_STATUS(tx->rollback(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Rolled-back row is gone.
    CHECK(verifySequential(path, "T", 1));
    removeFile(walPath(path));
    removeFile(path);
}

void testTableCreationAtomicity() {
    std::printf("table creation commit and rollback\n");
    const std::string path = uniquePath("createtx");
    buildBase(path, "Base", 0);

    // Commit a table creation.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        uint32_t id = 0;
        CHECK_STATUS(tx->createTable(makeTable("Created"), id), DbStatus::Ok);
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        std::vector<TableInfo> tables;
        db->listTables(tables);
        CHECK(tables.size() == 2);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        std::vector<TableInfo> tables;
        db->listTables(tables);
        CHECK(tables.size() == 2);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    // Rollback a table creation (including catalog growth).
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        uint32_t id = 0;
        CHECK_STATUS(tx->createTable(makeTable("RolledBack"), id), DbStatus::Ok);
        CHECK_STATUS(tx->rollback(), DbStatus::Ok);
        std::vector<TableInfo> tables;
        db->listTables(tables);
        CHECK(tables.size() == 2);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        std::vector<TableInfo> tables;
        db->listTables(tables);
        CHECK(tables.size() == 2);
        std::unique_ptr<Table> rolledBack;
        CHECK_STATUS(db->openTable("RolledBack", rolledBack), DbStatus::InvalidArgument);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testRowInsertRollback() {
    std::printf("row insert rollback across sessions\n");
    const std::string path = uniquePath("rowrollback");
    buildBase(path, "T", 5);

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    std::unique_ptr<Table> t;
    CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
    for (int i = 0; i < 20; ++i) {
        CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(100 + i))), DbStatus::Ok);
    }
    CHECK_STATUS(tx->rollback(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);
    CHECK(verifySequential(path, "T", 5));
    removeFile(walPath(path));
    removeFile(path);
}

void testMultiPageTransaction() {
    std::printf("multi-row transaction spanning heap pages\n");
    const std::string path = uniquePath("multipage");
    buildBase(path, "T", 20);

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    std::unique_ptr<Table> t;
    CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
    const int kRows = 400;
    for (int i = 0; i < kRows; ++i) {
        CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(20 + i))), DbStatus::Ok);
    }
    CHECK(t->heapPageCount() >= 3);
    CHECK(tx->modifiedPageCount() >= 3);
    CHECK_STATUS(tx->commit(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);
    CHECK(verifySequential(path, "T", 20 + kRows));
    removeFile(walPath(path));
    removeFile(path);
}

void testCatalogGrowthTransaction() {
    std::printf("transaction forcing catalog continuation growth\n");
    const std::string path = uniquePath("catgrowthtx");

    // A single transaction that creates many tables, forcing continuation pages.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        const int kTables = 60;
        for (int i = 0; i < kTables; ++i) {
            uint32_t id = 0;
            DbResult r = tx->createTable(makeTable("Table_" + std::to_string(i)), id);
            if (!r.isOk()) {
                CHECK_STATUS(r, DbStatus::Ok);
                break;
            }
        }
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        CHECK(tx->modifiedPageCount() >= 2);
        CHECK(db->diagnostics().catalogPageCount >= 2);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        CHECK(db->diagnostics().catalogPageCount >= 2);
        std::vector<TableInfo> tables;
        db->listTables(tables);
        CHECK(tables.size() == 60);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    // Rollback the same growth: the database returns to empty.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        for (int i = 0; i < 60; ++i) {
            uint32_t id = 0;
            tx->createTable(makeTable("More_" + std::to_string(i)), id);
        }
        CHECK_STATUS(tx->rollback(), DbStatus::Ok);
        std::vector<TableInfo> tables;
        db->listTables(tables);
        CHECK(tables.size() == 60);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testUncommittedNeverReachesDb() {
    std::printf("uncommitted pages never reach durable .gxdb\n");
    const std::string path = uniquePath("novisdirty");
    buildBase(path, "T", 10);

    const std::vector<uint8_t> before = readFileBytes(path);

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    std::unique_ptr<Table> t;
    CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
    for (int i = 0; i < 200; ++i) {
        CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(1000 + i))), DbStatus::Ok);
    }
    // Force eviction pressure by scanning the committed view (no effect on the
    // overlay) and reading many pages.
    CHECK(tx->modifiedPageCount() >= 2);

    // The durable database file must be byte-for-byte unchanged.
    const std::vector<uint8_t> during = readFileBytes(path);
    CHECK(before == during);

    CHECK_STATUS(tx->rollback(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    const std::vector<uint8_t> after = readFileBytes(path);
    CHECK(before == after);
    CHECK(verifySequential(path, "T", 10));
    removeFile(walPath(path));
    removeFile(path);
}

void testDirtyBufferCoherence() {
    std::printf("dirty buffer / commit coherence\n");
    const std::string path = uniquePath("bufcoh");
    buildBase(path, "T", 0);

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    std::unique_ptr<Table> t;
    CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
    for (int i = 0; i < 50; ++i) {
        CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(i))), DbStatus::Ok);
    }
    // The committed cache must not expose the uncommitted rows yet.
    CHECK(countRows(path, "T") == 0);
    CHECK_STATUS(tx->commit(), DbStatus::Ok);

    // After commit the committed view must see the rows without reopening.
    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(t->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    uint64_t count = 0;
    while (scan->next(row)) ++count;
    CHECK(count == 50);

    // A fresh scan handle also sees them.
    std::unique_ptr<Table> t2;
    CHECK_STATUS(db->openTable("T", t2), DbStatus::Ok);
    CHECK(t2->rowCount() == 50);

    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK(db->diagnostics().bufferDirty == 0);
    CHECK_STATUS(db->close(), DbStatus::Ok);
    CHECK(verifySequential(path, "T", 50));
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// WAL integrity and recovery.

void testCommittedWalRecovery() {
    std::printf("committed WAL recovery\n");
    const std::string path = uniquePath("walrecover");
    buildBase(path, "T", 30);
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    // Commit a transaction, then freeze before the database flush so the
    // committed data exists only in the WAL.
    {
        std::shared_ptr<BudgetState> state(new BudgetState());
        BudgetFileSystem fs(state);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db, &fs), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        for (int i = 0; i < 40; ++i) {
            CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(30 + i))), DbStatus::Ok);
        }
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        CHECK(db->wal().size() > kWalHeaderSize);
        state->freeze();
        db->close();
    }
    // The durable database file alone is still the pre-state; only the WAL has
    // the committed transaction.
    CHECK(readFileBytes(path) == baseBytes);
    CHECK(fileExists(walPath(path)));

    // Reopen triggers recovery and exposes the full committed state.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        CHECK(db->diagnostics().pagesRedone > 0);
        CHECK(db->diagnostics().walState == WalState::Clean);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    CHECK(verifySequential(path, "T", 70));
    removeFile(walPath(path));
    removeFile(path);
}

void testIncompleteWalDiscard() {
    std::printf("incomplete WAL discard\n");
    const std::string path = uniquePath("walincomplete");
    buildBase(path, "T", 10);
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    // Create a committed WAL (frozen before database apply).
    std::shared_ptr<BudgetState> state(new BudgetState());
    {
        BudgetFileSystem fs(state);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db, &fs), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        for (int i = 0; i < 5; ++i) {
            CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(10 + i))), DbStatus::Ok);
        }
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        state->freeze();
        db->close();
    }
    const std::vector<uint8_t> walBytes = readFileBytes(walPath(path));
    CHECK(walBytes.size() > kWalHeaderSize + kWalRecordPrefixSize);

    // Remove the COMMIT record exactly: the last complete record is a page
    // image / header image with no COMMIT, so the transaction is discarded.
    {
        const std::string work = uniquePath("incompletework");
        writeFileBytes(work, baseBytes);
        std::vector<uint8_t> bytes(walBytes.begin(),
                                   walBytes.end() - kWalRecordPrefixSize);
        writeFileBytes(walPath(work), bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::Ok);
        if (db) db->close();
        CHECK(verifySequential(work, "T", 10));
        removeFile(walPath(work));
        removeFile(work);
    }
    // A WAL with only a BEGIN record is also discarded.
    {
        const std::string work = uniquePath("incompletework2");
        writeFileBytes(work, baseBytes);
        std::vector<uint8_t> bytes(walBytes.begin(),
                                   walBytes.begin() + kWalHeaderSize + kWalRecordPrefixSize);
        writeFileBytes(walPath(work), bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::Ok);
        if (db) db->close();
        CHECK(verifySequential(work, "T", 10));
        removeFile(walPath(work));
        removeFile(work);
    }

    removeFile(walPath(path));
    removeFile(path);
}

void testWalIntegrity() {
    std::printf("WAL integrity and identity checks\n");
    const std::string path = uniquePath("walintegrity");
    buildBase(path, "T", 5);

    // Build a valid committed WAL once (frozen before the database flush), then
    // copy it for corruption cases.
    {
        std::shared_ptr<BudgetState> state(new BudgetState());
        BudgetFileSystem fs(state);
        std::unique_ptr<Database> db;
        Database::open(path, DatabaseOpenOptions(), db, &fs);
        std::unique_ptr<Transaction> tx;
        db->beginTransaction(tx);
        std::unique_ptr<Table> t;
        tx->openTable("T", t);
        for (int i = 0; i < 5; ++i) t->insert(makeRow(static_cast<uint64_t>(5 + i)));
        tx->commit();
        state->freeze();
        db->close();
    }
    const std::string validWal = walPath(path);
    const std::vector<uint8_t> walBytes = readFileBytes(validWal);
    CHECK(walBytes.size() > kWalHeaderSize);

    // Corrupt the WAL header magic.
    {
        const std::string work = uniquePath("walmagic");
        copyFileBytes(path, work);
        std::vector<uint8_t> bytes = walBytes;
        bytes[0] = 0x00;
        writeFileBytes(walPath(work), bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::WalCorrupt);
        removeFile(walPath(work));
        removeFile(work);
    }
    // Corrupt the WAL header checksum.
    {
        const std::string work = uniquePath("walhcrc");
        copyFileBytes(path, work);
        std::vector<uint8_t> bytes = walBytes;
        bytes[wal_header_offset::HeaderCrc32] ^= 0xFF;
        writeFileBytes(walPath(work), bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::WalCorrupt);
        removeFile(walPath(work));
        removeFile(work);
    }
    // Unsupported WAL version.
    {
        const std::string work = uniquePath("walver");
        copyFileBytes(path, work);
        std::vector<uint8_t> bytes = walBytes;
        storeLe16(bytes.data() + wal_header_offset::FormatVersion, 99);
        // Recompute header CRC so the version is what fails.
        bytes[wal_header_offset::HeaderCrc32] = 0;
        bytes[wal_header_offset::HeaderCrc32 + 1] = 0;
        bytes[wal_header_offset::HeaderCrc32 + 2] = 0;
        bytes[wal_header_offset::HeaderCrc32 + 3] = 0;
        {
            uint32_t crc = crc32Init();
            crc = crc32Update(crc, bytes.data(), wal_header_offset::HeaderCrc32);
            const uint8_t zero[4] = {0, 0, 0, 0};
            crc = crc32Update(crc, zero, 4);
            crc = crc32Update(crc, bytes.data() + wal_header_offset::HeaderCrc32 + 4,
                              kWalHeaderSize - wal_header_offset::HeaderCrc32 - 4);
            storeLe32(bytes.data() + wal_header_offset::HeaderCrc32, crc32Final(crc));
        }
        writeFileBytes(walPath(work), bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::WalUnsupportedVersion);
        removeFile(walPath(work));
        removeFile(work);
    }
    // Foreign database identity in the WAL header.
    {
        const std::string work = uniquePath("walmismatch");
        copyFileBytes(path, work);
        std::vector<uint8_t> bytes = walBytes;
        bytes[wal_header_offset::DatabaseId] ^= 0xFF;
        bytes[wal_header_offset::HeaderCrc32] = 0;
        bytes[wal_header_offset::HeaderCrc32 + 1] = 0;
        bytes[wal_header_offset::HeaderCrc32 + 2] = 0;
        bytes[wal_header_offset::HeaderCrc32 + 3] = 0;
        {
            uint32_t crc = crc32Init();
            crc = crc32Update(crc, bytes.data(), wal_header_offset::HeaderCrc32);
            const uint8_t zero[4] = {0, 0, 0, 0};
            crc = crc32Update(crc, zero, 4);
            crc = crc32Update(crc, bytes.data() + wal_header_offset::HeaderCrc32 + 4,
                              kWalHeaderSize - wal_header_offset::HeaderCrc32 - 4);
            storeLe32(bytes.data() + wal_header_offset::HeaderCrc32, crc32Final(crc));
        }
        writeFileBytes(walPath(work), bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::WalDatabaseMismatch);
        removeFile(walPath(work));
        removeFile(work);
    }
    // Corrupt a PAGE_IMAGE record body (record checksum must fail).
    {
        const std::string work = uniquePath("walrec");
        copyFileBytes(path, work);
        std::vector<uint8_t> bytes = walBytes;
        // Flip a byte well past the header and BEGIN record.
        if (bytes.size() > kWalHeaderSize + kWalRecordPrefixSize + 100) {
            bytes[kWalHeaderSize + kWalRecordPrefixSize + 100] ^= 0xFF;
        }
        writeFileBytes(walPath(work), bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::WalCorrupt);
        removeFile(walPath(work));
        removeFile(work);
    }

    removeFile(validWal);
    removeFile(path);
}

void testForeignWalDifferentDatabase() {
    std::printf("foreign WAL rejected across databases\n");
    const std::string pathA = uniquePath("fwalA");
    const std::string pathB = uniquePath("fwalB");
    buildBase(pathA, "T", 5);
    buildBase(pathB, "T", 7);

    // Build a committed WAL for A.
    {
        std::shared_ptr<BudgetState> state(new BudgetState());
        BudgetFileSystem fs(state);
        std::unique_ptr<Database> db;
        Database::open(pathA, DatabaseOpenOptions(), db, &fs);
        std::unique_ptr<Transaction> tx;
        db->beginTransaction(tx);
        std::unique_ptr<Table> t;
        tx->openTable("T", t);
        for (int i = 0; i < 5; ++i) t->insert(makeRow(static_cast<uint64_t>(5 + i)));
        tx->commit();
        state->freeze();
        db->close();
    }
    CHECK(fileExists(walPath(pathA)));

    // Place A's WAL beside B.
    copyFileBytes(walPath(pathA), walPath(pathB));
    std::unique_ptr<Database> dbB;
    CHECK_STATUS(Database::open(pathB, DatabaseOpenOptions(), dbB), DbStatus::WalDatabaseMismatch);
    CHECK(dbB == nullptr);

    removeFile(walPath(pathB));
    removeFile(walPath(pathA));
    removeFile(pathA);
    removeFile(pathB);
}

void testWalTruncation() {
    std::printf("WAL truncation matrix\n");
    const std::string path = uniquePath("waltrunc");
    buildBase(path, "T", 20);

    // Build a committed WAL and remember the pre-state database bytes.
    const std::vector<uint8_t> baseBytes = readFileBytes(path);
    std::shared_ptr<BudgetState> state(new BudgetState());
    BudgetFileSystem fs(state);
    uint64_t commitEnd = 0;
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db, &fs), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        for (int i = 0; i < 25; ++i) {
            CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(20 + i))), DbStatus::Ok);
        }
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        commitEnd = state->written;
        state->freeze();
        db->close();
    }
    const std::vector<uint8_t> fullWal = readFileBytes(walPath(path));
    CHECK(fullWal.size() == commitEnd);

    const uint64_t truncationPoints[] = {
        0, 1, 8, 32, 63, 64, 65, 70, 80,
        kWalHeaderSize + kWalRecordPrefixSize,
        kWalHeaderSize + kWalRecordPrefixSize + 1,
        kWalHeaderSize + kWalRecordPrefixSize + 100,
        commitEnd / 2,
        commitEnd - 1,
        commitEnd
    };
    for (size_t i = 0; i < sizeof(truncationPoints) / sizeof(truncationPoints[0]); ++i) {
        const uint64_t point = truncationPoints[i];
        if (point > fullWal.size()) continue;
        const std::string work = uniquePath("waltruncwork");
        writeFileBytes(work, baseBytes);
        std::vector<uint8_t> bytes(fullWal.begin(), fullWal.begin() + point);
        writeFileBytes(walPath(work), bytes);

        std::unique_ptr<Database> db;
        DbResult open = Database::open(work, DatabaseOpenOptions(), db);
        CHECK_STATUS(open, DbStatus::Ok);
        if (open.isOk() && db) {
            db->close();
        }
        // Before the durability point the transaction is discarded; at or after
        // it the full transaction is recovered.
        const bool committed = point >= commitEnd;
        CHECK(verifySequential(work, "T", committed ? 45 : 20));
        removeFile(walPath(work));
        removeFile(work);
    }
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Crash matrix.

void testCrashMatrix() {
    std::printf("commit crash matrix\n");
    const std::string path = uniquePath("crashmatrix");
    const int kPreRows = 40;
    const int kInsert = 60;
    buildBase(path, "T", kPreRows);
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    // Measure the durability point and write boundaries on a scratch copy.
    const std::string measure = uniquePath("crashmeasure");
    writeFileBytes(measure, baseBytes);
    removeFile(walPath(measure));
    std::vector<uint64_t> boundaries;
    const uint64_t commitEnd =
        measureCommit(measure, "T", kPreRows, kInsert, boundaries);
    removeFile(walPath(measure));
    removeFile(measure);
    CHECK(commitEnd > kWalHeaderSize);
    CHECK(boundaries.size() > 4);

    // Build the set of crash boundaries: every write start, plus a midpoint
    // inside each write (covers truncation inside a record body).
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
    for (std::set<uint64_t>::const_iterator it = points.begin(); it != points.end(); ++it) {
        const uint64_t budget = *it;
        const std::string work = uniquePath("crashwork");
        writeFileBytes(work, baseBytes);
        removeFile(walPath(work));

        std::shared_ptr<BudgetState> state(new BudgetState());
        state->budget = budget;
        runCrashedInsert(work, "T", kPreRows, kInsert, state);

        std::unique_ptr<Database> db;
        DbResult open = Database::open(work, DatabaseOpenOptions(), db);
        CHECK_STATUS(open, DbStatus::Ok);
        if (open.isOk() && db) {
            db->close();
        }
        const bool committed = budget >= commitEnd;
        const uint64_t expected = committed ? (kPreRows + kInsert) : kPreRows;
        if (!verifySequential(work, "T", expected)) {
            std::printf("  crash boundary %llu failed (expected %llu rows)\n",
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

void testCatalogGrowthCrashMatrix() {
    std::printf("catalog growth crash matrix\n");
    const std::string path = uniquePath("catcrash");
    const int kPreTables = 40;
    const int kNewTables = 20;

    // Pre-state with a catalog spanning at least one continuation page.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
        for (int i = 0; i < kPreTables; ++i) {
            uint32_t id = 0;
            db->createTable(makeTable("P" + std::to_string(i)), id);
        }
        db->flush();
        db->close();
    }
    CHECK(verifyTableCount(path, kPreTables));
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    const std::string measure = uniquePath("catcrashmeasure");
    writeFileBytes(measure, baseBytes);
    removeFile(walPath(measure));
    std::vector<uint64_t> boundaries;
    const uint64_t commitEnd =
        measureCreateTables(measure, 0, kNewTables, boundaries);
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
    for (std::set<uint64_t>::const_iterator it = points.begin(); it != points.end(); ++it) {
        const uint64_t budget = *it;
        const std::string work = uniquePath("catcrashwork");
        writeFileBytes(work, baseBytes);
        removeFile(walPath(work));

        std::shared_ptr<BudgetState> state(new BudgetState());
        state->budget = budget;
        runCrashedCreateTables(work, 0, kNewTables, state);

        const bool committed = budget >= commitEnd;
        const int expected = committed ? (kPreTables + kNewTables) : kPreTables;
        if (!verifyTableCount(work, expected)) {
            std::printf("  catalog crash boundary %llu failed (expected %d tables)\n",
                        static_cast<unsigned long long>(budget), expected);
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

void testCrashDuringRedo() {
    std::printf("crash during database redo\n");
    const std::string path = uniquePath("redoc");
    buildBase(path, "T", 20);
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    // Create a committed WAL (frozen before DB apply).
    std::shared_ptr<BudgetState> state(new BudgetState());
    {
        BudgetFileSystem fs(state);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db, &fs), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        for (int i = 0; i < 30; ++i) {
            CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(20 + i))), DbStatus::Ok);
        }
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        state->freeze();
        db->close();
    }
    const std::vector<uint8_t> walBytes = readFileBytes(walPath(path));

    // Recover with a budget that stops partway through applying DB pages.
    for (uint64_t budget = 0; budget < 60000; budget += 4096) {
        const std::string work = uniquePath("redowork");
        writeFileBytes(work, baseBytes);
        writeFileBytes(walPath(work), walBytes);

        std::shared_ptr<BudgetState> rstate(new BudgetState());
        rstate->budget = budget;
        BudgetFileSystem rfs(rstate);
        {
            std::unique_ptr<Database> db;
            Database::open(work, DatabaseOpenOptions(), db, &rfs); // may fail; partial apply
        }
        // Reopen normally: recovery must complete idempotently.
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::Ok);
        if (db) db->close();
        CHECK(verifySequential(work, "T", 50));
        removeFile(walPath(work));
        removeFile(work);
        if (budget > 50000) break;
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testCrashDuringCheckpoint() {
    std::printf("crash during WAL checkpoint\n");
    const std::string path = uniquePath("checkpointc");
    buildBase(path, "T", 10);
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    std::shared_ptr<BudgetState> state(new BudgetState());
    {
        BudgetFileSystem fs(state);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db, &fs), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        for (int i = 0; i < 20; ++i) {
            CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(10 + i))), DbStatus::Ok);
        }
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        state->freeze();
        db->close();
    }
    const std::vector<uint8_t> walBytes = readFileBytes(walPath(path));
    const uint64_t commitEnd = walBytes.size();

    // Allow the whole WAL plus a partial database flush, then stop during the
    // checkpoint. Reopen must still recover.
    for (uint64_t extra = 0; extra <= 3 * 4096; extra += 2048) {
        const std::string work = uniquePath("checkpointwork");
        writeFileBytes(work, baseBytes);
        writeFileBytes(walPath(work), walBytes);

        std::shared_ptr<BudgetState> rstate(new BudgetState());
        rstate->budget = commitEnd + extra;
        BudgetFileSystem rfs(rstate);
        {
            std::unique_ptr<Database> db;
            Database::open(work, DatabaseOpenOptions(), db, &rfs);
        }
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, DatabaseOpenOptions(), db), DbStatus::Ok);
        if (db) db->close();
        CHECK(verifySequential(work, "T", 30));
        removeFile(walPath(work));
        removeFile(work);
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testRecoveryIdempotence() {
    std::printf("recovery idempotence\n");
    const std::string path = uniquePath("idempotent");
    buildBase(path, "T", 15);

    std::shared_ptr<BudgetState> state(new BudgetState());
    {
        BudgetFileSystem fs(state);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db, &fs), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        for (int i = 0; i < 35; ++i) {
            CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(15 + i))), DbStatus::Ok);
        }
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        state->freeze();
        db->close();
    }

    // First recovery.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        CHECK(db->diagnostics().walState == WalState::Clean);
        db->close();
    }
    CHECK(verifySequential(path, "T", 50));
    const std::vector<uint8_t> afterFirst = readFileBytes(path);

    // Second and third opens must not replay anything or alter data.
    for (int i = 0; i < 2; ++i) {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        CHECK(db->diagnostics().pagesRedone == 0);
        db->close();
        CHECK(verifySequential(path, "T", 50));
        CHECK(readFileBytes(path) == afterFirst);
    }
    removeFile(walPath(path));
    removeFile(path);
}

void testReadOnlyRecoveryRequired() {
    std::printf("read-only recovery-required behavior\n");
    const std::string path = uniquePath("rorecover");
    buildBase(path, "T", 10);
    const std::vector<uint8_t> baseBytes = readFileBytes(path);

    std::shared_ptr<BudgetState> state(new BudgetState());
    {
        BudgetFileSystem fs(state);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db, &fs), DbStatus::Ok);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> t;
        CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
        for (int i = 0; i < 10; ++i) {
            CHECK_STATUS(t->insert(makeRow(static_cast<uint64_t>(10 + i))), DbStatus::Ok);
        }
        CHECK_STATUS(tx->commit(), DbStatus::Ok);
        state->freeze();
        db->close();
    }

    // Read-only open must refuse to mutate and report RecoveryRequired.
    {
        DatabaseOpenOptions ro;
        ro.readOnly = true;
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, ro, db), DbStatus::RecoveryRequired);
        CHECK(db == nullptr);
    }
    // The database is untouched: the .gxdb bytes are exactly the pre-state.
    CHECK(readFileBytes(path) == baseBytes);

    // A writable open recovers.
    {
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
        db->close();
    }
    CHECK(verifySequential(path, "T", 20));
    removeFile(walPath(path));
    removeFile(path);
}

// ---------------------------------------------------------------------------
// Stress.

void testRollbackStress() {
    std::printf("rollback stress (200 transactions)\n");
    const std::string path = uniquePath("rollbackstress");
    buildBase(path, "T", 0);

    uint64_t expected = 0;
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    std::unique_ptr<Table> t;
    CHECK_STATUS(db->openTable("T", t), DbStatus::Ok);

    for (int i = 0; i < 200; ++i) {
        const bool commit = (i % 3 != 1);
        std::unique_ptr<Transaction> tx;
        CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
        std::unique_ptr<Table> tt;
        CHECK_STATUS(tx->openTable("T", tt), DbStatus::Ok);
        CHECK_STATUS(tt->insert(makeRow(expected)), DbStatus::Ok);
        if (commit) {
            CHECK_STATUS(tx->commit(), DbStatus::Ok);
            ++expected;
        } else {
            CHECK_STATUS(tx->rollback(), DbStatus::Ok);
        }
    }
    CHECK_STATUS(db->close(), DbStatus::Ok);

    CHECK(verifySequential(path, "T", expected));
    removeFile(walPath(path));
    removeFile(path);
}

void testTransactionLimits() {
    std::printf("transaction resource limits\n");
    const std::string path = uniquePath("txlimit");
    buildBase(path, "T", 0);

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    // Fill the overlay with synthetic page images (ids need not exist).
    bool hitLimit = false;
    for (uint32_t i = 0; i < kMaxTransactionPages + 4; ++i) {
        DatabasePage page;
        page.pageId = static_cast<uint64_t>(i) + 1;
        page.type = PageType::Data;
        page.payload.assign(8, 0);
        page.payloadSize = 8;
        DbResult r = tx->writePage(page);
        if (r.status() == DbStatus::TransactionTooLarge) {
            hitLimit = true;
            break;
        }
    }
    CHECK(hitLimit);
    CHECK_STATUS(tx->rollback(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

void testDiagnostics() {
    std::printf("transaction/WAL diagnostics\n");
    const std::string path = uniquePath("diag3");
    buildBase(path, "T", 0);

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    CHECK(!db->diagnostics().transactionActive);
    CHECK(!db->diagnostics().recoveryRequired);
    CHECK(db->diagnostics().transactionModifiedPages == 0);

    std::unique_ptr<Transaction> tx;
    CHECK_STATUS(db->beginTransaction(tx), DbStatus::Ok);
    CHECK(db->diagnostics().transactionActive);
    CHECK(db->diagnostics().transactionId >= 1);
    CHECK(db->diagnostics().walPresent);
    std::unique_ptr<Table> t;
    CHECK_STATUS(tx->openTable("T", t), DbStatus::Ok);
    CHECK_STATUS(t->insert(makeRow(1)), DbStatus::Ok);
    CHECK(db->diagnostics().transactionModifiedPages >= 1);
    CHECK_STATUS(tx->commit(), DbStatus::Ok);
    CHECK(!db->diagnostics().transactionActive);
    CHECK(db->diagnostics().walBytes > 0);
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK(db->diagnostics().walState == WalState::Clean);
    CHECK_STATUS(db->close(), DbStatus::Ok);
    removeFile(walPath(path));
    removeFile(path);
}

void testDeterministicStress() {
    std::printf("deterministic transaction/recovery stress (250 lifecycles)\n");
    const std::string path = uniquePath("stress");
    buildBase(path, "T", 0);

    std::set<uint64_t> expected;
    uint64_t nextId = 0;

    for (int cycle = 0; cycle < 250; ++cycle) {
        const int mode = cycle % 8;
        if (mode == 0 || mode == 5) {
            // Implicit insert (commit).
            std::unique_ptr<Database> db;
            CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
            if (!db) break;
            std::unique_ptr<Table> t;
            if (db->openTable("T", t).isOk()) {
                CHECK_STATUS(t->insert(makeRow(nextId)), DbStatus::Ok);
                expected.insert(nextId);
                ++nextId;
            }
            db->close();
        } else if (mode == 1 || mode == 6) {
            // Explicit commit.
            std::unique_ptr<Database> db;
            CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
            if (!db) break;
            std::unique_ptr<Transaction> tx;
            if (db->beginTransaction(tx).isOk()) {
                std::unique_ptr<Table> t;
                if (tx->openTable("T", t).isOk()) {
                    const int n = (mode == 6) ? 3 : 1;
                    for (int k = 0; k < n; ++k) {
                        if (t->insert(makeRow(nextId)).isOk()) {
                            expected.insert(nextId);
                            ++nextId;
                        }
                    }
                    tx->commit();
                } else {
                    tx->rollback();
                }
            }
            db->close();
        } else if (mode == 2 || mode == 7) {
            // Explicit rollback.
            std::unique_ptr<Database> db;
            CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
            if (!db) break;
            std::unique_ptr<Transaction> tx;
            if (db->beginTransaction(tx).isOk()) {
                std::unique_ptr<Table> t;
                if (tx->openTable("T", t).isOk()) {
                    t->insert(makeRow(nextId));
                }
                tx->rollback();
            }
            db->close();
        } else if (mode == 3) {
            // Crash before commit.
            std::shared_ptr<BudgetState> state(new BudgetState());
            state->budget = 0;
            runCrashedInsert(path, "T", nextId, 1, state);
            std::unique_ptr<Database> db;
            CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
            if (db) db->close();
        } else {
            // Crash after a durable commit but before the database flush.
            std::shared_ptr<BudgetState> state(new BudgetState());
            BudgetFileSystem fs(state);
            std::unique_ptr<Database> db;
            if (Database::open(path, DatabaseOpenOptions(), db, &fs).isOk() && db) {
                std::unique_ptr<Transaction> tx;
                if (db->beginTransaction(tx).isOk()) {
                    std::unique_ptr<Table> t;
                    if (tx->openTable("T", t).isOk()) {
                        if (t->insert(makeRow(nextId)).isOk()) {
                            if (tx->commit().isOk()) {
                                expected.insert(nextId);
                                ++nextId;
                                state->freeze();
                            } else {
                                tx->rollback();
                            }
                        } else {
                            tx->rollback();
                        }
                    } else {
                        tx->rollback();
                    }
                }
                db->close();
            }
            std::unique_ptr<Database> db2;
            CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db2), DbStatus::Ok);
            if (db2) db2->close();
        }
    }

    // Final verification: exact logical state, chain/catalog validity.
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    if (db) {
        std::unique_ptr<Table> t;
        CHECK_STATUS(db->openTable("T", t), DbStatus::Ok);
        CHECK(t->rowCount() == expected.size());
        std::unique_ptr<TableScan> scan;
        CHECK_STATUS(t->scanStart(scan), DbStatus::Ok);
        std::vector<DbValue> row;
        std::set<uint64_t> seen;
        while (scan->next(row)) {
            seen.insert(static_cast<uint64_t>(row[0].int64Value()));
        }
        CHECK_STATUS(scan->status(), DbStatus::Ok);
        CHECK(seen == expected);
        // Zero outstanding recovery work after a clean close/reopen.
        CHECK_STATUS(db->flush(), DbStatus::Ok);
        CHECK(db->diagnostics().walState == WalState::Clean);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }
    {
        std::unique_ptr<Database> db2;
        CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db2), DbStatus::Ok);
        if (db2) {
            CHECK(db2->diagnostics().pagesRedone == 0);
            db2->close();
        }
    }
    removeFile(walPath(path));
    removeFile(path);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("guideXOS SQL3 write-ahead log and transaction tests\n");
    std::string dir = testDir();
    std::printf("temp dir: %s\n", dir.c_str());

    std::string cmd = "del /q \"" + dir + "\\*.gxdb\" >nul 2>nul";
    std::system(cmd.c_str());
    cmd = "del /q \"" + dir + "\\*.gxwal\" >nul 2>nul";
    std::system(cmd.c_str());

    testBeginCommitRollback();
    testDuplicateAndNoActive();
    testReadYourWrites();
    testTableCreationAtomicity();
    testRowInsertRollback();
    testMultiPageTransaction();
    testCatalogGrowthTransaction();
    testUncommittedNeverReachesDb();
    testDirtyBufferCoherence();
    testCommittedWalRecovery();
    testIncompleteWalDiscard();
    testWalIntegrity();
    testForeignWalDifferentDatabase();
    testWalTruncation();
    testCrashMatrix();
    testCatalogGrowthCrashMatrix();
    testCrashDuringRedo();
    testCrashDuringCheckpoint();
    testRecoveryIdempotence();
    testReadOnlyRecoveryRequired();
    testRollbackStress();
    testTransactionLimits();
    testDiagnostics();
    testDeterministicStress();

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
