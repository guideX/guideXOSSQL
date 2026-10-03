#pragma once
// guideXOS SQL -- Phase SQL2/SQL3
// Database: the relational facade over DatabaseFile + BufferManager + Catalog,
// now with crash-atomic transactions backed by a sidecar write-ahead log.
//
//   DatabaseEngine
//       |
//   Database  (also a PageAccess: the committed view)
//       |
//       +-- DatabaseFile      (page I/O, validation, durability)
//       +-- BufferManager     (bounded committed page cache)
//       +-- Catalog           (durable table/column metadata)
//       +-- HeapTable         (row storage, insert, scan)
//       +-- WriteAheadLog     (sidecar redo log, SQL3)
//       +-- Transaction       (single-writer private overlay, SQL3)
//
// The relational API is the execution target for the future SQL layer. It
// deliberately contains no parser, planner or SQL token concepts.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "database_buffer.h"
#include "database_catalog.h"
#include "database_diagnostics.h"
#include "database_file.h"
#include "database_pagestore.h"
#include "database_result.h"
#include "database_schema.h"
#include "database_wal.h"

namespace gxos {
namespace db {

class Table;
class Transaction;

class Database : public PageAccess {
public:
    // Creates a new database with an empty relational catalog.
    static DbResult create(const std::string& path,
                            const DatabaseCreateOptions& options,
                            std::unique_ptr<Database>& out,
                            IDatabaseFileSystem* fileSystem = nullptr);

    // Opens an existing database (SQL1 or SQL2) and loads its catalog. When a
    // committed WAL is present a writable open replays it; a read-only open
    // returns RecoveryRequired instead of mutating the database.
    static DbResult open(const std::string& path,
                          const DatabaseOpenOptions& options,
                          std::unique_ptr<Database>& out,
                          IDatabaseFileSystem* fileSystem = nullptr);

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Flushes dirty committed buffer pages and the catalog, then checkpoints
    // the WAL. Fails with TransactionAlreadyActive while a transaction is open.
    DbResult close();
    DbResult flush();

    bool isOpen() const { return _open; }
    bool isReadOnly() const override { return _readOnly; }

    // ---- PageAccess (committed view) ------------------------------------
    uint32_t pageSize() const override;
    uint64_t pageCount() const override;
    DbResult readPage(uint64_t pageId, DatabasePage& out) override;
    DbResult writePage(const DatabasePage& page) override;
    DbResult allocatePage(PageType type, uint64_t& outPageId) override;

    // ---- Explicit transactions ------------------------------------------
    // Begins the single active writer transaction. Fails with
    // TransactionAlreadyActive if one is already open. The caller owns the
    // returned transaction and must commit() or rollback() it.
    DbResult beginTransaction(std::unique_ptr<Transaction>& out);
    Transaction* activeTransaction() { return _activeTx; }
    const Transaction* activeTransaction() const { return _activeTx; }
    void finishTransaction(Transaction* tx, bool committed);

    // The page view the relational layer must use: the active transaction when
    // one exists, otherwise the committed view.
    PageAccess& pages();

    // ---- Relational API --------------------------------------------------
    DbResult createTable(const TableDefinition& def, uint32_t& outTableId);
    DbResult listTables(std::vector<TableInfo>& out) const;
    DbResult describeTable(const std::string& name, TableDefinition& out) const;
    DbResult openTable(const std::string& name, std::unique_ptr<Table>& out);

    const DatabaseDiagnostics& diagnostics() const { return _diagnostics; }

    // Accessors for the relational layer and tests.
    DatabaseFile& file() { return *_file; }
    BufferManager& buffer() { return *_buffer; }
    Catalog& catalog() { return _catalog; }
    WriteAheadLog& wal() { return *_wal; }
    uint32_t bufferCapacity() const;

    // Monotonic epoch bumped on rollback so cached heap-chain positions are
    // re-resolved. Committed transactions do not invalidate caches.
    uint64_t dataEpoch() const { return _dataEpoch; }

    // Marks the catalog as needing a durable re-save on commit.
    void markCatalogDirty();

    // Updates only the buffer-related diagnostics fields (cheap; safe to call
    // after every relational operation).
    void refreshBufferDiagnostics();

private:
    Database();
    DbResult openImpl(const std::string& path, const DatabaseOpenOptions& options,
                      const DatabaseCreateOptions& createOptions, bool create,
                      IDatabaseFileSystem* fileSystem);
    DbResult openAndRecoverWal(IDatabaseFileSystem& fileSystem);
    DbResult ensureWalOpen();
    DbResult applyRecovery(const std::vector<WalTransaction>& transactions);
    void refreshDiagnostics();

    std::unique_ptr<DatabaseFile> _file;
    std::unique_ptr<BufferManager> _buffer;
    std::unique_ptr<WriteAheadLog> _wal;
    Catalog _catalog;
    DatabaseDiagnostics _diagnostics;
    bool _open;
    bool _readOnly;
    bool _catalogDirty;
    Transaction* _activeTx;
    uint64_t _nextTxId;
    uint64_t _dataEpoch;
    std::string _walPath;
    IDatabaseFileSystem* _fileSystem;
};

} // namespace db
} // namespace gxos
