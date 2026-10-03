#pragma once
// guideXOS SQL -- Phase SQL1
// DatabaseEngine: public entry points for creating, opening and inspecting a
// native .gxdb database using the hosted file backend.
//
// This is the only layer that knows about a filesystem path. It constructs an
// IDatabaseFile and delegates all format/page logic to DatabaseFile.

#include <memory>
#include <string>

#include "database_diagnostics.h"
#include "database_file.h"
#include "database_result.h"

namespace gxos {
namespace db {

class DatabaseEngine {
public:
    // `fileSystem` selects the storage backend. A null value uses the hosted
    // stdio backend. Supplying a provider enables the native VFS boundary and
    // hosted crash injection.
    static DbResult createDatabase(const std::string& path,
                                   const DatabaseCreateOptions& options,
                                   std::unique_ptr<DatabaseFile>& out,
                                   IDatabaseFileSystem* fileSystem = nullptr);

    static DbResult openDatabase(const std::string& path,
                                 const DatabaseOpenOptions& options,
                                 std::unique_ptr<DatabaseFile>& out,
                                 IDatabaseFileSystem* fileSystem = nullptr);

    // Opens read-only with full page validation and returns diagnostics. On
    // failure `out` still receives a best-effort description of the fault.
    static DbResult inspectDatabase(const std::string& path,
                                    DatabaseDiagnostics& out);
};

} // namespace db
} // namespace gxos
