#include "database_relational.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include "database_endian.h"
#include "database_engine.h"
#include "database_heap.h"
#include "database_index.h"
#include "database_transaction.h"

namespace gxos {
namespace db {

namespace {

const uint32_t kDefaultBufferCapacity = 32;

static_assert(std::is_nothrow_move_assignable<DatabasePage>::value,
              "published page views must be installable without allocation");

// Human-readable rendering of a column DEFAULT value for diagnostics.
std::string describeDbValue(const DbValue& value) {
    if (value.isNull()) {
        return "NULL";
    }
    switch (value.type()) {
    case DbType::Boolean:
        return value.booleanValue() ? "TRUE" : "FALSE";
    case DbType::Int32:
        return std::to_string(static_cast<long long>(value.int32Value()));
    case DbType::Int64:
        return std::to_string(static_cast<long long>(value.int64Value()));
    case DbType::Float64: {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", value.float64Value());
        return buffer;
    }
    case DbType::Text:
        return "'" + value.textValue() + "'";
    case DbType::Blob: {
        const std::vector<uint8_t>& bytes = value.blobValue();
        std::string out = "X'";
        static const char* hex = "0123456789ABCDEF";
        for (size_t i = 0; i < bytes.size(); ++i) {
            out.push_back(hex[(bytes[i] >> 4) & 0xF]);
            out.push_back(hex[bytes[i] & 0xF]);
        }
        out += "'";
        return out;
    }
    default:
        return "?";
    }
}

} // namespace

Database::Database()
    : _file(), _buffer(), _wal(), _catalog(), _diagnostics(), _open(false),
      _readOnly(false), _catalogDirty(false), _walRecordAllocator(),
      _hasWalRecordAllocator(false), _transactionPageAllocator(),
      _hasTransactionPageAllocator(false), _activeTx(nullptr), _nextTxId(1),
      _transactionIdsExhausted(false), _dataEpoch(0), _schemaGeneration(1),
      _walPath(), _fileSystem(nullptr) {}

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

const GxAllocator* Database::scratchAllocator() const {
    if (_activeTx != nullptr) return &_activeTx->pageAllocator();
    return _buffer ? &_buffer->scratchAllocator() : nullptr;
}

DbResult Database::readPage(uint64_t pageId, DatabasePage& out) {
    if (!_buffer) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_diagnostics.recoveryRequired) {
        return DbResult::error(DbStatus::RecoveryRequired,
                               "committed page publication requires WAL recovery");
    }

    // Read the committed encoded image through the checked owner API. This
    // temporary uses the cache allocator only for this call; the owned bytes
    // are destroyed here and never escape with the database's allocator.
    GxOwnedPage encoded;
    const GxStatus ownerStatus = encoded.init(
        &_buffer->scratchAllocator(), pageSize());
    if (ownerStatus != GxStatus::Ok) {
        const DbStatus status =
            ownerStatus == GxStatus::OutOfMemory
                ? DbStatus::NoSpace
                : (ownerStatus == GxStatus::ResourceLimit ||
                           ownerStatus == GxStatus::Overflow
                       ? DbStatus::TransactionTooLarge
                       : DbStatus::Internal);
        return DbResult::error(status, "temporary page owner setup failed");
    }
    DbResult result = readOwnedPage(pageId, encoded);
    if (!result.isOk()) return result;

    DatabasePage prepared;
    try {
        result = DatabasePage::parse(encoded.encodedBytes(),
                                     encoded.byteCount(), encoded.pageSize(),
                                     pageId, prepared);
    } catch (const std::bad_alloc&) {
        return DbResult::error(
            DbStatus::NoSpace,
            "hosted compatibility page payload allocation failed");
    }
    if (!result.isOk()) return result;

    // The compatibility vector remains hosted, but is prepared and validated
    // before this nonallocating publication into the caller's destination.
    out = std::move(prepared);
    return DbResult::ok();
}

DbResult Database::readOwnedPage(uint64_t pageId, GxOwnedPage& out) {
    if (!_buffer) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_diagnostics.recoveryRequired) {
        return DbResult::error(DbStatus::RecoveryRequired,
                               "committed page publication requires WAL recovery");
    }
    return _buffer->readOwnedPage(pageId, out);
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

DbResult Database::installCommittedPageImage(uint64_t pageId,
                                             const uint8_t* image,
                                             size_t imageSize) {
    if (!_buffer) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    return _buffer->installPage(pageId, image, imageSize);
}

DbResult Database::allocatePage(PageType, uint64_t&) {
    return DbResult::error(DbStatus::Internal,
                           "the committed page view cannot allocate pages");
}

DbResult Database::openAndRecoverWal(IDatabaseFileSystem& fileSystem,
                                     bool validateAllPages) {
    try {
    DbResult lookup = fileSystem.exists(_walPath, _diagnostics.walPresent);
    if (!lookup.isOk()) {
        return DbResult::error(DbStatus::IoError,
                               "WAL path lookup failed: " + lookup.describe());
    }
    if (!_diagnostics.walPresent) {
        _diagnostics.walState = WalState::Absent;
        _nextTxId = 1;
        _transactionIdsExhausted = false;
        return _file->validateOpenedPages(validateAllPages);
    }

    std::unique_ptr<IDatabaseFile> walFile = fileSystem.createFile();
    if (!walFile) {
        return DbResult::error(DbStatus::Internal, "file-system provider returned no WAL file");
    }
    const FileOpenMode mode = _readOnly ? FileOpenMode::ReadOnly : FileOpenMode::ReadWrite;
    DbResult result = walFile->open(_walPath, mode, false);
    if (!result.isOk()) {
        return result;
    }

    if (_hasWalRecordAllocator) {
        _wal.reset(new WriteAheadLog(_walRecordAllocator));
    } else {
        _wal.reset(new WriteAheadLog());
    }
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

    if (_wal->lastTransactionId() == UINT64_MAX) {
        _nextTxId = UINT64_MAX;
        _transactionIdsExhausted = true;
    } else {
        _nextTxId = _wal->lastTransactionId() + 1;
        _transactionIdsExhausted = false;
    }

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
            if (result.status() == DbStatus::NoSpace ||
                result.status() == DbStatus::TransactionTooLarge ||
                result.status() == DbStatus::IoError ||
                result.status() == DbStatus::RecoveryRequired ||
                result.status() == DbStatus::WalCorrupt) {
                return result;
            }
            return DbResult::error(DbStatus::RecoveryFailed, result.message());
        }
    } else if (state == WalState::Incomplete && !_readOnly) {
        // Do not discard an incomplete WAL until the base file is independently
        // valid. In particular, a partial page with no complete committed redo
        // image is not evidence that its bytes may be dropped.
        result = _file->validateOpenedPages(validateAllPages);
        if (!result.isOk()) {
            return result;
        }
        result = _wal->checkpoint();
        if (!result.isOk()) {
            _wal.reset();
            return result;
        }
        _diagnostics.walState = WalState::Clean;
    } else if (state == WalState::Clean && !_readOnly) {
        // A clean checkpoint needs no rewrite. Avoid changing its bytes during
        // ordinary reopen so the clean state is idempotent.
        result = _file->validateOpenedPages(validateAllPages);
        if (!result.isOk()) {
            return result;
        }
    } else if (state == WalState::Incomplete || state == WalState::Clean) {
        result = _file->validateOpenedPages(validateAllPages);
        if (!result.isOk()) {
            if (result.status() == DbStatus::Truncated ||
                result.status() == DbStatus::CorruptHeader) {
                return DbResult::error(DbStatus::RecoveryRequired,
                                       "base file has an incomplete page extent; writable "
                                       "recovery evidence is unavailable");
            }
            return result;
        }
    }
    return DbResult::ok();
    } catch (const std::bad_alloc&) {
        return DbResult(DbStatus::NoSpace, std::string());
    }
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
    bool exists = false;
    DbResult lookup = filesystem.exists(_walPath, exists);
    if (!lookup.isOk()) {
        return DbResult::error(DbStatus::IoError,
                               "WAL path lookup failed: " + lookup.describe());
    }
    const FileOpenMode mode = exists ? FileOpenMode::ReadWrite : FileOpenMode::Create;
    DbResult result = walFile->open(_walPath, mode, mode == FileOpenMode::Create);
    if (!result.isOk()) {
        return result;
    }
    if (_hasWalRecordAllocator) {
        _wal.reset(new WriteAheadLog(_walRecordAllocator));
    } else {
        _wal.reset(new WriteAheadLog());
    }
    result = _wal->open(std::move(walFile), _file->header().databaseId,
                        _file->pageSize(), false);
    if (!result.isOk()) {
        _wal.reset();
        return result;
    }
    return DbResult::ok();
}

DbResult Database::applyRecovery(const std::vector<WalTransaction>& transactions) {
    try {
        DbResult result = validateRecoveryCoverage(transactions);
        if (!result.isOk()) return result;

        uint64_t redone = 0;
        for (size_t i = 0; i < transactions.size(); ++i) {
            const WalTransaction& tx = transactions[i];
            if (tx.hasHeaderImage &&
                tx.headerImage.pageCount >= _file->pageCount()) {
                // The database header can already reflect a later committed
                // image (for example, close persisted the latest page count
                // while the WAL still retains earlier commits). Do not replay
                // an older header image over that later state.
                result = _file->applyHeaderImage(tx.headerImage);
                if (!result.isOk()) return result;
            }
            for (const GxRecoveredPage* page = gxRecoveredPagesFirst(&tx.pages);
                 page; page = gxRecoveredPageNext(page)) {
                result = _file->writePageImage(
                    page->pageId, gxRecoveredPageImage(page), pageSize());
                if (!result.isOk()) return result;
                ++redone;
            }
        }
        result = _file->flush();
        if (!result.isOk()) return result;
        result = _file->validateOpenedPages(true);
        if (!result.isOk()) return result;
        _diagnostics.pagesRedone = redone;
        _diagnostics.lastRecoveryResult = "redo applied";
        return DbResult::ok();
    } catch (const std::bad_alloc&) {
        return DbResult::error(DbStatus::NoSpace, "alloc failed");
    }
}

DbResult Database::validateRecoveryCoverage(
    const std::vector<WalTransaction>& transactions) {
    uint64_t fileSize = 0;
    DbResult result = _file->getFileSize(fileSize);
    if (!result.isOk()) return result;

    // The on-disk header may already contain a later committed page count
    // than the first retained WAL header image. Validate WAL history against
    // its own preceding header image, while using the maximum of the physical
    // header and committed WAL headers as the recovery extent.
    uint64_t finalPageCount = _file->pageCount();
    uint64_t previousWalHeaderPageCount = 0;
    bool haveWalHeaderPageCount = false;
    uint64_t retainedPageCount = 0;
    for (size_t i = 0; i < transactions.size(); ++i) {
        const WalTransaction& tx = transactions[i];
        if (tx.hasHeaderImage) {
            if (tx.headerImage.pageSize != _file->pageSize() ||
                std::memcmp(tx.headerImage.databaseId, _file->header().databaseId, 16) != 0) {
                return DbResult::error(DbStatus::WalCorrupt,
                                       "recovery header image is incompatible with database");
            }
            if (haveWalHeaderPageCount &&
                tx.headerImage.pageCount < previousWalHeaderPageCount) {
                return DbResult::error(DbStatus::WalCorrupt,
                                       "committed WAL header images regress page count");
            }
            previousWalHeaderPageCount = tx.headerImage.pageCount;
            haveWalHeaderPageCount = true;
            if (tx.headerImage.pageCount > finalPageCount) {
                finalPageCount = tx.headerImage.pageCount;
            }
        }
        const size_t count = gxRecoveredPagesCount(&tx.pages);
        if (count > static_cast<size_t>(UINT64_MAX - retainedPageCount)) {
            return DbResult::error(DbStatus::TransactionTooLarge,
                                   "retained WAL page count overflows");
        }
        retainedPageCount += static_cast<uint64_t>(count);
    }

    uint64_t requiredBytes = 0;
    if (!checkedMulU64(finalPageCount, static_cast<uint64_t>(_file->pageSize()),
                       requiredBytes)) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "recovery page count overflows the database extent");
    }
    if (fileSize % _file->pageSize() != 0 && requiredBytes < fileSize) {
        return DbResult::error(DbStatus::RecoveryRequired,
                               "partial tail lies beyond the WAL's logical database extent");
    }

    const uint64_t firstMissingPage = fileSize / _file->pageSize();
    if (firstMissingPage == 0 && fileSize < _file->pageSize()) {
        return DbResult::error(DbStatus::RecoveryRequired,
                               "partial header page cannot be reconstructed from page images");
    }
    if (firstMissingPage < finalPageCount) {
        const uint64_t missingCount = finalPageCount - firstMissingPage;
        if (missingCount > retainedPageCount) {
            return DbResult::error(DbStatus::RecoveryRequired,
                                   "WAL does not contain every page needed to repair the tail");
        }

        const uint64_t bitmapBytes64 = missingCount / 8 +
                                       (missingCount % 8 != 0 ? 1 : 0);
        if (bitmapBytes64 > static_cast<uint64_t>(static_cast<size_t>(-1))) {
            return DbResult::error(DbStatus::TransactionTooLarge,
                                   "recovery coverage bitmap exceeds addressable memory");
        }
        const GxAllocator& allocator = _wal->recoveryAllocator();
        void* bitmapAllocation = nullptr;
        const GxStatus allocStatus = gxAllocatorAllocate(
            &allocator, static_cast<size_t>(bitmapBytes64), 1,
            &bitmapAllocation);
        if (allocStatus != GxStatus::Ok) {
            const DbStatus status =
                allocStatus == GxStatus::OutOfMemory
                    ? DbStatus::NoSpace
                    : allocStatus == GxStatus::ResourceLimit ||
                              allocStatus == GxStatus::Overflow
                          ? DbStatus::TransactionTooLarge
                          : DbStatus::Internal;
            return DbResult::error(status, "recovery coverage allocation failed");
        }
        uint8_t* coverage = static_cast<uint8_t*>(bitmapAllocation);
        for (size_t i = 0; i < static_cast<size_t>(bitmapBytes64); ++i) {
            coverage[i] = 0;
        }
        for (size_t i = 0; i < transactions.size(); ++i) {
            const WalTransaction& tx = transactions[i];
            for (const GxRecoveredPage* page = gxRecoveredPagesFirst(&tx.pages);
                 page; page = gxRecoveredPageNext(page)) {
                if (page->pageId >= firstMissingPage &&
                    page->pageId < finalPageCount) {
                    const uint64_t offset = page->pageId - firstMissingPage;
                    coverage[static_cast<size_t>(offset / 8)] |=
                        static_cast<uint8_t>(1u << (offset % 8));
                }
            }
        }
        bool complete = true;
        for (uint64_t bit = 0; bit < missingCount; ++bit) {
            if ((coverage[static_cast<size_t>(bit / 8)] &
                 static_cast<uint8_t>(1u << (bit % 8))) == 0) {
                complete = false;
                break;
            }
        }
        gxAllocatorFree(&allocator, bitmapAllocation);
        if (!complete) {
            return DbResult::error(DbStatus::RecoveryRequired,
                                   "WAL is missing a page image required by the partial tail");
        }
    } else if (fileSize % _file->pageSize() != 0) {
        return DbResult::error(DbStatus::RecoveryRequired,
                               "partial trailing bytes are outside all committed pages");
    }
    return DbResult::ok();
}

DbResult Database::openImpl(const std::string& path, const DatabaseOpenOptions& options,
                            const DatabaseCreateOptions& createOptions, bool create,
                            IDatabaseFileSystem* fileSystem) {
    IDatabaseFileSystem& filesystem = fileSystem ? *fileSystem : defaultFileSystem();
    _fileSystem = &filesystem;
    _hasWalRecordAllocator = options.walRecordAllocator != nullptr;
    if (_hasWalRecordAllocator) {
        _walRecordAllocator = *options.walRecordAllocator;
    }
    const GxAllocator* transactionAllocator = create
        ? createOptions.transactionPageAllocator
        : options.transactionPageAllocator;
    _hasTransactionPageAllocator = transactionAllocator != nullptr;
    if (_hasTransactionPageAllocator) {
        _transactionPageAllocator = *transactionAllocator;
    }
    const uint32_t bufferCapacity =
        options.bufferCapacity != 0 ? options.bufferCapacity : kDefaultBufferCapacity;

    if (create) {
        DbResult result = DatabaseEngine::createDatabase(path, createOptions, _file, &filesystem);
        if (!result.isOk()) {
            return result;
        }
    } else {
        DatabaseOpenOptions storageOptions = options;
        storageOptions.deferPageValidationForWalRecovery = true;
        DbResult result = DatabaseEngine::openDatabase(path, storageOptions, _file, &filesystem);
        if (!result.isOk()) {
            return result;
        }
    }

    try {
        _buffer.reset(new BufferManager(*_file, bufferCapacity,
                                        options.pageCacheAllocator));
    } catch (const std::bad_alloc&) {
        (void)_file->close();
        _file.reset();
        return DbResult::error(DbStatus::NoSpace,
                               "buffer manager allocation failed");
    }
    DbResult bufferResult = _buffer->init();
    if (!bufferResult.isOk()) {
        _buffer.reset();
        (void)_file->close();
        _file.reset();
        return bufferResult;
    }
    _readOnly = options.readOnly;
    _walPath = walPathFor(path);

    if (create) {
        // A freshly created database has a new identity; any sidecar WAL from a
        // previous occupant is stale and must not be replayed.
        DbResult removeResult = filesystem.remove(_walPath);
        if (!removeResult.isOk()) {
            return DbResult::error(DbStatus::RecoveryFailed,
                                   "failed to remove stale WAL sidecar: " +
                                       removeResult.describe());
        }
    }

    DbResult result = openAndRecoverWal(filesystem, options.validateAllPages);
    if (!result.isOk()) {
        _wal.reset();
        _buffer.reset();
        _file->close();
        _file.reset();
        return result;
    }

    try {
        result = _catalog.load(*this, _file->rootPageId(), _file->pageSize(),
                               _file->pageCount());
    } catch (const std::bad_alloc&) {
        result = DbResult(DbStatus::NoSpace, std::string());
    }
    if (!result.isOk()) {
        _wal.reset();
        _buffer.reset();
        _file->close();
        _file.reset();
        return result;
    }

    if (!_readOnly && _diagnostics.walState == WalState::Committed) {
        result = _wal->checkpoint();
        if (!result.isOk()) {
            _wal.reset();
            _buffer.reset();
            (void)_file->close();
            _file.reset();
            return result;
        }
        _diagnostics.walState = WalState::Clean;
        _diagnostics.recoveryRequired = false;
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
    createOptions.pageCacheAllocator = options.pageCacheAllocator;
    createOptions.transactionPageAllocator = options.transactionPageAllocator;

    DatabaseOpenOptions createOpen;
    createOpen.bufferCapacity = options.bufferCapacity;
    createOpen.pageCacheAllocator = options.pageCacheAllocator;
    createOpen.transactionPageAllocator = options.transactionPageAllocator;
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
        DbResult walClosed = _wal->close();
        _wal.reset();
        if (result.isOk() && !walClosed.isOk()) {
            result = walClosed;
        }
    }
    DbResult closed = _file->close();
    _file.reset();
    _open = false;
    _catalogDirty = false;
    if (!result.isOk()) return result;
    return closed;
}

DbResult Database::flush() {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::ok();
    }
    if (_diagnostics.recoveryRequired) {
        return DbResult::error(DbStatus::RecoveryRequired,
                               "committed WAL must be replayed before checkpoint");
    }
    if (_activeTx != nullptr) {
        return DbResult::error(DbStatus::TransactionAlreadyActive,
                               "cannot flush while a transaction is active");
    }

    DbResult result = _buffer->flush();
    if (!result.isOk()) {
        return result;
    }
    if (_wal && _wal->isOpen() &&
        (_diagnostics.walState != WalState::Clean ||
         _wal->bytesSinceCheckpoint() != 0)) {
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
    if (_transactionIdsExhausted) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "transaction identifier space is exhausted");
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
    const uint64_t transactionId = _nextTxId;
    std::unique_ptr<Transaction> tx;
    try {
        tx.reset(new Transaction(*this, transactionId, basePageCount,
                                 _hasTransactionPageAllocator
                                     ? &_transactionPageAllocator
                                     : nullptr));
    } catch (const std::bad_alloc&) {
        return DbResult::error(DbStatus::NoSpace,
                               "transaction object allocation failed");
    }
    DbResult ownerInit = tx->init();
    if (!ownerInit.isOk()) return ownerInit;
    if (transactionId == UINT64_MAX) {
        _transactionIdsExhausted = true;
    } else {
        _nextTxId = transactionId + 1;
    }
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

DbResult Database::createIndex(const IndexDefinition& def, uint32_t& outIndexId) {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    if (_activeTx != nullptr) {
        return _activeTx->createIndex(def, outIndexId);
    }

    std::unique_ptr<Transaction> tx;
    DbResult result = beginTransaction(tx);
    if (!result.isOk()) {
        return result;
    }
    result = tx->createIndex(def, outIndexId);
    if (!result.isOk()) {
        tx->rollback();
        return result;
    }
    return tx->commit();
}

DbResult Database::dropIndex(const std::string& name) {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    const Catalog::IndexRecord* rec = _catalog.findIndex(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such index");
    }
    if (_activeTx != nullptr) {
        DbResult result = _catalog.removeIndex(rec->indexId);
        if (result.isOk()) {
            markCatalogDirty();
            ++_schemaGeneration;
        }
        return result;
    }

    std::unique_ptr<Transaction> tx;
    DbResult result = beginTransaction(tx);
    if (!result.isOk()) {
        return result;
    }
    result = _catalog.removeIndex(rec->indexId);
    if (!result.isOk()) {
        tx->rollback();
        return result;
    }
    markCatalogDirty();
    ++_schemaGeneration;
    return tx->commit();
}

DbResult Database::dropTable(const std::string& name) {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    const Catalog::TableRecord* rec = _catalog.findTable(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such table");
    }
    if (_activeTx != nullptr) {
        DbResult result = _catalog.removeTable(rec->tableId);
        if (result.isOk()) {
            markCatalogDirty();
            ++_schemaGeneration;
        }
        return result;
    }

    std::unique_ptr<Transaction> tx;
    DbResult result = beginTransaction(tx);
    if (!result.isOk()) {
        return result;
    }
    result = _catalog.removeTable(rec->tableId);
    if (!result.isOk()) {
        tx->rollback();
        return result;
    }
    markCatalogDirty();
    ++_schemaGeneration;
    return tx->commit();
}

DbResult Database::alterTableAddColumn(const std::string& name, const ColumnDefinition& col) {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }
    const Catalog::TableRecord* rec = _catalog.findTable(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such table");
    }
    const uint32_t tableId = rec->tableId;

    // Validate: NOT NULL + no default on a non-empty table is rejected.
    if (rec->rowCount > 0 && !col.nullable && !col.hasDefault) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "cannot add NOT NULL column without default to non-empty table");
    }

    if (_activeTx != nullptr) {
        Table table(*this, tableId);
        return table.alterAddColumn(col);
    }

    std::unique_ptr<Transaction> tx;
    DbResult result = beginTransaction(tx);
    if (!result.isOk()) {
        return result;
    }
    Table table(*this, tableId);
    result = table.alterAddColumn(col);
    if (!result.isOk()) {
        tx->rollback();
        return result;
    }
    return tx->commit();
}

DbResult Database::listForeignKeys(std::vector<ForeignKeyInfo>& out) const {
    out.clear();
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const std::vector<Catalog::ForeignKeyRecord>& fks = _catalog.foreignKeys();
    out.reserve(fks.size());
    for (size_t i = 0; i < fks.size(); ++i) {
        ForeignKeyInfo info;
        info.foreignKeyId = fks[i].foreignKeyId;
        info.childTableId = fks[i].childTableId;
        info.childColumnOrdinal = fks[i].childColumnOrdinal;
        info.parentTableId = fks[i].parentTableId;
        info.parentColumnOrdinal = fks[i].parentColumnOrdinal;
        info.referencedIndexId = fks[i].referencedIndexId;
        info.supportIndexId = fks[i].supportIndexId;
        const Catalog::TableRecord* childTable = _catalog.findTable(fks[i].childTableId);
        if (childTable != nullptr) {
            info.childTableName = childTable->name;
            if (fks[i].childColumnOrdinal < childTable->columns.size()) {
                info.childColumnName = childTable->columns[fks[i].childColumnOrdinal].name;
            }
        }
        const Catalog::TableRecord* parentTable = _catalog.findTable(fks[i].parentTableId);
        if (parentTable != nullptr) {
            info.parentTableName = parentTable->name;
            if (fks[i].parentColumnOrdinal < parentTable->columns.size()) {
                info.parentColumnName = parentTable->columns[fks[i].parentColumnOrdinal].name;
            }
        }
        out.push_back(info);
    }
    return DbResult::ok();
}

DbResult Database::validateIntegrity(std::string& message) {
    message.clear();
    if (!_open) {
        message = "database is not open";
        return DbResult::error(DbStatus::NotOpen, message);
    }
    const std::vector<Catalog::ForeignKeyRecord>& fks = _catalog.foreignKeys();
    for (size_t i = 0; i < fks.size(); ++i) {
        const Catalog::ForeignKeyRecord& fk = fks[i];
        const Catalog::TableRecord* child = _catalog.findTable(fk.childTableId);
        if (child == nullptr) {
            message = "foreign key references unknown child table";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
        const Catalog::TableRecord* parent = _catalog.findTable(fk.parentTableId);
        if (parent == nullptr) {
            message = "foreign key references unknown parent table";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
        if (fk.childColumnOrdinal >= child->columns.size()) {
            message = "foreign key child column out of range";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
        if (fk.parentColumnOrdinal >= parent->columns.size()) {
            message = "foreign key parent column out of range";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
        if (child->columns[fk.childColumnOrdinal].type !=
            parent->columns[fk.parentColumnOrdinal].type) {
            message = "foreign key column types do not match";
            return DbResult::error(DbStatus::CorruptPage, message);
        }

        const Catalog::IndexRecord* referenced = _catalog.findIndex(fk.referencedIndexId);
        if (referenced == nullptr) {
            message = "foreign key references unknown index";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
        if (referenced->tableId != fk.parentTableId ||
            referenced->columnOrdinal != fk.parentColumnOrdinal ||
            !indexKindIsUnique(referenced->kind())) {
            message = "foreign key referenced index is not a UNIQUE/PK index on the parent column";
            return DbResult::error(DbStatus::CorruptPage, message);
        }

        const Catalog::IndexRecord* support = _catalog.findIndex(fk.supportIndexId);
        if (support == nullptr) {
            message = "foreign key support index missing";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
        if (support->tableId != fk.childTableId ||
            support->columnOrdinal != fk.childColumnOrdinal) {
            message = "foreign key support index is not on the child column";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
        if (!support->systemOwned() || support->ownerForeignKeyId != fk.foreignKeyId) {
            message = "foreign key support index ownership mismatch";
            return DbResult::error(DbStatus::CorruptPage, message);
        }

        IndexValidation supportValidation;
        if (!validateIndex(fk.supportIndexId, supportValidation).isOk() ||
            !supportValidation.ok) {
            message = "foreign key support index failed structural validation";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
        IndexValidation refValidation;
        if (!validateIndex(fk.referencedIndexId, refValidation).isOk() ||
            !refValidation.ok) {
            message = "foreign key referenced index failed structural validation";
            return DbResult::error(DbStatus::CorruptPage, message);
        }

        // Every non-NULL child key must resolve through the referenced index.
        Table childTable(*this, fk.childTableId);
        std::unique_ptr<TableScan> scan;
        DbResult scanResult = childTable.scanStart(scan);
        if (!scanResult.isOk()) {
            message = "cannot scan foreign key child table";
            return scanResult;
        }
        const DbType childType = child->columns[fk.childColumnOrdinal].type;
        std::vector<DbValue> row;
        while (scan->next(row)) {
            if (fk.childColumnOrdinal >= row.size()) {
                message = "child row is missing the foreign key column";
                return DbResult::error(DbStatus::CorruptPage, message);
            }
            const DbValue& value = row[fk.childColumnOrdinal];
            if (value.isNull()) {
                continue;
            }
            std::vector<uint8_t> key;
            if (!encodeIndexKey(childType, value, key)) {
                message = "foreign key child key could not be encoded";
                return DbResult::error(DbStatus::CorruptPage, message);
            }
            std::vector<RowLocator> locators;
            if (!indexLookup(fk.referencedIndexId, key, locators).isOk()) {
                message = "foreign key referenced index lookup failed";
                return DbResult::error(DbStatus::CorruptPage, message);
            }
            if (locators.empty()) {
                message = "dangling foreign key reference";
                return DbResult::error(DbStatus::CorruptPage, message);
            }
        }
        if (!scan->status().isOk()) {
            message = "foreign key child scan failed";
            return scan->status();
        }
    }

    // No orphan system-owned support index may exist without its FK record.
    const std::vector<Catalog::IndexRecord>& indexes = _catalog.indexes();
    for (size_t i = 0; i < indexes.size(); ++i) {
        if (indexes[i].ownerForeignKeyId != 0 &&
            _catalog.findForeignKey(indexes[i].ownerForeignKeyId) == nullptr) {
            message = "orphan foreign-key support index";
            return DbResult::error(DbStatus::CorruptPage, message);
        }
    }
    return DbResult::ok();
}

DbResult Database::buildIndex(uint32_t indexId) {
    const Catalog::IndexRecord* rec = _catalog.findIndex(indexId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::Internal, "index record not found");
    }
    const Catalog::TableRecord* tableRec = _catalog.findTable(rec->tableId);
    if (tableRec == nullptr) {
        return DbResult::error(DbStatus::Internal, "index table not found");
    }
    const DbType type = tableRec->columns[rec->columnOrdinal].type;
    const uint32_t maxKey = maxIndexKeyBytes(pageSize());
    const bool unique = rec->unique();

    IndexTree tree(pages(), type, indexId, activeTransaction(),
                   scratchAllocator());
    uint64_t root = rec->rootPageId;
    uint64_t entryCount = 0;

    Table table(*this, rec->tableId);
    std::unique_ptr<TableScan> scan;
    DbResult result = table.scanStart(scan);
    if (!result.isOk()) {
        return result;
    }
    std::vector<DbValue> row;
    while (scan->next(row)) {
        std::vector<uint8_t> key;
        if (!encodeIndexKey(type, row[rec->columnOrdinal], key)) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "value is not indexable for this index");
        }
        if (key.size() > maxKey) {
            return DbResult::error(DbStatus::InvalidArgument,
                                   "indexed value exceeds the maximum index key size");
        }
        if (root == 0) {
            result = tree.createEmpty(root);
            if (!result.isOk()) {
                return result;
            }
            _catalog.setIndexRoot(indexId, root);
        }
        uint64_t newRoot = root;
        bool inserted = false;
        result = tree.insert(key, RowLocator(scan->currentPageId(), scan->currentSlotIndex()),
                             root, unique, newRoot, inserted);
        if (!result.isOk()) {
            return result;
        }
        if (newRoot != root) {
            root = newRoot;
            _catalog.setIndexRoot(indexId, root);
        }
        if (inserted) {
            ++entryCount;
        }
    }
    if (!scan->status().isOk()) {
        return scan->status();
    }
    _catalog.setIndexEntryCount(indexId, entryCount);
    markCatalogDirty();
    return DbResult::ok();
}

DbResult Database::listIndexes(std::vector<IndexInfo>& out) const {
    out.clear();
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const std::vector<Catalog::IndexRecord>& indexes = _catalog.indexes();
    out.reserve(indexes.size());
    for (size_t i = 0; i < indexes.size(); ++i) {
        IndexInfo info;
        info.indexId = indexes[i].indexId;
        info.name = indexes[i].name;
        info.tableId = indexes[i].tableId;
        info.columnOrdinal = indexes[i].columnOrdinal;
        info.kind = indexes[i].kind();
        info.rootPageId = indexes[i].rootPageId;
        info.entryCount = indexes[i].entryCount;
        info.formatVersion = indexes[i].formatVersion;
        const Catalog::TableRecord* table = _catalog.findTable(indexes[i].tableId);
        if (table != nullptr) {
            info.tableName = table->name;
            if (indexes[i].columnOrdinal < table->columns.size()) {
                info.columnName = table->columns[indexes[i].columnOrdinal].name;
                info.columnType = table->columns[indexes[i].columnOrdinal].type;
            }
        }
        out.push_back(info);
    }
    return DbResult::ok();
}

DbResult Database::describeIndex(const std::string& name, IndexInfo& out) const {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const Catalog::IndexRecord* rec = _catalog.findIndex(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such index");
    }
    out = IndexInfo();
    out.indexId = rec->indexId;
    out.name = rec->name;
    out.tableId = rec->tableId;
    out.columnOrdinal = rec->columnOrdinal;
    out.kind = rec->kind();
    out.rootPageId = rec->rootPageId;
    out.entryCount = rec->entryCount;
    out.formatVersion = rec->formatVersion;
    const Catalog::TableRecord* table = _catalog.findTable(rec->tableId);
    if (table != nullptr) {
        out.tableName = table->name;
        if (rec->columnOrdinal < table->columns.size()) {
            out.columnName = table->columns[rec->columnOrdinal].name;
            out.columnType = table->columns[rec->columnOrdinal].type;
        }
    }
    return DbResult::ok();
}

const Catalog::IndexRecord* Database::findIndexForColumn(uint32_t tableId,
                                                         uint32_t columnOrdinal) const {
    const std::vector<Catalog::IndexRecord>& indexes = _catalog.indexes();
    for (size_t i = 0; i < indexes.size(); ++i) {
        if (indexes[i].tableId == tableId &&
            indexes[i].columnOrdinal == columnOrdinal) {
            return &indexes[i];
        }
    }
    return nullptr;
}

DbResult Database::indexLookup(uint32_t indexId, const std::vector<uint8_t>& logicalKey,
                               std::vector<RowLocator>& out) {
    out.clear();
    const Catalog::IndexRecord* rec = _catalog.findIndex(indexId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such index");
    }
    if (rec->rootPageId == 0) {
        return DbResult::ok();
    }
    const Catalog::TableRecord* table = _catalog.findTable(rec->tableId);
    if (table == nullptr) {
        return DbResult::error(DbStatus::Internal, "index table not found");
    }
    IndexTree tree(pages(), table->columns[rec->columnOrdinal].type, indexId,
                   activeTransaction(), scratchAllocator());
    std::vector<IndexEntry> entries;
    DbResult result = tree.lookupAll(logicalKey, rec->rootPageId, entries);
    if (!result.isOk()) {
        return result;
    }
    out.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        out.push_back(entries[i].locator);
    }
    return DbResult::ok();
}

DbResult Database::indexRange(uint32_t indexId, const IndexRangeBound& bound,
                              std::vector<RowLocator>& out) {
    out.clear();
    const Catalog::IndexRecord* rec = _catalog.findIndex(indexId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such index");
    }
    if (rec->rootPageId == 0) {
        return DbResult::ok();
    }
    const Catalog::TableRecord* table = _catalog.findTable(rec->tableId);
    if (table == nullptr) {
        return DbResult::error(DbStatus::Internal, "index table not found");
    }
    IndexTree tree(pages(), table->columns[rec->columnOrdinal].type, indexId,
                   activeTransaction(), scratchAllocator());
    std::vector<IndexEntry> entries;
    DbResult result = tree.rangeScan(bound, rec->rootPageId, entries);
    if (!result.isOk()) {
        return result;
    }
    out.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        out.push_back(entries[i].locator);
    }
    return DbResult::ok();
}

DbResult Database::validateIndex(uint32_t indexId, IndexValidation& out) {
    out = IndexValidation();
    const Catalog::IndexRecord* rec = _catalog.findIndex(indexId);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such index");
    }
    const Catalog::TableRecord* table = _catalog.findTable(rec->tableId);
    if (table == nullptr) {
        return DbResult::error(DbStatus::Internal, "index table not found");
    }
    if (rec->rootPageId == 0) {
        out.ok = true;
        return DbResult::ok();
    }
    IndexTree tree(pages(), table->columns[rec->columnOrdinal].type, indexId,
                   activeTransaction(), scratchAllocator());
    return tree.validate(rec->rootPageId, out);
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
    if (_wal && _wal->isOpen()) {
        uint64_t walBytes = 0;
        DbResult sizeResult = _wal->getSize(walBytes);
        if (sizeResult.isOk()) {
            _diagnostics.walBytes = walBytes;
        } else {
            _diagnostics.walState = WalState::Corrupt;
            _diagnostics.recoveryRequired = true;
            _diagnostics.lastRecoveryResult =
                "failed to query WAL size: " + sizeResult.describe();
        }
    } else {
        _diagnostics.walBytes = 0;
    }

    _diagnostics.tables.clear();
    const std::vector<Catalog::TableRecord>& tables = _catalog.tables();
    for (size_t i = 0; i < tables.size(); ++i) {
        TableDiagnostics td;
        td.tableId = tables[i].tableId;
        td.name = tables[i].name;
        td.schemaVersion = tables[i].schemaVersion;
        td.columnCount = static_cast<uint32_t>(tables[i].columns.size());
        td.heapPageCount = tables[i].heapPageCount;
        td.rowCount = tables[i].rowCount;
        for (size_t c = 0; c < tables[i].columns.size(); ++c) {
            const ColumnDefinition& col = tables[i].columns[c];
            ColumnDiagnostics cd;
            cd.name = col.name;
            cd.typeName = dbTypeName(col.type);
            cd.nullable = col.nullable;
            cd.hasDefault = col.hasDefault;
            if (col.hasDefault) {
                cd.defaultValue = describeDbValue(col.defaultValue);
            }
            td.columns.push_back(cd);
        }
        _diagnostics.tables.push_back(td);
    }

    _diagnostics.indexes.clear();
    _diagnostics.indexCount = _catalog.indexCount();
    const std::vector<Catalog::IndexRecord>& indexes = _catalog.indexes();
    for (size_t i = 0; i < indexes.size(); ++i) {
        IndexDiagnostics id;
        id.indexId = indexes[i].indexId;
        id.name = indexes[i].name;
        id.tableId = indexes[i].tableId;
        id.unique = indexes[i].unique();
        id.primaryKey = indexes[i].primaryKey();
        id.systemOwned = indexes[i].systemOwned();
        id.ownerForeignKeyId = indexes[i].ownerForeignKeyId;
        id.rootPageId = indexes[i].rootPageId;
        id.entryCount = indexes[i].entryCount;
        id.formatVersion = indexes[i].formatVersion;
        const Catalog::TableRecord* table = _catalog.findTable(indexes[i].tableId);
        if (table != nullptr) {
            id.tableName = table->name;
            if (indexes[i].columnOrdinal < table->columns.size()) {
                id.columnName = table->columns[indexes[i].columnOrdinal].name;
            }
        }
        _diagnostics.indexes.push_back(id);
    }

    _diagnostics.foreignKeys.clear();
    const std::vector<Catalog::ForeignKeyRecord>& fks = _catalog.foreignKeys();
    _diagnostics.foreignKeyCount = static_cast<uint32_t>(fks.size());
    for (size_t i = 0; i < fks.size(); ++i) {
        ForeignKeyDiagnostics fd;
        fd.foreignKeyId = fks[i].foreignKeyId;
        fd.childTableId = fks[i].childTableId;
        fd.parentTableId = fks[i].parentTableId;
        fd.referencedIndexId = fks[i].referencedIndexId;
        fd.supportIndexId = fks[i].supportIndexId;
        const Catalog::TableRecord* child = _catalog.findTable(fks[i].childTableId);
        if (child != nullptr) {
            fd.childTableName = child->name;
            if (fks[i].childColumnOrdinal < child->columns.size()) {
                fd.childColumnName = child->columns[fks[i].childColumnOrdinal].name;
            }
        }
        const Catalog::TableRecord* parent = _catalog.findTable(fks[i].parentTableId);
        if (parent != nullptr) {
            fd.parentTableName = parent->name;
            if (fks[i].parentColumnOrdinal < parent->columns.size()) {
                fd.parentColumnName = parent->columns[fks[i].parentColumnOrdinal].name;
            }
        }
        _diagnostics.foreignKeys.push_back(fd);
    }
    if (_file) {
        _diagnostics.state = _file->diagnostics().state;
    }
}

} // namespace db
} // namespace gxos
