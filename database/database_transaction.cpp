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
      _pageImages(), _pageIndexes(), _catalogSnapshot(db.catalog()),
      _catalogChanged(false), _active(true) {}

Transaction::~Transaction() {
    gxTransactionPagesDestroy(&_pageImages);
}

DbResult Transaction::init() {
    if (!_pageAllocator.allocate || !_pageAllocator.deallocate) {
        size_t maximumAllocation = static_cast<size_t>(kMaxTransactionPages) *
                                   sizeof(GxTransactionPageEntry);
        if (maximumAllocation < pageSize()) maximumAllocation = pageSize();
        const GxStatus hostedStatus = gxHostedAllocatorInit(
            &_pageAllocator, maximumAllocation);
        if (hostedStatus != GxStatus::Ok) {
            return transactionPageStatus(hostedStatus,
                                         "transaction page allocator initialization failed");
        }
    }
    const GxStatus status = gxTransactionPagesInit(
        &_pageImages, &_pageAllocator, pageSize(), kMaxTransactionPages);
    if (status != GxStatus::Ok) {
        return transactionPageStatus(status,
                                     "transaction page-owner table allocation failed");
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
    std::map<uint64_t, size_t>::const_iterator it = _pageIndexes.find(pageId);
    if (it != _pageIndexes.end()) {
        const GxTransactionPageEntry* entry =
            gxTransactionPagesAt(&_pageImages, it->second);
        if (!entry || entry->pageId != pageId || !entry->image) {
            return DbResult::error(DbStatus::Internal,
                                   "transaction page-owner index is inconsistent");
        }
        try {
            return DatabasePage::parse(entry->image, pageSize(), pageId, out);
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
    std::map<uint64_t, size_t>::iterator it = _pageIndexes.find(page.pageId);
    if (it != _pageIndexes.end()) {
        const GxStatus status = gxTransactionPagesReplacePage(
            &_pageImages, it->second, page.pageId,
            static_cast<uint16_t>(page.type), page.flags, page.generation,
            page.payload.empty() ? nullptr : page.payload.data(),
            page.payload.size());
        if (status != GxStatus::Ok) {
            return transactionPageStatus(status,
                                         "transaction page replacement failed");
        }
        return DbResult::ok();
    }
    if (gxTransactionPagesCount(&_pageImages) >= kMaxTransactionPages) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "transaction modified page limit exceeded");
    }
    size_t index = 0;
    const GxStatus appendStatus = gxTransactionPagesAppendPage(
        &_pageImages, page.pageId, static_cast<uint16_t>(page.type),
        page.flags, page.generation,
        page.payload.empty() ? nullptr : page.payload.data(),
        page.payload.size(), &index);
    if (appendStatus != GxStatus::Ok) {
        return transactionPageStatus(appendStatus,
                                     "transaction page staging failed");
    }
    try {
        const std::pair<std::map<uint64_t, size_t>::iterator, bool> inserted =
            _pageIndexes.emplace(page.pageId, index);
        if (!inserted.second) {
            (void)gxTransactionPagesPopLast(&_pageImages, index);
            return DbResult::error(DbStatus::Internal,
                                   "transaction page id was staged twice");
        }
    } catch (const std::bad_alloc&) {
        (void)gxTransactionPagesPopLast(&_pageImages, index);
        return DbResult::error(DbStatus::NoSpace,
                               "transaction page lookup allocation failed");
    }
    return DbResult::ok();
}

DbResult Transaction::allocatePage(PageType type, uint64_t& outPageId) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    if (_nextPageId >= kMaxPageCount) {
        return DbResult::error(DbStatus::NoSpace, "database reached the page count limit");
    }
    if (gxTransactionPagesCount(&_pageImages) >= kMaxTransactionPages) {
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
    for (size_t i = 0; i < gxTransactionPagesCount(&_pageImages); ++i) {
        const GxTransactionPageEntry* entry =
            gxTransactionPagesAt(&_pageImages, i);
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
    for (size_t i = 0; i < gxTransactionPagesCount(&_pageImages); ++i) {
        const GxTransactionPageEntry* entry =
            gxTransactionPagesAt(&_pageImages, i);
        DatabasePage page;
        try {
            if (!entry || !entry->image) {
                result = DbResult::error(DbStatus::Internal,
                                         "transaction page owner is incomplete");
            } else {
                result = DatabasePage::parse(entry->image, pageSize(),
                                             entry->pageId, page);
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
    gxTransactionPagesDestroy(&out.pageImages);
    out.pageIndexes.clear();
    const GxStatus cloneStatus = gxTransactionPagesClone(
        &out.pageImages, &_pageImages);
    if (cloneStatus != GxStatus::Ok) {
        if (cloneStatus == GxStatus::OutOfMemory ||
            cloneStatus == GxStatus::UnsupportedAlignment) {
            incrementSaturated(&_pageImages.stats.allocationFailures);
        }
        return transactionPageStatus(cloneStatus,
                                     "statement page snapshot allocation failed");
    }
    try {
        out.pageIndexes = _pageIndexes;
        out.catalog = _db.catalog();
    } catch (const std::bad_alloc&) {
        incrementSaturated(&_pageImages.stats.allocationFailures);
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
    const GxTransactionPageStats observed = _pageImages.stats;
    (void)gxTransactionPagesMove(&_pageImages, &savepoint.pageImages);
    const size_t restoredBytes = _pageImages.stats.currentBytes;
    _pageImages.stats = observed;
    _pageImages.stats.currentBytes = restoredBytes;
    _pageIndexes.swap(savepoint.pageIndexes);
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
    gxTransactionPagesClear(&_pageImages);
    _pageIndexes.clear();
    _catalogChanged = false;
    _db.finishTransaction(this, false);
    _active = false;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
