#include "database_native_io.h"

#include <utility>

namespace gxos {
namespace db {
namespace {

DbResult bridgeError(int32_t status, const char* operation) {
    const char* detail = "native file operation failed";
    DbStatus dbStatus = DbStatus::IoError;
    switch (static_cast<NativeFileStatus>(status)) {
    case NativeFileStatus::Ok:
        return DbResult::ok();
    case NativeFileStatus::NotFound:
        dbStatus = DbStatus::NotDatabase; detail = "native file was not found"; break;
    case NativeFileStatus::AlreadyExists:
        dbStatus = DbStatus::AlreadyExists; detail = "native file already exists"; break;
    case NativeFileStatus::InvalidArgument:
    case NativeFileStatus::InvalidName:
    case NativeFileStatus::IsDirectory:
        dbStatus = DbStatus::InvalidArgument; detail = "native file argument is invalid"; break;
    case NativeFileStatus::StaleHandle:
        dbStatus = DbStatus::NotOpen; detail = "native file handle is stale"; break;
    case NativeFileStatus::ReadOnly:
        dbStatus = DbStatus::ReadOnly; detail = "native file is read-only"; break;
    case NativeFileStatus::NoSpace:
        dbStatus = DbStatus::NoSpace; detail = "native filesystem has no space"; break;
    case NativeFileStatus::NotSupported:
    case NativeFileStatus::ResourceLimit:
        dbStatus = DbStatus::OutOfBounds; detail = "native operation exceeds supported range"; break;
    case NativeFileStatus::Busy:
        dbStatus = DbStatus::Internal; detail = "native file resources are busy"; break;
    case NativeFileStatus::Corrupt:
        dbStatus = DbStatus::IoError; detail = "native filesystem metadata is corrupt"; break;
    case NativeFileStatus::ShortRead:
        dbStatus = DbStatus::Truncated; detail = "native file returned a short read"; break;
    case NativeFileStatus::ShortWrite:
        dbStatus = DbStatus::IoError; detail = "native file returned a short write"; break;
    case NativeFileStatus::IoTimeout:
        dbStatus = DbStatus::IoError; detail = "native file operation timed out"; break;
    case NativeFileStatus::Io:
    default:
        break;
    }
    return DbResult::error(dbStatus, std::string(operation) + ": " + detail +
                           " (bridge status " + std::to_string(status) + ")");
}

bool completeApi(const NativeDatabaseFileApi& api) {
    return api.open && api.getSize && api.readAt && api.writeAt &&
        api.truncateTo && api.flush && api.close && api.exists && api.remove;
}

DbResult invalidApi(const char* operation) {
    return DbResult::error(DbStatus::Internal,
                           std::string(operation) + ": native bridge API is incomplete");
}

} // namespace

NativeDatabaseFile::NativeDatabaseFile(const NativeDatabaseFileApi& api)
    : _api(api), _handle(kInvalidNativeFileHandle), _readOnly(false) {}

NativeDatabaseFile::~NativeDatabaseFile() {
    (void)close();
}

DbResult NativeDatabaseFile::open(const std::string& path, FileOpenMode mode,
                                  bool truncateExisting) {
    DbResult closed = close();
    if (!closed.isOk()) return closed;
    if (!completeApi(_api)) return invalidApi("open");
    if (path.empty()) return DbResult::error(DbStatus::InvalidArgument, "empty file path");

    NativeFileOpenMode nativeMode;
    switch (mode) {
    case FileOpenMode::ReadOnly: nativeMode = NativeFileOpenMode::ReadOnly; break;
    case FileOpenMode::ReadWrite: nativeMode = NativeFileOpenMode::ReadWrite; break;
    case FileOpenMode::Create: nativeMode = NativeFileOpenMode::Create; break;
    default:
        return DbResult::error(DbStatus::InvalidArgument, "unknown native file open mode");
    }
    NativeFileHandle handle = kInvalidNativeFileHandle;
    const int32_t status = _api.open(path.c_str(), static_cast<int32_t>(nativeMode),
                                     truncateExisting ? 1 : 0, &handle);
    DbResult result = bridgeError(status, "open");
    if (!result.isOk()) return result;
    if (handle == kInvalidNativeFileHandle) {
        return DbResult::error(DbStatus::Internal,
                               "open: native bridge returned an invalid successful handle");
    }
    _handle = handle;
    _readOnly = mode == FileOpenMode::ReadOnly;
    _path = path;
    return DbResult::ok();
}

DbResult NativeDatabaseFile::close() {
    if (_handle == kInvalidNativeFileHandle) return DbResult::ok();
    if (!_api.close) {
        _handle = kInvalidNativeFileHandle;
        _readOnly = false;
        return invalidApi("close");
    }
    const NativeFileHandle handle = _handle;
    _handle = kInvalidNativeFileHandle;
    _readOnly = false;
    return bridgeError(_api.close(handle), "close");
}

DbResult NativeDatabaseFile::getSize(uint64_t& outSize) {
    if (!isOpen()) return DbResult::error(DbStatus::NotOpen, "size: file is not open");
    if (!_api.getSize) return invalidApi("size");
    return bridgeError(_api.getSize(_handle, &outSize), "size");
}

bool NativeDatabaseFile::readAt(uint64_t offset, void* buffer, size_t length) {
    return isOpen() && _api.readAt &&
        _api.readAt(_handle, offset, buffer, length) ==
            static_cast<int32_t>(NativeFileStatus::Ok);
}

bool NativeDatabaseFile::writeAt(uint64_t offset, const void* buffer, size_t length) {
    return isOpen() && !_readOnly && _api.writeAt &&
        _api.writeAt(_handle, offset, buffer, length) ==
            static_cast<int32_t>(NativeFileStatus::Ok);
}

bool NativeDatabaseFile::truncateTo(uint64_t size) {
    return isOpen() && !_readOnly && _api.truncateTo &&
        _api.truncateTo(_handle, size) == static_cast<int32_t>(NativeFileStatus::Ok);
}

bool NativeDatabaseFile::flush() {
    return isOpen() && _api.flush &&
        _api.flush(_handle) == static_cast<int32_t>(NativeFileStatus::Ok);
}

std::unique_ptr<IDatabaseFile> NativeDatabaseFileSystem::createFile() {
    return std::unique_ptr<IDatabaseFile>(new NativeDatabaseFile(_api));
}

DbResult NativeDatabaseFileSystem::exists(const std::string& path, bool& outExists) {
    outExists = false;
    if (!completeApi(_api)) return invalidApi("exists");
    if (path.empty()) return DbResult::error(DbStatus::InvalidArgument, "empty file path");
    uint8_t exists = 0;
    DbResult result = bridgeError(_api.exists(path.c_str(), &exists), "exists");
    if (!result.isOk()) return result;
    if (exists > 1) return DbResult::error(DbStatus::Internal, "exists: invalid bridge boolean");
    outExists = exists != 0;
    return DbResult::ok();
}

DbResult NativeDatabaseFileSystem::remove(const std::string& path) {
    if (!completeApi(_api)) return invalidApi("remove");
    if (path.empty()) return DbResult::error(DbStatus::InvalidArgument, "empty file path");
    return bridgeError(_api.remove(path.c_str()), "remove");
}

} // namespace db
} // namespace gxos
