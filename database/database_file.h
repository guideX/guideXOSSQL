#pragma once
// guideXOS SQL -- Phase SQL1
// DatabaseFile: owns an opened .gxdb file and implements page-oriented I/O,
// append-only allocation, validation and the SQL1 durability contract.
//
// Durability contract (SQL1):
//   * writePage/allocatePage issue the underlying positional write immediately.
//   * flush() writes the header page last (so the header never references an
//     unwritten page) and then requests an OS durability barrier.
//   * close() flushes best-effort before closing.
//   * There is NO write-ahead log and NO atomic multi-page transaction. A
//     crash can leave uncommitted orphan pages past the header's page count;
//     those are ignored on reopen and overwritten by the next allocation.
//     Full WAL / crash-atomic transactions are deferred to a later phase.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "database_diagnostics.h"
#include "database_format.h"
#include "database_header.h"
#include "database_io.h"
#include "database_page.h"
#include "database_result.h"

namespace gxos {
namespace db {

struct DatabaseCreateOptions {
    uint32_t pageSize;
    bool overwriteExisting;
    bool generateDatabaseId;
    uint8_t databaseId[16];
    uint64_t creationTimeUnixNanos; // 0 => use current time
    uint32_t bufferCapacity;        // 0 => default buffer size

    DatabaseCreateOptions()
        : pageSize(kDefaultPageSize),
          overwriteExisting(false),
          generateDatabaseId(true),
          creationTimeUnixNanos(0),
          bufferCapacity(0) {
        for (int i = 0; i < 16; ++i) {
            databaseId[i] = 0;
        }
    }
};

struct DatabaseOpenOptions {
    bool readOnly;
    bool validateAllPages;
    uint32_t bufferCapacity; // 0 => default buffer size

    DatabaseOpenOptions() : readOnly(false), validateAllPages(false), bufferCapacity(0) {}
};

class DatabaseFile {
public:
    DatabaseFile();
    ~DatabaseFile();

    DatabaseFile(DatabaseFile&&) noexcept;
    DatabaseFile& operator=(DatabaseFile&&) noexcept;
    DatabaseFile(const DatabaseFile&) = delete;
    DatabaseFile& operator=(const DatabaseFile&) = delete;

    // Creates a new database on `file` (ownership transferred). `file` must
    // already be open and writable/empty.
    static DbResult create(std::unique_ptr<IDatabaseFile> file,
                           const DatabaseCreateOptions& options,
                           std::unique_ptr<DatabaseFile>& out);

    // Opens and validates an existing database on `file`.
    static DbResult open(std::unique_ptr<IDatabaseFile> file,
                         const DatabaseOpenOptions& options,
                         std::unique_ptr<DatabaseFile>& out);

    // Flushes any pending header update and closes the file. Returns the flush
    // result when a write-back was attempted.
    DbResult close();
    bool isOpen() const { return _open; }
    bool isReadOnly() const { return _readOnly; }

    // Reads a full page. Page 0 (the header page) is not readable here.
    DbResult readPage(uint64_t pageId, DatabasePage& out);

    // Overwrites an already-allocated page (1 <= pageId < pageCount).
    DbResult writePage(const DatabasePage& page);

    // Appends a new page. Returns its id in `outPageId`.
    DbResult allocatePage(PageType type, uint64_t& outPageId);

    // Durability barrier: writes a dirty header, then flushes the file.
    DbResult flush();

    const DatabaseHeader& header() const { return _header; }
    uint32_t pageSize() const { return _header.pageSize; }
    uint64_t pageCount() const { return _header.pageCount; }
    uint64_t rootPageId() const { return _header.rootPageId; }
    const DatabaseDiagnostics& diagnostics() const { return _diagnostics; }
    DbResult lastError() const { return _lastError; }

private:
    DbResult fail(DbStatus status, const std::string& message);
    DbResult readPageBytes(uint64_t pageId, std::vector<uint8_t>& out);
    DbResult writeHeaderPage();
    DbResult validateBootstrap(const DatabasePage& root);
    bool computePageOffset(uint64_t pageId, uint64_t& outOffset) const;
    void refreshDiagnostics();

    std::unique_ptr<IDatabaseFile> _file;
    DatabaseHeader _header;
    DatabaseDiagnostics _diagnostics;
    DbResult _lastError;
    bool _open;
    bool _readOnly;
    bool _headerDirty;
};

} // namespace db
} // namespace gxos
