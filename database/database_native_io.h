#pragma once

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>

#include "database_io.h"

namespace gxos {
namespace db {

// Integer ABI for the native bridge exported by guideXOS Server's
// native_database_file.h. Values intentionally match its C enums.
enum class NativeFileStatus : int32_t {
    Ok = 0, NotFound = 1, AlreadyExists = 2, InvalidArgument = 3,
    InvalidName = 4, StaleHandle = 5, IsDirectory = 6, ReadOnly = 7,
    NoSpace = 8, NotSupported = 9, ResourceLimit = 10, Busy = 11,
    Corrupt = 12, ShortRead = 13, ShortWrite = 14, IoTimeout = 15, Io = 16
};

enum class NativeFileOpenMode : int32_t {
    ReadOnly = 0, ReadWrite = 1, Create = 2
};

typedef uint32_t NativeFileHandle;
static const NativeFileHandle kInvalidNativeFileHandle = 0xFFFFFFFFu;

// The freestanding bridge functions can be adapted to this table directly or
// through tiny enum-casting shims. Tests inject an in-memory implementation.
struct NativeDatabaseFileApi {
    int32_t (*open)(const char*, int32_t, uint8_t, NativeFileHandle*);
    int32_t (*getSize)(NativeFileHandle, uint64_t*);
    int32_t (*readAt)(NativeFileHandle, uint64_t, void*, size_t);
    int32_t (*writeAt)(NativeFileHandle, uint64_t, const void*, size_t);
    int32_t (*truncateTo)(NativeFileHandle, uint64_t);
    int32_t (*flush)(NativeFileHandle);
    int32_t (*close)(NativeFileHandle);
    int32_t (*exists)(const char*, uint8_t*);
    int32_t (*remove)(const char*);
};

class NativeDatabaseFile : public IDatabaseFile {
public:
    explicit NativeDatabaseFile(const NativeDatabaseFileApi& api);
    ~NativeDatabaseFile() override;

    NativeDatabaseFile(const NativeDatabaseFile&) = delete;
    NativeDatabaseFile& operator=(const NativeDatabaseFile&) = delete;

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override;
    DbResult close() override;
    bool isOpen() const override { return _handle != kInvalidNativeFileHandle; }
    bool isReadOnly() const override { return _readOnly; }
    DbResult getSize(uint64_t& outSize) override;
    bool readAt(uint64_t offset, void* buffer, size_t length) override;
    bool writeAt(uint64_t offset, const void* buffer, size_t length) override;
    bool truncateTo(uint64_t size) override;
    bool flush() override;
    const std::string& path() const override { return _path; }

private:
    const NativeDatabaseFileApi _api;
    NativeFileHandle _handle;
    bool _readOnly;
    std::string _path;
};

class NativeDatabaseFileSystem : public IDatabaseFileSystem {
public:
    explicit NativeDatabaseFileSystem(const NativeDatabaseFileApi& api) : _api(api) {}
    std::unique_ptr<IDatabaseFile> createFile() override;
    DbResult exists(const std::string& path, bool& outExists) override;
    DbResult remove(const std::string& path) override;

private:
    const NativeDatabaseFileApi _api;
};

} // namespace db
} // namespace gxos
