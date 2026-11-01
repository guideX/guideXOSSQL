#ifndef GX_NATIVE_SQL_INDEX_PAGE_PROVIDER_H
#define GX_NATIVE_SQL_INDEX_PAGE_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/native_database_file.h"
#include "gx_index_leaf_traversal.h"

// This adapter supports only a caller-attested immutable committed image. The
// Server read-only handle prevents writes through this handle; it does not
// prevent another handle or process from modifying the same file.
enum GxNativeSqlReadConsistency {
    GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE = 1,
    GX_NATIVE_SQL_CONSISTENCY_LIVE_SINGLE_INSTANCE = 2
};

enum GxNativeSqlImageError {
    GX_NATIVE_SQL_IMAGE_OK = 0,
    GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT,
    GX_NATIVE_SQL_IMAGE_UNSUPPORTED_MODE,
    GX_NATIVE_SQL_IMAGE_BAD_STATE,
    GX_NATIVE_SQL_IMAGE_FILE_OPEN,
    GX_NATIVE_SQL_IMAGE_FILE_SIZE,
    GX_NATIVE_SQL_IMAGE_TRUNCATED,
    GX_NATIVE_SQL_IMAGE_HEADER_CORRUPT,
    GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH,
    GX_NATIVE_SQL_IMAGE_INVALID_ROOT,
    GX_NATIVE_SQL_IMAGE_OFFSET_OVERFLOW,
    GX_NATIVE_SQL_IMAGE_INVALID_PAGE,
    GX_NATIVE_SQL_IMAGE_READ_FAILURE,
    GX_NATIVE_SQL_IMAGE_CORRUPT_PAGE,
    GX_NATIVE_SQL_IMAGE_ACTIVE_BORROW,
    GX_NATIVE_SQL_IMAGE_INVALID_RELEASE,
    GX_NATIVE_SQL_IMAGE_CLOSE_FAILURE
};

// The SQL owner captures these values from one committed catalog/page view.
// The adapter checks the database header fields and extent against them, but
// does not parse the hosted STL-based Catalog to discover an index root.
// indexRootPageId == 0 denotes an index with no allocated root page.
struct GxNativeSqlIndexImageIdentity {
    uint32_t pageSize;
    uint64_t pageCount;
    uint64_t catalogRootPageId;
    uint64_t indexRootPageId;
    uint32_t ownerIndexId;
    GxNativeSqlReadConsistency consistency;
};

// Caller-owned, allocation-free operation context. pageBuffer must be at
// least pageSize bytes and remains owned by the caller until close succeeds.
// The adapter owns the read-only Server file handle from open through close.
struct GxNativeSqlIndexPageProviderContext {
    GxDbFileHandle fileHandle;
    uint8_t active;
    uint8_t borrowed;
    uint8_t* pageBuffer;
    size_t pageBufferCapacity;
    uint32_t pageSize;
    uint64_t pageCount;
    uint64_t fileSize;
    uint64_t catalogRootPageId;
    uint64_t indexRootPageId;
    uint32_t ownerIndexId;
    GxDbFileStatus nativeFileStatus;
    GxDbFileStatus cleanupFileStatus;
    GxNativeSqlImageError error;
};

void gxNativeSqlIndexPageProviderInit(
    GxNativeSqlIndexPageProviderContext* context);

// Opens the named .gxdb image using the actual guideXOS Server file bridge in
// read-only mode. The caller must guarantee that the image cannot be modified
// by any writer for the complete open/operate/close interval. Live access is
// rejected. Identity must have been captured from that same committed image.
GxNativeSqlImageError gxNativeSqlIndexPageProviderOpen(
    GxNativeSqlIndexPageProviderContext* context,
    const char* logicalDatabasePath,
    const GxNativeSqlIndexImageIdentity* identity,
    uint8_t* pageBuffer, size_t pageBufferCapacity);

// Returns the borrowed page-provider callbacks for the production portable
// gxIndexLookupAll/gxIndexRangeScan operations.
GxIndexPageProvider gxNativeSqlIndexPageProviderBind(
    GxNativeSqlIndexPageProviderContext* context);

// Closes the owned bridge handle. It refuses to end a view while a page borrow
// is active. A failed bridge close still consumes the Server bridge handle.
GxNativeSqlImageError gxNativeSqlIndexPageProviderClose(
    GxNativeSqlIndexPageProviderContext* context);

// Shared checked page-offset calculation. Server's VFS positional bridge is
// limited to a 32-bit file extent even though SQL page identifiers are u64.
int gxNativeSqlIndexPageOffset(uint64_t pageId, uint32_t pageSize,
                               uint64_t* outOffset);

#endif
