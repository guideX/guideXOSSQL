#include "database_relational.h"

#include <cstring>

#include "database_endian.h"
#include "database_engine.h"
#include "database_heap.h"
#include "database_transaction.h"

namespace gxos {
namespace db {

namespace {

const uint32_t kDefaultBufferCapacity = 32;

} // namespace

Database::Database()
    : _file(), _buffer(), _wal(), _catalog(), _diagnostics(), _open(false),
      _readOnly(false), _catalogDirty(false), _activeTx(nullptr), _nextTxId(1),
      _dataEpoch(0), _walPath(), _fileSystem(nullptr) {}

Database::~Database() {
    if (_open) {
        close();
    }
}

void Database::markCatalogDirty() {
    _catalogDirty = true;
    if (_activeTx != nullptr) {
        _activeTx->markCatalogChanged();
    }
}

void Database::refreshBufferDiagnostics() {
    if (_buffer) {
        _diagnostics.bufferCapacity = _buffer->capacity();
        _diagnostics.bufferResident = _buffer->residentCount();
        _diagnostics.bufferDirty = _buffer->dirtyCount();
    } else {
        _diagnostics.bufferCapacity = 0;
        _diagnostics.bufferResident = 0;
        _diagnostics.bufferDirty = 0;
    }
    _diagnostics.transactionActive = (_activeTx != nullptr);
    _diagnostics.transactionId = _activeTx ? _activeTx->id() : 0;
    _diagnostics.transactionModifiedPages =
        _activeTx ? _activeTx->modifiedPageCount() : 0;
}

uint32_t Database::bufferCapacity() const {
    return _buffer ? _buffer->capacity() : 0;
}

uint32_t Database::pageSize() const {
    return _file ? _file->pageSize() : 0;
}

uint64_t Database::pageCount() const {
    return _file ? _file->pageCount() : 0;
}

PageAccess& Database::pages() {
    if (_activeTx != nullptr) {
        return *static_cast<PageAccess*>(_activeTx);
    }
    return *this;
}

DbResult Database::readPage(uint64_t pageId, DatabasePage& out) {
    if (!_buffer) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    BufferSlot* slot = nullptr;
    DbResult result = _buffer->pinPage(pageId, slot);
    if (!result.isOk()) {
        return result;
    }
    out = slot->page;
    _buffer->unpin(*slot);
    return DbResult::ok();
}

DbResult Database::writePage(const DatabasePage& page) {
    if (!_buffer) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    return _buffer->installPage(page.pageId, page);
}

DbResult Database::allocatePage(PageType, uint64_t&) {
    return DbResult::error(DbStatus::Internal,
                           "the committed page view cannot allocate pages");
}

DbResult Database::openAndRecoverWal(IDatabaseFileSystem& fileSystem) {
    _diagnostics.walPresent = fileSystem.exists(_walPath);
    if (!_diagnostics.walPresent) {
        _diagnostics.walState = WalState::Absent;
        _nextTxId = 1;
        return DbResult::ok();
    }

    std::unique_ptr<IDatabaseFile> walFile = fileSystem.createFile();
    const FileOpenMode mode = _readOnly ? FileOpenMode::ReadOnly : FileOpenMode::ReadWrite;
    DbResult result = walFile->open(_walPath, mode, false);
    if (!result.isOk()) {
        return result;
    }

    _wal.reset(new WriteAheadLog());
    result = _wal->open(std::move(walFile), _file->header().databaseId,
                        _file->pageSize(), _readOnly);
    if (!result.isOk()) {
        _wal.reset();
        return result;
    }

    WalState state = WalState::Clean;
    std::vector<WalTransaction> transactions;
    std::string message;
    result = _wal->scan(state, transactions, message);
    _diagnostics.walState = state;
    if (!result.isOk()) {
        _diagnostics.lastRecoveryResult = message;
        _wal.reset();
        return result;
    }

    _nextTxId = _wal->lastTransactionId() + 1;

    if (state == WalState::Committed) {
        if (_readOnly) {
            _diagnostics.recoveryRequired = true;
            _wal.reset();
            return DbResult::error(DbStatus::RecoveryRequired,
                                   "a committed WAL requires recovery");
        }
        result = applyRecovery(transactions);
        if (!result.isOk()) {
            _wal.reset();
            return DbResult::error(DbStatus::RecoveryFailed, result.message());
        }
    } else if ((state == WalState::Incomplete || state == WalState::Clean) && !_readOnly) {
        // Discard any incomplete tail and normalize the header.
        result = _wal->checkpoint();
        if (!result.isOk()) {
            _wal.reset();
            return result;
        }
        _diagnostics.walState = WalState::Clean;
    }
    return DbResult::ok();
}

DbResult Database::ensureWalOpen() {
    if (_wal && _wal->isOpen()) {
        return DbResult::ok();
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    IDatabaseFileSystem& filesystem = _fileSystem ? *_fileSystem : defaultFileSystem();
    std::unique_ptr<IDatabaseFile> walFile = filesystem.createFile();
    const bool exists = filesystem.exists(_walPath);
    const FileOpenMode mode = exists ? FileOpenMode::ReadWrite : FileOpenMode::Create;
    DbResult result = walFile->open(_walPath, mode, mode == FileOpenMode::Create);
    if (!result.isOk()) {
        return result;
    }
    _wal.reset(new WriteAheadLog());
    result = _wal->open(std::move(walFile), _file->header().databaseId,
                        _file->pageSize(), false);
    if (!result.isOk()) {
        _wal.reset();
        return result;
    }
    return DbResult::ok();
}

DbResult Database::applyRecovery(const std::vector<WalTransaction>& transactions) {
    uint64_t redone = 0;
    for (size_t i = 0; i < transactions.size(); ++i) {
        const WalTransaction& tx = transactions[i];
        if (tx.hasHeaderImage) {
            DbResult result = _file->applyHeaderImage(tx.headerImage);
            if (!result.isOk()) {
                return result;
            }
        }
        for (size_t j = 0; j < tx.pages.size(); ++j) {
            DbResult result = _file->writePage(tx.pages[j]);
            if (!result.isOk()) {
                return result;
            }
            ++redone;
        }
    }
    DbResult result = _file->flush();
    if (!result.isOk()) {
        return result;
    }
    result = _wal->checkpoint();
    if (!result.isOk()) {
        return result;
    }
    _diagnostics.walState = WalState::Clean;
    _diagnostics.recoveryRequired = false;
    _diagnostics.pagesRedone = redone;
    _diagnostics.lastRecoveryResult = "redo applied";
    return DbResult::ok();
}

DbResult Database::openImpl(const std::string& path, const DatabaseOpenOptions& options,
                            const DatabaseCreateOptions& createOptions, bool create,
                            IDatabaseFileSystem* fileSystem) {
    IDatabaseFileSystem& filesystem = fileSystem ? *fileSystem : defaultFileSystem();
    _fileSystem = &filesystem;
    const uint32_t bufferCapacity =
        options.bufferCapacity != 0 ? options.bufferCapacity : kDefaultBufferCapacity;

    if (create) {
        DbResult result = DatabaseEngine::createDatabase(path, createOptions, _file, &filesystem);
        if (!result.isOk()) {
            return result;
        }
    } else {
        DbResult result = DatabaseEngine::openDatabase(path, options, _file, &filesystem);
        if (!result.isOk()) {
            return result;
        }
    }

    _buffer.reset(new BufferManager(*_file, bufferCapacity));
    _readOnly = options.readOnly;
    _walPath = walPathFor(path);

    if (create) {
        // A freshly created database has a new identity; any sidecar WAL from a
        // previous occupant is stale and must not be replayed.
        filesystem.remove(_walPath);
    }

    DbResult result = openAndRecoverWal(filesystem);
    if (!result.isOk()) {
        _wal.reset();
        _buffer.reset();
        _file->close();
        _file.reset();
        return result;
    }

    result = _catalog.load(*this, _file->rootPageId(), _file->pageSize(), _file->pageCount());
    if (!result.isOk()) {
        _wal.reset();
        _buffer.reset();
        _file->close();
        _file.reset();
        return result;
    }

    _open = true;
    _catalogDirty = false;
    refreshDiagnostics();
    return DbResult::ok();
}

DbResult Database::create(const std::string& path, const DatabaseCreateOptions& options,
                          std::unique_ptr<Database>& out, IDatabaseFileSystem* fileSystem) {
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty database path");
    }
    if (!isSupportedPageSize(options.pageSize)) {
        return DbResult::error(DbStatus::InvalidArgument, "unsupported page size");
    }

    std::unique_ptr<Database> db(new Database());
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = options.pageSize;
    createOptions.overwriteExisting = options.overwriteExisting;
    createOptions.generateDatabaseId = options.generateDatabaseId;
    std::memcpy(createOptions.databaseId, options.databaseId, 16);
    createOptions.creationTimeUnixNanos = options.creationTimeUnixNanos;
    createOptions.bufferCapacity = options.bufferCapacity;

    DatabaseOpenOptions createOpen;
    createOpen.bufferCapacity = options.bufferCapacity;
    DbResult result = db->openImpl(path, createOpen, createOptions, true, fileSystem);
    if (!result.isOk()) {
        return result;
    }
    out = std::move(db);
    return DbResult::ok();
}

DbResult Database::open(const std::string& path, const DatabaseOpenOptions& options,
                        std::unique_ptr<Database>& out, IDatabaseFileSystem* fileSystem) {
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty database path");
    }

    std::unique_ptr<Database> db(new Database());
    DbResult result = db->openImpl(path, options, DatabaseCreateOptions(), false, fileSystem);
    if (!result.isOk()) {
        return result;
    }
    out = std::move(db);
    return DbResult::ok();
}

DbResult Database::close() {
    if (!_open) {
        return DbResult::ok();
    }
    DbResult result = DbResult::ok();
    if (!_readOnly) {
        if (_activeTx != nullptr) {
            // Never leave an uncommitted transaction dangling: discard it.
            Transaction* tx = _activeTx;
            _activeTx = nullptr;
            tx->rollback();
        }
        result = flush();
    }
    _activeTx = nullptr;
    _buffer.reset();
    if (_wal) {
        _wal->close();
        _wal.reset();
    }
    DbResult closed = _file->close();
    _file.reset();
    _open = false;
    _catalogDirty = false;
    if (!result.isOk()) {
        return result;
    }
    return closed;
}

DbResult Database::flush() {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::ok();
    }
    if (_activeTx != nullptr) {
        return DbResult::error(DbStatus::TransactionAlreadyActive,
                               "cannot flush while a transaction is active");
    }

    DbResult result = _buffer->flush();
    if (!result.isOk()) {
        return result;
    }
    if (_wal && _wal->isOpen()) {
        result = _wal->checkpoint();
        if (!result.isOk()) {
            return result;
        }
        _diagnostics.walState = WalState::Clean;
    }
    _diagnostics.recoveryRequired = false;
    _catalogDirty = false;
    refreshDiagnostics();
    return DbResult::ok();
}

DbResult Database::beginTransaction(std::unique_ptr<Transaction>& out) {
    out.reset();
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    if (_activeTx != nullptr) {
        return DbResult::error(DbStatus::TransactionAlreadyActive,
                               "a writer transaction is already active");
    }

    DbResult walResult = ensureWalOpen();
    if (!walResult.isOk()) {
        return walResult;
    }

    // Bound the WAL: checkpoint before starting a new transaction when the log
    // has grown large, so it cannot grow without limit.
    if (_wal && _wal->isOpen() && _wal->bytesSinceCheckpoint() >= kAutoCheckpointWalBytes) {
        DbResult flushed = flush();
        if (!flushed.isOk()) {
            return flushed;
        }
    }

    const uint64_t basePageCount = _file->pageCount();
    std::unique_ptr<Transaction> tx(new Transaction(*this, _nextTxId++, basePageCount));
    _activeTx = tx.get();
    out = std::move(tx);
    refreshDiagnostics();
    return DbResult::ok();
}

void Database::finishTransaction(Transaction* tx, bool committed) {
    if (_activeTx == tx) {
        _activeTx = nullptr;
    }
    if (!committed) {
        ++_dataEpoch;
    }
    refreshDiagnostics();
}

DbResult Database::createTable(const TableDefinition& def, uint32_t& outTableId) {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    if (_activeTx != nullptr) {
        return _activeTx->createTable(def, outTableId);
    }

    std::unique_ptr<Transaction> tx;
    DbResult result = beginTransaction(tx);
    if (!result.isOk()) {
        return result;
    }
    result = tx->createTable(def, outTableId);
    if (!result.isOk()) {
        tx->rollback();
        return result;
    }
    return tx->commit();
}

DbResult Database::listTables(std::vector<TableInfo>& out) const {
    out.clear();
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const std::vector<Catalog::TableRecord>& tables = _catalog.tables();
    out.reserve(tables.size());
    for (size_t i = 0; i < tables.size(); ++i) {
        TableInfo info;
        info.tableId = tables[i].tableId;
        info.name = tables[i].name;
        info.columnCount = static_cast<uint32_t>(tables[i].columns.size());
        info.heapPageCount = tables[i].heapPageCount;
        info.rowCount = tables[i].rowCount;
        out.push_back(info);
    }
    return DbResult::ok();
}

DbResult Database::describeTable(const std::string& name, TableDefinition& out) const {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const Catalog::TableRecord* rec = _catalog.findTable(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such table");
    }
    out.name = rec->name;
    out.columns = rec->columns;
    return DbResult::ok();
}

DbResult Database::openTable(const std::string& name, std::unique_ptr<Table>& out) {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const Catalog::TableRecord* rec = _catalog.findTable(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such table");
    }
    out.reset(new Table(*this, rec->tableId));
    return DbResult::ok();
}

void Database::refreshDiagnostics() {
    _diagnostics.open = _open;
    _diagnostics.readOnly = _readOnly;
    if (_file) {
        _diagnostics.formatMajor = _file->header().formatMajor;
        _diagnostics.formatMinor = _file->header().formatMinor;
        _diagnostics.pageSize = _file->pageSize();
        _diagnostics.pageCount = _file->pageCount();
        _diagnostics.rootPageId = _file->rootPageId();
        _diagnostics.databaseId = formatDatabaseId(_file->header().databaseId);
        _diagnostics.fileSizeBytes = _file->diagnostics().fileSizeBytes;
    }
    _diagnostics.tableCount = _catalog.tableCount();
    _diagnostics.catalogPageCount = _catalog.catalogPageCount();
    if (_buffer) {
        _diagnostics.bufferCapacity = _buffer->capacity();
        _diagnostics.bufferResident = _buffer->residentCount();
        _diagnostics.bufferDirty = _buffer->dirtyCount();
    } else {
        _diagnostics.bufferCapacity = 0;
        _diagnostics.bufferResident = 0;
        _diagnostics.bufferDirty = 0;
    }

    _diagnostics.transactionActive = (_activeTx != nullptr);
    _diagnostics.transactionId = _activeTx ? _activeTx->id() : 0;
    _diagnostics.transactionModifiedPages =
        _activeTx ? _activeTx->modifiedPageCount() : 0;
    _diagnostics.walPresent = (_wal != nullptr && _wal->isOpen());
    _diagnostics.walBytes = _wal ? _wal->size() : 0;

    _diagnostics.tables.clear();
    const std::vector<Catalog::TableRecord>& tables = _catalog.tables();
    for (size_t i = 0; i < tables.size(); ++i) {
        TableDiagnostics td;
        td.tableId = tables[i].tableId;
        td.name = tables[i].name;
        td.columnCount = static_cast<uint32_t>(tables[i].columns.size());
        td.heapPageCount = tables[i].heapPageCount;
        td.rowCount = tables[i].rowCount;
        _diagnostics.tables.push_back(td);
    }
    if (_file) {
        _diagnostics.state = _file->diagnostics().state;
    }
}

} // namespace db
} // namespace gxos
