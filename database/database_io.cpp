#include "database_io.h"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "database_endian.h"

namespace gxos {
namespace db {

namespace {

// 64-bit seek helper. Kept in one place so every platform uses full-width
// offsets; no silent truncation to 32-bit `long`.
bool seekTo(FILE* file, uint64_t offset) {
    if (offset > static_cast<uint64_t>(INT64_MAX)) {
        return false;
    }
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool fileExists(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }
    return (info.st_mode & S_IFREG) != 0;
}

} // namespace

bool hostFileExists(const std::string& path) {
    return fileExists(path);
}

HostDatabaseFile::HostDatabaseFile() : _file(nullptr), _readOnly(false) {}

HostDatabaseFile::~HostDatabaseFile() {
    close();
}

DbResult HostDatabaseFile::open(const std::string& path, FileOpenMode mode,
                                bool truncateExisting) {
    close();
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty file path");
    }

    const bool exists = fileExists(path);

    if (mode == FileOpenMode::Create) {
        if (exists && !truncateExisting) {
            struct stat info;
            if (stat(path.c_str(), &info) == 0 && info.st_size > 0) {
                return DbResult::error(DbStatus::AlreadyExists,
                                       "file already exists and overwrite was not requested");
            }
        }
        _file = std::fopen(path.c_str(), "w+b");
        _readOnly = false;
    } else if (mode == FileOpenMode::ReadOnly) {
        _file = std::fopen(path.c_str(), "rb");
        _readOnly = true;
    } else {
        _file = std::fopen(path.c_str(), "r+b");
        _readOnly = false;
    }

    if (_file == nullptr) {
        _readOnly = false;
        return DbResult::error(DbStatus::IoError,
                               std::string("fopen failed: ") + std::strerror(errno));
    }

    _path = path;
    return DbResult::ok();
}

void HostDatabaseFile::close() {
    if (_file != nullptr) {
        std::fclose(_file);
        _file = nullptr;
    }
    _readOnly = false;
}

uint64_t HostDatabaseFile::size() {
    if (_file == nullptr) {
        return 0;
    }
#if defined(_WIN32)
    __int64 current = _ftelli64(_file);
    if (current < 0) {
        return 0;
    }
    if (_fseeki64(_file, 0, SEEK_END) != 0) {
        return 0;
    }
    __int64 end = _ftelli64(_file);
    _fseeki64(_file, current, SEEK_SET);
    return end < 0 ? 0 : static_cast<uint64_t>(end);
#else
    off_t current = ftello(_file);
    if (current < 0) {
        return 0;
    }
    if (fseeko(_file, 0, SEEK_END) != 0) {
        return 0;
    }
    off_t end = ftello(_file);
    fseeko(_file, current, SEEK_SET);
    return end < 0 ? 0 : static_cast<uint64_t>(end);
#endif
}

bool HostDatabaseFile::readAt(uint64_t offset, void* buffer, size_t length) {
    if (_file == nullptr || buffer == nullptr) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (!seekTo(_file, offset)) {
        return false;
    }
    const size_t read = std::fread(buffer, 1, length, _file);
    return read == length;
}

bool HostDatabaseFile::writeAt(uint64_t offset, const void* buffer, size_t length) {
    if (_file == nullptr || _readOnly) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (!seekTo(_file, offset)) {
        return false;
    }
    const size_t written = std::fwrite(buffer, 1, length, _file);
    return written == length;
}

bool HostDatabaseFile::truncateTo(uint64_t size) {
    if (_file == nullptr || _readOnly) {
        return false;
    }
    if (size > static_cast<uint64_t>(INT64_MAX)) {
        return false;
    }
    if (std::fflush(_file) != 0) {
        return false;
    }
#if defined(_WIN32)
    return _chsize_s(_fileno(_file), static_cast<__int64>(size)) == 0;
#else
    return ftruncate(fileno(_file), static_cast<off_t>(size)) == 0;
#endif
}

bool HostDatabaseFile::flush() {
    if (_file == nullptr) {
        return false;
    }
    if (std::fflush(_file) != 0) {
        return false;
    }
#if defined(_WIN32)
    return _commit(_fileno(_file)) == 0;
#else
    return fsync(fileno(_file)) == 0;
#endif
}

} // namespace db
} // namespace gxos
