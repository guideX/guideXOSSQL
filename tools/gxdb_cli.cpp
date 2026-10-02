// guideXOS SQL -- Phase SQL1
// Small command-line helper for creating and inspecting .gxdb files.
// This is a diagnostics aid for later Database Manager / Developer Studio
// tooling, not a database manager UI.
//
//   gxdb_cli create <path> [pageSize]
//   gxdb_cli inspect <path>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "database_engine.h"
#include "database_file.h"
#include "database_format.h"
#include "database_relational.h"

using namespace gxos::db;

namespace {

void printDiagnostics(const DatabaseDiagnostics& d, DbStatus status) {
    std::printf("state:               %s\n", dbStatusName(status));
    std::printf("open:                %s\n", d.open ? "yes" : "no");
    std::printf("read-only:           %s\n", d.readOnly ? "yes" : "no");
    std::printf("format version:      %u.%u\n", static_cast<unsigned>(d.formatMajor),
                static_cast<unsigned>(d.formatMinor));
    std::printf("page size:           %u\n", d.pageSize);
    std::printf("page count:          %llu\n", static_cast<unsigned long long>(d.pageCount));
    std::printf("root page id:        %llu\n", static_cast<unsigned long long>(d.rootPageId));
    std::printf("file size (bytes):   %llu\n",
                static_cast<unsigned long long>(d.fileSizeBytes));
    std::printf("database identity:   %s\n", d.databaseId.c_str());
    std::printf("table count:         %u\n", static_cast<unsigned>(d.tableCount));
    std::printf("catalog pages:       %u\n", static_cast<unsigned>(d.catalogPageCount));
    std::printf("buffer capacity:     %u\n", static_cast<unsigned>(d.bufferCapacity));
    std::printf("buffer resident:     %u\n", static_cast<unsigned>(d.bufferResident));
    std::printf("buffer dirty:        %u\n", static_cast<unsigned>(d.bufferDirty));
    for (size_t i = 0; i < d.tables.size(); ++i) {
        const TableDiagnostics& t = d.tables[i];
        std::printf("  table [%u] \"%s\": %u columns, %u heap pages, %llu rows\n",
                    static_cast<unsigned>(t.tableId), t.name.c_str(),
                    static_cast<unsigned>(t.columnCount),
                    static_cast<unsigned>(t.heapPageCount),
                    static_cast<unsigned long long>(t.rowCount));
    }
    if (!d.lastValidationFailure.empty()) {
        std::printf("last validation:     %s\n", d.lastValidationFailure.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage:\n  gxdb_cli create <path> [pageSize]\n  gxdb_cli inspect <path>\n");
        return 2;
    }

    const std::string command = argv[1];
    const std::string path = argv[2];

    if (command == "create") {
        DatabaseCreateOptions options;
        if (argc >= 4) {
            options.pageSize = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
        }
        std::unique_ptr<Database> db;
        DbResult result = Database::create(path, options, db);
        if (!result.isOk()) {
            std::printf("create failed: %s\n", result.describe().c_str());
            return 1;
        }
        printDiagnostics(db->diagnostics(), DbStatus::Ok);
        db->close();
        return 0;
    }

    if (command == "inspect") {
        DatabaseDiagnostics diagnostics;
        DbResult result = DatabaseEngine::inspectDatabase(path, diagnostics);
        printDiagnostics(diagnostics, result.status());
        if (!result.isOk()) {
            std::printf("inspect failed: %s\n", result.describe().c_str());
            return 1;
        }
        return 0;
    }

    std::printf("unknown command: %s\n", command.c_str());
    return 2;
}
