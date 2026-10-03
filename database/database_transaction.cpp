#include "database_transaction.h"

#include "database_heap.h"
#include "database_relational.h"
#include "database_wal.h"

namespace gxos {
namespace db {

Transaction::Transaction(Database& db, uint64_t id, uint64_t basePageCount)
    : _db(db), _id(id), _basePageCount(basePageCount), _nextPageId(basePageCount),
      _pages(), _order(), _catalogSnapshot(db.catalog()), _catalogChanged(false),
      _active(true) {}

Transaction::~Transaction() {}

uint32_t Transaction::pageSize() const { return _db.pageSize(); }

uint64_t Transaction::pageCount() const { return _nextPageId; }

DbResult Transaction::readPage(uint64_t pageId, DatabasePage& out) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    if (pageId == kHeaderPageId) {
        return DbResult::error(DbStatus::InvalidArgument, "page 0 is the header page");
    }
    std::map<uint64_t, DatabasePage>::const_iterator it = _pages.find(pageId);
    if (it != _pages.end()) {
        out = it->second;
        return DbResult::ok();
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
    std::map<uint64_t, DatabasePage>::iterator it = _pages.find(page.pageId);
    if (it == _pages.end()) {
        if (_pages.size() >= kMaxTransactionPages) {
            return DbResult::error(DbStatus::TransactionTooLarge,
                                   "transaction modified page limit exceeded");
        }
        _order.push_back(page.pageId);
    }
    _pages[page.pageId] = page;
    return DbResult::ok();
}

DbResult Transaction::allocatePage(PageType type, uint64_t& outPageId) {
    if (!_active) {
        return DbResult::error(DbStatus::NoActiveTransaction, "transaction is not active");
    }
    if (_nextPageId >= kMaxPageCount) {
        return DbResult::error(DbStatus::NoSpace, "database reached the page count limit");
    }
    if (_pages.size() >= kMaxTransactionPages) {
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "transaction modified page limit exceeded");
    }
    const uint64_t id = _nextPageId++;
    DatabasePage page;
    page.pageId = id;
    page.type = type;
    page.flags = 0;
    page.payloadSize = 0;
    page.generation = 0;
    _pages[id] = page;
    _order.push_back(id);
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
    markCatalogChanged();
    return DbResult::ok();
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
    for (size_t i = 0; i < _order.size(); ++i) {
        result = wal.appendPageImage(_id, _pages[_order[i]]);
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
        _db.finishTransaction(this, true);
        _active = false;
        return DbResult::error(DbStatus::CommitFailed, result.message());
    }
    for (size_t i = 0; i < _order.size(); ++i) {
        result = _db.writePage(_pages[_order[i]]);
        if (!result.isOk()) {
            _db.finishTransaction(this, true);
            _active = false;
            return DbResult::error(DbStatus::CommitFailed, result.message());
        }
    }

    _db.finishTransaction(this, true);
    _active = false;
    return DbResult::ok();
}

void Transaction::beginStatement(TransactionSavepoint& out) {
    out.pages = _pages;
    out.order = _order;
    out.catalog = _db.catalog();
    out.nextPageId = _nextPageId;
    out.catalogChanged = _catalogChanged;
}

void Transaction::rollbackStatement(const TransactionSavepoint& savepoint) {
    _pages = savepoint.pages;
    _order = savepoint.order;
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
    _pages.clear();
    _order.clear();
    _catalogChanged = false;
    _db.finishTransaction(this, false);
    _active = false;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
