#include "kernel/native_database_file.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

const int kHostSeekSet = SEEK_SET;
const int kHostSeekCur = SEEK_CUR;
const int kHostSeekEnd = SEEK_END;

#ifdef SEEK_SET
#undef SEEK_SET
#endif
#ifdef SEEK_CUR
#undef SEEK_CUR
#endif
#ifdef SEEK_END
#undef SEEK_END
#endif

#include "kernel/vfs.h"

#include "database_catalog.h"
#include "database_endian.h"
#include "database_heap.h"
#include "database_header.h"
#include "database_index.h"
#include "database_relational.h"
#include "database_schema.h"
#include "database_types.h"
#include "gx_native_sql_index_page_provider.h"
#include "gx_index_lookup.h"
#include "gx_page_codec.h"

using namespace gxos::db;

namespace {

#if defined(GX_R3AG_QUALIFICATION)
const char* const kQualificationName = "R3AG";
const char* const kLogicalDatabasePath = "/R3AG/IMAGE.gxdb";
const char* const kBridgeDatabasePath = "/R3AG/IMAGE.GDB";
const char* const kTestDirectoryName = "gxos_r3ag_native_provider";
#else
const char* const kQualificationName = "R3AF";
const char* const kLogicalDatabasePath = "/R3AF/IMAGE.gxdb";
const char* const kBridgeDatabasePath = "/R3AF/IMAGE.GDB";
const char* const kTestDirectoryName = "gxos_r3af_native_provider";
#endif
const char* g_fixtureQualificationName = kQualificationName;

struct HostFileHandle {
    FILE* file;
    HostFileHandle() : file(nullptr) {}
};

HostFileHandle g_handles[32];
std::string g_imagePath;
std::string g_bridgePath;
uint32_t g_checks = 0;
uint32_t g_failures = 0;
uint32_t g_readCount = 0;
uint32_t g_shortReadAt = 0;
uint32_t g_readOnlyOpenCount = 0;
bool g_failSize = false;
bool g_scheduleShortReadAfterPointCallback = false;

void check(bool value, const char* name) {
    ++g_checks;
    if (!value) {
        ++g_failures;
        std::printf("FAIL: %s\n", name);
    }
}

#define CHECK(value) check((value), #value)

int32_t hostOpen(const char* path, uint16_t flags) {
    if (!path || g_imagePath.empty() || g_bridgePath != path ||
        (flags & kernel::vfs::OPEN_WRITE) != 0) {
        return kernel::vfs::VFS_ERR_READ_ONLY;
    }
    FILE* file = std::fopen(g_imagePath.c_str(), "rb");
    if (!file) return kernel::vfs::VFS_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < 32; ++i) {
        if (g_handles[i].file) continue;
        g_handles[i].file = file;
        ++g_readOnlyOpenCount;
        return static_cast<int32_t>(i);
    }
    std::fclose(file);
    return kernel::vfs::VFS_ERR_TOO_MANY;
}

int32_t hostClose(uint8_t handle) {
    if (handle >= 32 || !g_handles[handle].file) return kernel::vfs::VFS_ERR_INVALID;
    const int result = std::fclose(g_handles[handle].file);
    g_handles[handle].file = nullptr;
    return result == 0 ? kernel::vfs::VFS_OK : kernel::vfs::VFS_ERR_IO;
}

int32_t hostRead(uint8_t handle, void* buffer, uint32_t length) {
    if (handle >= 32 || !g_handles[handle].file || (!buffer && length)) {
        return kernel::vfs::VFS_ERR_INVALID;
    }
    ++g_readCount;
    uint32_t request = length;
    if (g_shortReadAt != 0 && g_readCount == g_shortReadAt && request != 0) {
        --request;
        g_shortReadAt = 0;
    }
    const size_t count = std::fread(buffer, 1, request, g_handles[handle].file);
    if (count < request && std::ferror(g_handles[handle].file)) {
        return kernel::vfs::VFS_ERR_IO;
    }
    return static_cast<int32_t>(count);
}

int32_t hostWrite(uint8_t, const void*, uint32_t) {
    return kernel::vfs::VFS_ERR_READ_ONLY;
}

int32_t hostSeek(uint8_t handle, int64_t offset, uint8_t origin) {
    if (handle >= 32 || !g_handles[handle].file || offset < 0 ||
        offset > 0xFFFFFFFFll) return kernel::vfs::VFS_ERR_INVALID;
    int nativeOrigin = kHostSeekSet;
    if (origin == kernel::vfs::SEEK_CUR) nativeOrigin = kHostSeekCur;
    else if (origin == kernel::vfs::SEEK_END) nativeOrigin = kHostSeekEnd;
    else if (origin != kernel::vfs::SEEK_SET) return kernel::vfs::VFS_ERR_INVALID;
    return std::fseek(g_handles[handle].file, static_cast<long>(offset), nativeOrigin) == 0
        ? kernel::vfs::VFS_OK : kernel::vfs::VFS_ERR_IO;
}

int64_t hostTell(uint8_t handle) {
    if (handle >= 32 || !g_handles[handle].file) return -1;
    return static_cast<int64_t>(std::ftell(g_handles[handle].file));
}

int64_t hostSize(uint8_t handle) {
    if (g_failSize) return -1;
    if (handle >= 32 || !g_handles[handle].file) return -1;
    const long original = std::ftell(g_handles[handle].file);
    if (original < 0 || std::fseek(g_handles[handle].file, 0, kHostSeekEnd) != 0) return -1;
    const long size = std::ftell(g_handles[handle].file);
    if (std::fseek(g_handles[handle].file, original, kHostSeekSet) != 0) return -1;
    return static_cast<int64_t>(size);
}

int32_t hostFlush(uint8_t handle) {
    return handle < 32 && g_handles[handle].file
        ? kernel::vfs::VFS_OK : kernel::vfs::VFS_ERR_INVALID;
}

int32_t hostTruncate(uint8_t, uint64_t) { return kernel::vfs::VFS_ERR_READ_ONLY; }

int32_t hostStat(const char* path, uint32_t* type, uint64_t* size) {
    if (!path || g_bridgePath != path) return kernel::vfs::VFS_ERR_NOT_FOUND;
    std::ifstream input(g_imagePath.c_str(), std::ios::binary | std::ios::ate);
    if (!input) return kernel::vfs::VFS_ERR_NOT_FOUND;
    const std::streamoff end = input.tellg();
    if (end < 0) return kernel::vfs::VFS_ERR_IO;
    if (type) *type = static_cast<uint32_t>(kernel::vfs::FILE_TYPE_REGULAR);
    if (size) *size = static_cast<uint64_t>(end);
    return kernel::vfs::VFS_OK;
}

int32_t hostUnlink(const char*) { return kernel::vfs::VFS_ERR_NOT_SUPPORTED; }

const GxDbNativeFileOps kHostOps = {
    hostOpen, hostClose, hostRead, hostWrite, hostSeek, hostTell, hostSize,
    hostFlush, hostTruncate, hostStat, hostUnlink
};

std::string testDirectory() {
    const char* base = std::getenv("TEMP");
    if (!base) base = std::getenv("TMP");
    if (!base) base = ".";
    const std::string path = std::string(base) + "\\" + kTestDirectoryName;
#if defined(_WIN32)
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0700);
#endif
    return path;
}

std::string uniquePath(const char* suffix) {
    static uint32_t sequence = 0;
    char name[96];
    std::snprintf(name, sizeof(name), "\\image_%lu_%lu_%s.gxdb",
                  static_cast<unsigned long>(std::time(nullptr)),
                  static_cast<unsigned long>(sequence++), suffix);
    return testDirectory() + name;
}

void removeDatabaseFiles(const std::string& path) {
    std::remove(path.c_str());
    const size_t dot = path.find_last_of('.');
    const std::string wal = dot == std::string::npos
        ? path + ".gxwal" : path.substr(0, dot) + ".gxwal";
    std::remove(wal.c_str());
}

std::string walSidecarPath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    return dot == std::string::npos ? path + ".gxwal"
                                    : path.substr(0, dot) + ".gxwal";
}

bool walSidecarAbsent(const std::string& path) {
    std::ifstream input(walSidecarPath(path).c_str(), std::ios::binary);
    return !input.good();
}

struct Fixture {
    std::string path;
    uint32_t pageSize;
    uint64_t pageCount;
    uint64_t catalogRootPageId;
    uint16_t formatMajor;
    uint16_t formatMinor;
    uint16_t catalogVersion;
    uint64_t creationTimeUnixNanos;
    uint8_t databaseId[16];
    uint64_t roots[6];
    uint32_t indexIds[6];
    uint32_t indexTableIds[6];
    uint32_t indexColumnOrdinals[6];
    uint16_t indexFormatVersions[6];
    uint32_t ownerForeignKeyIds[6];
    uint64_t indexEntryCounts[6];
    uint16_t rootTypes[6];
    uint32_t tableId;
    Fixture() : path(), pageSize(512), pageCount(0), catalogRootPageId(0),
                formatMajor(0), formatMinor(0), catalogVersion(0),
                creationTimeUnixNanos(0), databaseId(), roots(), indexIds(),
                indexTableIds(), indexColumnOrdinals(), indexFormatVersions(),
                ownerForeignKeyIds(), indexEntryCounts(), rootTypes(), tableId(0) {}
};

#if defined(GX_R3AG_QUALIFICATION)
void testFixture(const Fixture& fixture, uint32_t rowCount);
void testBoundaryIndexes(const Fixture& fixture);
void testIdentityAndModes(const Fixture& fixture);
void testRootOwnerAssociation(const Fixture& fixture);
void testVisitorExitReleasesBorrow(const Fixture& fixture);
void testProviderOpenFailures(const Fixture& fixture);
void testUnexpectedPageType(const Fixture& fixture);
void testTruncatedAndInvalidHandle(const Fixture& fixture);
void testProviderFailureAfterVisitor(const Fixture& fixture);
void testCorruptionCopies(const Fixture& leaf, const Fixture& multilevel);
#endif

std::vector<DbValue> makeRow(int64_t id) {
    std::vector<DbValue> values;
    values.push_back(DbValue::int64(id));
    values.push_back(id % 13 == 0 ? DbValue::null() : DbValue::int64(7));
    values.push_back(id % 5 == 0 ? DbValue::null()
                                 : DbValue::int32(static_cast<int32_t>(id % 4)));
    values.push_back(id % 7 == 0 ? DbValue::null()
                                 : DbValue::boolean(id % 2 == 0));
    if (id % 11 == 0) values.push_back(DbValue::null());
    else if (id % 3 == 0) values.push_back(DbValue::text(std::string("a\0b", 3)));
    else values.push_back(DbValue::text("other"));
    values.push_back(id == 0 ? DbValue::int64(INT64_MIN) :
                     id == 79 ? DbValue::int64(INT64_MAX) :
                     DbValue::int64(id - 40));
    values.push_back(id == 0 ? DbValue::int32(INT32_MIN) :
                     id == 1 ? DbValue::int32(INT32_MAX) :
                     DbValue::int32(static_cast<int32_t>(id) - 40));
    return values;
}

bool createFixture(uint32_t rowCount, const char* suffix, Fixture& out) {
    out.path = uniquePath(suffix);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    std::unique_ptr<Database> database;
    if (!Database::create(out.path, createOptions, database).isOk() || !database) return false;

    TableDefinition tableDefinition("Rows");
    tableDefinition.columns.push_back(ColumnDefinition("Id", DbType::Int64, false));
    tableDefinition.columns.push_back(ColumnDefinition("K64", DbType::Int64, true));
    tableDefinition.columns.push_back(ColumnDefinition("K32", DbType::Int32, true));
    tableDefinition.columns.push_back(ColumnDefinition("KBool", DbType::Boolean, true));
    tableDefinition.columns.push_back(ColumnDefinition("KText", DbType::Text, true));
    tableDefinition.columns.push_back(ColumnDefinition("Edge64", DbType::Int64, false));
    tableDefinition.columns.push_back(ColumnDefinition("Edge32", DbType::Int32, false));
    if (!database->createTable(tableDefinition, out.tableId).isOk()) return false;

    const char* names[] = {"IX_Rows_K64", "IX_Rows_K32", "IX_Rows_KBool",
                           "IX_Rows_KText", "IX_Rows_Edge64", "IX_Rows_Edge32"};
    const uint32_t ordinals[] = {1, 2, 3, 4, 5, 6};
    for (size_t i = 0; i < 6; ++i) {
        IndexDefinition definition(names[i], out.tableId, ordinals[i], IndexKind::Ordinary);
        if (!database->createIndex(definition, out.indexIds[i]).isOk()) return false;
    }

    std::unique_ptr<Table> table;
    if (!database->openTable("Rows", table).isOk() || !table) return false;
    for (uint32_t i = 0; i < rowCount; ++i) {
        if (!table->insert(makeRow(static_cast<int64_t>(i))).isOk()) return false;
    }

    DatabaseReadView view;
    if (!database->acquireReadView(view).isOk()) return false;
    out.pageSize = view.pageSize();
    out.pageCount = view.pageCount();
    out.catalogRootPageId = database->file().rootPageId();
    out.formatMajor = database->file().header().formatMajor;
    out.formatMinor = database->file().header().formatMinor;
    out.creationTimeUnixNanos = database->file().header().creationTimeUnixNanos;
    std::memcpy(out.databaseId, database->file().header().databaseId,
                sizeof(out.databaseId));
    out.catalogVersion = database->catalog().version();
    bool rootsOk = true;
    for (size_t i = 0; i < 6; ++i) {
        DbType type = DbType::Unknown;
        if (!view.indexRoot(out.indexIds[i], out.roots[i], type).isOk()) rootsOk = false;
        const Catalog::IndexRecord* record = database->catalog().findIndex(out.indexIds[i]);
        if (!record) {
            rootsOk = false;
        } else {
            out.indexTableIds[i] = record->tableId;
            out.indexColumnOrdinals[i] = record->columnOrdinal;
            out.indexFormatVersions[i] = record->formatVersion;
            out.ownerForeignKeyIds[i] = record->ownerForeignKeyId;
            out.indexEntryCounts[i] = record->entryCount;
        }
        if (out.roots[i] != 0) {
            DatabasePage rootPage;
            if (!view.readPage(out.roots[i], rootPage).isOk()) rootsOk = false;
            else out.rootTypes[i] = static_cast<uint16_t>(rootPage.type);
        }
    }
    view.release();
    if (!rootsOk) return false;
    return database->close().isOk();
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
}

#if defined(GX_R3AG_QUALIFICATION)
extern "C" int gxR3agSha256File(const char* path, char* outHex65);

std::string sha256File(const std::string& path) {
    char digest[65] = {};
    if (!gxR3agSha256File(path.c_str(), digest)) return std::string();
    return std::string(digest);
}
#endif

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

typedef void (*PayloadMutation)(uint8_t*, size_t, uint64_t);

bool mutatePage(const std::string& path, uint32_t pageSize, uint64_t pageId,
                PayloadMutation mutation, uint64_t value) {
    std::vector<uint8_t> image = readBytes(path);
    if (pageSize == 0 || pageId > UINT64_MAX / pageSize) return false;
    const uint64_t offset64 = pageId * pageSize;
    if (offset64 > image.size() || pageSize > image.size() - offset64) return false;
    const size_t offset = static_cast<size_t>(offset64);
    GxDatabasePageInfo info;
    if (gxDatabasePageInspect(image.data() + offset, pageSize, pageSize,
                              pageId, &info) != GxStatus::Ok) return false;
    const size_t payloadLength = pageSize - kPageHeaderSize;
    std::vector<uint8_t> payload(
        image.begin() + offset + kPageHeaderSize,
        image.begin() + offset + pageSize);
    mutation(payload.data(), payloadLength, value);
    if (gxDatabasePageEncode(image.data() + offset, pageSize, pageId,
            info.pageType, info.flags, info.generation,
            payload.data(), payloadLength) != GxStatus::Ok) return false;
    return writeBytes(path, image);
}

#if defined(GX_R3AG_QUALIFICATION)
bool mutatePageType(const std::string& path, uint32_t pageSize, uint64_t pageId,
                    uint16_t newPageType) {
    std::vector<uint8_t> image = readBytes(path);
    if (pageSize == 0 || pageId > UINT64_MAX / pageSize) return false;
    const uint64_t offset64 = pageId * pageSize;
    if (offset64 > image.size() || pageSize > image.size() - offset64) return false;
    const size_t offset = static_cast<size_t>(offset64);
    GxDatabasePageInfo info;
    if (gxDatabasePageInspect(image.data() + offset, pageSize, pageSize,
                              pageId, &info) != GxStatus::Ok) return false;
    const size_t payloadLength = pageSize - kPageHeaderSize;
    std::vector<uint8_t> payload(
        image.begin() + offset + kPageHeaderSize,
        image.begin() + offset + pageSize);
    if (gxDatabasePageEncode(image.data() + offset, pageSize, pageId,
            newPageType, info.flags, info.generation,
            payload.data(), payloadLength) != GxStatus::Ok) return false;
    return writeBytes(path, image);
}

bool mutateHeaderFormatMajor(const std::string& path, uint16_t formatMajor) {
    std::vector<uint8_t> image = readBytes(path);
    if (image.size() < kHeaderSize) return false;
    const uint8_t* bytes = image.data();
    DatabaseHeader header;
    header.formatMajor = formatMajor;
    header.formatMinor = loadLe16(bytes + header_offset::FormatMinor);
    header.pageSize = loadLe32(bytes + header_offset::PageSize);
    std::memcpy(header.databaseId, bytes + header_offset::DatabaseId,
                sizeof(header.databaseId));
    header.creationTimeUnixNanos = loadLe64(bytes + header_offset::CreationTime);
    header.pageCount = loadLe64(bytes + header_offset::PageCount);
    header.rootPageId = loadLe64(bytes + header_offset::RootPageId);
    header.flags = loadLe32(bytes + header_offset::Flags);
    header.headerSize = loadLe32(bytes + header_offset::HeaderSize);
    storeLe32(image.data() + header_offset::HeaderCrc32, header.computeCrc());
    return writeBytes(path, image);
}
#endif

void corruptInternalSignature(uint8_t* payload, size_t length, uint64_t) {
    if (length) payload[0] = 0;
}

void corruptLeafEntry(uint8_t* payload, size_t length, uint64_t) {
    if (length >= 8) gxos::db::storeLe32(payload + length - 8, static_cast<uint32_t>(length));
}

void corruptLeafSuccessor(uint8_t* payload, size_t length, uint64_t pageCount) {
    if (length >= 16) gxos::db::storeLe64(payload + 8, pageCount);
}

struct ScanRow {
    RowLocator locator;
    std::vector<DbValue> values;
};

bool loadHostedRows(const Fixture& fixture, std::unique_ptr<Database>& database,
                    std::vector<ScanRow>& rows) {
    DatabaseOpenOptions options;
    options.readOnly = true;
    if (!Database::open(fixture.path, options, database).isOk() || !database) return false;
    std::unique_ptr<Table> table;
    if (!database->openTable("Rows", table).isOk() || !table) return false;
    std::unique_ptr<TableScan> scan;
    if (!table->scanStart(scan).isOk() || !scan) return false;
    std::vector<DbValue> values;
    while (scan->next(values)) {
        ScanRow row;
        row.locator = RowLocator(scan->currentPageId(), scan->currentSlotIndex());
        row.values = values;
        rows.push_back(row);
    }
    return scan->status().isOk();
}

bool sameLocators(const std::vector<RowLocator>& left,
                  const std::vector<RowLocator>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].pageId != right[i].pageId || left[i].slot != right[i].slot) return false;
    }
    return true;
}

int compareBytes(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right) {
    const size_t shared = left.size() < right.size() ? left.size() : right.size();
    for (size_t i = 0; i < shared; ++i) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    return left.size() < right.size() ? -1 : (left.size() > right.size() ? 1 : 0);
}

bool valueInRange(const DbValue& value, const DbValue* lower, bool lowerInclusive,
                  const DbValue* upper, bool upperInclusive, DbType type) {
    std::vector<uint8_t> encoded;
    if (!encodeIndexKey(type, value, encoded)) return false;
    if (lower) {
        std::vector<uint8_t> bound;
        if (!encodeIndexKey(type, *lower, bound)) return false;
        const int cmp = compareBytes(encoded, bound);
        if (cmp < 0 || (cmp == 0 && !lowerInclusive)) return false;
    }
    if (upper) {
        std::vector<uint8_t> bound;
        if (!encodeIndexKey(type, *upper, bound)) return false;
        const int cmp = compareBytes(encoded, bound);
        if (cmp > 0 || (cmp == 0 && !upperInclusive)) return false;
    }
    return true;
}

std::vector<RowLocator> expectedPoint(const std::vector<ScanRow>& rows,
                                      size_t ordinal, DbType type,
                                      const DbValue& key) {
    std::vector<uint8_t> target;
    encodeIndexKey(type, key, target);
    std::vector<RowLocator> result;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<uint8_t> encoded;
        encodeIndexKey(type, rows[i].values[ordinal], encoded);
        if (encoded == target) result.push_back(rows[i].locator);
    }
    return result;
}

std::vector<RowLocator> expectedRange(const std::vector<ScanRow>& rows,
                                      size_t ordinal, DbType type,
                                      const DbValue* lower, bool lowerInclusive,
                                      const DbValue* upper, bool upperInclusive) {
    struct OrderedLocator { std::vector<uint8_t> key; RowLocator locator; };
    std::vector<OrderedLocator> ordered;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!valueInRange(rows[i].values[ordinal], lower, lowerInclusive,
                          upper, upperInclusive, type)) continue;
        OrderedLocator item;
        encodeIndexKey(type, rows[i].values[ordinal], item.key);
        item.locator = rows[i].locator;
        ordered.push_back(item);
    }
    std::sort(ordered.begin(), ordered.end(), [](const OrderedLocator& a,
                                                  const OrderedLocator& b) {
        const int cmp = compareBytes(a.key, b.key);
        if (cmp != 0) return cmp < 0;
        if (a.locator.pageId != b.locator.pageId) return a.locator.pageId < b.locator.pageId;
        return a.locator.slot < b.locator.slot;
    });
    std::vector<RowLocator> result;
    for (size_t i = 0; i < ordered.size(); ++i) result.push_back(ordered[i].locator);
    return result;
}

struct FixtureQuerySpec {
    std::string name;
    size_t image;
    size_t index;
    size_t ordinal;
    bool point;
    DbType type;
    DbValue key;
    bool hasLower;
    DbValue lower;
    bool lowerInclusive;
    bool hasUpper;
    DbValue upper;
    bool upperInclusive;
};

FixtureQuerySpec makePointSpec(const char* name, size_t image, size_t index,
                               size_t ordinal, DbType type, const DbValue& key) {
    FixtureQuerySpec spec = {};
    spec.name = name; spec.image = image; spec.index = index; spec.ordinal = ordinal;
    spec.point = true; spec.type = type; spec.key = key;
    spec.lowerInclusive = true; spec.upperInclusive = true;
    return spec;
}

FixtureQuerySpec makeRangeSpec(const char* name, size_t image, size_t index,
                               size_t ordinal, DbType type,
                               bool hasLower, const DbValue& lower, bool lowerInclusive,
                               bool hasUpper, const DbValue& upper, bool upperInclusive) {
    FixtureQuerySpec spec = {};
    spec.name = name; spec.image = image; spec.index = index; spec.ordinal = ordinal;
    spec.point = false; spec.type = type; spec.hasLower = hasLower; spec.lower = lower;
    spec.lowerInclusive = lowerInclusive; spec.hasUpper = hasUpper; spec.upper = upper;
    spec.upperInclusive = upperInclusive;
    return spec;
}

std::vector<FixtureQuerySpec> fixtureQuerySpecs() {
    std::vector<FixtureQuerySpec> specs;
    specs.push_back(makePointSpec("leaf_k64_7", 0, 0, 1, DbType::Int64,
                                  DbValue::int64(7)));
    specs.push_back(makePointSpec("duplicate_k64_7", 1, 0, 1, DbType::Int64,
                                  DbValue::int64(7)));
    specs.push_back(makePointSpec("missing_k64_999", 1, 0, 1, DbType::Int64,
                                  DbValue::int64(999)));
    specs.push_back(makePointSpec("null_k64", 1, 0, 1, DbType::Int64,
                                  DbValue::null()));
    specs.push_back(makePointSpec("boolean_true", 1, 2, 3, DbType::Boolean,
                                  DbValue::boolean(true)));
    specs.push_back(makePointSpec("text_embedded_zero", 1, 3, 4, DbType::Text,
                                  DbValue::text(std::string("a\0b", 3))));
    specs.push_back(makePointSpec("int64_minimum", 1, 4, 5, DbType::Int64,
                                  DbValue::int64(INT64_MIN)));
    specs.push_back(makePointSpec("int64_maximum", 1, 4, 5, DbType::Int64,
                                  DbValue::int64(INT64_MAX)));
    specs.push_back(makePointSpec("int32_minimum", 1, 5, 6, DbType::Int32,
                                  DbValue::int32(INT32_MIN)));
    specs.push_back(makePointSpec("int32_maximum", 1, 5, 6, DbType::Int32,
                                  DbValue::int32(INT32_MAX)));
    specs.push_back(makeRangeSpec("range32_inclusive_1_2", 1, 1, 2, DbType::Int32,
        true, DbValue::int32(1), true, true, DbValue::int32(2), true));
    specs.push_back(makeRangeSpec("range32_exclusive_1_2", 1, 1, 2, DbType::Int32,
        true, DbValue::int32(1), false, true, DbValue::int32(2), false));
    specs.push_back(makeRangeSpec("range32_lower_only", 1, 1, 2, DbType::Int32,
        true, DbValue::int32(1), true, false, DbValue::null(), true));
    specs.push_back(makeRangeSpec("range32_upper_only", 1, 1, 2, DbType::Int32,
        false, DbValue::null(), true, true, DbValue::int32(2), true));
    specs.push_back(makeRangeSpec("range32_unbounded", 1, 1, 2, DbType::Int32,
        false, DbValue::null(), true, false, DbValue::null(), true));
    specs.push_back(makeRangeSpec("range64_full_bounds", 1, 4, 5, DbType::Int64,
        true, DbValue::int64(INT64_MIN), true,
        true, DbValue::int64(INT64_MAX), true));
    specs.push_back(makeRangeSpec("range64_unbounded", 1, 4, 5, DbType::Int64,
        false, DbValue::null(), true, false, DbValue::null(), true));
    return specs;
}

std::string typeName(DbType type) {
    switch (type) {
    case DbType::Boolean: return "Boolean";
    case DbType::Int32: return "Int32";
    case DbType::Int64: return "Int64";
    case DbType::Text: return "Text";
    default: return "Unknown";
    }
}

std::string valueText(const DbValue& value) {
    if (value.isNull()) return "NULL";
    if (value.type() == DbType::Boolean) return value.booleanValue() ? "true" : "false";
    if (value.type() == DbType::Int32) return std::to_string(value.int32Value());
    if (value.type() == DbType::Int64) return std::to_string(value.int64Value());
    if (value.type() == DbType::Text) {
        static const char hex[] = "0123456789abcdef";
        std::string encoded;
        for (size_t i = 0; i < value.textValue().size(); ++i) {
            const uint8_t byte = static_cast<uint8_t>(value.textValue()[i]);
            encoded.push_back(hex[byte >> 4]);
            encoded.push_back(hex[byte & 0x0f]);
        }
        return encoded;
    }
    return "-";
}

std::string locatorText(const std::vector<RowLocator>& locators) {
    if (locators.empty()) return "-";
    std::ostringstream output;
    for (size_t i = 0; i < locators.size(); ++i) {
        if (i) output << ',';
        output << locators[i].pageId << ':' << locators[i].slot;
    }
    return output.str();
}

struct LocatorCollector { std::vector<RowLocator> locators; };

GxIndexLookupVisitControl collectPoint(void* context, uint64_t pageId,
                                       uint32_t slot) {
    if (g_scheduleShortReadAfterPointCallback) {
        g_shortReadAt = g_readCount + 1;
        g_scheduleShortReadAfterPointCallback = false;
    }
    static_cast<LocatorCollector*>(context)->locators.push_back(RowLocator(pageId, slot));
    return GxIndexLookupVisitControl::Continue;
}

GxIndexRangeScanVisitControl collectRange(void* context, const uint8_t*, size_t,
                                          uint64_t pageId, uint32_t slot) {
    static_cast<LocatorCollector*>(context)->locators.push_back(RowLocator(pageId, slot));
    return GxIndexRangeScanVisitControl::Continue;
}

GxIndexLookupStatus runLookup(const Fixture& fixture,
                              GxNativeSqlIndexPageProviderContext& context,
                              const std::vector<uint8_t>& key,
                              LocatorCollector& collector,
                              uint64_t maxLeafPages = UINT64_MAX) {
    const GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(&context);
    uint64_t visited[kMaxIndexDepth + 1] = {};
    std::vector<uint8_t> lower(key.size() + 12, 0);
    std::copy(key.begin(), key.end(), lower.begin());
    std::vector<uint8_t> previous(fixture.pageSize - kPageHeaderSize);
    GxIndexLookupResult result = {};
    return gxIndexLookupAll(&provider, context.indexRootPageId,
        context.pageCount, context.ownerIndexId, key.data(), key.size(),
        kMaxIndexDepth, visited, kMaxIndexDepth + 1,
        maxLeafPages == UINT64_MAX ? context.pageCount - 1 : maxLeafPages,
        lower.data(), lower.size(), previous.data(), previous.size(),
        collectPoint, &collector, &result);
}

GxIndexLookupStatus runRange(const Fixture& fixture,
                             GxNativeSqlIndexPageProviderContext& context,
                             const std::vector<uint8_t>* lowerKey,
                             bool lowerInclusive,
                             const std::vector<uint8_t>* upperKey,
                             bool upperInclusive,
                             LocatorCollector& collector,
                             uint64_t maxLeafPages = UINT64_MAX) {
    const GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(&context);
    uint64_t visited[kMaxIndexDepth + 1] = {};
    size_t probeBytes = 12;
    if (lowerKey && lowerKey->size() + 12 > probeBytes) probeBytes = lowerKey->size() + 12;
    if (!lowerKey && upperKey && upperKey->size() + 12 > probeBytes)
        probeBytes = upperKey->size() + 12;
    std::vector<uint8_t> lower(probeBytes, 0);
    std::vector<uint8_t> previous(fixture.pageSize - kPageHeaderSize);
    GxIndexLookupResult result = {};
    return gxIndexRangeScan(&provider, context.indexRootPageId, context.pageCount,
        context.ownerIndexId, lowerKey != nullptr,
        lowerKey ? lowerKey->data() : nullptr, lowerKey ? lowerKey->size() : 0,
        lowerInclusive, upperKey != nullptr,
        upperKey ? upperKey->data() : nullptr, upperKey ? upperKey->size() : 0,
        upperInclusive, kMaxIndexDepth, visited, kMaxIndexDepth + 1,
        maxLeafPages == UINT64_MAX ? context.pageCount - 1 : maxLeafPages,
        lower.data(), lower.size(), previous.data(), previous.size(),
        collectRange, &collector, &result);
}

bool openProvider(const Fixture& fixture, size_t index,
                  std::vector<uint8_t>& buffer,
                  GxNativeSqlIndexPageProviderContext& context) {
    g_imagePath = fixture.path;
    g_bridgePath = kBridgeDatabasePath;
    gxNativeSqlIndexPageProviderInit(&context);
    buffer.resize(fixture.pageSize);
    GxNativeSqlIndexImageIdentity identity = {};
    identity.pageSize = fixture.pageSize;
    identity.pageCount = fixture.pageCount;
    identity.catalogRootPageId = fixture.catalogRootPageId;
    identity.indexRootPageId = fixture.roots[index];
    identity.ownerIndexId = fixture.indexIds[index];
    identity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;
    return gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_OK;
}

#if defined(GX_R3AG_QUALIFICATION)
std::string joinPath(const std::string& directory, const char* file) {
    if (directory.empty()) return std::string(file);
    const char last = directory[directory.size() - 1];
    return directory + ((last == '\\' || last == '/') ? "" : "\\") + file;
}

bool fileExists(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return input.good();
}

bool writeManifestField(std::ofstream& output, const std::string& key,
                        const std::string& value) {
    if (key.empty() || value.find('\n') != std::string::npos ||
        value.find('\r') != std::string::npos) return false;
    output << key << '=' << value << '\n';
    return output.good();
}

std::string indexName(size_t index) {
    static const char* const names[] = {
        "IX_Rows_K64", "IX_Rows_K32", "IX_Rows_KBool", "IX_Rows_KText",
        "IX_Rows_Edge64", "IX_Rows_Edge32"
    };
    return index < 6 ? names[index] : std::string();
}

bool writeImageManifest(std::ofstream& output, size_t imageIndex,
                        const char* filename, const Fixture& fixture,
                        uint32_t rowCount) {
    const std::string prefix = "image." + std::to_string(imageIndex) + ".";
    const std::vector<uint8_t> bytes = readBytes(fixture.path);
    const std::string digest = sha256File(fixture.path);
    if (bytes.empty() || digest.size() != 64 || !walSidecarAbsent(fixture.path)) return false;
    DatabaseHeader header;
    if (!DatabaseHeader::parse(bytes.data(), bytes.size(), header).isOk()) return false;
    bool ok = true;
    ok = ok && writeManifestField(output, prefix + "file", filename);
    ok = ok && writeManifestField(output, prefix + "file_length", std::to_string(bytes.size()));
    ok = ok && writeManifestField(output, prefix + "sha256", digest);
    ok = ok && writeManifestField(output, prefix + "format_major", std::to_string(header.formatMajor));
    ok = ok && writeManifestField(output, prefix + "format_minor", std::to_string(header.formatMinor));
    ok = ok && writeManifestField(output, prefix + "database_id", formatDatabaseId(header.databaseId));
    ok = ok && writeManifestField(output, prefix + "creation_time_unix_nanos",
                                  std::to_string(header.creationTimeUnixNanos));
    ok = ok && writeManifestField(output, prefix + "page_size", std::to_string(fixture.pageSize));
    ok = ok && writeManifestField(output, prefix + "page_count", std::to_string(fixture.pageCount));
    ok = ok && writeManifestField(output, prefix + "catalog_root_page_id",
                                  std::to_string(fixture.catalogRootPageId));
    ok = ok && writeManifestField(output, prefix + "catalog_version",
                                  std::to_string(fixture.catalogVersion));
    ok = ok && writeManifestField(output, prefix + "table_id", std::to_string(fixture.tableId));
    ok = ok && writeManifestField(output, prefix + "row_count", std::to_string(rowCount));
    ok = ok && writeManifestField(output, prefix + "writer_closed", "true");
    ok = ok && writeManifestField(output, prefix + "wal_sidecar_absent", "true");
    ok = ok && writeManifestField(output, prefix + "index_count", "6");
    for (size_t index = 0; index < 6; ++index) {
        const std::string item = prefix + "index." + std::to_string(index) + ".";
        ok = ok && writeManifestField(output, item + "name", indexName(index));
        ok = ok && writeManifestField(output, item + "id", std::to_string(fixture.indexIds[index]));
        ok = ok && writeManifestField(output, item + "table_id",
                                      std::to_string(fixture.indexTableIds[index]));
        ok = ok && writeManifestField(output, item + "column_ordinal",
                                      std::to_string(fixture.indexColumnOrdinals[index]));
        ok = ok && writeManifestField(output, item + "root_page_id",
                                      std::to_string(fixture.roots[index]));
        ok = ok && writeManifestField(output, item + "owner_index_id",
                                      std::to_string(fixture.indexIds[index]));
        ok = ok && writeManifestField(output, item + "owner_foreign_key_id",
                                      std::to_string(fixture.ownerForeignKeyIds[index]));
        ok = ok && writeManifestField(output, item + "format_version",
                                      std::to_string(fixture.indexFormatVersions[index]));
        ok = ok && writeManifestField(output, item + "entry_count",
                                      std::to_string(fixture.indexEntryCounts[index]));
        const char* rootType = fixture.rootTypes[index] ==
            static_cast<uint16_t>(PageType::IndexLeaf) ? "leaf" :
            fixture.rootTypes[index] == static_cast<uint16_t>(PageType::IndexInternal)
                ? "internal" : "unknown";
        ok = ok && writeManifestField(output, item + "root_page_type", rootType);
    }
    return ok;
}

bool writeFixtureManifest(const std::string& directory,
                          const std::string& sqlWriterCommit,
                          const std::string& serverCommit,
                          const Fixture& leaf, const Fixture& multilevel) {
    const std::string path = joinPath(directory, "fixture.manifest");
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << "GUIDEXOS_SQL_FIXTURE_MANIFEST_V1\n";
    bool ok = true;
    ok = ok && writeManifestField(output, "fixture_id", "NDB1R4P11R3AH_NATIVE_SQL_READ");
    ok = ok && writeManifestField(output, "sql_writer_source_commit", sqlWriterCommit);
    ok = ok && writeManifestField(output, "server_baseline_commit", serverCommit);
    ok = ok && writeManifestField(output, "creation_method",
        "production_Database_create_createTable_createIndex_Table_insert_close");
    ok = ok && writeManifestField(output, "recovery_required", "false");
    ok = ok && writeManifestField(output, "image_count", "2");
    ok = ok && writeManifestField(output, "query_count",
        std::to_string(fixtureQuerySpecs().size()));
    ok = ok && writeImageManifest(output, 0, "leaf-only.gxdb", leaf, 2);
    ok = ok && writeImageManifest(output, 1, "multilevel.gxdb", multilevel, 80);

    const Fixture* images[] = {&leaf, &multilevel};
    const std::vector<FixtureQuerySpec> queries = fixtureQuerySpecs();
    for (size_t i = 0; i < queries.size() && ok; ++i) {
        const FixtureQuerySpec& query = queries[i];
        std::unique_ptr<Database> hosted;
        std::vector<ScanRow> rows;
        if (!loadHostedRows(*images[query.image], hosted, rows)) return false;
        const std::vector<RowLocator> expected = query.point
            ? expectedPoint(rows, query.ordinal, query.type, query.key)
            : expectedRange(rows, query.ordinal, query.type,
                query.hasLower ? &query.lower : nullptr, query.lowerInclusive,
                query.hasUpper ? &query.upper : nullptr, query.upperInclusive);
        const std::string prefix = "query." + std::to_string(i) + ".";
        ok = ok && writeManifestField(output, prefix + "name", query.name);
        ok = ok && writeManifestField(output, prefix + "image", std::to_string(query.image));
        ok = ok && writeManifestField(output, prefix + "index", std::to_string(query.index));
        ok = ok && writeManifestField(output, prefix + "operation", query.point ? "point" : "range");
        ok = ok && writeManifestField(output, prefix + "key_type", typeName(query.type));
        ok = ok && writeManifestField(output, prefix + "key",
                                      query.point ? valueText(query.key) : "-");
        ok = ok && writeManifestField(output, prefix + "lower",
                                      query.hasLower ? valueText(query.lower) : "-");
        ok = ok && writeManifestField(output, prefix + "lower_inclusive",
                                      query.lowerInclusive ? "true" : "false");
        ok = ok && writeManifestField(output, prefix + "upper",
                                      query.hasUpper ? valueText(query.upper) : "-");
        ok = ok && writeManifestField(output, prefix + "upper_inclusive",
                                      query.upperInclusive ? "true" : "false");
        ok = ok && writeManifestField(output, prefix + "expected_count",
                                      std::to_string(expected.size()));
        ok = ok && writeManifestField(output, prefix + "expected_ordered_locators",
                                      locatorText(expected));
        if (!hosted->close().isOk()) ok = false;
    }
    output.flush();
    return ok && output.good();
}

int packageQualificationFixture(const char* directory,
                                const char* sqlWriterCommit,
                                const char* serverCommit) {
    if (!directory || !sqlWriterCommit || !serverCommit ||
        std::string(sqlWriterCommit).empty() || std::string(serverCommit).empty()) return 2;
    const std::string outDir(directory);
    const std::string leafOut = joinPath(outDir, "leaf-only.gxdb");
    const std::string multiOut = joinPath(outDir, "multilevel.gxdb");
    const std::string manifestOut = joinPath(outDir, "fixture.manifest");
    if (fileExists(leafOut) || fileExists(multiOut) || fileExists(manifestOut)) {
        std::fprintf(stderr, "refusing to overwrite an existing fixture package\n");
        return 2;
    }
#if defined(_WIN32)
    _mkdir(outDir.c_str());
#else
    mkdir(outDir.c_str(), 0700);
#endif
    Fixture leaf, multilevel;
    if (!createFixture(2, "r3ah_leaf", leaf) ||
        !createFixture(80, "r3ah_multilevel", multilevel)) {
        removeDatabaseFiles(leaf.path);
        removeDatabaseFiles(multilevel.path);
        return 1;
    }
    const std::vector<uint8_t> leafBytes = readBytes(leaf.path);
    const std::vector<uint8_t> multiBytes = readBytes(multilevel.path);
    const std::string leafTemp = leaf.path;
    const std::string multiTemp = multilevel.path;
    bool ok = !leafBytes.empty() && !multiBytes.empty() &&
        writeBytes(leafOut, leafBytes) && writeBytes(multiOut, multiBytes);
    leaf.path = leafOut;
    multilevel.path = multiOut;
    ok = ok && writeFixtureManifest(outDir, sqlWriterCommit, serverCommit,
                                    leaf, multilevel);
    removeDatabaseFiles(leafTemp);
    removeDatabaseFiles(multiTemp);
    if (!ok) return 1;
    std::printf("Fixture package written: %s\n", outDir.c_str());
    return 0;
}

bool parseManifestText(const std::string& text, std::map<std::string, std::string>& fields) {
    if (text.empty() || text.size() > 1024 * 1024) return false;
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || line != "GUIDEXOS_SQL_FIXTURE_MANIFEST_V1") return false;
    size_t lineCount = 1;
    while (std::getline(input, line)) {
        if (++lineCount > 4096 || line.empty() || line.find('\r') != std::string::npos) return false;
        const size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0 || separator + 1 == line.size()) return false;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        for (size_t i = 0; i < key.size(); ++i) {
            const char c = key[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
        }
        if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos ||
            fields.find(key) != fields.end()) return false;
        fields[key] = value;
    }
    return input.eof();
}

bool parseUnsigned(const std::string& text, uint64_t& out) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool getNumber(const std::map<std::string, std::string>& fields,
               const std::string& key, uint64_t& out) {
    std::map<std::string, std::string>::const_iterator found = fields.find(key);
    return found != fields.end() && parseUnsigned(found->second, out);
}

bool manifestSchema(const std::map<std::string, std::string>& fields) {
    std::set<std::string> required;
    const char* globals[] = {"fixture_id", "sql_writer_source_commit", "server_baseline_commit",
        "creation_method", "recovery_required", "image_count", "query_count"};
    for (size_t i = 0; i < sizeof(globals) / sizeof(globals[0]); ++i) required.insert(globals[i]);
    uint64_t imageCount = 0, queryCount = 0;
    if (!getNumber(fields, "image_count", imageCount) || imageCount != 2 ||
        !getNumber(fields, "query_count", queryCount) ||
        queryCount != fixtureQuerySpecs().size()) return false;
    const char* imageFields[] = {"file", "file_length", "sha256", "format_major",
        "format_minor", "database_id", "creation_time_unix_nanos", "page_size",
        "page_count", "catalog_root_page_id", "catalog_version", "table_id",
        "row_count", "writer_closed", "wal_sidecar_absent", "index_count"};
    const char* indexFields[] = {"name", "id", "table_id", "column_ordinal", "root_page_id",
        "owner_index_id", "owner_foreign_key_id", "format_version", "entry_count",
        "root_page_type"};
    for (size_t image = 0; image < 2; ++image) {
        const std::string prefix = "image." + std::to_string(image) + ".";
        for (size_t i = 0; i < sizeof(imageFields) / sizeof(imageFields[0]); ++i)
            required.insert(prefix + imageFields[i]);
        uint64_t indexCount = 0;
        if (!getNumber(fields, prefix + "index_count", indexCount) || indexCount != 6) return false;
        for (size_t index = 0; index < 6; ++index) {
            const std::string item = prefix + "index." + std::to_string(index) + ".";
            for (size_t i = 0; i < sizeof(indexFields) / sizeof(indexFields[0]); ++i)
                required.insert(item + indexFields[i]);
        }
    }
    const char* queryFields[] = {"name", "image", "index", "operation", "key_type", "key",
        "lower", "lower_inclusive", "upper", "upper_inclusive", "expected_count",
        "expected_ordered_locators"};
    for (size_t query = 0; query < queryCount; ++query) {
        const std::string prefix = "query." + std::to_string(query) + ".";
        for (size_t i = 0; i < sizeof(queryFields) / sizeof(queryFields[0]); ++i)
            required.insert(prefix + queryFields[i]);
    }
    if (required.size() != fields.size()) return false;
    for (std::set<std::string>::const_iterator key = required.begin(); key != required.end(); ++key)
        if (fields.find(*key) == fields.end()) return false;
    return fields.find("fixture_id")->second == "NDB1R4P11R3AH_NATIVE_SQL_READ" &&
        fields.find("recovery_required")->second == "false" &&
        fields.find("creation_method")->second ==
            "production_Database_create_createTable_createIndex_Table_insert_close";
}

bool parseLocatorList(const std::string& text, std::vector<RowLocator>& out) {
    out.clear();
    if (text == "-") return true;
    if (text.empty()) return false;
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = text.find(',', start);
        const size_t end = comma == std::string::npos ? text.size() : comma;
        const size_t colon = text.find(':', start);
        if (colon == std::string::npos || colon >= end) return false;
        uint64_t pageId = 0, slot = 0;
        if (!parseUnsigned(text.substr(start, colon - start), pageId) ||
            !parseUnsigned(text.substr(colon + 1, end - colon - 1), slot) ||
            slot > UINT32_MAX) return false;
        out.push_back(RowLocator(pageId, static_cast<uint32_t>(slot)));
        if (comma == std::string::npos) break;
        start = comma + 1;
        if (start == text.size()) return false;
    }
    return true;
}

bool rootTypeMatches(const std::string& text, uint16_t type) {
    return (text == "leaf" && type == static_cast<uint16_t>(PageType::IndexLeaf)) ||
           (text == "internal" && type == static_cast<uint16_t>(PageType::IndexInternal));
}

bool loadAndValidateImage(const std::string& directory,
                          const std::map<std::string, std::string>& fields,
                          size_t imageNumber, Fixture& fixture,
                          std::vector<ScanRow>& rows) {
    const std::string prefix = "image." + std::to_string(imageNumber) + ".";
    const std::string filename = imageNumber == 0 ? "leaf-only.gxdb" : "multilevel.gxdb";
    if (fields.find(prefix + "file")->second != filename ||
        fields.find(prefix + "writer_closed")->second != "true" ||
        fields.find(prefix + "wal_sidecar_absent")->second != "true") return false;
    fixture.path = joinPath(directory, filename.c_str());
    if (!walSidecarAbsent(fixture.path)) return false;
    const std::vector<uint8_t> bytes = readBytes(fixture.path);
    if (bytes.empty() || sha256File(fixture.path) != fields.find(prefix + "sha256")->second) return false;
    uint64_t fileLength = 0, formatMajor = 0, formatMinor = 0, pageSize = 0,
        pageCount = 0, catalogRoot = 0, catalogVersion = 0, creationTime = 0,
        tableId = 0, rowCount = 0;
    if (!getNumber(fields, prefix + "file_length", fileLength) || fileLength != bytes.size() ||
        !getNumber(fields, prefix + "format_major", formatMajor) ||
        !getNumber(fields, prefix + "format_minor", formatMinor) ||
        !getNumber(fields, prefix + "page_size", pageSize) ||
        !getNumber(fields, prefix + "page_count", pageCount) ||
        !getNumber(fields, prefix + "catalog_root_page_id", catalogRoot) ||
        !getNumber(fields, prefix + "catalog_version", catalogVersion) ||
        !getNumber(fields, prefix + "creation_time_unix_nanos", creationTime) ||
        !getNumber(fields, prefix + "table_id", tableId) ||
        !getNumber(fields, prefix + "row_count", rowCount) ||
        pageSize < kMinPageSize || pageSize > kMaxPageSize ||
        (pageSize & (pageSize - 1)) != 0 || pageCount < 2 ||
        pageCount > kMaxPageCount || pageCount > UINT64_MAX / pageSize ||
        pageSize * pageCount != fileLength || catalogRoot == 0 || catalogRoot >= pageCount) return false;
    DatabaseHeader header;
    if (!DatabaseHeader::parse(bytes.data(), kHeaderSize, header).isOk() ||
        header.formatMajor != formatMajor || header.formatMinor != formatMinor ||
        header.pageSize != pageSize || header.pageCount != pageCount ||
        header.rootPageId != catalogRoot || formatDatabaseId(header.databaseId) !=
            fields.find(prefix + "database_id")->second ||
        header.creationTimeUnixNanos != creationTime) return false;
    if (header.formatMajor != kFormatMajor) return false;

    fixture.pageSize = static_cast<uint32_t>(pageSize);
    fixture.pageCount = pageCount;
    fixture.catalogRootPageId = catalogRoot;
    fixture.formatMajor = static_cast<uint16_t>(formatMajor);
    fixture.formatMinor = static_cast<uint16_t>(formatMinor);
    fixture.catalogVersion = static_cast<uint16_t>(catalogVersion);
    fixture.tableId = static_cast<uint32_t>(tableId);
    std::memcpy(fixture.databaseId, header.databaseId, sizeof(fixture.databaseId));

    DatabaseOpenOptions options;
    options.readOnly = true;
    std::unique_ptr<Database> database;
    if (!Database::open(fixture.path, options, database).isOk() || !database ||
        database->file().pageSize() != fixture.pageSize ||
        database->file().pageCount() != fixture.pageCount ||
        database->file().rootPageId() != fixture.catalogRootPageId ||
        database->catalog().version() != fixture.catalogVersion) return false;
    DatabaseReadView view;
    if (!database->acquireReadView(view).isOk() || view.pageSize() != fixture.pageSize ||
        view.pageCount() != fixture.pageCount) return false;

    for (size_t index = 0; index < 6; ++index) {
        const std::string item = prefix + "index." + std::to_string(index) + ".";
        uint64_t indexId = 0, indexTable = 0, ordinal = 0, root = 0, owner = 0,
            ownerFk = 0, format = 0, entries = 0, viewRoot = 0;
        if (!getNumber(fields, item + "id", indexId) || indexId > UINT32_MAX ||
            !getNumber(fields, item + "table_id", indexTable) || indexTable != tableId ||
            !getNumber(fields, item + "column_ordinal", ordinal) || ordinal > UINT32_MAX ||
            !getNumber(fields, item + "root_page_id", root) || root == 0 || root >= pageCount ||
            !getNumber(fields, item + "owner_index_id", owner) || owner != indexId ||
            !getNumber(fields, item + "owner_foreign_key_id", ownerFk) ||
            !getNumber(fields, item + "format_version", format) ||
            !getNumber(fields, item + "entry_count", entries)) return false;
        const Catalog::IndexRecord* record = database->catalog().findIndex(
            static_cast<uint32_t>(indexId));
        if (!record || record->name != fields.find(item + "name")->second ||
            record->tableId != indexTable || record->columnOrdinal != ordinal ||
            record->rootPageId != root || record->indexId != owner ||
            record->ownerForeignKeyId != ownerFk || record->formatVersion != format ||
            record->entryCount != entries) return false;
        DbType keyType = DbType::Unknown;
        if (!view.indexRoot(static_cast<uint32_t>(indexId), viewRoot, keyType).isOk() ||
            viewRoot != root) return false;
        DatabasePage rootPage;
        if (!view.readPage(root, rootPage).isOk() || rootPage.pageId != root ||
            !rootTypeMatches(fields.find(item + "root_page_type")->second,
                             static_cast<uint16_t>(rootPage.type))) return false;
        fixture.indexIds[index] = static_cast<uint32_t>(indexId);
        fixture.indexTableIds[index] = static_cast<uint32_t>(indexTable);
        fixture.indexColumnOrdinals[index] = static_cast<uint32_t>(ordinal);
        fixture.indexFormatVersions[index] = static_cast<uint16_t>(format);
        fixture.ownerForeignKeyIds[index] = static_cast<uint32_t>(ownerFk);
        fixture.indexEntryCounts[index] = entries;
        fixture.roots[index] = root;
        fixture.rootTypes[index] = static_cast<uint16_t>(rootPage.type);

        std::vector<uint8_t> providerBuffer;
        GxNativeSqlIndexPageProviderContext context;
        if (!openProvider(fixture, index, providerBuffer, context)) return false;
        GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(&context);
        GxIndexPageView nativeRoot = {};
        if (provider.acquire(provider.context, root, &nativeRoot) !=
                GxIndexPageProviderStatus::Ok || nativeRoot.pageId != root ||
            nativeRoot.pageType != static_cast<uint16_t>(rootPage.type)) return false;
        provider.release(provider.context, nativeRoot.pageId, &nativeRoot);
        if (context.borrowed || gxNativeSqlIndexPageProviderClose(&context) !=
            GX_NATIVE_SQL_IMAGE_OK) return false;
    }
    view.release();
    std::unique_ptr<Table> table;
    std::unique_ptr<TableScan> scan;
    if (!database->openTable("Rows", table).isOk() || !table ||
        !table->scanStart(scan).isOk() || !scan) return false;
    std::vector<DbValue> values;
    while (scan->next(values)) {
        ScanRow row;
        row.locator = RowLocator(scan->currentPageId(), scan->currentSlotIndex());
        row.values = values;
        rows.push_back(row);
    }
    if (!scan->status().isOk() || rows.size() != rowCount || !database->close().isOk()) return false;
    return true;
}

bool queryManifestMatches(const std::map<std::string, std::string>& fields,
                          const FixtureQuerySpec& query, size_t queryNumber,
                          const Fixture images[2],
                          const std::vector<ScanRow> rows[2]) {
    const std::string prefix = "query." + std::to_string(queryNumber) + ".";
    uint64_t imageNumber = 0, indexNumber = 0, expectedCount = 0;
    const char* operation = query.point ? "point" : "range";
    if (fields.find(prefix + "name")->second != query.name ||
        !getNumber(fields, prefix + "image", imageNumber) || imageNumber != query.image ||
        !getNumber(fields, prefix + "index", indexNumber) || indexNumber != query.index ||
        fields.find(prefix + "operation")->second != operation ||
        fields.find(prefix + "key_type")->second != typeName(query.type) ||
        fields.find(prefix + "key")->second != (query.point ? valueText(query.key) : "-") ||
        fields.find(prefix + "lower")->second != (query.hasLower ? valueText(query.lower) : "-") ||
        fields.find(prefix + "lower_inclusive")->second != (query.lowerInclusive ? "true" : "false") ||
        fields.find(prefix + "upper")->second != (query.hasUpper ? valueText(query.upper) : "-") ||
        fields.find(prefix + "upper_inclusive")->second != (query.upperInclusive ? "true" : "false") ||
        !getNumber(fields, prefix + "expected_count", expectedCount)) return false;
    std::vector<RowLocator> manifestRows;
    if (!parseLocatorList(fields.find(prefix + "expected_ordered_locators")->second,
                          manifestRows) || manifestRows.size() != expectedCount) return false;
    const std::vector<RowLocator> independent = query.point
        ? expectedPoint(rows[query.image], query.ordinal, query.type, query.key)
        : expectedRange(rows[query.image], query.ordinal, query.type,
            query.hasLower ? &query.lower : nullptr, query.lowerInclusive,
            query.hasUpper ? &query.upper : nullptr, query.upperInclusive);
    if (!sameLocators(independent, manifestRows)) return false;

    std::vector<uint8_t> pageBuffer;
    GxNativeSqlIndexPageProviderContext context;
    if (!openProvider(images[query.image], query.index, pageBuffer, context)) return false;
    LocatorCollector actual;
    GxIndexLookupStatus status = GxIndexLookupStatus::InvalidArgument;
    if (query.point) {
        std::vector<uint8_t> encoded;
        if (!encodeIndexKey(query.type, query.key, encoded)) return false;
        status = runLookup(images[query.image], context, encoded, actual);
    } else {
        std::vector<uint8_t> lower, upper;
        const std::vector<uint8_t>* lowerPtr = nullptr;
        const std::vector<uint8_t>* upperPtr = nullptr;
        if (query.hasLower) {
            if (!encodeIndexKey(query.type, query.lower, lower)) return false;
            lowerPtr = &lower;
        }
        if (query.hasUpper) {
            if (!encodeIndexKey(query.type, query.upper, upper)) return false;
            upperPtr = &upper;
        }
        status = runRange(images[query.image], context, lowerPtr, query.lowerInclusive,
                          upperPtr, query.upperInclusive, actual);
    }
    const bool matched = status == GxIndexLookupStatus::Complete &&
        !context.borrowed && sameLocators(actual.locators, manifestRows);
    return gxNativeSqlIndexPageProviderClose(&context) == GX_NATIVE_SQL_IMAGE_OK && matched;
}

bool verifyFixturePackageText(const std::string& directory, const std::string& manifestText,
                              bool runQueries, Fixture images[2]) {
    std::map<std::string, std::string> fields;
    if (!parseManifestText(manifestText, fields) || !manifestSchema(fields)) return false;
    std::vector<uint8_t> bytesBefore[2];
    std::vector<ScanRow> rows[2];
    for (size_t image = 0; image < 2; ++image) {
        if (!loadAndValidateImage(directory, fields, image, images[image], rows[image])) return false;
        bytesBefore[image] = readBytes(images[image].path);
        if (bytesBefore[image].empty()) return false;
    }
    if (runQueries) {
        const std::vector<FixtureQuerySpec> queries = fixtureQuerySpecs();
        for (size_t i = 0; i < queries.size(); ++i) {
            for (size_t repeat = 0; repeat < 2; ++repeat)
                if (!queryManifestMatches(fields, queries[i], i, images, rows)) return false;
        }
    }
    for (size_t image = 0; image < 2; ++image) {
        if (readBytes(images[image].path) != bytesBefore[image] ||
            sha256File(images[image].path) != fields.find(
                "image." + std::to_string(image) + ".sha256")->second) return false;
    }
    return true;
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return false;
    out.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return !input.bad() && out.size() <= 1024 * 1024;
}

bool replaceManifestField(std::string& text, const std::string& key,
                          const std::string& value, bool remove = false) {
    const std::string prefix = key + "=";
    const size_t start = text.find(prefix);
    if (start == std::string::npos || text.find(prefix, start + prefix.size()) != std::string::npos)
        return false;
    const size_t end = text.find('\n', start);
    if (end == std::string::npos) return false;
    if (remove) text.erase(start, end + 1 - start);
    else text.replace(start + prefix.size(), end - start - prefix.size(), value);
    return true;
}

int verifyQualificationFixture(const char* directory) {
    if (!directory) return 2;
    g_fixtureQualificationName = "R3AH";
    const std::string outDir(directory);
    std::string manifest;
    if (!readTextFile(joinPath(outDir, "fixture.manifest"), manifest)) return 1;
    Fixture images[2];
    const bool valid = verifyFixturePackageText(outDir, manifest, true, images);
    CHECK(valid);
    if (!valid) return 1;
    std::string corrupted = manifest;
    corrupted.erase(0, std::string("GUIDEXOS_SQL_FIXTURE_MANIFEST_V1\n").size());
    CHECK(corrupted.size() < manifest.size());
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "image.1.sha256", std::string(64, '0')));
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "image.1.file_length", "1"));
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "image.1.format_major", "99"));
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "image.1.page_size", "256"));
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "image.1.page_count", "1"));
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "image.1.index.0.root_page_id",
                               std::to_string(images[1].pageCount)));
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "image.1.index.0.owner_index_id", "99999"));
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "image.1.index.0.id", "", true));
    CHECK(!verifyFixturePackageText(outDir, corrupted, false, images));
    corrupted = manifest;
    CHECK(replaceManifestField(corrupted, "query.1.expected_ordered_locators", "9999:0"));
    CHECK(!verifyFixturePackageText(outDir, corrupted, true, images));

    testFixture(images[0], 2);
    testFixture(images[1], 80);
    testBoundaryIndexes(images[1]);
    testIdentityAndModes(images[1]);
    testRootOwnerAssociation(images[1]);
    testVisitorExitReleasesBorrow(images[1]);
    testProviderOpenFailures(images[1]);
    testUnexpectedPageType(images[0]);
    testTruncatedAndInvalidHandle(images[1]);
    testProviderFailureAfterVisitor(images[1]);
    testCorruptionCopies(images[0], images[1]);
    return g_failures == 0 ? 0 : 1;
}
#endif

void testPageOffsetChecks() {
    uint64_t offset = 99;
    CHECK(gxNativeSqlIndexPageOffset(4, 512, &offset) && offset == 2048);
    offset = 99;
    CHECK(!gxNativeSqlIndexPageOffset(UINT64_MAX, 65536, &offset));
    CHECK(offset == 99);
    CHECK(!gxNativeSqlIndexPageOffset(0x01000000ull, 512, &offset));
}

void testIdentityAndModes(const Fixture& fixture) {
    std::vector<uint8_t> buffer(fixture.pageSize);
    GxNativeSqlIndexPageProviderContext context;
    gxNativeSqlIndexPageProviderInit(&context);
    GxNativeSqlIndexImageIdentity identity = {};
    identity.pageSize = fixture.pageSize;
    identity.pageCount = fixture.pageCount;
    identity.catalogRootPageId = fixture.catalogRootPageId;
    identity.indexRootPageId = fixture.roots[0];
    identity.ownerIndexId = fixture.indexIds[0];
    identity.consistency = GX_NATIVE_SQL_CONSISTENCY_LIVE_SINGLE_INSTANCE;
    g_imagePath = fixture.path;
    g_bridgePath = kBridgeDatabasePath;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_UNSUPPORTED_MODE);
    identity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;
    identity.indexRootPageId = fixture.pageCount;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_INVALID_ROOT);
    identity.indexRootPageId = fixture.roots[0];
    identity.catalogRootPageId++;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH);
    CHECK(!context.active);
}

#if defined(GX_R3AG_QUALIFICATION)
void testRootOwnerAssociation(const Fixture& fixture) {
    std::vector<uint8_t> buffer;
    GxNativeSqlIndexPageProviderContext context;
    CHECK(openProvider(fixture, 0, buffer, context));

    // The root and owner must come from the same catalog record. The adapter
    // checks the supplied root against the captured image extent; the
    // production portable decoder rejects a root owned by another index.
    context.ownerIndexId = fixture.indexIds[1];
    std::vector<uint8_t> key;
    CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(7), key));
    LocatorCollector results;
    CHECK(runLookup(fixture, context, key, results) ==
          GxIndexLookupStatus::CorruptPage);
    CHECK(results.locators.empty());
    CHECK(!context.borrowed);
    CHECK(gxNativeSqlIndexPageProviderClose(&context) ==
          GX_NATIVE_SQL_IMAGE_OK);
}

GxIndexLookupStatus runLookupStaged(
    const Fixture& fixture, GxNativeSqlIndexPageProviderContext& context,
    const std::vector<uint8_t>& key, std::vector<RowLocator>& published) {
    LocatorCollector staging;
    const GxIndexLookupStatus status = runLookup(fixture, context, key, staging);
    if (status == GxIndexLookupStatus::Complete) {
        published.swap(staging.locators);
    } else {
        published.clear();
    }
    return status;
}

struct VisitorCounter { uint32_t calls; };

GxIndexLookupVisitControl stopPointVisitor(void* opaque, uint64_t, uint32_t) {
    ++static_cast<VisitorCounter*>(opaque)->calls;
    return GxIndexLookupVisitControl::Stop;
}

GxIndexLookupVisitControl failPointVisitor(void* opaque, uint64_t, uint32_t) {
    ++static_cast<VisitorCounter*>(opaque)->calls;
    return GxIndexLookupVisitControl::Error;
}

GxIndexRangeScanVisitControl stopRangeVisitor(void* opaque, const uint8_t*,
                                              size_t, uint64_t, uint32_t) {
    ++static_cast<VisitorCounter*>(opaque)->calls;
    return GxIndexRangeScanVisitControl::Stop;
}

GxIndexRangeScanVisitControl failRangeVisitor(void* opaque, const uint8_t*,
                                              size_t, uint64_t, uint32_t) {
    ++static_cast<VisitorCounter*>(opaque)->calls;
    return GxIndexRangeScanVisitControl::Error;
}

void testVisitorExitReleasesBorrow(const Fixture& fixture) {
    std::vector<uint8_t> key;
    CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(7), key));
    for (uint32_t failure = 0; failure < 2; ++failure) {
        std::vector<uint8_t> buffer;
        GxNativeSqlIndexPageProviderContext context;
        CHECK(openProvider(fixture, 0, buffer, context));
        GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(&context);
        uint64_t visited[kMaxIndexDepth + 1] = {};
        std::vector<uint8_t> lower(key.size() + 12, 0);
        std::copy(key.begin(), key.end(), lower.begin());
        std::vector<uint8_t> previous(fixture.pageSize - kPageHeaderSize);
        GxIndexLookupResult result = {};
        VisitorCounter counter = {};
        const GxIndexLookupStatus status = gxIndexLookupAll(
            &provider, context.indexRootPageId, context.pageCount,
            context.ownerIndexId, key.data(), key.size(), kMaxIndexDepth,
            visited, kMaxIndexDepth + 1, context.pageCount - 1,
            lower.data(), lower.size(), previous.data(), previous.size(),
            failure == 0 ? stopPointVisitor : failPointVisitor,
            &counter, &result);
        CHECK(status == (failure == 0 ? GxIndexLookupStatus::VisitorStopped
                                     : GxIndexLookupStatus::VisitorFailure));
        CHECK(counter.calls == 1);
        CHECK(!context.borrowed);
        CHECK(gxNativeSqlIndexPageProviderClose(&context) ==
              GX_NATIVE_SQL_IMAGE_OK);
    }

    for (uint32_t failure = 0; failure < 2; ++failure) {
        std::vector<uint8_t> buffer;
        GxNativeSqlIndexPageProviderContext context;
        CHECK(openProvider(fixture, 1, buffer, context));
        GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(&context);
        uint64_t visited[kMaxIndexDepth + 1] = {};
        uint8_t physicalLowerBound[12] = {};
        std::vector<uint8_t> previous(fixture.pageSize - kPageHeaderSize);
        GxIndexLookupResult result = {};
        VisitorCounter counter = {};
        const GxIndexLookupStatus status = gxIndexRangeScan(
            &provider, context.indexRootPageId, context.pageCount,
            context.ownerIndexId, false, nullptr, 0, true, false, nullptr, 0,
            true, kMaxIndexDepth, visited, kMaxIndexDepth + 1,
            context.pageCount - 1, physicalLowerBound,
            sizeof(physicalLowerBound), previous.data(), previous.size(),
            failure == 0 ? stopRangeVisitor : failRangeVisitor,
            &counter, &result);
        CHECK(status == (failure == 0 ? GxIndexLookupStatus::VisitorStopped
                                     : GxIndexLookupStatus::VisitorFailure));
        CHECK(counter.calls == 1);
        CHECK(!context.borrowed);
        CHECK(gxNativeSqlIndexPageProviderClose(&context) ==
              GX_NATIVE_SQL_IMAGE_OK);
    }
}
#endif

void testFixture(const Fixture& fixture, uint32_t rowCount) {
    std::printf("%s immutable image: rows=%u\n", g_fixtureQualificationName, rowCount);
    const std::vector<uint8_t> before = readBytes(fixture.path);
#if defined(GX_R3AG_QUALIFICATION)
    const std::string sourceHashBefore = sha256File(fixture.path);
    CHECK(sourceHashBefore.size() == 64);
    std::printf("  source SHA-256 before=%s\n", sourceHashBefore.c_str());
#endif
    std::unique_ptr<Database> hosted;
    std::vector<ScanRow> rows;
    CHECK(loadHostedRows(fixture, hosted, rows));
    CHECK(rows.size() == rowCount);

    std::vector<uint8_t> buffer;
    GxNativeSqlIndexPageProviderContext context;
    CHECK(openProvider(fixture, 0, buffer, context));
    CHECK(context.active && context.pageCount == fixture.pageCount);
    CHECK(g_readOnlyOpenCount > 0);
    const uint8_t writeProbe = 0;
    CHECK(gx_db_file_write_at(context.fileHandle, 0, &writeProbe, 1) ==
          GX_DB_FILE_READ_ONLY);

    if (rowCount == 0) {
        CHECK(fixture.roots[0] == 0);
        std::vector<uint8_t> key;
        encodeIndexKey(DbType::Int64, DbValue::int64(7), key);
        std::vector<RowLocator> hostedResult;
        CHECK(hosted->indexLookup(fixture.indexIds[0], key, hostedResult).isOk());
        CHECK(hostedResult.empty());
        CHECK(gxNativeSqlIndexPageProviderClose(&context) == GX_NATIVE_SQL_IMAGE_OK);
        CHECK(readBytes(fixture.path) == before);
#if defined(GX_R3AG_QUALIFICATION)
        const std::string sourceHashAfter = sha256File(fixture.path);
        CHECK(sourceHashAfter == sourceHashBefore);
        std::printf("  source SHA-256 after =%s\n", sourceHashAfter.c_str());
#endif
        CHECK(hosted->close().isOk());
        return;
    }

    // Verify explicit borrow lifetime and reuse of caller-owned page storage.
    GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(&context);
    GxIndexPageView first = {};
    CHECK(provider.acquire(provider.context, fixture.roots[0], &first) ==
          GxIndexPageProviderStatus::Ok);
    CHECK(first.bytes == buffer.data() + kPageHeaderSize && first.pageId == fixture.roots[0]);
    CHECK(first.pageType == static_cast<uint16_t>(rowCount >= 80
          ? PageType::IndexInternal : PageType::IndexLeaf));
    CHECK(gxNativeSqlIndexPageProviderClose(&context) == GX_NATIVE_SQL_IMAGE_ACTIVE_BORROW);
    provider.release(provider.context, first.pageId, &first);
    GxIndexPageView second = {};
    CHECK(provider.acquire(provider.context, fixture.roots[0], &second) ==
          GxIndexPageProviderStatus::Ok);
    CHECK(second.bytes == first.bytes);
    provider.release(provider.context, second.pageId, &second);
    GxIndexPageView missing = {};
    CHECK(provider.acquire(provider.context, fixture.pageCount, &missing) ==
          GxIndexPageProviderStatus::InvalidPageId);
    CHECK(context.error == GX_NATIVE_SQL_IMAGE_INVALID_PAGE && !context.borrowed);

    std::vector<DbValue> pointKeys;
    std::vector<std::pair<size_t, DbValue> > queries;
    pointKeys.push_back(DbValue::int64(7)); queries.push_back(std::make_pair(1u, pointKeys.back()));
    pointKeys.push_back(DbValue::int64(999)); queries.push_back(std::make_pair(1u, pointKeys.back()));
    pointKeys.push_back(DbValue::null()); queries.push_back(std::make_pair(1u, pointKeys.back()));
    pointKeys.push_back(DbValue::int32(2)); queries.push_back(std::make_pair(2u, pointKeys.back()));
    pointKeys.push_back(DbValue::boolean(true)); queries.push_back(std::make_pair(3u, pointKeys.back()));
    pointKeys.push_back(DbValue::text(std::string("a\0b", 3)));
    queries.push_back(std::make_pair(4u, pointKeys.back()));
    pointKeys.push_back(DbValue::null()); queries.push_back(std::make_pair(4u, pointKeys.back()));

    for (size_t query = 0; query < queries.size(); ++query) {
        const size_t ordinal = queries[query].first;
        const size_t index = ordinal - 1;
        std::vector<uint8_t> key;
        CHECK(encodeIndexKey(ordinal == 1 ? DbType::Int64 :
                             ordinal == 2 ? DbType::Int32 :
                             ordinal == 3 ? DbType::Boolean : DbType::Text,
                             queries[query].second, key));
        const DbType type = ordinal == 1 ? DbType::Int64 :
                            ordinal == 2 ? DbType::Int32 :
                            ordinal == 3 ? DbType::Boolean : DbType::Text;
        const std::vector<RowLocator> independent =
            expectedPoint(rows, ordinal, type, queries[query].second);
        std::vector<RowLocator> hostedResult;
        CHECK(hosted->indexLookup(fixture.indexIds[index], key, hostedResult).isOk());
        CHECK(sameLocators(hostedResult, independent));

        context.indexRootPageId = fixture.roots[index];
        context.ownerIndexId = fixture.indexIds[index];
        LocatorCollector actual;
        CHECK(runLookup(fixture, context, key, actual) == GxIndexLookupStatus::Complete);
        CHECK(sameLocators(actual.locators, independent));
        CHECK(!context.borrowed);
    }

    // Bounded, one-sided, and unbounded scans use production portable traversal.
    const std::vector<uint8_t> lower32 = []() {
        std::vector<uint8_t> key;
        encodeIndexKey(DbType::Int32, DbValue::int32(1), key);
        return key;
    }();
    const std::vector<uint8_t> upper32 = []() {
        std::vector<uint8_t> key;
        encodeIndexKey(DbType::Int32, DbValue::int32(2), key);
        return key;
    }();
    context.indexRootPageId = fixture.roots[1];
    context.ownerIndexId = fixture.indexIds[1];
    struct RangeCase { const std::vector<uint8_t>* lower; bool lowerInclusive;
                       const std::vector<uint8_t>* upper; bool upperInclusive;
                       const DbValue* lowValue; const DbValue* highValue; };
    const DbValue lowValue = DbValue::int32(1);
    const DbValue highValue = DbValue::int32(2);
    const RangeCase cases[] = {
        {&lower32, true, &upper32, true, &lowValue, &highValue},
        {&lower32, true, nullptr, true, &lowValue, nullptr},
        {nullptr, true, nullptr, true, nullptr, nullptr},
        {nullptr, true, &upper32, true, nullptr, &highValue},
        {&lower32, true, &upper32, false, &lowValue, &highValue},
        {&lower32, false, &upper32, true, &lowValue, &highValue}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        LocatorCollector actual;
        CHECK(runRange(fixture, context, cases[i].lower, cases[i].lowerInclusive,
                       cases[i].upper, cases[i].upperInclusive, actual) ==
              GxIndexLookupStatus::Complete);
        const std::vector<RowLocator> independent = expectedRange(
            rows, 2, DbType::Int32, cases[i].lowValue, cases[i].lowerInclusive,
            cases[i].highValue, cases[i].upperInclusive);
        IndexRangeBound bound;
        if (cases[i].lowValue) {
            bound.hasLower = true; bound.lowerInclusive = cases[i].lowerInclusive;
            bound.lower = *cases[i].lowValue;
        }
        if (cases[i].highValue) {
            bound.hasUpper = true; bound.upperInclusive = cases[i].upperInclusive;
            bound.upper = *cases[i].highValue;
        }
        std::vector<RowLocator> hostedRange;
        CHECK(hosted->indexRange(fixture.indexIds[1], bound, hostedRange).isOk());
        CHECK(sameLocators(hostedRange, independent));
        CHECK(sameLocators(actual.locators, independent));
    }

    if (rowCount >= 80) {
        std::vector<uint8_t> duplicateKey;
        encodeIndexKey(DbType::Int64, DbValue::int64(7), duplicateKey);
        context.indexRootPageId = fixture.roots[0];
        context.ownerIndexId = fixture.indexIds[0];
        LocatorCollector limited;
        CHECK(runLookup(fixture, context, duplicateKey, limited, 1) ==
              GxIndexLookupStatus::TraversalLimit);
        CHECK(!limited.locators.empty() && !context.borrowed);

        // The real bridge reports a short positional read after the first leaf
        // has delivered rows. The caller must discard that staged partial set.
        LocatorCollector partial;
        g_shortReadAt = 0;
        g_scheduleShortReadAfterPointCallback = true;
        const GxIndexLookupStatus failed = runLookup(
            fixture, context, duplicateKey, partial);
        CHECK(failed != GxIndexLookupStatus::Complete);
        CHECK(context.error == GX_NATIVE_SQL_IMAGE_TRUNCATED);
        CHECK(!partial.locators.empty() && !context.borrowed);
        partial.locators.clear();
        CHECK(partial.locators.empty());
    }

    CHECK(gxNativeSqlIndexPageProviderClose(&context) == GX_NATIVE_SQL_IMAGE_OK);
    CHECK(!context.active && context.fileHandle == GX_DB_FILE_INVALID_HANDLE);
    CHECK(readBytes(fixture.path) == before);
#if defined(GX_R3AG_QUALIFICATION)
    const std::string sourceHashAfter = sha256File(fixture.path);
    CHECK(sourceHashAfter == sourceHashBefore);
    std::printf("  source SHA-256 after =%s\n", sourceHashAfter.c_str());
#endif
    hosted->close();
}

void testTruncatedAndInvalidHandle(const Fixture& fixture) {
    const std::string truncatedPath = fixture.path + ".copy";
    std::vector<uint8_t> bytes = readBytes(fixture.path);
    if (!bytes.empty()) bytes.pop_back();
    CHECK(writeBytes(truncatedPath, bytes));
    g_imagePath = truncatedPath;
    g_bridgePath = kBridgeDatabasePath;
    GxNativeSqlIndexPageProviderContext context;
    gxNativeSqlIndexPageProviderInit(&context);
    std::vector<uint8_t> buffer(fixture.pageSize);
    GxNativeSqlIndexImageIdentity identity = {};
    identity.pageSize = fixture.pageSize;
    identity.pageCount = fixture.pageCount;
    identity.catalogRootPageId = fixture.catalogRootPageId;
    identity.indexRootPageId = fixture.roots[0];
    identity.ownerIndexId = fixture.indexIds[0];
    identity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_TRUNCATED);
    std::remove(truncatedPath.c_str());

    uint64_t size = 0;
    CHECK(gx_db_file_get_size(GX_DB_FILE_INVALID_HANDLE, &size) == GX_DB_FILE_STALE_HANDLE);

    Fixture source = fixture;
    source.path = fixture.path;
    std::vector<uint8_t> pageBuffer;
    CHECK(openProvider(source, 0, pageBuffer, context));
    const GxDbFileHandle staleHandle = context.fileHandle;
    CHECK(gx_db_file_close(staleHandle) == GX_DB_FILE_OK);
    GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(&context);
    GxIndexPageView view = {};
    CHECK(provider.acquire(provider.context, fixture.roots[0], &view) ==
          GxIndexPageProviderStatus::Error);
    CHECK(context.nativeFileStatus == GX_DB_FILE_STALE_HANDLE);
    CHECK(context.error == GX_NATIVE_SQL_IMAGE_READ_FAILURE);
    CHECK(gxNativeSqlIndexPageProviderClose(&context) ==
          GX_NATIVE_SQL_IMAGE_CLOSE_FAILURE);
}

void testBoundaryIndexes(const Fixture& fixture) {
    std::unique_ptr<Database> hosted;
    std::vector<ScanRow> rows;
    CHECK(loadHostedRows(fixture, hosted, rows));
    if (!hosted || rows.empty()) return;

    struct BoundaryQuery { size_t index; size_t ordinal; DbType type; DbValue value; };
    const BoundaryQuery points[] = {
        {4, 5, DbType::Int64, DbValue::int64(INT64_MIN)},
        {4, 5, DbType::Int64, DbValue::int64(INT64_MAX)},
        {5, 6, DbType::Int32, DbValue::int32(INT32_MIN)},
        {5, 6, DbType::Int32, DbValue::int32(INT32_MAX)}
    };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        std::vector<uint8_t> key;
        CHECK(encodeIndexKey(points[i].type, points[i].value, key));
        const std::vector<RowLocator> expected = expectedPoint(
            rows, points[i].ordinal, points[i].type, points[i].value);
        std::vector<RowLocator> hostedResult;
        CHECK(hosted->indexLookup(fixture.indexIds[points[i].index], key,
                                  hostedResult).isOk());
        CHECK(sameLocators(hostedResult, expected));

        GxNativeSqlIndexPageProviderContext context;
        std::vector<uint8_t> pageBuffer;
        CHECK(openProvider(fixture, points[i].index, pageBuffer, context));
        LocatorCollector actual;
        CHECK(runLookup(fixture, context, key, actual) ==
              GxIndexLookupStatus::Complete);
        CHECK(sameLocators(actual.locators, expected));
        CHECK(!context.borrowed);
        CHECK(gxNativeSqlIndexPageProviderClose(&context) == GX_NATIVE_SQL_IMAGE_OK);
    }

    const DbValue low64 = DbValue::int64(INT64_MIN);
    const DbValue high64 = DbValue::int64(INT64_MAX);
    std::vector<uint8_t> low64Key, high64Key;
    CHECK(encodeIndexKey(DbType::Int64, low64, low64Key));
    CHECK(encodeIndexKey(DbType::Int64, high64, high64Key));
    GxNativeSqlIndexPageProviderContext rangeContext;
    std::vector<uint8_t> rangeBuffer;
    CHECK(openProvider(fixture, 4, rangeBuffer, rangeContext));
    LocatorCollector bounded;
    CHECK(runRange(fixture, rangeContext, &low64Key, true, &high64Key, true,
                   bounded) == GxIndexLookupStatus::Complete);
    const std::vector<RowLocator> expected64 = expectedRange(
        rows, 5, DbType::Int64, &low64, true, &high64, true);
    CHECK(sameLocators(bounded.locators, expected64));
    CHECK(expected64.size() == rows.size());
    CHECK(gxNativeSqlIndexPageProviderClose(&rangeContext) == GX_NATIVE_SQL_IMAGE_OK);

    const DbValue low32 = DbValue::int32(INT32_MIN);
    const DbValue high32 = DbValue::int32(INT32_MAX);
    std::vector<uint8_t> low32Key, high32Key;
    CHECK(encodeIndexKey(DbType::Int32, low32, low32Key));
    CHECK(encodeIndexKey(DbType::Int32, high32, high32Key));
    CHECK(openProvider(fixture, 5, rangeBuffer, rangeContext));
    LocatorCollector exclusive;
    CHECK(runRange(fixture, rangeContext, &low32Key, false, &high32Key, false,
                   exclusive) == GxIndexLookupStatus::Complete);
    const std::vector<RowLocator> expected32 = expectedRange(
        rows, 6, DbType::Int32, &low32, false, &high32, false);
    CHECK(sameLocators(exclusive.locators, expected32));
    CHECK(expected32.size() + 2 == rows.size());
    CHECK(gxNativeSqlIndexPageProviderClose(&rangeContext) == GX_NATIVE_SQL_IMAGE_OK);
    CHECK(hosted->close().isOk());
}

void testProviderFailureAfterVisitor(const Fixture& fixture) {
    std::vector<uint8_t> buffer;
    GxNativeSqlIndexPageProviderContext context;
    CHECK(openProvider(fixture, 0, buffer, context));
    context.indexRootPageId = fixture.roots[0];
    context.ownerIndexId = fixture.indexIds[0];
    std::vector<uint8_t> key;
    encodeIndexKey(DbType::Int64, DbValue::int64(7), key);
    LocatorCollector partial;
    g_shortReadAt = 0;
    g_scheduleShortReadAfterPointCallback = true;
    const GxIndexLookupStatus status = runLookup(fixture, context, key, partial);
    CHECK(status == GxIndexLookupStatus::ProviderFailure ||
          status == GxIndexLookupStatus::MissingPage ||
          status == GxIndexLookupStatus::InvalidPageId);
    CHECK(context.error == GX_NATIVE_SQL_IMAGE_TRUNCATED);
    CHECK(!partial.locators.empty() && !context.borrowed);
    partial.locators.clear();
    CHECK(partial.locators.empty());
#if defined(GX_R3AG_QUALIFICATION)
    std::vector<RowLocator> published;
    g_shortReadAt = 0;
    g_scheduleShortReadAfterPointCallback = true;
    const GxIndexLookupStatus stagedStatus = runLookupStaged(
        fixture, context, key, published);
    CHECK(stagedStatus != GxIndexLookupStatus::Complete);
    CHECK(context.error == GX_NATIVE_SQL_IMAGE_TRUNCATED);
    CHECK(published.empty());
    CHECK(!context.borrowed);
#endif
    CHECK(gxNativeSqlIndexPageProviderClose(&context) == GX_NATIVE_SQL_IMAGE_OK);
}

void testCorruptionCopies(const Fixture& leaf, const Fixture& multilevel) {
    struct CorruptionCase {
        const Fixture* source;
        size_t index;
        PayloadMutation mutation;
        uint64_t mutationValue;
        DbValue key;
        DbType type;
        GxIndexLookupStatus expectedStatus;
    };
    const CorruptionCase cases[] = {
        {&multilevel, 0, corruptInternalSignature, 0, DbValue::int64(7),
         DbType::Int64, GxIndexLookupStatus::CorruptPage},
        {&leaf, 0, corruptLeafEntry, 0, DbValue::int64(7),
         DbType::Int64, GxIndexLookupStatus::CorruptPage},
        {&leaf, 0, corruptLeafSuccessor, leaf.pageCount, DbValue::int64(7),
         DbType::Int64, GxIndexLookupStatus::InvalidPageId}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Fixture copy = *cases[i].source;
        copy.path += ".copy";
        std::vector<uint8_t> source = readBytes(cases[i].source->path);
        CHECK(writeBytes(copy.path, source));
        CHECK(mutatePage(copy.path, copy.pageSize, copy.roots[cases[i].index],
                         cases[i].mutation, cases[i].mutationValue));
        std::vector<uint8_t> buffer;
        GxNativeSqlIndexPageProviderContext context;
        CHECK(openProvider(copy, cases[i].index, buffer, context));
        std::vector<uint8_t> key;
        CHECK(encodeIndexKey(cases[i].type, cases[i].key, key));
        LocatorCollector partial;
        const GxIndexLookupStatus status = runLookup(copy, context, key, partial);
        CHECK(status == cases[i].expectedStatus);
        if (cases[i].mutation == corruptLeafSuccessor) {
            partial.locators.clear();
            CHECK(partial.locators.empty());
        }
        CHECK(!context.borrowed);
        CHECK(gxNativeSqlIndexPageProviderClose(&context) == GX_NATIVE_SQL_IMAGE_OK);
        std::remove(copy.path.c_str());
    }
}

#if defined(GX_R3AG_QUALIFICATION)
void testUnexpectedPageType(const Fixture& fixture) {
    Fixture copy = fixture;
    copy.path += ".wrong_type";
    CHECK(writeBytes(copy.path, readBytes(fixture.path)));
    CHECK(mutatePageType(copy.path, copy.pageSize, copy.roots[0],
                         static_cast<uint16_t>(PageType::Data)));
    std::vector<uint8_t> buffer;
    GxNativeSqlIndexPageProviderContext context;
    CHECK(openProvider(copy, 0, buffer, context));
    GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(&context);
    GxIndexPageView view = {};
    CHECK(provider.acquire(provider.context, fixture.roots[0], &view) ==
          GxIndexPageProviderStatus::Error);
    CHECK(context.error == GX_NATIVE_SQL_IMAGE_CORRUPT_PAGE);
    CHECK(!context.borrowed);
    CHECK(gxNativeSqlIndexPageProviderClose(&context) ==
          GX_NATIVE_SQL_IMAGE_OK);
    std::remove(copy.path.c_str());
}

void testProviderOpenFailures(const Fixture& fixture) {
    std::vector<uint8_t> buffer(fixture.pageSize);
    GxNativeSqlIndexPageProviderContext context;
    gxNativeSqlIndexPageProviderInit(&context);
    GxNativeSqlIndexImageIdentity identity = {};
    identity.pageSize = fixture.pageSize;
    identity.pageCount = fixture.pageCount;
    identity.catalogRootPageId = fixture.catalogRootPageId;
    identity.indexRootPageId = fixture.roots[0];
    identity.ownerIndexId = fixture.indexIds[0];
    identity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;
    g_imagePath = fixture.path;
    g_bridgePath = kBridgeDatabasePath;

    CHECK(gxNativeSqlIndexPageProviderOpen(nullptr, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) ==
        GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT);
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, nullptr, &identity,
        buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT);
    identity.pageSize = 256;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT);
    identity.pageSize = fixture.pageSize;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size() - 1) ==
        GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT);

    g_imagePath = fixture.path + ".missing";
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_FILE_OPEN);
    CHECK(context.nativeFileStatus == GX_DB_FILE_NOT_FOUND);

    g_imagePath = fixture.path;
    g_failSize = true;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_FILE_SIZE);
    CHECK(context.nativeFileStatus == GX_DB_FILE_IO);
    CHECK(context.cleanupFileStatus == GX_DB_FILE_OK);
    g_failSize = false;

    Fixture badHeader = fixture;
    badHeader.path += ".bad_header";
    std::vector<uint8_t> header = readBytes(fixture.path);
    if (!header.empty()) header[0] ^= 0xff;
    CHECK(writeBytes(badHeader.path, header));
    g_imagePath = badHeader.path;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) ==
        GX_NATIVE_SQL_IMAGE_HEADER_CORRUPT);
    CHECK(!context.active);
    std::remove(badHeader.path.c_str());

    Fixture unsupportedVersion = fixture;
    unsupportedVersion.path += ".unsupported_version";
    CHECK(writeBytes(unsupportedVersion.path, readBytes(fixture.path)));
    CHECK(mutateHeaderFormatMajor(unsupportedVersion.path, kFormatMajor + 1));
    g_imagePath = unsupportedVersion.path;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, kLogicalDatabasePath,
        &identity, buffer.data(), buffer.size()) ==
        GX_NATIVE_SQL_IMAGE_HEADER_CORRUPT);
    CHECK(!context.active);
    std::remove(unsupportedVersion.path.c_str());
    g_imagePath = fixture.path;
}
#endif

} // namespace

// The real Server bridge object refers to production VFS symbols for its
// default operation table. These inert definitions satisfy that table in this
// hosted contract test; all exercised operations use kHostOps above.
namespace kernel { namespace vfs {
uint8_t open(const char*, uint16_t) { return 0xFF; }
Status close(uint8_t) { return VFS_ERR_NOT_SUPPORTED; }
int32_t read(uint8_t, void*, uint32_t) { return VFS_ERR_NOT_SUPPORTED; }
int32_t write(uint8_t, const void*, uint32_t) { return VFS_ERR_NOT_SUPPORTED; }
Status seek(uint8_t, int64_t, SeekOrigin) { return VFS_ERR_NOT_SUPPORTED; }
int64_t tell(uint8_t) { return -1; }
int64_t file_size(uint8_t) { return -1; }
Status flush(uint8_t) { return VFS_ERR_NOT_SUPPORTED; }
Status truncate_shrink(uint8_t, uint64_t) { return VFS_ERR_NOT_SUPPORTED; }
Status stat(const char*, FileInfo*) { return VFS_ERR_NOT_SUPPORTED; }
Status unlink(const char*) { return VFS_ERR_NOT_SUPPORTED; }
} }

int main(int argc, char** argv) {
    gx_db_file_bind_ops(&kHostOps);
#if defined(GX_R3AG_QUALIFICATION)
    if (argc == 5 && std::string(argv[1]) == "--package-fixture") {
        const int result = packageQualificationFixture(argv[2], argv[3], argv[4]);
        gx_db_file_bind_ops(nullptr);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--verify-fixture") {
        const int result = verifyQualificationFixture(argv[2]);
        gx_db_file_bind_ops(nullptr);
        std::printf("NDB1R4P11R3AH native fixture: %u passed, %u failed\n",
                    g_checks - g_failures, g_failures);
        return result == 0 && g_failures == 0 ? 0 : 1;
    }
#endif
    testPageOffsetChecks();

    Fixture empty;
    Fixture leaf;
    Fixture multilevel;
    CHECK(createFixture(0, "empty", empty));
    CHECK(createFixture(1, "leaf", leaf));
    CHECK(createFixture(80, "multi", multilevel));
    if (empty.pageCount) testFixture(empty, 0);
    if (leaf.pageCount) testFixture(leaf, 1);
    if (multilevel.pageCount) {
        testFixture(multilevel, 80);
        testBoundaryIndexes(multilevel);
        testIdentityAndModes(multilevel);
#if defined(GX_R3AG_QUALIFICATION)
        testRootOwnerAssociation(multilevel);
        testVisitorExitReleasesBorrow(multilevel);
        testProviderOpenFailures(multilevel);
        testUnexpectedPageType(leaf);
#endif
        testTruncatedAndInvalidHandle(multilevel);
        testProviderFailureAfterVisitor(multilevel);
        testCorruptionCopies(leaf, multilevel);
    }

    removeDatabaseFiles(empty.path);
    removeDatabaseFiles(leaf.path);
    removeDatabaseFiles(multilevel.path);
    gx_db_file_bind_ops(nullptr);
    std::printf("NDB1R4P11%s native SQL immutable image provider: %u passed, %u failed\n",
                kQualificationName,
                g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
