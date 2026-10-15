#include "database_engine.h"

#include <utility>

#include "database_endian.h"
#include "database_relational.h"

namespace gxos {
namespace db {

DbResult DatabaseEngine::createDatabase(const std::string& path,
                                        const DatabaseCreateOptions& options,
                                        std::unique_ptr<DatabaseFile>& out,
                                        IDatabaseFileSystem* fileSystem) {
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty database path");
    }
    if (!isSupportedPageSize(options.pageSize)) {
        return DbResult::error(DbStatus::InvalidArgument, "unsupported page size");
    }

    IDatabaseFileSystem& filesystem = fileSystem ? *fileSystem : defaultFileSystem();
    bool existedBefore = false;
    DbResult lookup = filesystem.exists(path, existedBefore);
    if (!lookup.isOk()) {
        return DbResult::error(DbStatus::IoError,
                               "database path lookup failed: " + lookup.describe());
    }

    std::unique_ptr<IDatabaseFile> file = filesystem.createFile();
    if (!file) {
        return DbResult::error(DbStatus::Internal, "file-system provider returned no file");
    }
    DbResult result = file->open(path, FileOpenMode::Create, options.overwriteExisting);
    if (!result.isOk()) {
        return result;
    }

    result = DatabaseFile::create(std::move(file), options, out);
    if (!result.isOk()) {
        // Create mode has already created or truncated the provider file. Do
        // not leave a partial database behind even if the path existed empty
        // or was deliberately overwritten.
        DbResult cleanup = filesystem.remove(path);
        if (!cleanup.isOk()) {
            return DbResult::error(DbStatus::RecoveryFailed,
                                   result.describe() + "; provider cleanup failed: " +
                                       cleanup.describe());
        }
    }
    return result;
}

DbResult DatabaseEngine::openDatabase(const std::string& path,
                                      const DatabaseOpenOptions& options,
                                      std::unique_ptr<DatabaseFile>& out,
                                      IDatabaseFileSystem* fileSystem) {
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty database path");
    }
    IDatabaseFileSystem& filesystem = fileSystem ? *fileSystem : defaultFileSystem();
    bool exists = false;
    DbResult lookup = filesystem.exists(path, exists);
    if (!lookup.isOk()) {
        return DbResult::error(DbStatus::IoError,
                               "database path lookup failed: " + lookup.describe());
    }
    if (!exists) {
        return DbResult::error(DbStatus::NotDatabase, "no such database file");
    }

    std::unique_ptr<IDatabaseFile> file = filesystem.createFile();
    if (!file) {
        return DbResult::error(DbStatus::Internal, "file-system provider returned no file");
    }
    const FileOpenMode mode = options.readOnly ? FileOpenMode::ReadOnly
                                               : FileOpenMode::ReadWrite;
    DbResult result = file->open(path, mode, false);
    if (!result.isOk()) {
        return result;
    }
    return DatabaseFile::open(std::move(file), options, out);
}

DbResult DatabaseEngine::inspectDatabase(const std::string& path,
                                         DatabaseDiagnostics& out) {
    DatabaseOpenOptions options;
    options.readOnly = true;
    options.validateAllPages = true;

    std::unique_ptr<Database> database;
    DbResult result = Database::open(path, options, database);
    if (result.isOk() && database) {
        out = database->diagnostics();
        database->close();
        return DbResult::ok();
    }

    out.reset();
    out.state = result.status();
    out.lastValidationFailure = result.describe();
    return result;
}

} // namespace db
} // namespace gxos
