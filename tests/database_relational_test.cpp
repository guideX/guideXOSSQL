// guideXOS SQL -- Phase SQL2
// Hosted acceptance tests for the relational catalog and heap tables.
//
// No external test framework; same approach as the SQL1 suite. Every case
// writes real .gxdb files under the OS temp directory.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "database_catalog.h"
#include "database_checksum.h"
#include "database_endian.h"
#include "database_engine.h"
#include "database_format.h"
#include "database_heap.h"
#include "database_relational.h"
#include "database_schema.h"
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

void checkStatusIn(const DbResult& result, DbStatus a, DbStatus b, const char* what,
                   int line) {
    if (result.status() == a || result.status() == b) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- expected %s or %s, got %s (%s)\n", line, what,
                    dbStatusName(a), dbStatusName(b), dbStatusName(result.status()),
                    result.message().c_str());
    }
}

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)
#define CHECK_STATUS(expr, expected) checkStatus((expr), (expected), #expr, __LINE__)
#define CHECK_STATUS_IN(expr, a, b) checkStatusIn((expr), (a), (b), #expr, __LINE__)

std::string testDir() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    std::string dir = std::string(base) + "/gxos_gxdb_sql2_tests";
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

uint64_t fileSizeOf(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return 0;
    return static_cast<uint64_t>(info.st_size);
}

void mutateCopy(const std::string& base, const std::string& work, size_t offset,
                uint8_t value) {
    std::vector<uint8_t> bytes = readFileBytes(base);
    if (offset < bytes.size()) bytes[offset] = value;
    writeFileBytes(work, bytes);
}

// Recomputes the CRC of the page at `pageId` so structural validation (rather
// than the checksum) is exercised after a payload mutation.
void fixPageCrc(std::vector<uint8_t>& bytes, uint64_t pageId, uint32_t pageSize) {
    const size_t pageOffset = static_cast<size_t>(pageId) * pageSize;
    if (pageOffset + pageSize > bytes.size()) return;
    uint8_t* page = bytes.data() + pageOffset;
    for (int i = 0; i < 4; ++i) page[page_offset::PageCrc32 + i] = 0;
    uint32_t crc = crc32(page, pageSize);
    storeLe32(page + page_offset::PageCrc32, crc);
}

void fixHeaderCrc(std::vector<uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize) return;
    for (int i = 0; i < 4; ++i) bytes[header_offset::HeaderCrc32 + i] = 0;
    uint32_t crc = crc32(bytes.data(), kHeaderSize);
    storeLe32(bytes.data() + header_offset::HeaderCrc32, crc);
}

void setHeaderField64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    storeLe64(bytes.data() + offset, value);
}

std::vector<uint8_t> deterministicPayload(size_t size, uint32_t seed) {
    std::vector<uint8_t> payload(size);
    uint32_t x = seed * 2654435761u + 1u;
    for (size_t i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        payload[i] = static_cast<uint8_t>((x >> 16) & 0xFFu);
    }
    return payload;
}

// Deterministic per-row value generation for the large dataset test.
void expectedRowValues(uint64_t index, std::vector<DbValue>& out) {
    out.clear();
    out.push_back(DbValue::int64(static_cast<int64_t>(index)));
    out.push_back(DbValue::text("User_" + std::to_string(index)));
    out.push_back(DbValue::boolean(index % 2 == 0));
    out.push_back(DbValue::float64(static_cast<double>(index) * 1.5));
}

TableDefinition makeUsersSchema() {
    TableDefinition def("Users");
    def.columns.push_back(ColumnDefinition("Id", DbType::Int64, false));
    def.columns.push_back(ColumnDefinition("Name", DbType::Text, false));
    def.columns.push_back(ColumnDefinition("Active", DbType::Boolean, false));
    return def;
}

// ---------------------------------------------------------------------------

void testCreateAndReopen() {
    std::printf("relational create/reopen\n");
    const std::string path = uniquePath("create");

    std::unique_ptr<Database> db;
    DbResult result = Database::create(path, DatabaseCreateOptions(), db);
    CHECK_STATUS(result, DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    uint32_t usersId = 0;
    result = db->createTable(makeUsersSchema(), usersId);
    CHECK_STATUS(result, DbStatus::Ok);
    CHECK(usersId == 1);

    uint32_t productsId = 0;
    TableDefinition products("Products");
    products.columns.push_back(ColumnDefinition("Sku", DbType::Int32, false));
    products.columns.push_back(ColumnDefinition("Label", DbType::Text, true));
    result = db->createTable(products, productsId);
    CHECK_STATUS(result, DbStatus::Ok);
    CHECK(productsId == 2);

    uint32_t emptyId = 0;
    result = db->createTable(TableDefinition("EmptyTable"), emptyId);
    CHECK_STATUS(result, DbStatus::Ok);

    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<Database> reopened;
    DatabaseOpenOptions openOptions;
    CHECK_STATUS(Database::open(path, openOptions, reopened), DbStatus::Ok);
    CHECK(reopened != nullptr);
    if (!reopened) return;

    std::vector<TableInfo> tables;
    CHECK_STATUS(reopened->listTables(tables), DbStatus::Ok);
    CHECK(tables.size() == 3);
    if (tables.size() == 3) {
        CHECK(tables[0].name == "Users");
        CHECK(tables[0].columnCount == 3);
        CHECK(tables[1].name == "Products");
        CHECK(tables[1].columnCount == 2);
        CHECK(tables[2].name == "EmptyTable");
        CHECK(tables[2].columnCount == 0);
    }

    TableDefinition desc;
    CHECK_STATUS(reopened->describeTable("Users", desc), DbStatus::Ok);
    CHECK(desc.name == "Users");
    CHECK(desc.columns.size() == 3);
    if (desc.columns.size() == 3) {
        CHECK(desc.columns[0].name == "Id");
        CHECK(desc.columns[0].type == DbType::Int64);
        CHECK(!desc.columns[0].nullable);
        CHECK(desc.columns[0].ordinal == 0);
        CHECK(desc.columns[1].name == "Name");
        CHECK(desc.columns[1].type == DbType::Text);
        CHECK(desc.columns[2].name == "Active");
        CHECK(desc.columns[2].type == DbType::Boolean);
    }

    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testDuplicateHandling() {
    std::printf("duplicate handling and case policy\n");
    const std::string path = uniquePath("dup");

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;

    uint32_t id = 0;
    CHECK_STATUS(db->createTable(makeUsersSchema(), id), DbStatus::Ok);

    // Duplicate table name.
    CHECK_STATUS(db->createTable(makeUsersSchema(), id), DbStatus::AlreadyExists);

    // Case-sensitive: Users / users / USERS are distinct.
    TableDefinition lower("users");
    lower.columns.push_back(ColumnDefinition("Id", DbType::Int64, false));
    CHECK_STATUS(db->createTable(lower, id), DbStatus::Ok);
    CHECK(id == 2);

    TableDefinition upper("USERS");
    upper.columns.push_back(ColumnDefinition("Id", DbType::Int64, false));
    CHECK_STATUS(db->createTable(upper, id), DbStatus::Ok);
    CHECK(id == 3);

    // Duplicate column name.
    TableDefinition dupCol("DupCol");
    dupCol.columns.push_back(ColumnDefinition("A", DbType::Int32, false));
    dupCol.columns.push_back(ColumnDefinition("A", DbType::Int32, false));
    CHECK_STATUS(db->createTable(dupCol, id), DbStatus::AlreadyExists);

    // Empty table name.
    CHECK_STATUS(db->createTable(TableDefinition(""), id), DbStatus::InvalidArgument);

    // Empty column name.
    TableDefinition emptyCol("EmptyCol");
    emptyCol.columns.push_back(ColumnDefinition("", DbType::Int32, false));
    CHECK_STATUS(db->createTable(emptyCol, id), DbStatus::InvalidArgument);

    // Unsupported column type.
    TableDefinition badType("BadType");
    badType.columns.push_back(ColumnDefinition("X", DbType::Unknown, false));
    CHECK_STATUS(db->createTable(badType, id), DbStatus::InvalidArgument);

    // Over-long table name (65 bytes).
    TableDefinition longName(std::string(65, 'L'));
    longName.columns.push_back(ColumnDefinition("Id", DbType::Int32, false));
    CHECK_STATUS(db->createTable(longName, id), DbStatus::InvalidArgument);

    // 64-byte table name is accepted.
    TableDefinition maxName(std::string(64, 'M'));
    maxName.columns.push_back(ColumnDefinition("Id", DbType::Int32, false));
    CHECK_STATUS(db->createTable(maxName, id), DbStatus::Ok);

    CHECK_STATUS(db->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testTypePersistence() {
    std::printf("type persistence\n");
    const std::string path = uniquePath("types");

    std::unique_ptr<Database> db;
    DatabaseCreateOptions options;
    options.pageSize = 65536; // large page so max-size text/blob fit
    CHECK_STATUS(Database::create(path, options, db), DbStatus::Ok);
    if (!db) return;

    TableDefinition def("Types");
    def.columns.push_back(ColumnDefinition("B", DbType::Boolean, true));
    def.columns.push_back(ColumnDefinition("I32", DbType::Int32, true));
    def.columns.push_back(ColumnDefinition("I64", DbType::Int64, true));
    def.columns.push_back(ColumnDefinition("F64", DbType::Float64, true));
    def.columns.push_back(ColumnDefinition("T", DbType::Text, true));
    def.columns.push_back(ColumnDefinition("Bl", DbType::Blob, true));
    def.columns.push_back(ColumnDefinition("N", DbType::Int64, true));
    uint32_t id = 0;
    CHECK_STATUS(db->createTable(def, id), DbStatus::Ok);

    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("Types", table), DbStatus::Ok);

    // Row 0: all NULL.
    table->insert({DbValue::null(), DbValue::null(), DbValue::null(), DbValue::null(),
                   DbValue::null(), DbValue::null(), DbValue::null()});
    // Row 1: representative values.
    table->insert({DbValue::boolean(false), DbValue::int32(0), DbValue::int64(0),
                   DbValue::float64(0.0), DbValue::text(""), DbValue::blob(nullptr, 0),
                   DbValue::int64(-1)});
    // Row 2: extremes.
    table->insert({DbValue::boolean(true), DbValue::int32(INT32_MIN),
                   DbValue::int64(INT64_MIN), DbValue::float64(3.141592653589793),
                   DbValue::text("hello world"), DbValue::blob(nullptr, 0),
                   DbValue::int64(INT64_MAX)});
    // Row 3: binary blob with zero bytes.
    const uint8_t bin[6] = {0x00, 0x01, 0x00, 0xFF, 0x00, 0x7F};
    table->insert({DbValue::boolean(true), DbValue::int32(INT32_MAX),
                   DbValue::int64(1), DbValue::float64(-2.5),
                   DbValue::text("unicode: \xC3\xA9\xE2\x82\xAC"),
                   DbValue::blob(bin, sizeof(bin)), DbValue::null()});

    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    if (!reopened) return;
    std::unique_ptr<Table> rt;
    CHECK_STATUS(reopened->openTable("Types", rt), DbStatus::Ok);

    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(rt->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;

    // Row 0: all NULL.
    CHECK(scan->next(row));
    CHECK(row.size() == 7);
    for (int i = 0; i < 7; ++i) CHECK(row[i].isNull());

    // Row 1.
    CHECK(scan->next(row));
    CHECK(row[0].type() == DbType::Boolean && !row[0].booleanValue());
    CHECK(row[1].type() == DbType::Int32 && row[1].int32Value() == 0);
    CHECK(row[2].type() == DbType::Int64 && row[2].int64Value() == 0);
    CHECK(row[3].type() == DbType::Float64 && row[3].float64Value() == 0.0);
    CHECK(row[4].type() == DbType::Text && row[4].textValue().empty());
    CHECK(row[5].type() == DbType::Blob && row[5].blobValue().empty());
    CHECK(row[6].type() == DbType::Int64 && row[6].int64Value() == -1);

    // Row 2.
    CHECK(scan->next(row));
    CHECK(row[0].booleanValue());
    CHECK(row[1].int32Value() == INT32_MIN);
    CHECK(row[2].int64Value() == INT64_MIN);
    CHECK(row[3].float64Value() == 3.141592653589793);
    CHECK(row[4].textValue() == "hello world");
    CHECK(row[6].int64Value() == INT64_MAX);

    // Row 3.
    CHECK(scan->next(row));
    CHECK(row[1].int32Value() == INT32_MAX);
    CHECK(row[2].int64Value() == 1);
    CHECK(row[3].float64Value() == -2.5);
    CHECK(row[4].textValue() == "unicode: \xC3\xA9\xE2\x82\xAC");
    CHECK(row[5].blobValue().size() == 6);
    if (row[5].blobValue().size() == 6) {
        CHECK(row[5].blobValue()[0] == 0x00);
        CHECK(row[5].blobValue()[1] == 0x01);
        CHECK(row[5].blobValue()[2] == 0x00);
        CHECK(row[5].blobValue()[3] == 0xFF);
        CHECK(row[5].blobValue()[4] == 0x00);
        CHECK(row[5].blobValue()[5] == 0x7F);
    }
    CHECK(row[6].isNull());

    CHECK(!scan->next(row));
    CHECK_STATUS(scan->status(), DbStatus::Ok);
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testTypeBoundaries() {
    std::printf("type boundaries\n");
    const std::string path = uniquePath("boundaries");

    std::unique_ptr<Database> db;
    DatabaseCreateOptions options;
    options.pageSize = 65536; // largest page
    CHECK_STATUS(Database::create(path, options, db), DbStatus::Ok);
    if (!db) return;

    TableDefinition def("T");
    def.columns.push_back(ColumnDefinition("Txt", DbType::Text, false));
    def.columns.push_back(ColumnDefinition("Blb", DbType::Blob, false));
    uint32_t id = 0;
    CHECK_STATUS(db->createTable(def, id), DbStatus::Ok);
    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("T", table), DbStatus::Ok);

    // A large text and blob that fit in a 64K page are accepted.
    const uint32_t kBig = 30000;
    std::vector<uint8_t> bigBlob(kBig, 0xAB);
    CHECK_STATUS(table->insert({DbValue::text(std::string(kBig, 'x')),
                                DbValue::blob(bigBlob.data(), bigBlob.size())}),
                 DbStatus::Ok);

    // The text policy maximum + 1 is rejected.
    CHECK_STATUS(table->insert({DbValue::text(std::string(kMaxTextBytes + 1, 'y')),
                                DbValue::blob(nullptr, 0)}),
                 DbStatus::InvalidArgument);

    // A blob at the policy maximum + 1 is rejected.
    std::vector<uint8_t> hugeBlob(kMaxBlobBytes + 1, 0xCD);
    CHECK_STATUS(table->insert({DbValue::text("ok"),
                                DbValue::blob(hugeBlob.data(), hugeBlob.size())}),
                 DbStatus::InvalidArgument);

    // A text at the policy maximum does not fit any page and is rejected.
    CHECK_STATUS(table->insert({DbValue::text(std::string(kMaxTextBytes, 'z')),
                                DbValue::blob(nullptr, 0)}),
                 DbStatus::InvalidArgument);

    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    if (!reopened) return;
    std::unique_ptr<Table> rt;
    CHECK_STATUS(reopened->openTable("T", rt), DbStatus::Ok);
    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(rt->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    CHECK(scan->next(row));
    CHECK(row[0].textValue().size() == kBig);
    CHECK(row[1].blobValue().size() == kBig);
    CHECK(!scan->next(row));
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testTypeErrors() {
    std::printf("type errors\n");
    const std::string path = uniquePath("typeerr");

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;

    TableDefinition def("T");
    def.columns.push_back(ColumnDefinition("A", DbType::Int64, false));
    def.columns.push_back(ColumnDefinition("B", DbType::Text, true));
    def.columns.push_back(ColumnDefinition("C", DbType::Boolean, false));
    uint32_t id = 0;
    CHECK_STATUS(db->createTable(def, id), DbStatus::Ok);

    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("T", table), DbStatus::Ok);

    // Wrong number of values.
    CHECK_STATUS(table->insert({DbValue::int64(1)}), DbStatus::InvalidArgument);
    CHECK_STATUS(table->insert({DbValue::int64(1), DbValue::text("x"),
                                DbValue::boolean(true), DbValue::int64(4)}),
                 DbStatus::InvalidArgument);

    // Incompatible type.
    CHECK_STATUS(table->insert({DbValue::text("x"), DbValue::text("y"),
                                DbValue::boolean(true)}),
                 DbStatus::InvalidArgument);
    CHECK_STATUS(table->insert({DbValue::int64(1), DbValue::int64(2),
                                DbValue::boolean(true)}),
                 DbStatus::InvalidArgument);

    // NULL in NOT NULL column.
    CHECK_STATUS(table->insert({DbValue::null(), DbValue::text("y"),
                                DbValue::boolean(true)}),
                 DbStatus::InvalidArgument);
    CHECK_STATUS(table->insert({DbValue::int64(1), DbValue::text("y"),
                                DbValue::null()}),
                 DbStatus::InvalidArgument);

    // NULL in nullable column is fine.
    CHECK_STATUS(table->insert({DbValue::int64(1), DbValue::null(),
                                DbValue::boolean(false)}),
                 DbStatus::Ok);

    // Oversized text/blob.
    CHECK_STATUS(table->insert({DbValue::int64(2),
                                DbValue::text(std::string(kMaxTextBytes + 1, 'x')),
                                DbValue::boolean(true)}),
                 DbStatus::InvalidArgument);
    CHECK_STATUS(table->insert({DbValue::int64(3), DbValue::null(),
                                DbValue::boolean(true)}),
                 DbStatus::Ok);

    CHECK_STATUS(db->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testMultipleRows() {
    std::printf("multiple rows across pages\n");
    const std::string path = uniquePath("rows");

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;

    uint32_t id = 0;
    CHECK_STATUS(db->createTable(makeUsersSchema(), id), DbStatus::Ok);
    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("Users", table), DbStatus::Ok);

    const int kRows = 500;
    for (int i = 0; i < kRows; ++i) {
        DbResult result = table->insert({DbValue::int64(i),
                                         DbValue::text("Name_" + std::to_string(i)),
                                         DbValue::boolean(i % 2 == 0)});
        if (!result.isOk()) {
            CHECK_STATUS(result, DbStatus::Ok);
            break;
        }
    }
    CHECK(table->heapPageCount() >= 3);
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    if (!reopened) return;
    std::unique_ptr<Table> rt;
    CHECK_STATUS(reopened->openTable("Users", rt), DbStatus::Ok);
    CHECK(rt->heapPageCount() >= 3);
    CHECK(rt->rowCount() == static_cast<uint64_t>(kRows));

    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(rt->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    int count = 0;
    while (scan->next(row)) {
        CHECK(row.size() == 3);
        if (row.size() == 3) {
            CHECK(row[0].type() == DbType::Int64);
            CHECK(row[0].int64Value() == count);
            CHECK(row[1].textValue() == "Name_" + std::to_string(count));
            CHECK(row[2].booleanValue() == (count % 2 == 0));
        }
        ++count;
    }
    CHECK_STATUS(scan->status(), DbStatus::Ok);
    CHECK(count == kRows);
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testMultipleTables() {
    std::printf("multiple tables interleaved\n");
    const std::string path = uniquePath("multitable");

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;

    uint32_t aId = 0;
    CHECK_STATUS(db->createTable(makeUsersSchema(), aId), DbStatus::Ok);
    uint32_t bId = 0;
    TableDefinition bdef("Products");
    bdef.columns.push_back(ColumnDefinition("Sku", DbType::Int32, false));
    bdef.columns.push_back(ColumnDefinition("Price", DbType::Float64, false));
    CHECK_STATUS(db->createTable(bdef, bId), DbStatus::Ok);

    std::unique_ptr<Table> a;
    std::unique_ptr<Table> b;
    CHECK_STATUS(db->openTable("Users", a), DbStatus::Ok);
    CHECK_STATUS(db->openTable("Products", b), DbStatus::Ok);

    for (int i = 0; i < 200; ++i) {
        CHECK_STATUS(a->insert({DbValue::int64(i),
                                DbValue::text("U" + std::to_string(i)),
                                DbValue::boolean(true)}), DbStatus::Ok);
        CHECK_STATUS(b->insert({DbValue::int32(i),
                                DbValue::float64(i * 0.25)}), DbStatus::Ok);
    }
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    if (!reopened) return;
    std::unique_ptr<Table> ra;
    std::unique_ptr<Table> rb;
    CHECK_STATUS(reopened->openTable("Users", ra), DbStatus::Ok);
    CHECK_STATUS(reopened->openTable("Products", rb), DbStatus::Ok);

    std::unique_ptr<TableScan> sa;
    CHECK_STATUS(ra->scanStart(sa), DbStatus::Ok);
    std::vector<DbValue> row;
    int count = 0;
    while (sa->next(row)) {
        CHECK(row[0].int64Value() == count);
        CHECK(row[1].textValue() == "U" + std::to_string(count));
        ++count;
    }
    CHECK(count == 200);

    std::unique_ptr<TableScan> sb;
    CHECK_STATUS(rb->scanStart(sb), DbStatus::Ok);
    count = 0;
    while (sb->next(row)) {
        CHECK(row[0].int32Value() == count);
        CHECK(row[1].float64Value() == count * 0.25);
        ++count;
    }
    CHECK(count == 200);
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testCatalogGrowth() {
    std::printf("catalog growth beyond root page\n");
    const std::string path = uniquePath("cataloggrowth");

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;

    const int kTables = 60;
    for (int i = 0; i < kTables; ++i) {
        TableDefinition def("Table_" + std::to_string(i));
        for (int c = 0; c < 5; ++c) {
            def.columns.push_back(ColumnDefinition("Col" + std::to_string(c),
                                                   DbType::Int32, false));
        }
        uint32_t id = 0;
        DbResult result = db->createTable(def, id);
        if (!result.isOk()) {
            CHECK_STATUS(result, DbStatus::Ok);
            break;
        }
    }
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK(db->diagnostics().catalogPageCount >= 2);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    if (!reopened) return;
    CHECK(reopened->diagnostics().catalogPageCount >= 2);

    std::vector<TableInfo> tables;
    CHECK_STATUS(reopened->listTables(tables), DbStatus::Ok);
    CHECK(tables.size() == static_cast<size_t>(kTables));
    for (int i = 0; i < kTables; ++i) {
        TableDefinition desc;
        DbResult result = reopened->describeTable("Table_" + std::to_string(i), desc);
        if (!result.isOk()) {
            CHECK_STATUS(result, DbStatus::Ok);
            break;
        }
        CHECK(desc.columns.size() == 5);
    }
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testBufferManager() {
    std::printf("buffer manager\n");
    const std::string path = uniquePath("buffer");

    std::unique_ptr<Database> db;
    DatabaseCreateOptions options;
    options.bufferCapacity = 2; // deliberately small
    CHECK_STATUS(Database::create(path, options, db), DbStatus::Ok);
    if (!db) return;
    CHECK(db->bufferCapacity() == 2);

    uint32_t id = 0;
    CHECK_STATUS(db->createTable(makeUsersSchema(), id), DbStatus::Ok);
    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("Users", table), DbStatus::Ok);

    // Insert enough rows to allocate several heap pages.
    for (int i = 0; i < 300; ++i) {
        CHECK_STATUS(table->insert({DbValue::int64(i),
                                    DbValue::text("N" + std::to_string(i)),
                                    DbValue::boolean(i % 2 == 0)}), DbStatus::Ok);
    }
    CHECK(db->diagnostics().bufferDirty > 0);
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK(db->diagnostics().bufferDirty == 0);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Reopen with a tiny cache and scan (exercises eviction + dirty handling).
    std::unique_ptr<Database> reopened;
    DatabaseOpenOptions openOptions;
    openOptions.bufferCapacity = 2;
    CHECK_STATUS(Database::open(path, openOptions, reopened), DbStatus::Ok);
    if (!reopened) return;
    std::unique_ptr<Table> rt;
    CHECK_STATUS(reopened->openTable("Users", rt), DbStatus::Ok);
    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(rt->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    int count = 0;
    while (scan->next(row)) {
        ++count;
    }
    CHECK_STATUS(scan->status(), DbStatus::Ok);
    CHECK(count == 300);
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testLifecycle() {
    std::printf("lifecycle (10 cycles)\n");
    const std::string path = uniquePath("lifecycle");
    DatabaseCreateOptions createOptions;
    createOptions.overwriteExisting = true;

    bool allOk = true;
    for (int cycle = 0; cycle < 10 && allOk; ++cycle) {
        std::unique_ptr<Database> db;
        DbResult result;
        if (cycle == 0) {
            result = Database::create(path, createOptions, db);
        } else {
            result = Database::open(path, DatabaseOpenOptions(), db);
        }
        if (!result.isOk() || !db) { allOk = false; break; }

        if (cycle == 0) {
            uint32_t id = 0;
            result = db->createTable(makeUsersSchema(), id);
            if (!result.isOk()) { allOk = false; break; }
        }

        std::unique_ptr<Table> table;
        result = db->openTable("Users", table);
        if (!result.isOk()) { allOk = false; break; }

        const int base = cycle * 100;
        for (int i = 0; i < 100; ++i) {
            result = table->insert({DbValue::int64(base + i),
                                    DbValue::text("C" + std::to_string(cycle) + "_" + std::to_string(i)),
                                    DbValue::boolean(i % 3 == 0)});
            if (!result.isOk()) { allOk = false; break; }
        }
        if (!allOk) break;
        if (!db->flush().isOk()) { allOk = false; break; }
        if (!db->close().isOk()) { allOk = false; break; }

        std::unique_ptr<Database> reopened;
        result = Database::open(path, DatabaseOpenOptions(), reopened);
        if (!result.isOk() || !reopened) { allOk = false; break; }
        std::unique_ptr<Table> rt;
        result = reopened->openTable("Users", rt);
        if (!result.isOk()) { allOk = false; break; }
        std::unique_ptr<TableScan> scan;
        result = rt->scanStart(scan);
        if (!result.isOk()) { allOk = false; break; }
        std::vector<DbValue> row;
        int count = 0;
        while (scan->next(row)) {
            if (row.size() != 3 || row[0].int64Value() != count) { allOk = false; break; }
            ++count;
        }
        if (!allOk) break;
        if (count != (cycle + 1) * 100) { allOk = false; break; }
        if (!reopened->close().isOk()) { allOk = false; break; }
    }
    CHECK(allOk);
    std::remove(path.c_str());
}

void testStatsPersistenceAcrossSessions() {
    std::printf("stats persistence across sessions\n");
    const std::string path = uniquePath("statspersist");

    // Session 1: create table, insert, flush, close.
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;
    uint32_t id = 0;
    CHECK_STATUS(db->createTable(makeUsersSchema(), id), DbStatus::Ok);
    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("Users", table), DbStatus::Ok);
    for (int i = 0; i < 10; ++i) {
        CHECK_STATUS(table->insert({DbValue::int64(i), DbValue::text("A"),
                                    DbValue::boolean(true)}), DbStatus::Ok);
    }
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Session 2: reopen, insert more, flush, close. The catalog was already
    // saved in session 1, so this exercises the insert-marks-dirty path.
    std::unique_ptr<Database> db2;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db2), DbStatus::Ok);
    if (!db2) return;
    std::unique_ptr<Table> table2;
    CHECK_STATUS(db2->openTable("Users", table2), DbStatus::Ok);
    for (int i = 10; i < 20; ++i) {
        CHECK_STATUS(table2->insert({DbValue::int64(i), DbValue::text("B"),
                                     DbValue::boolean(false)}), DbStatus::Ok);
    }
    CHECK(table2->rowCount() == 20);
    CHECK_STATUS(db2->flush(), DbStatus::Ok);
    CHECK_STATUS(db2->close(), DbStatus::Ok);

    // Session 3: verify the durable row count and heap page count.
    std::unique_ptr<Database> db3;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db3), DbStatus::Ok);
    if (!db3) return;
    std::unique_ptr<Table> table3;
    CHECK_STATUS(db3->openTable("Users", table3), DbStatus::Ok);
    CHECK(table3->rowCount() == 20);
    CHECK(table3->heapPageCount() >= 1);
    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(table3->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    int count = 0;
    while (scan->next(row)) {
        ++count;
    }
    CHECK(count == 20);
    CHECK_STATUS(db3->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testBufferManagerDirect() {
    std::printf("buffer manager direct\n");
    const std::string path = uniquePath("bufdirect");
    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;
    DatabaseFile& file = db->file();

    uint64_t p[4];
    for (int i = 0; i < 4; ++i) {
        CHECK_STATUS(file.allocatePage(PageType::Data, p[i]), DbStatus::Ok);
    }

    BufferManager buf(file, 2);
    CHECK(buf.capacity() == 2);

    // Cache hit: pinning the same page twice returns the same slot.
    BufferSlot* a = nullptr;
    CHECK_STATUS(buf.pinPage(p[0], a), DbStatus::Ok);
    BufferSlot* aAgain = nullptr;
    CHECK_STATUS(buf.pinPage(p[0], aAgain), DbStatus::Ok);
    CHECK(aAgain == a);
    CHECK(buf.residentCount() == 1);
    buf.unpin(*a);
    buf.unpin(*aAgain);

    // Eviction: with p0,p1 resident, pinning p2 evicts the LRU (p0).
    BufferSlot* b = nullptr;
    CHECK_STATUS(buf.pinPage(p[1], b), DbStatus::Ok);
    BufferSlot* c = nullptr;
    CHECK_STATUS(buf.pinPage(p[2], c), DbStatus::Ok);
    CHECK(buf.residentCount() == 2);
    buf.unpin(*b);
    buf.unpin(*c);

    // Pinning: a pinned page is never evicted; exhausting pins fails.
    BufferSlot* d = nullptr;
    CHECK_STATUS(buf.pinPage(p[1], d), DbStatus::Ok);
    BufferSlot* e = nullptr;
    CHECK_STATUS(buf.pinPage(p[2], e), DbStatus::Ok);
    BufferSlot* f = nullptr;
    CHECK_STATUS(buf.pinPage(p[3], f), DbStatus::Internal);
    buf.unpin(*d);
    buf.unpin(*e);

    // Dirty eviction: a dirty page is written back when evicted.
    BufferSlot* g = nullptr;
    CHECK_STATUS(buf.pinPage(p[1], g), DbStatus::Ok);
    buf.markDirty(*g);
    CHECK(buf.dirtyCount() == 1);
    buf.unpin(*g);
    BufferSlot* g2 = nullptr;
    CHECK_STATUS(buf.pinPage(p[2], g2), DbStatus::Ok);
    buf.unpin(*g2);
    BufferSlot* h = nullptr;
    CHECK_STATUS(buf.pinPage(p[3], h), DbStatus::Ok);
    CHECK(buf.dirtyCount() == 0);
    buf.unpin(*h);

    // Flush writes all dirty pages.
    BufferSlot* i = nullptr;
    CHECK_STATUS(buf.pinPage(p[1], i), DbStatus::Ok);
    buf.markDirty(*i);
    CHECK(buf.dirtyCount() == 1);
    CHECK_STATUS(buf.flush(), DbStatus::Ok);
    CHECK(buf.dirtyCount() == 0);
    buf.unpin(*i);

    CHECK_STATUS(db->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testReadOnly() {
    std::printf("read-only behavior\n");
    const std::string path = uniquePath("readonly");

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;
    uint32_t id = 0;
    CHECK_STATUS(db->createTable(makeUsersSchema(), id), DbStatus::Ok);
    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("Users", table), DbStatus::Ok);
    CHECK_STATUS(table->insert({DbValue::int64(1), DbValue::text("A"),
                                DbValue::boolean(true)}), DbStatus::Ok);
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    DatabaseOpenOptions ro;
    ro.readOnly = true;
    std::unique_ptr<Database> dbro;
    CHECK_STATUS(Database::open(path, ro, dbro), DbStatus::Ok);
    if (!dbro) return;
    CHECK(dbro->isReadOnly());

    // Discovery works.
    std::vector<TableInfo> tables;
    CHECK_STATUS(dbro->listTables(tables), DbStatus::Ok);
    CHECK(tables.size() == 1);
    TableDefinition desc;
    CHECK_STATUS(dbro->describeTable("Users", desc), DbStatus::Ok);
    CHECK(desc.columns.size() == 3);

    // Scan works.
    std::unique_ptr<Table> rt;
    CHECK_STATUS(dbro->openTable("Users", rt), DbStatus::Ok);
    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(rt->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    CHECK(scan->next(row));
    CHECK(row[0].int64Value() == 1);
    CHECK(!scan->next(row));

    // Creation and insertion fail cleanly.
    uint32_t newId = 0;
    CHECK_STATUS(dbro->createTable(TableDefinition("Nope"), newId), DbStatus::ReadOnly);
    CHECK_STATUS(rt->insert({DbValue::int64(2), DbValue::text("B"),
                             DbValue::boolean(false)}),
                 DbStatus::ReadOnly);
    CHECK_STATUS(dbro->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testEmptyTable() {
    std::printf("empty table scan\n");
    const std::string path = uniquePath("empty");

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;
    uint32_t id = 0;
    CHECK_STATUS(db->createTable(TableDefinition("Empty"), id), DbStatus::Ok);
    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("Empty", table), DbStatus::Ok);
    CHECK(table->heapPageCount() == 0);
    CHECK(table->rowCount() == 0);

    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(table->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    CHECK(!scan->next(row));
    CHECK_STATUS(scan->status(), DbStatus::Ok);

    // Insert into a zero-column table.
    CHECK_STATUS(table->insert({}), DbStatus::Ok);
    CHECK(table->rowCount() == 1);
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    if (!reopened) return;
    std::unique_ptr<Table> rt;
    CHECK_STATUS(reopened->openTable("Empty", rt), DbStatus::Ok);
    CHECK(rt->rowCount() == 1);
    std::unique_ptr<TableScan> rscan;
    CHECK_STATUS(rt->scanStart(rscan), DbStatus::Ok);
    CHECK(rscan->next(row));
    CHECK(row.empty());
    CHECK(!rscan->next(row));
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testApiDemo() {
    std::printf("api demonstration\n");
    const std::string path = uniquePath("demo");

    std::unique_ptr<Database> db;
    Database::create(path, DatabaseCreateOptions(), db);

    TableDefinition users;
    users.name = "Users";
    users.columns.push_back(ColumnDefinition("Id", DbType::Int64, false));
    users.columns.push_back(ColumnDefinition("Name", DbType::Text, false));
    users.columns.push_back(ColumnDefinition("Enabled", DbType::Boolean, false));
    uint32_t id = 0;
    db->createTable(users, id);

    std::unique_ptr<Table> table;
    db->openTable("Users", table);
    table->insert({DbValue::int64(1), DbValue::text("Alice"), DbValue::boolean(true)});
    table->insert({DbValue::int64(2), DbValue::text("Bob"), DbValue::boolean(false)});

    db->flush();
    db->close();

    Database::open(path, DatabaseOpenOptions(), db);
    db->openTable("Users", table);

    std::unique_ptr<TableScan> scan;
    table->scanStart(scan);
    std::vector<DbValue> row;
    int found = 0;
    while (scan->next(row)) {
        if (row[0].int64Value() == 1) {
            CHECK(row[1].textValue() == "Alice");
            CHECK(row[2].booleanValue());
            ++found;
        } else if (row[0].int64Value() == 2) {
            CHECK(row[1].textValue() == "Bob");
            CHECK(!row[2].booleanValue());
            ++found;
        }
    }
    CHECK(found == 2);
    db->close();
    std::remove(path.c_str());
}

void testLargeDataset() {
    std::printf("large deterministic dataset (1200 rows)\n");
    const std::string path = uniquePath("large");

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::create(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (!db) return;

    TableDefinition def("Events");
    def.columns.push_back(ColumnDefinition("Idx", DbType::Int64, false));
    def.columns.push_back(ColumnDefinition("Name", DbType::Text, false));
    def.columns.push_back(ColumnDefinition("Flag", DbType::Boolean, false));
    def.columns.push_back(ColumnDefinition("Score", DbType::Float64, false));
    uint32_t id = 0;
    CHECK_STATUS(db->createTable(def, id), DbStatus::Ok);
    std::unique_ptr<Table> table;
    CHECK_STATUS(db->openTable("Events", table), DbStatus::Ok);

    const int kRows = 1200;
    for (int i = 0; i < kRows; ++i) {
        std::vector<DbValue> values;
        expectedRowValues(i, values);
        DbResult result = table->insert(values);
        if (!result.isOk()) {
            CHECK_STATUS(result, DbStatus::Ok);
            break;
        }
    }
    CHECK(table->heapPageCount() >= 3);
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    if (!reopened) return;
    std::unique_ptr<Table> rt;
    CHECK_STATUS(reopened->openTable("Events", rt), DbStatus::Ok);
    CHECK(rt->rowCount() == static_cast<uint64_t>(kRows));

    std::unique_ptr<TableScan> scan;
    CHECK_STATUS(rt->scanStart(scan), DbStatus::Ok);
    std::vector<DbValue> row;
    int count = 0;
    while (scan->next(row)) {
        std::vector<DbValue> expected;
        expectedRowValues(count, expected);
        CHECK(row.size() == 4);
        if (row.size() == 4) {
            CHECK(row[0].int64Value() == expected[0].int64Value());
            CHECK(row[1].textValue() == expected[1].textValue());
            CHECK(row[2].booleanValue() == expected[2].booleanValue());
            CHECK(row[3].float64Value() == expected[3].float64Value());
        }
        ++count;
    }
    CHECK_STATUS(scan->status(), DbStatus::Ok);
    CHECK(count == kRows);
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Corruption tests.

void buildCorruptBase(const std::string& path) {
    std::unique_ptr<Database> db;
    Database::create(path, DatabaseCreateOptions(), db);
    uint32_t id = 0;
    db->createTable(makeUsersSchema(), id);
    std::unique_ptr<Table> table;
    db->openTable("Users", table);
    for (int i = 0; i < 50; ++i) {
        table->insert({DbValue::int64(i), DbValue::text("N" + std::to_string(i)),
                       DbValue::boolean(i % 2 == 0)});
    }
    db->flush();
    db->close();
}

// Builds a base with enough rows to span several heap pages.
void buildMultiPageHeapBase(const std::string& path) {
    std::unique_ptr<Database> db;
    Database::create(path, DatabaseCreateOptions(), db);
    uint32_t id = 0;
    db->createTable(makeUsersSchema(), id);
    std::unique_ptr<Table> table;
    db->openTable("Users", table);
    for (int i = 0; i < 300; ++i) {
        table->insert({DbValue::int64(i), DbValue::text("N" + std::to_string(i)),
                       DbValue::boolean(i % 2 == 0)});
    }
    db->flush();
    db->close();
}

// Builds a base whose catalog spans multiple pages.
void buildMultiPageCatalogBase(const std::string& path) {
    std::unique_ptr<Database> db;
    Database::create(path, DatabaseCreateOptions(), db);
    for (int i = 0; i < 60; ++i) {
        TableDefinition def("T" + std::to_string(i));
        for (int c = 0; c < 5; ++c) {
            def.columns.push_back(ColumnDefinition("C" + std::to_string(c),
                                                   DbType::Int32, false));
        }
        uint32_t id = 0;
        db->createTable(def, id);
    }
    db->flush();
    db->close();
}

void testCatalogCorruption() {
    std::printf("catalog corruption\n");
    const std::string base = uniquePath("catcorruptbase");
    buildCorruptBase(base);
    const uint32_t pageSize = 4096;
    DatabaseOpenOptions openOptions;

    // Corrupt catalog magic.
    {
        const std::string work = uniquePath("catmagic");
        mutateCopy(base, work, static_cast<size_t>(kBootstrapPageId) * pageSize, 0x00);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    // Corrupt catalog version.
    {
        const std::string work = uniquePath("catversion");
        std::vector<uint8_t> bytes = readFileBytes(base);
        storeLe16(bytes.data() + static_cast<size_t>(kBootstrapPageId) * pageSize +
                      kPageHeaderSize + 4, 99);
        fixPageCrc(bytes, kBootstrapPageId, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::UnsupportedVersion);
        std::remove(work.c_str());
    }
    // Impossible table count.
    {
        const std::string work = uniquePath("cattablecount");
        std::vector<uint8_t> bytes = readFileBytes(base);
        storeLe32(bytes.data() + static_cast<size_t>(kBootstrapPageId) * pageSize +
                      kPageHeaderSize + 8, 0xFFFFFFu);
        fixPageCrc(bytes, kBootstrapPageId, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    // Invalid continuation page pointer.
    {
        const std::string work = uniquePath("catcont");
        std::vector<uint8_t> bytes = readFileBytes(base);
        storeLe32(bytes.data() + static_cast<size_t>(kBootstrapPageId) * pageSize +
                      kPageHeaderSize + 16, 1); // continuationCount = 1
        storeLe32(bytes.data() + static_cast<size_t>(kBootstrapPageId) * pageSize +
                      kPageHeaderSize + 20, 99); // firstContinuation = 99 (invalid)
        fixPageCrc(bytes, kBootstrapPageId, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    // Invalid table id (0) in the first table record.
    {
        const std::string work = uniquePath("cattableid");
        std::vector<uint8_t> bytes = readFileBytes(base);
        const size_t recOffset = static_cast<size_t>(kBootstrapPageId) * pageSize +
                                 kPageHeaderSize + kCatalogRootHeaderSize;
        storeLe32(bytes.data() + recOffset + 0, 0); // tableId = 0
        fixPageCrc(bytes, kBootstrapPageId, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    // Impossible column count.
    {
        const std::string work = uniquePath("catcolcount");
        std::vector<uint8_t> bytes = readFileBytes(base);
        const size_t recOffset = static_cast<size_t>(kBootstrapPageId) * pageSize +
                                 kPageHeaderSize + kCatalogRootHeaderSize;
        storeLe32(bytes.data() + recOffset + 8, 0xFFFFu); // columnCount huge
        fixPageCrc(bytes, kBootstrapPageId, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    // Malformed table name length.
    {
        const std::string work = uniquePath("catnamelen");
        std::vector<uint8_t> bytes = readFileBytes(base);
        const size_t recOffset = static_cast<size_t>(kBootstrapPageId) * pageSize +
                                 kPageHeaderSize + kCatalogRootHeaderSize;
        storeLe32(bytes.data() + recOffset + 36, 0xFFFFu); // nameLength huge
        fixPageCrc(bytes, kBootstrapPageId, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    // Unsupported column type id.
    {
        const std::string work = uniquePath("cattypeid");
        std::vector<uint8_t> bytes = readFileBytes(base);
        // Table record layout from recOffset: 40-byte header, then the name
        // "Users" (5 bytes), then column records. The first column's type
        // field is at recOffset + 40 + 5 + 4 = recOffset + 49.
        const size_t recOffset = static_cast<size_t>(kBootstrapPageId) * pageSize +
                                 kPageHeaderSize + kCatalogRootHeaderSize;
        storeLe16(bytes.data() + recOffset + 49, 99); // unsupported type
        fixPageCrc(bytes, kBootstrapPageId, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    std::remove(base.c_str());

    // Catalog continuation cycle.
    {
        const std::string mbase = uniquePath("catcyclebase");
        buildMultiPageCatalogBase(mbase);
        const std::string work = uniquePath("catcycle");
        std::vector<uint8_t> bytes = readFileBytes(mbase);
        // Read firstContinuationPageId from the root header.
        const size_t rootPayload = static_cast<size_t>(kBootstrapPageId) * pageSize + kPageHeaderSize;
        const uint32_t firstCont = loadLe32(bytes.data() + rootPayload + 20);
        CHECK(firstCont >= 2);
        // Make the first continuation page point to itself (cycle).
        const size_t contPayload = static_cast<size_t>(firstCont) * pageSize + kPageHeaderSize;
        storeLe64(bytes.data() + contPayload + 8, firstCont); // next = self
        fixPageCrc(bytes, firstCont, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> out;
        CHECK_STATUS(Database::open(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
        std::remove(mbase.c_str());
    }
}

void testHeapCorruption() {
    std::printf("heap corruption\n");
    const std::string base = uniquePath("heapcorruptbase");
    buildCorruptBase(base);
    const uint32_t pageSize = 4096;
    const uint64_t heapPage = 2; // first heap page of a fresh single-table db
    DatabaseOpenOptions openOptions;

    // Wrong page type on a heap page.
    {
        const std::string work = uniquePath("heaptype");
        std::vector<uint8_t> bytes = readFileBytes(base);
        storeLe16(bytes.data() + static_cast<size_t>(heapPage) * pageSize +
                      page_offset::PageType, 1); // Catalog instead of Data
        fixPageCrc(bytes, heapPage, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, openOptions, db), DbStatus::Ok);
        if (db) {
            std::unique_ptr<Table> table;
            if (db->openTable("Users", table).isOk()) {
                std::unique_ptr<TableScan> scan;
                if (table->scanStart(scan).isOk()) {
                    std::vector<DbValue> row;
                    CHECK(!scan->next(row));
                    CHECK_STATUS(scan->status(), DbStatus::CorruptPage);
                }
            }
            db->close();
        }
        std::remove(work.c_str());
    }
    // Impossible slot count.
    {
        const std::string work = uniquePath("heapslots");
        std::vector<uint8_t> bytes = readFileBytes(base);
        storeLe32(bytes.data() + static_cast<size_t>(heapPage) * pageSize +
                      kPageHeaderSize + kHeapSlotCountOffset, 0xFFFFu);
        fixPageCrc(bytes, heapPage, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, openOptions, db), DbStatus::Ok);
        if (db) {
            std::unique_ptr<Table> table;
            if (db->openTable("Users", table).isOk()) {
                std::unique_ptr<TableScan> scan;
                if (table->scanStart(scan).isOk()) {
                    std::vector<DbValue> row;
                    CHECK(!scan->next(row));
                    CHECK_STATUS(scan->status(), DbStatus::CorruptPage);
                }
            }
            db->close();
        }
        std::remove(work.c_str());
    }
    // Row extending outside the page (corrupt a slot's length).
    {
        const std::string work = uniquePath("heaprow");
        std::vector<uint8_t> bytes = readFileBytes(base);
        // Slot 0 is at payload offset capacity - 8.
        const uint32_t capacity = pageSize - kPageHeaderSize;
        const uint32_t slotOffset = capacity - 8u;
        storeLe32(bytes.data() + static_cast<size_t>(heapPage) * pageSize +
                      kPageHeaderSize + slotOffset + 4, capacity + 100u);
        fixPageCrc(bytes, heapPage, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, openOptions, db), DbStatus::Ok);
        if (db) {
            std::unique_ptr<Table> table;
            if (db->openTable("Users", table).isOk()) {
                std::unique_ptr<TableScan> scan;
                if (table->scanStart(scan).isOk()) {
                    std::vector<DbValue> row;
                    CHECK(!scan->next(row));
                    CHECK_STATUS(scan->status(), DbStatus::CorruptPage);
                }
            }
            db->close();
        }
        std::remove(work.c_str());
    }
    // Malformed variable-length offset inside a row (corrupt the text length).
    {
        const std::string work = uniquePath("heapvarlen");
        std::vector<uint8_t> bytes = readFileBytes(base);
        // Row 0 starts at payload offset kHeapHeaderSize (24). Its layout:
        // [0..8) header, [8] null bitmap (3 cols -> 1 byte), [9..17) fixed
        // (Int64 8 + Boolean 1), [17..21) var length (Text), [21..) text bytes.
        const size_t rowStart = static_cast<size_t>(heapPage) * pageSize + kPageHeaderSize +
                                kHeapHeaderSize;
        storeLe32(bytes.data() + rowStart + 17, 0xFFFFu);
        fixPageCrc(bytes, heapPage, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, openOptions, db), DbStatus::Ok);
        if (db) {
            std::unique_ptr<Table> table;
            if (db->openTable("Users", table).isOk()) {
                std::unique_ptr<TableScan> scan;
                if (table->scanStart(scan).isOk()) {
                    std::vector<DbValue> row;
                    CHECK(!scan->next(row));
                    CHECK_STATUS(scan->status(), DbStatus::CorruptPage);
                }
            }
            db->close();
        }
        std::remove(work.c_str());
    }
    std::remove(base.c_str());

    // Invalid next-page pointer and page-chain cycle need a multi-page table.
    const std::string mbase = uniquePath("heapchainbase");
    buildMultiPageHeapBase(mbase);

    // Invalid next-page pointer (points outside the allocated range).
    {
        const std::string work = uniquePath("heapnext");
        std::vector<uint8_t> bytes = readFileBytes(mbase);
        // Heap page 2 header: nextPageId at payload offset 8.
        const size_t hdr = static_cast<size_t>(2) * pageSize + kPageHeaderSize;
        storeLe64(bytes.data() + hdr + kHeapNextPageOffset, 999u);
        fixPageCrc(bytes, 2, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, openOptions, db), DbStatus::Ok);
        if (db) {
            std::unique_ptr<Table> table;
            if (db->openTable("Users", table).isOk()) {
                std::unique_ptr<TableScan> scan;
                if (table->scanStart(scan).isOk()) {
                    std::vector<DbValue> row;
                    int count = 0;
                    while (scan->next(row)) {
                        ++count;
                    }
                    CHECK(count > 0);
                    CHECK(count < 300);
                    CHECK_STATUS(scan->status(), DbStatus::CorruptPage);
                }
            }
            db->close();
        }
        std::remove(work.c_str());
    }
    // Page-chain cycle (page 2 points to itself).
    {
        const std::string work = uniquePath("heapcycle");
        std::vector<uint8_t> bytes = readFileBytes(mbase);
        const size_t hdr = static_cast<size_t>(2) * pageSize + kPageHeaderSize;
        storeLe64(bytes.data() + hdr + kHeapNextPageOffset, 2u); // page 2 -> page 2
        fixPageCrc(bytes, 2, pageSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<Database> db;
        CHECK_STATUS(Database::open(work, openOptions, db), DbStatus::Ok);
        if (db) {
            std::unique_ptr<Table> table;
            if (db->openTable("Users", table).isOk()) {
                std::unique_ptr<TableScan> scan;
                if (table->scanStart(scan).isOk()) {
                    std::vector<DbValue> row;
                    int count = 0;
                    while (scan->next(row)) {
                        ++count;
                    }
                    CHECK(count > 0);
                    CHECK(count < 300);
                    CHECK_STATUS(scan->status(), DbStatus::CorruptPage);
                }
            }
            db->close();
        }
        std::remove(work.c_str());
    }
    std::remove(mbase.c_str());
}

void testSql1Compatibility() {
    std::printf("SQL1 database compatibility\n");
    // A SQL1 database (created via DatabaseEngine) must open through the
    // relational layer as an empty catalog.
    const std::string path = uniquePath("sql1compat");

    std::unique_ptr<DatabaseFile> file;
    CHECK_STATUS(DatabaseEngine::createDatabase(path, DatabaseCreateOptions(), file),
                 DbStatus::Ok);
    if (file) file->close();

    std::unique_ptr<Database> db;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), db), DbStatus::Ok);
    if (db) {
        std::vector<TableInfo> tables;
        CHECK_STATUS(db->listTables(tables), DbStatus::Ok);
        CHECK(tables.empty());
        // Creating a table upgrades the catalog in place.
        uint32_t id = 0;
        CHECK_STATUS(db->createTable(makeUsersSchema(), id), DbStatus::Ok);
        CHECK_STATUS(db->flush(), DbStatus::Ok);
        CHECK_STATUS(db->close(), DbStatus::Ok);
    }

    // Reopen and verify the table survived the upgrade.
    std::unique_ptr<Database> reopened;
    CHECK_STATUS(Database::open(path, DatabaseOpenOptions(), reopened), DbStatus::Ok);
    if (reopened) {
        std::vector<TableInfo> tables;
        CHECK_STATUS(reopened->listTables(tables), DbStatus::Ok);
        CHECK(tables.size() == 1);
        CHECK_STATUS(reopened->close(), DbStatus::Ok);
    }
    std::remove(path.c_str());
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("guideXOS SQL2 relational catalog and heap table tests\n");
    std::string dir = testDir();
    std::printf("temp dir: %s\n", dir.c_str());

    // Remove stale files from any previous crashed run so unique names do not
    // collide (the case counter resets each run).
    std::string cmd = "del /q \"" + dir + "\\*.gxdb\" >nul 2>nul";
    if (std::system(cmd.c_str()) == 0) {
        // best effort
    }

    testCreateAndReopen();
    testDuplicateHandling();
    testTypePersistence();
    testTypeBoundaries();
    testTypeErrors();
    testMultipleRows();
    testMultipleTables();
    testCatalogGrowth();
    testBufferManager();
    testBufferManagerDirect();
    testLifecycle();
    testStatsPersistenceAcrossSessions();
    testReadOnly();
    testEmptyTable();
    testApiDemo();
    testLargeDataset();
    testCatalogCorruption();
    testHeapCorruption();
    testSql1Compatibility();

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
