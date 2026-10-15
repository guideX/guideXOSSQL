#include "database_native_io.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "database_engine.h"

using namespace gxos::db;

namespace {

struct Handle {
    bool open;
    bool readOnly;
    std::string path;
    Handle() : open(false), readOnly(false) {}
};

std::map<std::string, std::vector<uint8_t> > g_files;
Handle g_handles[32];
int32_t g_existsStatus = 0;
int32_t g_sizeStatus = 0;
int32_t g_flushStatus = 0;
int32_t g_closeStatus = 0;
int32_t g_removeStatus = 0;
int32_t g_readStatus = 0;
int32_t g_writeStatus = 0;
uint32_t g_checks = 0;
uint32_t g_failures = 0;

int32_t mockOpen(const char* path, int32_t mode, uint8_t truncateExisting,
                 NativeFileHandle* out) {
    if (!path || !out || mode < 0 || mode > 2) return 3;
    *out = kInvalidNativeFileHandle;
    std::map<std::string, std::vector<uint8_t> >::iterator file = g_files.find(path);
    const bool exists = file != g_files.end();
    if (mode != 2 && !exists) return 1;
    if (mode == 2 && exists && !truncateExisting && !file->second.empty()) return 2;
    if (mode == 2 && !exists) file = g_files.insert(std::make_pair(path, std::vector<uint8_t>())).first;
    if (mode == 2 && truncateExisting) file->second.clear();
    for (uint32_t i = 0; i < 32; ++i) {
        if (!g_handles[i].open) {
            g_handles[i].open = true;
            g_handles[i].readOnly = mode == 0;
            g_handles[i].path = path;
            *out = i;
            return 0;
        }
    }
    return 11;
}

int32_t mockGetSize(NativeFileHandle handle, uint64_t* out) {
    if (g_sizeStatus) return g_sizeStatus;
    if (!out || handle >= 32 || !g_handles[handle].open) return 5;
    *out = g_files[g_handles[handle].path].size();
    return 0;
}

int32_t mockReadAt(NativeFileHandle handle, uint64_t offset, void* buffer, size_t length) {
    if (g_readStatus) return g_readStatus;
    if (handle >= 32 || !g_handles[handle].open || (!buffer && length)) return 3;
    std::vector<uint8_t>& bytes = g_files[g_handles[handle].path];
    if (offset > bytes.size() || length > bytes.size() - static_cast<size_t>(offset)) return 13;
    if (length) std::copy(bytes.begin() + static_cast<size_t>(offset),
                          bytes.begin() + static_cast<size_t>(offset) + length,
                          static_cast<uint8_t*>(buffer));
    return 0;
}

int32_t mockWriteAt(NativeFileHandle handle, uint64_t offset, const void* buffer, size_t length) {
    if (g_writeStatus) return g_writeStatus;
    if (handle >= 32 || !g_handles[handle].open || g_handles[handle].readOnly ||
        (!buffer && length)) return 7;
    if (offset > 0xFFFFFFFFull || length > 0xFFFFFFFFull - offset) return 10;
    std::vector<uint8_t>& bytes = g_files[g_handles[handle].path];
    if (offset + length > bytes.size()) bytes.resize(static_cast<size_t>(offset + length));
    if (length) std::copy(static_cast<const uint8_t*>(buffer),
                          static_cast<const uint8_t*>(buffer) + length,
                          bytes.begin() + static_cast<size_t>(offset));
    return 0;
}

int32_t mockTruncate(NativeFileHandle handle, uint64_t size) {
    if (handle >= 32 || !g_handles[handle].open || g_handles[handle].readOnly) return 7;
    std::vector<uint8_t>& bytes = g_files[g_handles[handle].path];
    if (size > bytes.size()) return 9;
    bytes.resize(static_cast<size_t>(size));
    return 0;
}

int32_t mockFlush(NativeFileHandle handle) {
    if (handle >= 32 || !g_handles[handle].open) return 5;
    return g_flushStatus;
}

int32_t mockClose(NativeFileHandle handle) {
    if (handle >= 32 || !g_handles[handle].open) return 5;
    g_handles[handle].open = false;
    return g_closeStatus;
}

int32_t mockExists(const char* path, uint8_t* exists) {
    if (g_existsStatus) return g_existsStatus;
    if (!path || !exists) return 3;
    *exists = g_files.find(path) == g_files.end() ? 0 : 1;
    return 0;
}

int32_t mockRemove(const char* path) {
    if (g_removeStatus) return g_removeStatus;
    return g_files.erase(path) ? 0 : 0;
}

const NativeDatabaseFileApi kApi = {
    mockOpen, mockGetSize, mockReadAt, mockWriteAt, mockTruncate,
    mockFlush, mockClose, mockExists, mockRemove
};

void check(bool passed, const char* name) {
    ++g_checks;
    if (!passed) {
        ++g_failures;
        std::printf("FAIL: %s\n", name);
    }
}

void testProviderContract() {
    g_files.clear();
    NativeDatabaseFileSystem fs(kApi);
    bool exists = true;
    DbResult result = fs.exists("/data/test.gxdb", exists);
    check(result.isOk() && !exists, "native filesystem reports missing path as absence");
    g_existsStatus = static_cast<int32_t>(NativeFileStatus::IoTimeout);
    result = fs.exists("/data/test.gxdb", exists);
    check(!result.isOk() && result.status() == DbStatus::IoError && !exists,
          "native lookup error cannot become absence");
    g_existsStatus = 0;

    std::unique_ptr<IDatabaseFile> file = fs.createFile();
    result = file->open("/data/test.gxdb", FileOpenMode::Create, false);
    check(result.isOk() && file->isOpen(), "SQL file opens through injected native bridge API");
    std::vector<uint8_t> payload(8192);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 29u + 7u);
    check(file->writeAt(37, payload.data(), payload.size()),
          "SQL positional write reaches native bridge and permits normal growth");
    uint64_t size = 0;
    check(file->getSize(size).isOk() && size == payload.size() + 37,
          "SQL size query returns native status and exact size");
    g_sizeStatus = static_cast<int32_t>(NativeFileStatus::Io);
    result = file->getSize(size);
    check(!result.isOk() && result.status() == DbStatus::IoError,
          "native size failure remains distinct from an empty file");
    g_sizeStatus = 0;
    std::vector<uint8_t> actual(payload.size());
    check(file->readAt(37, actual.data(), actual.size()) && actual == payload,
          "SQL positional read returns exact native bytes");
    check(!file->readAt(size - 4, actual.data(), 16), "SQL adapter rejects a short page read");
    check(file->flush(), "SQL flush reaches native durability barrier");
    g_flushStatus = static_cast<int32_t>(NativeFileStatus::IoTimeout);
    check(!file->flush(), "native flush failure is returned to SQL");
    g_flushStatus = 0;
    check(file->truncateTo(64), "SQL checkpoint shrink maps to native shrink");
    check(!file->truncateTo(65), "SQL truncate growth fails when native bridge cannot grow");

    g_closeStatus = static_cast<int32_t>(NativeFileStatus::IoTimeout);
    result = file->close();
    check(!result.isOk() && result.status() == DbStatus::IoError && !file->isOpen(),
          "native close failure propagates and invalidates the SQL handle");
    g_closeStatus = 0;

    g_removeStatus = static_cast<int32_t>(NativeFileStatus::Io);
    result = fs.remove("/data/test.gxdb");
    check(!result.isOk() && result.status() == DbStatus::IoError,
          "provider cleanup returns native remove failure");
    g_removeStatus = 0;
    check(fs.remove("/data/test.gxdb").isOk(), "provider cleanup removes native file");
}

void testSqlEngineUsesNativeAdapter() {
    g_files.clear();
    NativeDatabaseFileSystem fs(kApi);
    DatabaseCreateOptions createOptions;
    std::unique_ptr<DatabaseFile> created;
    DbResult createdResult = DatabaseEngine::createDatabase(
        "/engine.gxdb", createOptions, created, &fs);
    check(createdResult.isOk() && created, "SQL engine creates a database through native provider");
    if (!createdResult.isOk() || !created) return;
    check(created->close().isOk(), "SQL engine closes native-backed database cleanly");

    DatabaseOpenOptions openOptions;
    openOptions.readOnly = true;
    openOptions.validateAllPages = true;
    std::unique_ptr<DatabaseFile> reopened;
    DbResult openResult = DatabaseEngine::openDatabase(
        "/engine.gxdb", openOptions, reopened, &fs);
    check(openResult.isOk() && reopened,
          "SQL engine reopens and validates database through native provider");
    if (reopened) check(reopened->close().isOk(), "reopened native-backed database closes");
}

} // namespace

int main() {
    testProviderContract();
    testSqlEngineUsesNativeAdapter();
    std::printf("native SQL adapter: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
