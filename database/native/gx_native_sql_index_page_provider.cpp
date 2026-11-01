#include "gx_native_sql_index_page_provider.h"

#include "database_checksum.h"
#include "database_endian.h"
#include "database_format.h"
#include "gx_page_codec.h"

namespace {

uint32_t headerCrc(const uint8_t* bytes) {
    // Match DatabaseHeader::computeCrc: serialized fields through byte 63 are
    // retained, while the CRC slot and reserved tail are zero for the digest.
    uint8_t scratch[gxos::db::kHeaderSize] = {};
    for (size_t i = 0; i < gxos::db::header_offset::HeaderCrc32; ++i) {
        scratch[i] = bytes[i];
    }
    return gxos::db::crc32(scratch, sizeof(scratch));
}

GxIndexPageProviderStatus acquirePage(void* opaque, uint64_t pageId,
                                      GxIndexPageView* outView) {
    GxNativeSqlIndexPageProviderContext* context =
        static_cast<GxNativeSqlIndexPageProviderContext*>(opaque);
    if (!context || !outView) {
        if (context) {
            context->error = GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT;
        }
        return GxIndexPageProviderStatus::Error;
    }

    outView->bytes = 0;
    outView->length = 0;
    outView->pageId = 0;
    outView->pageType = 0;
    outView->token = 0;
    if (!context->active || context->fileHandle == GX_DB_FILE_INVALID_HANDLE) {
        context->error = GX_NATIVE_SQL_IMAGE_BAD_STATE;
        return GxIndexPageProviderStatus::Error;
    }
    if (context->borrowed) {
        context->error = GX_NATIVE_SQL_IMAGE_ACTIVE_BORROW;
        return GxIndexPageProviderStatus::Error;
    }
    if (pageId == 0 || pageId >= context->pageCount) {
        context->error = GX_NATIVE_SQL_IMAGE_INVALID_PAGE;
        return GxIndexPageProviderStatus::InvalidPageId;
    }

    uint64_t offset = 0;
    if (!gxNativeSqlIndexPageOffset(pageId, context->pageSize, &offset) ||
        offset > context->fileSize ||
        context->pageSize > context->fileSize - offset) {
        context->error = GX_NATIVE_SQL_IMAGE_OFFSET_OVERFLOW;
        return GxIndexPageProviderStatus::Error;
    }

    context->nativeFileStatus = gx_db_file_read_at(
        context->fileHandle, offset, context->pageBuffer, context->pageSize);
    if (context->nativeFileStatus != GX_DB_FILE_OK) {
        context->error = context->nativeFileStatus == GX_DB_FILE_SHORT_READ
            ? GX_NATIVE_SQL_IMAGE_TRUNCATED
            : GX_NATIVE_SQL_IMAGE_READ_FAILURE;
        return GxIndexPageProviderStatus::Error;
    }

    GxDatabasePageInfo info;
    if (gxDatabasePageInspect(context->pageBuffer, context->pageSize,
                              context->pageSize, pageId, &info) != GxStatus::Ok ||
        info.payloadSize != context->pageSize - gxos::db::kPageHeaderSize ||
        (info.pageType != static_cast<uint16_t>(gxos::db::PageType::IndexLeaf) &&
         info.pageType != static_cast<uint16_t>(gxos::db::PageType::IndexInternal))) {
        context->error = GX_NATIVE_SQL_IMAGE_CORRUPT_PAGE;
        return GxIndexPageProviderStatus::Error;
    }

    outView->bytes = context->pageBuffer + gxos::db::kPageHeaderSize;
    outView->length = context->pageSize - gxos::db::kPageHeaderSize;
    outView->pageId = info.pageId;
    outView->pageType = info.pageType;
    outView->token = context;
    context->borrowed = 1;
    context->error = GX_NATIVE_SQL_IMAGE_OK;
    return GxIndexPageProviderStatus::Ok;
}

void releasePage(void* opaque, uint64_t pageId, const GxIndexPageView* view) {
    GxNativeSqlIndexPageProviderContext* context =
        static_cast<GxNativeSqlIndexPageProviderContext*>(opaque);
    if (!context) return;
    if (!context->active || !context->borrowed || !view ||
        view->token != context || view->pageId != pageId ||
        view->bytes != context->pageBuffer + gxos::db::kPageHeaderSize ||
        view->length != context->pageSize - gxos::db::kPageHeaderSize) {
        context->error = GX_NATIVE_SQL_IMAGE_INVALID_RELEASE;
        return;
    }
    context->borrowed = 0;
}

GxNativeSqlImageError closeAfterFailure(
    GxNativeSqlIndexPageProviderContext* context,
    GxNativeSqlImageError failure) {
    const GxDbFileStatus operationStatus = context->nativeFileStatus;
    context->cleanupFileStatus = GX_DB_FILE_OK;
    if (context->fileHandle != GX_DB_FILE_INVALID_HANDLE) {
        context->cleanupFileStatus = gx_db_file_close(context->fileHandle);
        context->fileHandle = GX_DB_FILE_INVALID_HANDLE;
    }
    context->nativeFileStatus = operationStatus;
    context->active = 0;
    context->error = failure;
    return failure;
}

GxNativeSqlImageError inspectDatabaseHeader(
    GxNativeSqlIndexPageProviderContext* context,
    const GxNativeSqlIndexImageIdentity* identity) {
    context->nativeFileStatus = gx_db_file_read_at(
        context->fileHandle, 0, context->pageBuffer, gxos::db::kHeaderSize);
    if (context->nativeFileStatus != GX_DB_FILE_OK) {
        context->error = context->nativeFileStatus == GX_DB_FILE_SHORT_READ
            ? GX_NATIVE_SQL_IMAGE_TRUNCATED
            : GX_NATIVE_SQL_IMAGE_READ_FAILURE;
        return context->error;
    }

    const uint8_t* bytes = context->pageBuffer;
    if (bytes[0] != gxos::db::kFileMagic[0] ||
        bytes[1] != gxos::db::kFileMagic[1] ||
        bytes[2] != gxos::db::kFileMagic[2] ||
        bytes[3] != gxos::db::kFileMagic[3] ||
        bytes[4] != gxos::db::kFileMagic[4] ||
        bytes[5] != gxos::db::kFileMagic[5] ||
        bytes[6] != gxos::db::kFileMagic[6] ||
        bytes[7] != gxos::db::kFileMagic[7] ||
        gxos::db::loadLe16(bytes + gxos::db::header_offset::FormatMajor) !=
            gxos::db::kFormatMajor ||
        gxos::db::loadLe32(bytes + gxos::db::header_offset::HeaderSize) !=
            gxos::db::kHeaderSize ||
        gxos::db::loadLe32(bytes + gxos::db::header_offset::HeaderCrc32) !=
            headerCrc(bytes)) {
        context->error = GX_NATIVE_SQL_IMAGE_HEADER_CORRUPT;
        return context->error;
    }

    const uint32_t pageSize =
        gxos::db::loadLe32(bytes + gxos::db::header_offset::PageSize);
    const uint64_t pageCount =
        gxos::db::loadLe64(bytes + gxos::db::header_offset::PageCount);
    const uint64_t catalogRootPageId =
        gxos::db::loadLe64(bytes + gxos::db::header_offset::RootPageId);
    if (pageSize != identity->pageSize || pageCount != identity->pageCount ||
        catalogRootPageId != identity->catalogRootPageId) {
        context->error = GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH;
        return context->error;
    }
    if (pageCount < 2 || pageCount > gxos::db::kMaxPageCount ||
        catalogRootPageId == 0 || catalogRootPageId >= pageCount) {
        context->error = GX_NATIVE_SQL_IMAGE_HEADER_CORRUPT;
        return context->error;
    }
    return GX_NATIVE_SQL_IMAGE_OK;
}

} // namespace

void gxNativeSqlIndexPageProviderInit(
    GxNativeSqlIndexPageProviderContext* context) {
    if (!context) return;
    context->fileHandle = GX_DB_FILE_INVALID_HANDLE;
    context->active = 0;
    context->borrowed = 0;
    context->pageBuffer = 0;
    context->pageBufferCapacity = 0;
    context->pageSize = 0;
    context->pageCount = 0;
    context->fileSize = 0;
    context->catalogRootPageId = 0;
    context->indexRootPageId = 0;
    context->ownerIndexId = 0;
    context->nativeFileStatus = GX_DB_FILE_OK;
    context->cleanupFileStatus = GX_DB_FILE_OK;
    context->error = GX_NATIVE_SQL_IMAGE_OK;
}

int gxNativeSqlIndexPageOffset(uint64_t pageId, uint32_t pageSize,
                               uint64_t* outOffset) {
    if (!outOffset || pageSize == 0 || pageId > UINT64_MAX / pageSize) {
        return 0;
    }
    const uint64_t offset = pageId * pageSize;
    if (offset > 0xFFFFFFFFull ||
        pageSize > 0xFFFFFFFFull - offset) {
        return 0;
    }
    *outOffset = offset;
    return 1;
}

GxNativeSqlImageError gxNativeSqlIndexPageProviderOpen(
    GxNativeSqlIndexPageProviderContext* context,
    const char* logicalDatabasePath,
    const GxNativeSqlIndexImageIdentity* identity,
    uint8_t* pageBuffer, size_t pageBufferCapacity) {
    if (!context) return GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT;
    if (context->active || context->fileHandle != GX_DB_FILE_INVALID_HANDLE) {
        context->error = GX_NATIVE_SQL_IMAGE_BAD_STATE;
        return context->error;
    }
    context->error = GX_NATIVE_SQL_IMAGE_OK;
    context->nativeFileStatus = GX_DB_FILE_OK;
    if (!logicalDatabasePath || !identity || !pageBuffer) {
        context->error = GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT;
        return context->error;
    }
    if (identity->consistency != GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE) {
        context->error = GX_NATIVE_SQL_IMAGE_UNSUPPORTED_MODE;
        return context->error;
    }
    if (!gxos::db::isSupportedPageSize(identity->pageSize) ||
        pageBufferCapacity < identity->pageSize || identity->pageCount < 2 ||
        identity->pageCount > gxos::db::kMaxPageCount ||
        identity->ownerIndexId == 0) {
        context->error = GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT;
        return context->error;
    }
    if (identity->catalogRootPageId == 0 ||
        identity->catalogRootPageId >= identity->pageCount ||
        (identity->indexRootPageId != 0 &&
         identity->indexRootPageId >= identity->pageCount)) {
        context->error = GX_NATIVE_SQL_IMAGE_INVALID_ROOT;
        return context->error;
    }
    uint64_t expectedSize = 0;
    if (identity->pageCount > UINT64_MAX / identity->pageSize) {
        context->error = GX_NATIVE_SQL_IMAGE_OFFSET_OVERFLOW;
        return context->error;
    }
    expectedSize = identity->pageCount * identity->pageSize;
    if (expectedSize > 0xFFFFFFFFull) {
        context->error = GX_NATIVE_SQL_IMAGE_OFFSET_OVERFLOW;
        return context->error;
    }

    context->pageBuffer = pageBuffer;
    context->pageBufferCapacity = pageBufferCapacity;
    context->pageSize = identity->pageSize;
    context->pageCount = identity->pageCount;
    context->catalogRootPageId = identity->catalogRootPageId;
    context->indexRootPageId = identity->indexRootPageId;
    context->ownerIndexId = identity->ownerIndexId;

    context->nativeFileStatus = gx_db_file_open(
        logicalDatabasePath, GX_DB_OPEN_READ_ONLY, 0, &context->fileHandle);
    if (context->nativeFileStatus != GX_DB_FILE_OK) {
        context->fileHandle = GX_DB_FILE_INVALID_HANDLE;
        context->error = GX_NATIVE_SQL_IMAGE_FILE_OPEN;
        return context->error;
    }
    context->active = 1;

    context->nativeFileStatus = gx_db_file_get_size(
        context->fileHandle, &context->fileSize);
    if (context->nativeFileStatus != GX_DB_FILE_OK) {
        return closeAfterFailure(context, GX_NATIVE_SQL_IMAGE_FILE_SIZE);
    }
    if (context->fileSize < gxos::db::kHeaderSize) {
        return closeAfterFailure(context, GX_NATIVE_SQL_IMAGE_TRUNCATED);
    }
    if (context->fileSize < expectedSize) {
        return closeAfterFailure(context, GX_NATIVE_SQL_IMAGE_TRUNCATED);
    }
    if (context->fileSize % identity->pageSize != 0) {
        return closeAfterFailure(context, GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH);
    }

    const GxNativeSqlImageError headerStatus =
        inspectDatabaseHeader(context, identity);
    if (headerStatus != GX_NATIVE_SQL_IMAGE_OK) {
        return closeAfterFailure(context, headerStatus);
    }
    context->error = GX_NATIVE_SQL_IMAGE_OK;
    return context->error;
}

GxIndexPageProvider gxNativeSqlIndexPageProviderBind(
    GxNativeSqlIndexPageProviderContext* context) {
    GxIndexPageProvider provider = {context, acquirePage, releasePage};
    return provider;
}

GxNativeSqlImageError gxNativeSqlIndexPageProviderClose(
    GxNativeSqlIndexPageProviderContext* context) {
    if (!context) return GX_NATIVE_SQL_IMAGE_INVALID_ARGUMENT;
    if (!context->active || context->fileHandle == GX_DB_FILE_INVALID_HANDLE) {
        context->error = GX_NATIVE_SQL_IMAGE_BAD_STATE;
        return context->error;
    }
    if (context->borrowed) {
        context->error = GX_NATIVE_SQL_IMAGE_ACTIVE_BORROW;
        return context->error;
    }
    context->nativeFileStatus = gx_db_file_close(context->fileHandle);
    context->fileHandle = GX_DB_FILE_INVALID_HANDLE;
    context->active = 0;
    if (context->nativeFileStatus != GX_DB_FILE_OK) {
        context->error = GX_NATIVE_SQL_IMAGE_CLOSE_FAILURE;
        return context->error;
    }
    context->error = GX_NATIVE_SQL_IMAGE_OK;
    return context->error;
}
