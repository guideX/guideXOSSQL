#include "database_transaction.h"

#include "database_heap.h"
#include "database_relational.h"
#include "database_wal.h"
#include "portable/gx_allocator_hosted.h"

#include <new>
#include <utility>

namespace gxos {
namespace db {

namespace {

void incrementSaturated(uint64_t* value) {
    if (*value != UINT64_MAX) ++*value;
}

DbResult transactionPageStatus(GxStatus status, const char* operation) {
    if (status == GxStatus::OutOfMemory ||
        status == GxStatus::UnsupportedAlignment) {
        return DbResult::error(DbStatus::NoSpace, operation);
    }
    if (status == GxStatus::ResourceLimit || status == GxStatus::Overflow) {
        return DbResult::error(DbStatus::TransactionTooLarge, operation);
    }
    if (status == GxStatus::CorruptData) {
        return DbResult::error(DbStatus::CorruptPage, operation);
    }
    return DbResult::error(DbStatus::InvalidArgument, operation);
}

} // namespace

Transaction::Transaction(Database& db, uint64_t id, uint64_t basePageCount,
                         const GxAllocator* pageAllocator)
    : _db(db), _id(id), _basePageCount(basePageCount), _nextPageId(basePageCount),
      _pageAllocator(pageAllocator ? *pageAllocator : GxAllocator()),
      _pageOverlay(), _catalogSnapshot(db.catalog()),
      _catalogChanged(false), _active(true) {}

Transaction::~Transaction() {
    gxTransactionPageOverlayDestroy(&_pageOverlay);
}

DbResult Transaction::init() {
    if (!_pageAllocator.allocate || !_pageAllocator.deallocate) {
        size_t maximumAllocation = static_cast<size_t>(kMaxTransactionPages) *
                                   sizeof(GxTransactionPageEntry);
        size_t indexCapacity = 0;
        size_t indexBytes = 0;
        const GxStatus indexSizeStatus = gxTransactionPageIndexStorageSize(
            kMaxTransactionPages, &indexCapacity, &indexBytes);
        if (indexSizeStatus != GxStatus::Ok) {
            return transactionPageStatus(indexSizeStatus,
                                         "transaction page index size overflow");
        }
        if (maximumAllocation < indexBytes) maximumAllocation = indexBytes;
        if (maximumAllocation < pageSize()) maximumAllocation = pageSize();
        const GxStatus hostedStatus = gxHostedAllocatorInit(
            &_pageAllocator, maximumAllocation);
        if (hostedStatus != GxStatus::Ok) {
            return transactionPageStatus(hostedStatus,
                                         "transaction page allocator initialization failed");
        }
    }
    const GxStatus status = gxTransactionPageOverlayInit(
        &_pageOverlay, &_pageAllocator, pageSize(), kMaxTransactionPages);
    if (status != GxStatus::Ok) {
        return transactionPageStatus(status,
                                     "transaction page overlay allocation failed");
    }
    return DbResult::ok();
}

uint32_t Transaction::pageSize() const { return _db.pageSize(); }

uint64_t Transaction::pageCount() const { return _nextPageId; }

DbResult Transaction::readPage(uint64_t pageId, DatabasePage& out) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    if (pageId == kHeaderPageId) {
        return DbResult::error(DbStatus::InvalidArgument, "page 0 is the header page");
    }
    bool found = false;
    size_t ownerIndex = 0;
    const GxStatus lookupStatus = gxTransactionPageOverlayFind(
        &_pageOverlay, pageId, &found, &ownerIndex);
    if (lookupStatus == GxStatus::CorruptData) {
        return DbResult::error(DbStatus::Internal,
                               "transaction page-owner index is inconsistent");
    }
    if (lookupStatus != GxStatus::Ok) {
        return transactionPageStatus(lookupStatus,
                                     "transaction page lookup failed");
    }
    if (found) {
        const GxTransactionPageEntry* entry = gxTransactionPageOverlayAt(
            &_pageOverlay, ownerIndex);
        if (!entry || entry->pageId != pageId || !entry->image) {
            return DbResult::error(DbStatus::Internal,
                                   "transaction page-owner index is inconsistent");
        }
        try {
            return DatabasePage::parse(entry->image, pageSize(), pageSize(),
                                       pageId, out);
        } catch (const std::bad_alloc&) {
            return DbResult::error(DbStatus::NoSpace,
                                   "transaction page read allocation failed");
        }
    }
    if (pageId < _basePageCount) {
        return _db.readPage(pageId, out);
    }
    return DbResult::error(DbStatus::OutOfBounds, "page id outside transaction view");
}

DbResult Transaction::writePage(const DatabasePage& page) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    if (page.pageId == kHeaderPageId) {
        return DbResult::error(DbStatus::InvalidArgument, "page 0 is the header page");
    }
    bool replaced = false;
    const GxStatus status = gxTransactionPageOverlayStage(
        &_pageOverlay, page.pageId, static_cast<uint16_t>(page.type),
        page.flags, page.generation,
        page.payload.empty() ? nullptr : page.payload.data(),
        page.payload.size(), &replaced);
    if (status == GxStatus::CorruptData) {
        return DbResult::error(DbStatus::Internal,
                               "transaction page-owner index is inconsistent");
    }
    if (status != GxStatus::Ok) {
        return transactionPageStatus(status,
                                     "transaction page staging failed");
    }
    (void)replaced;
    return DbResult::ok();
}

DbResult Transaction::allocatePage(PageType type, uint64_t& outPageId) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    if (_nextPageId >= kMaxPageCount) {
        return DbResult::error(DbStatus::NoSpace, "database reached the page count limit");
    }
    if (gxTransactionPageOverlayCount(&_pageOverlay) >= kMaxTransactionPages) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "transaction modified page limit exceeded");
    }
    const uint64_t id = _nextPageId;
    DatabasePage page;
    page.pageId = id;
    page.type = type;
    page.flags = 0;
    page.payloadSize = 0;
    page.generation = 0;
    DbResult result = writePage(page);
    if (!result.isOk()) return result;
    ++_nextPageId;
    outPageId = id;
    return DbResult::ok();
}

DbResult Transaction::createTable(const TableDefinition& def, uint32_t& outTableId) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    DbResult result = _db.catalog().addTable(def, pageSize(), outTableId);
    if (!result.isOk()) {
        return result;
    }
    ++_db._schemaGeneration;
    markCatalogChanged();
    return DbResult::ok();
}

DbResult Transaction::createIndex(const IndexDefinition& def, uint32_t& outIndexId) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    DbResult result = _db.catalog().addIndex(def, pageSize(), outIndexId);
    if (!result.isOk()) {
        return result;
    }
    ++_db._schemaGeneration;
    markCatalogChanged();
    result = _db.buildIndex(outIndexId);
    return result;
}

DbResult Transaction::openTable(const std::string& name, std::unique_ptr<Table>& out) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    const Catalog::TableRecord* rec = _db.catalog().findTable(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such table");
    }
    out.reset(new Table(_db, rec->tableId));
    return DbResult::ok();
}

DbResult Transaction::commit() {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }

    // 1. Serialize the catalog into the transaction overlay if it changed.
    if (_catalogChanged) {
        DbResult result = _db.catalog().save(*this, _db.file().rootPageId(), pageSize());
        if (!result.isOk()) {
            rollback();
            return DbResult::error(DbStatus::CommitFailed, result.message());
        }
    }

    WriteAheadLog& wal = _db.wal();

    // 2. BEGIN.
    DbResult result = wal.appendBegin(_id);
    if (!result.isOk()) {
        rollback();
        return DbResult::error(DbStatus::CommitFailed, result.message());
    }

    // 3. PAGE_IMAGE records.
    for (size_t i = 0; i < gxTransactionPageOverlayCount(&_pageOverlay); ++i) {
        const GxTransactionPageEntry* entry =
            gxTransactionPageOverlayAt(&_pageOverlay, i);
        if (!entry || !entry->image) {
            rollback();
            return DbResult::error(DbStatus::Internal,
                                   "transaction page owner is incomplete");
        }
        result = wal.appendPageImageImage(_id, entry->pageId, entry->image,
                                         pageSize());
        if (!result.isOk()) {
            rollback();
            return DbResult::error(DbStatus::CommitFailed, result.message());
        }
    }

    // 4. DB_HEADER record when the page count changed.
    if (headerChanged()) {
        DatabaseHeader image = _db.file().header();
        image.pageCount = _nextPageId;
        result = wal.appendHeaderImage(_id, image);
        if (!result.isOk()) {
            rollback();
            return DbResult::error(DbStatus::CommitFailed, result.message());
        }
    }

    // 5. COMMIT.
    result = wal.appendCommit(_id);
    if (!result.isOk()) {
        rollback();
        return DbResult::error(DbStatus::CommitFailed, result.message());
    }

    // 6. Durability point.
    result = wal.sync();
    if (!result.isOk()) {
        rollback();
        return DbResult::error(DbStatus::CommitFailed, result.message());
    }

    // 7. Publish committed page images into the committed view.
    result = _db.file().setPageCount(_nextPageId);
    if (!result.isOk()) {
        _db._diagnostics.recoveryRequired = true;
        _db.finishTransaction(this, true);
        _active = false;
        return DbResult::error(DbStatus::CommitFailed, result.message());
    }
    for (size_t i = 0; i < gxTransactionPageOverlayCount(&_pageOverlay); ++i) {
        const GxTransactionPageEntry* entry =
            gxTransactionPageOverlayAt(&_pageOverlay, i);
        DatabasePage page;
        try {
            if (!entry || !entry->image) {
                result = DbResult::error(DbStatus::Internal,
                                         "transaction page owner is incomplete");
            } else {
                result = DatabasePage::parse(entry->image, pageSize(),
                                             pageSize(), entry->pageId, page);
                if (result.isOk()) result = _db.writePage(page);
            }
        } catch (const std::bad_alloc&) {
            result = DbResult::error(DbStatus::NoSpace,
                                     "committed page view allocation failed");
        }
        if (!result.isOk()) {
            // WAL COMMIT is already durable. Keep reads from the committed
            // view blocked until reopening replays the retained WAL.
            _db._diagnostics.recoveryRequired = true;
            _db.finishTransaction(this, true);
            _active = false;
            return DbResult::error(DbStatus::CommitFailed, result.message());
        }
    }

    _db.finishTransaction(this, true);
    _active = false;
    return DbResult::ok();
}

DbResult Transaction::beginStatement(TransactionSavepoint& out) {
    gxTransactionPageOverlayDestroy(&out.pageOverlay);
    const GxStatus cloneStatus = gxTransactionPageOverlayClone(
        &out.pageOverlay, &_pageOverlay);
    if (cloneStatus != GxStatus::Ok) {
        if (cloneStatus == GxStatus::OutOfMemory ||
            cloneStatus == GxStatus::UnsupportedAlignment) {
            incrementSaturated(&_pageOverlay.pages.stats.allocationFailures);
        }
        return transactionPageStatus(cloneStatus,
                                     "statement page snapshot allocation failed");
    }
    try {
        out.catalog = _db.catalog();
    } catch (const std::bad_alloc&) {
        incrementSaturated(&_pageOverlay.pages.stats.allocationFailures);
        gxTransactionPageOverlayDestroy(&out.pageOverlay);
        return DbResult::error(DbStatus::NoSpace,
                               "statement snapshot allocation failed");
    }
    out.nextPageId = _nextPageId;
    out.catalogChanged = _catalogChanged;
    return DbResult::ok();
}

void Transaction::rollbackStatement(TransactionSavepoint& savepoint) {
    // The snapshot has already paid every allocation cost. Swap/move restores
    // the prior statement state without a fallible allocation.
    const GxTransactionPageStats observedPages = _pageOverlay.pages.stats;
    const GxTransactionPageIndexStats observedIndex = _pageOverlay.index.stats;
    (void)gxTransactionPageOverlayMove(&_pageOverlay, &savepoint.pageOverlay);
    const size_t restoredBytes = _pageOverlay.pages.stats.currentBytes;
    _pageOverlay.pages.stats = observedPages;
    _pageOverlay.pages.stats.currentBytes = restoredBytes;
    _pageOverlay.index.stats = observedIndex;
    _db.catalog() = savepoint.catalog;
    _nextPageId = savepoint.nextPageId;
    _catalogChanged = savepoint.catalogChanged;
}

void Transaction::releaseStatement(const TransactionSavepoint&) {}

DbResult Transaction::rollback() {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    _db.catalog() = _catalogSnapshot;
    gxTransactionPageOverlayClear(&_pageOverlay);
    _catalogChanged = false;
    _db.finishTransaction(this, false);
    _active = false;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
