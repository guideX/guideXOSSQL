#include "database_engine.h"

#include <cstdio>
#include <utility>

#include "database_endian.h"
#include "database_relational.h"

namespace gxos {
namespace db {

DbResult DatabaseEngine::createDatabase(const std::string& path,
                                        const DatabaseCreateOptions& options,
                                        std::unique_ptr<DatabaseFile>& out) {
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty database path");
    }
    if (!isSupportedPageSize(options.pageSize)) {
        return DbResult::error(DbStatus::InvalidArgument, "unsupported page size");
    }

    const bool existedBefore = hostFileExists(path);

    std::unique_ptr<IDatabaseFile> file(new HostDatabaseFile());
    DbResult result = file->open(path, FileOpenMode::Create, options.overwriteExisting);
    if (!result.isOk()) {
        return result;
    }

    result = DatabaseFile::create(std::move(file), options, out);
    if (!result.isOk() && !existedBefore) {
        // We created a partial file; do not leave a broken .gxdb behind.
        std::remove(path.c_str());
    }
    return result;
}

DbResult DatabaseEngine::openDatabase(const std::string& path,
                                      const DatabaseOpenOptions& options,
                                      std::unique_ptr<DatabaseFile>& out) {
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty database path");
    }
    if (!hostFileExists(path)) {
        return DbResult::error(DbStatus::NotDatabase, "no such database file");
    }

    std::unique_ptr<IDatabaseFile> file(new HostDatabaseFile());
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
