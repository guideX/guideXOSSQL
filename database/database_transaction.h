#pragma once
// guideXOS SQL -- Phase SQL3
// Transaction: a bounded, single-writer, crash-atomic relational transaction.
//
// A transaction keeps every page it modifies or allocates in a private overlay
// (a complete page image per page). Reads observe the overlay first and the
// committed database second. No overlay page is ever written to the .gxdb
// before COMMIT: the buffer manager and DatabaseFile are only touched during
// the commit apply step, after the WAL has been made durable.
//
// Commit ordering (the durability point is the WAL flush):
//
//   1. serialize the catalog (if changed) into the overlay
//   2. append BEGIN
//   3. append every PAGE_IMAGE
//   4. append DB_HEADER (only when the page count changed)
//   5. append COMMIT
//   6. flush WAL                       <-- DURABILITY POINT
//   7. publish page images into the committed buffer + raise page count
//
// Rollback discards the overlay and restores the catalog snapshot without
// touching the .gxdb.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "database_catalog.h"
#include "database_pagestore.h"
#include "database_result.h"
#include "database_schema.h"

namespace gxos {
namespace db {

class Database;
class Table;

// Bound on the number of pages one transaction may modify. Chosen so the
// overlay cannot grow without limit; a future phase can spill larger
// transactions if needed.
const uint32_t kMaxTransactionPages = 4096;

class Transaction : public PageAccess {
public:
    Transaction(Database& db, uint64_t id, uint64_t basePageCount);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    uint32_t pageSize() const override;
    uint64_t pageCount() const override;
    bool isReadOnly() const override { return false; }
    DbResult readPage(uint64_t pageId, DatabasePage& out) override;
    DbResult writePage(const DatabasePage& page) override;
    DbResult allocatePage(PageType type, uint64_t& outPageId) override;

    DbResult createTable(const TableDefinition& def, uint32_t& outTableId);
    DbResult openTable(const std::string& name, std::unique_ptr<Table>& out);

    DbResult commit();
    DbResult rollback();

    uint64_t id() const { return _id; }
    uint32_t modifiedPageCount() const { return static_cast<uint32_t>(_pages.size()); }
    bool isActive() const { return _active; }
    void markCatalogChanged() { _catalogChanged = true; }
    uint64_t finalPageCount() const { return _nextPageId; }
    bool headerChanged() const { return _nextPageId != _basePageCount; }

private:
    Database& _db;
    uint64_t _id;
    uint64_t _basePageCount;
    uint64_t _nextPageId;
    std::map<uint64_t, DatabasePage> _pages;
    std::vector<uint64_t> _order;
    Catalog _catalogSnapshot;
    bool _catalogChanged;
    bool _active;
};

} // namespace db
} // namespace gxos
