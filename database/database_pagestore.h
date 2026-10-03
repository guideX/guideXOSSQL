#pragma once
// guideXOS SQL -- Phase SQL3
// PageAccess: the single authoritative page read/write/allocate boundary used
// by the relational layer (Catalog, HeapTable).
//
//   Catalog / HeapTable
//          |
//       PageAccess
//        +--> Transaction (private overlay)
//        +--> Database    (committed view: buffer cache + DatabaseFile)
//
// Every persistent relational mutation must flow through a PageAccess
// implementation. A Transaction keeps modified and newly allocated pages in a
// transaction-private overlay so no uncommitted page can reach durable .gxdb
// state. CommittedPageAccess is the committed view: reads come from the bounded
// buffer cache (falling back to the file); writes install committed page images
// into the cache.

#include <cstdint>

#include "database_page.h"
#include "database_result.h"

namespace gxos {
namespace db {

class PageAccess {
public:
    virtual ~PageAccess() {}

    virtual uint32_t pageSize() const = 0;
    virtual uint64_t pageCount() const = 0;
    virtual bool isReadOnly() const = 0;

    // Reads a full page image. Page 0 (the header page) is not readable here.
    virtual DbResult readPage(uint64_t pageId, DatabasePage& out) = 0;

    // Records a complete page image (existing page or transaction-allocated).
    virtual DbResult writePage(const DatabasePage& page) = 0;

    // Allocates a new page and returns its id. The page image is owned by the
    // store until it is written.
    virtual DbResult allocatePage(PageType type, uint64_t& outPageId) = 0;
};

} // namespace db
} // namespace gxos
