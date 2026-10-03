#pragma once
// guideXOS SQL -- Phase SQL1
// Byte-oriented file abstraction for the database storage layer.
//
// The engine talks to storage only through IDatabaseFile. The hosted build
// provides HostDatabaseFile (stdio backed). A future guideXOS phase can supply
// an adapter over the native VFS / block device without touching the format or
// page logic. All operations are positional (readAt/writeAt) so the storage
// layer never depends on hidden file cursor state.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "database_result.h"

namespace gxos {
namespace db {

enum class FileOpenMode {
    ReadOnly,
    ReadWrite,
    Create
};

class IDatabaseFile {
public:
    virtual ~IDatabaseFile() {}

    // Opens (or creates) the file. When mode == Create:
    //   - if the file exists and truncateExisting == false and its size > 0,
    //     returns DbStatus::AlreadyExists without modifying it;
    //   - otherwise creates/truncates it.
    virtual DbResult open(const std::string& path, FileOpenMode mode,
                          bool truncateExisting) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool isReadOnly() const = 0;

    virtual uint64_t size() = 0;

    // Positional reads/writes. Return false on short/failed I/O or overflow.
    virtual bool readAt(uint64_t offset, void* buffer, size_t length) = 0;
    virtual bool writeAt(uint64_t offset, const void* buffer, size_t length) = 0;
    virtual bool truncateTo(uint64_t size) = 0;

    // Flushes userspace buffers and requests an OS-level durability barrier.
    virtual bool flush() = 0;

    virtual const std::string& path() const = 0;
};

// Factory for database/WAL files. The engine never constructs a concrete file
// directly, which keeps the storage backend independent (hosted stdio today,
// native guideXOS VFS later) and allows hosted crash injection.
class IDatabaseFileSystem {
public:
    virtual ~IDatabaseFileSystem() {}

    // Creates an unopened file object. The caller then calls open().
    virtual std::unique_ptr<IDatabaseFile> createFile() = 0;

    virtual bool exists(const std::string& path) = 0;

    // Removes a file if present. Returns true when the file is absent
    // afterwards.
    virtual bool remove(const std::string& path) = 0;
};

// stdio-backed implementation used by hosted tests and the hosted server.
class HostDatabaseFile : public IDatabaseFile {
public:
    HostDatabaseFile();
    ~HostDatabaseFile();

    HostDatabaseFile(const HostDatabaseFile&) = delete;
    HostDatabaseFile& operator=(const HostDatabaseFile&) = delete;

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override;
    void close() override;
    bool isOpen() const override { return _file != nullptr; }
    bool isReadOnly() const override { return _readOnly; }

    uint64_t size() override;
    bool readAt(uint64_t offset, void* buffer, size_t length) override;
    bool writeAt(uint64_t offset, const void* buffer, size_t length) override;
    bool truncateTo(uint64_t size) override;
    bool flush() override;

    const std::string& path() const override { return _path; }

private:
    FILE* _file;
    bool _readOnly;
    std::string _path;
};

// Returns true if a regular file exists at `path`.
bool hostFileExists(const std::string& path);

// Hosted file-system provider (stdio). Returns a process-wide instance.
class HostFileSystem : public IDatabaseFileSystem {
public:
    std::unique_ptr<IDatabaseFile> createFile() override;
    bool exists(const std::string& path) override;
    bool remove(const std::string& path) override;
};

IDatabaseFileSystem& defaultFileSystem();

} // namespace db
} // namespace gxos
