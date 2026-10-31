#include "kernel/native_database_file.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
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
#include "database_index.h"
#include "database_relational.h"
#include "database_schema.h"
#include "database_types.h"
#include "gx_native_sql_index_page_provider.h"
#include "gx_index_lookup.h"
#include "gx_page_codec.h"

using namespace gxos::db;

namespace {

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
    const std::string path = std::string(base) + "\\gxos_r3af_native_provider";
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

struct Fixture {
    std::string path;
    uint32_t pageSize;
    uint64_t pageCount;
    uint64_t catalogRootPageId;
    uint64_t roots[4];
    uint32_t indexIds[4];
    uint32_t tableId;
    Fixture() : path(), pageSize(512), pageCount(0), catalogRootPageId(0),
                roots(), indexIds(), tableId(0) {}
};

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
    if (!database->createTable(tableDefinition, out.tableId).isOk()) return false;

    const char* names[] = {"IX_Rows_K64", "IX_Rows_K32", "IX_Rows_KBool", "IX_Rows_KText"};
    const uint32_t ordinals[] = {1, 2, 3, 4};
    for (size_t i = 0; i < 4; ++i) {
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
    bool rootsOk = true;
    for (size_t i = 0; i < 4; ++i) {
        DbType type = DbType::Unknown;
        if (!view.indexRoot(out.indexIds[i], out.roots[i], type).isOk()) rootsOk = false;
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
    g_bridgePath = "/R3AF/IMAGE.GDB";
    gxNativeSqlIndexPageProviderInit(&context);
    buffer.resize(fixture.pageSize);
    GxNativeSqlIndexImageIdentity identity = {};
    identity.pageSize = fixture.pageSize;
    identity.pageCount = fixture.pageCount;
    identity.catalogRootPageId = fixture.catalogRootPageId;
    identity.indexRootPageId = fixture.roots[index];
    identity.ownerIndexId = fixture.indexIds[index];
    identity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;
    return gxNativeSqlIndexPageProviderOpen(&context, "/R3AF/IMAGE.gxdb",
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_OK;
}

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
    g_bridgePath = "/R3AF/IMAGE.GDB";
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, "/R3AF/IMAGE.gxdb",
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_UNSUPPORTED_MODE);
    identity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;
    identity.indexRootPageId = fixture.pageCount;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, "/R3AF/IMAGE.gxdb",
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_INVALID_ROOT);
    identity.indexRootPageId = fixture.roots[0];
    identity.catalogRootPageId++;
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, "/R3AF/IMAGE.gxdb",
        &identity, buffer.data(), buffer.size()) == GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH);
    CHECK(!context.active);
}

void testFixture(const Fixture& fixture, uint32_t rowCount) {
    std::printf("R3AF immutable image: rows=%u\n", rowCount);
    const std::vector<uint8_t> before = readBytes(fixture.path);
    std::unique_ptr<Database> hosted;
    std::vector<ScanRow> rows;
    CHECK(loadHostedRows(fixture, hosted, rows));
    CHECK(rows.size() == rowCount);

    std::vector<uint8_t> buffer;
    GxNativeSqlIndexPageProviderContext context;
    CHECK(openProvider(fixture, 0, buffer, context));
    CHECK(context.active && context.pageCount == fixture.pageCount);
    CHECK(g_readOnlyOpenCount > 0);

    if (rowCount == 0) {
        CHECK(fixture.roots[0] == 0);
        std::vector<uint8_t> key;
        encodeIndexKey(DbType::Int64, DbValue::int64(7), key);
        std::vector<RowLocator> hostedResult;
        CHECK(hosted->indexLookup(fixture.indexIds[0], key, hostedResult).isOk());
        CHECK(hostedResult.empty());
        CHECK(gxNativeSqlIndexPageProviderClose(&context) == GX_NATIVE_SQL_IMAGE_OK);
        CHECK(readBytes(fixture.path) == before);
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
        {nullptr, true, nullptr, true, nullptr, nullptr}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        LocatorCollector actual;
        CHECK(runRange(fixture, context, cases[i].lower, cases[i].lowerInclusive,
                       cases[i].upper, cases[i].upperInclusive, actual) ==
              GxIndexLookupStatus::Complete);
        const std::vector<RowLocator> independent = expectedRange(
            rows, 2, DbType::Int32, cases[i].lowValue, true,
            cases[i].highValue, true);
        IndexRangeBound bound;
        if (cases[i].lowValue) {
            bound.hasLower = true; bound.lowerInclusive = true; bound.lower = *cases[i].lowValue;
        }
        if (cases[i].highValue) {
            bound.hasUpper = true; bound.upperInclusive = true; bound.upper = *cases[i].highValue;
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
    hosted->close();
}

void testTruncatedAndInvalidHandle(const Fixture& fixture) {
    const std::string truncatedPath = fixture.path + ".copy";
    std::vector<uint8_t> bytes = readBytes(fixture.path);
    if (!bytes.empty()) bytes.pop_back();
    CHECK(writeBytes(truncatedPath, bytes));
    g_imagePath = truncatedPath;
    g_bridgePath = "/R3AF/IMAGE.GDB";
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
    CHECK(gxNativeSqlIndexPageProviderOpen(&context, "/R3AF/IMAGE.gxdb",
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

int main() {
    gx_db_file_bind_ops(&kHostOps);
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
        testIdentityAndModes(multilevel);
        testTruncatedAndInvalidHandle(multilevel);
        testProviderFailureAfterVisitor(multilevel);
        testCorruptionCopies(leaf, multilevel);
    }

    removeDatabaseFiles(empty.path);
    removeDatabaseFiles(leaf.path);
    removeDatabaseFiles(multilevel.path);
    gx_db_file_bind_ops(nullptr);
    std::printf("NDB1R4P11R3AF native SQL immutable image provider: %u passed, %u failed\n",
                g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
