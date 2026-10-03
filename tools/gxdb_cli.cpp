// guideXOS SQL -- Phase SQL1/SQL4
// Small command-line helper for creating, inspecting and querying .gxdb files.
// This is a diagnostics aid for later Database Manager / Developer Studio
// tooling, not a database manager UI.
//
//   gxdb_cli create <path> [pageSize]
//   gxdb_cli inspect <path>
//   gxdb_cli sql <path> "<sql>"
//   gxdb_cli run <path> <sqlFile>
//   gxdb_cli shell <path>
//
// The SQL commands are thin wrappers over the same SqlEngine used by the
// library; no parsing or execution logic lives here. Formatting is deliberately
// simple and lives only in the CLI.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <sys/stat.h>

#include "database_engine.h"
#include "database_file.h"
#include "database_format.h"
#include "database_relational.h"
#include "database_sql.h"

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

std::string formatValue(const DbValue& value) {
    if (value.isNull()) {
        return "NULL";
    }
    switch (value.type()) {
    case DbType::Boolean:
        return value.booleanValue() ? "true" : "false";
    case DbType::Int32:
        return std::to_string(static_cast<long long>(value.int32Value()));
    case DbType::Int64:
        return std::to_string(static_cast<long long>(value.int64Value()));
    case DbType::Float64: {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.10g", value.float64Value());
        return buffer;
    }
    case DbType::Text:
        return value.textValue();
    case DbType::Blob: {
        const std::vector<uint8_t>& bytes = value.blobValue();
        std::string out = "0x";
        const size_t limit = bytes.size() < 16 ? bytes.size() : 16;
        for (size_t i = 0; i < limit; ++i) {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%02X", bytes[i]);
            out += hex;
        }
        if (bytes.size() > limit) {
            out += "...";
        }
        return out;
    }
    default:
        return "?";
    }
}

void printResultSet(const SqlResultSet& rs) {
    if (!rs.hasResult()) {
        return;
    }
    std::string header;
    std::string separator;
    for (size_t i = 0; i < rs.columnCount(); ++i) {
        if (i != 0) {
            header += " | ";
            separator += "-+-";
        }
        header += rs.column(i).name;
        separator += std::string(rs.column(i).name.size(), '-');
    }
    std::printf("%s\n%s\n", header.c_str(), separator.c_str());
    for (size_t r = 0; r < rs.rowCount(); ++r) {
        std::string line;
        for (size_t c = 0; c < rs.columnCount(); ++c) {
            if (c != 0) {
                line += " | ";
            }
            line += formatValue(rs.value(r, c));
        }
        std::printf("%s\n", line.c_str());
    }
    std::printf("(%llu row%s)\n", static_cast<unsigned long long>(rs.rowCount()),
                rs.rowCount() == 1 ? "" : "s");
}

void printStatementResult(const SqlStatementResult& sr) {
    if (sr.ok) {
        if (sr.resultSet.hasResult()) {
            printResultSet(sr.resultSet);
        } else if (sr.type == SqlStatementType::CreateTable) {
            std::printf("OK: created table '%s'\n", sr.objectName.c_str());
        } else if (sr.type == SqlStatementType::Insert) {
            std::printf("OK: inserted %llu row%s\n",
                        static_cast<unsigned long long>(sr.affectedRows),
                        sr.affectedRows == 1 ? "" : "s");
        } else if (sr.type == SqlStatementType::Update) {
            std::printf("OK: updated %llu row%s\n",
                        static_cast<unsigned long long>(sr.affectedRows),
                        sr.affectedRows == 1 ? "" : "s");
        } else if (sr.type == SqlStatementType::Delete) {
            std::printf("OK: deleted %llu row%s\n",
                        static_cast<unsigned long long>(sr.affectedRows),
                        sr.affectedRows == 1 ? "" : "s");
        } else {
            std::printf("OK: %s\n", sqlStatementTypeName(sr.type));
        }
        return;
    }
    std::printf("ERROR [%s]: %s\n", sqlErrorCodeName(sr.error.code),
                sr.error.message.c_str());
    if (sr.error.hasLocation) {
        std::printf("       at line %u, column %u\n",
                    static_cast<unsigned>(sr.error.line),
                    static_cast<unsigned>(sr.error.column));
    }
}

void printExecutionResult(const SqlExecutionResult& result) {
    if (result.statements.empty()) {
        if (result.ok) {
            std::printf("OK: no statements\n");
        } else {
            std::printf("ERROR [%s]: %s\n", sqlErrorCodeName(result.error.code),
                        result.error.message.c_str());
            if (result.error.hasLocation) {
                std::printf("       at line %u, column %u\n",
                            static_cast<unsigned>(result.error.line),
                            static_cast<unsigned>(result.error.column));
            }
        }
        return;
    }
    for (size_t i = 0; i < result.statements.size(); ++i) {
        printStatementResult(result.statements[i]);
    }
}

bool readSqlFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0 || static_cast<size_t>(size) > kSqlMaxInputBytes) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    size_t read = size > 0 ? std::fread(&out[0], 1, out.size(), f) : 0;
    std::fclose(f);
    return read == out.size();
}

std::unique_ptr<Database> openOrCreate(const std::string& path) {
    std::unique_ptr<Database> db;
    DbResult result = Database::open(path, DatabaseOpenOptions(), db);
    if (result.isOk()) {
        return db;
    }
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        result = Database::create(path, DatabaseCreateOptions(), db);
        if (result.isOk()) {
            return db;
        }
    }
    std::printf("open failed: %s\n", result.describe().c_str());
    return nullptr;
}

int runSql(const std::string& path, const std::string& sql) {
    std::unique_ptr<Database> db = openOrCreate(path);
    if (!db) {
        return 1;
    }
    int exitCode = 0;
    {
        SqlEngine engine(*db);
        SqlExecutionResult result = engine.execute(sql);
        printExecutionResult(result);
        exitCode = result.ok ? 0 : 1;
    }
    db->close();
    return exitCode;
}

int runShell(const std::string& path) {
    std::unique_ptr<Database> db = openOrCreate(path);
    if (!db) {
        return 1;
    }
    int exitCode = 0;
    {
        SqlEngine engine(*db);
        std::string buffer;
        char line[4096];
        std::printf("guideXOS SQL shell. End a statement with ';'. Type 'exit' to quit.\n");
        while (true) {
            std::printf(buffer.empty() ? "gxsql> " : "    -> ");
            if (std::fgets(line, sizeof(line), stdin) == nullptr) {
                break;
            }
            std::string text(line);
            while (!text.empty() && (text[text.size() - 1] == '\n' ||
                                     text[text.size() - 1] == '\r')) {
                text.erase(text.size() - 1);
            }
            if (buffer.empty() && (text == "exit" || text == "quit")) {
                break;
            }
            buffer += text;
            buffer += "\n";
            if (text.find(';') == std::string::npos) {
                continue;
            }
            SqlExecutionResult result = engine.execute(buffer);
            printExecutionResult(result);
            if (!result.ok) {
                exitCode = 1;
            }
            buffer.clear();
        }
    }
    db->close();
    return exitCode;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf(
            "usage:\n"
            "  gxdb_cli create <path> [pageSize]\n"
            "  gxdb_cli inspect <path>\n"
            "  gxdb_cli sql <path> \"<sql>\"\n"
            "  gxdb_cli run <path> <sqlFile>\n"
            "  gxdb_cli shell <path>\n");
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

    if (command == "sql") {
        if (argc < 4) {
            std::printf("usage: gxdb_cli sql <path> \"<sql>\"\n");
            return 2;
        }
        return runSql(path, argv[3]);
    }

    if (command == "run") {
        if (argc < 4) {
            std::printf("usage: gxdb_cli run <path> <sqlFile>\n");
            return 2;
        }
        std::string sql;
        if (!readSqlFile(argv[3], sql)) {
            std::printf("cannot read SQL file (or it exceeds the size limit): %s\n",
                        argv[3]);
            return 1;
        }
        return runSql(path, sql);
    }

    if (command == "shell") {
        return runShell(path);
    }

    std::printf("unknown command: %s\n", command.c_str());
    return 2;
}
