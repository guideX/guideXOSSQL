#pragma once
// guideXOS SQL -- Phase SQL2
// Database: the relational facade over DatabaseFile + BufferManager + Catalog.
//
//   DatabaseEngine
//       |
//   Database
//       |
//       +-- DatabaseFile   (page I/O, validation, durability)
//       +-- BufferManager  (bounded page cache)
//       +-- Catalog        (durable table/column metadata)
//       +-- HeapTable       (row storage, insert, scan)
//
// The relational API is the execution target for the future SQL layer. It
// deliberately contains no parser, planner or SQL token concepts.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "database_catalog.h"
#include "database_diagnostics.h"
#include "database_file.h"
#include "database_result.h"
#include "database_schema.h"

namespace gxos {
namespace db {

class Table;

class Database {
public:
    // Creates a new database with an empty relational catalog.
    static DbResult create(const std::string& path,
                            const DatabaseCreateOptions& options,
                            std::unique_ptr<Database>& out);

    // Opens an existing database (SQL1 or SQL2) and loads its catalog.
    static DbResult open(const std::string& path,
                          const DatabaseOpenOptions& options,
                          std::unique_ptr<Database>& out);

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Flushes dirty buffer pages and the catalog, then closes the file.
    DbResult close();

    // Writes all dirty pages and the catalog to the file and requests a
    // durability barrier.
    DbResult flush();

    bool isOpen() const { return _open; }
    bool isReadOnly() const { return _readOnly; }

    // Relational API.
    DbResult createTable(const TableDefinition& def, uint32_t& outTableId);
    DbResult listTables(std::vector<TableInfo>& out) const;
    DbResult describeTable(const std::string& name, TableDefinition& out) const;
    DbResult openTable(const std::string& name, std::unique_ptr<Table>& out);

    const DatabaseDiagnostics& diagnostics() const { return _diagnostics; }

    // Accessors for the relational layer.
    DatabaseFile& file() { return *_file; }
    BufferManager& buffer() { return *_buffer; }
    Catalog& catalog() { return _catalog; }
    uint32_t bufferCapacity() const;

    // Marks the catalog as needing a durable re-save on the next flush().
    void markCatalogDirty();

    // Updates only the buffer-related diagnostics fields (cheap; safe to call
    // after every relational operation).
    void refreshBufferDiagnostics();

private:
    Database();
    DbResult openImpl(const std::string& path, const DatabaseOpenOptions& options,
                      const DatabaseCreateOptions& createOptions, bool create);
    void refreshDiagnostics();

    std::unique_ptr<DatabaseFile> _file;
    std::unique_ptr<BufferManager> _buffer;
    Catalog _catalog;
    DatabaseDiagnostics _diagnostics;
    bool _open;
    bool _readOnly;
    bool _catalogDirty;
};

} // namespace db
} // namespace gxos
