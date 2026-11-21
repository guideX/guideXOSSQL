#include "gx_native_sql_read_service.h"

#include "database_endian.h"
#include "database_format.h"
#include "gx_index_boolean_key.h"
#include "gx_index_integer_key.h"
#include "gx_index_internal_node.h"
#include "gx_index_lookup.h"
#include "gx_page_codec.h"

// The Server adds this diagnostic-gated bridge probe alongside its VFS. Keep
// the native adapter source-compatible with the older hosted bridge used by
// the SQL provider regression target, which does not declare that optional API.
extern "C" GxDbFileStatus gx_db_file_is_read_only(
    GxDbFileHandle handle, uint8_t* outReadOnly);

namespace {

const char kFixturePath[] = "/system/Apps/R3AI/MULTI.gxdb";
const uint32_t kPageSize = 512;
const uint64_t kPageCount = 490;
const uint64_t kCatalogRootPageId = 1;
const uint64_t kCreationTimeUnixNanos = 1793556264120817800ull;
const size_t kFixtureBytes = 250880;
const uint32_t kMaxDepth = gxos::db::kMaxIndexDepth;
const uint32_t kMaxResults = static_cast<uint32_t>(GX_NATIVE_SQL_READ_MAX_RESULTS);

struct IndexIdentity {
    GxNativeSqlReadIndex identity;
    uint64_t rootPageId;
    uint32_t ownerIndexId;
    uint32_t keyType;
    GxNativeSqlIndexCatalogEntry catalog;
};

// This trusted package configuration is also the query dispatch catalog.
// Names/types/capabilities are sourced from the reviewed R3AH package
// manifest; identity/root/owner must match the actual index pages at runtime.
const IndexIdentity kIndexes[] = {
    {GX_NATIVE_SQL_INDEX_ROWS_K64, 234, 1, GX_NATIVE_SQL_KEY_INT64,
     {GX_NATIVE_SQL_INDEX_ROWS_K64, "IX_Rows_K64", GX_NATIVE_SQL_KEY_INT64,
      GX_NATIVE_SQL_INDEX_CAP_POINT,
      GX_NATIVE_SQL_COMPARE_SIGNED_ASCENDING_NULLS_FIRST, kMaxResults}},
    {GX_NATIVE_SQL_INDEX_ROWS_K32, 148, 2, GX_NATIVE_SQL_KEY_INT32,
     {GX_NATIVE_SQL_INDEX_ROWS_K32, "IX_Rows_K32", GX_NATIVE_SQL_KEY_INT32,
      GX_NATIVE_SQL_INDEX_CAP_RANGE,
      GX_NATIVE_SQL_COMPARE_SIGNED_ASCENDING_NULLS_FIRST, kMaxResults}},
    {GX_NATIVE_SQL_INDEX_ROWS_KBOOL, 124, 3, GX_NATIVE_SQL_KEY_BOOLEAN,
     {GX_NATIVE_SQL_INDEX_ROWS_KBOOL, "IX_Rows_KBool", GX_NATIVE_SQL_KEY_BOOLEAN,
      GX_NATIVE_SQL_INDEX_CAP_POINT,
      GX_NATIVE_SQL_COMPARE_BOOLEAN_ASCENDING_NULLS_FIRST, kMaxResults}}
};
const size_t kIndexCount = sizeof(kIndexes) / sizeof(kIndexes[0]);

struct StagedLocator {
    uint64_t pageId;
    uint32_t slot;
};

struct Collector {
    StagedLocator rows[GX_NATIVE_SQL_READ_MAX_RESULTS];
    size_t count;
    uint8_t overflow;
};

const uint8_t kExpectedDatabaseId[16] = {
    0x32,0xB4,0x66,0x22,0xAA,0x73,0x49,0x3D,
    0xA2,0x6D,0xE8,0x26,0xDE,0x2B,0xB7,0x90
};

bool identityFor(GxNativeSqlReadIndex index, IndexIdentity* out) {
    if (!out) return false;
    for (size_t i = 0; i < kIndexCount; ++i) {
        if (kIndexes[i].identity == index) {
            *out = kIndexes[i];
            return true;
        }
    }
    return false;
}

bool validEncodedKey(const uint8_t* bytes, size_t length, uint32_t keyType) {
    if (keyType == GX_NATIVE_SQL_KEY_INT64) {
        int64_t value = 0;
        bool isNull = false;
        return gxDecodeIndexInt64Key(bytes, length, &value, &isNull) ==
            GxStatus::Ok;
    }
    if (keyType == GX_NATIVE_SQL_KEY_INT32) {
        int32_t value = 0;
        bool isNull = false;
        return gxDecodeIndexInt32Key(bytes, length, &value, &isNull) ==
            GxStatus::Ok;
    }
    if (keyType == GX_NATIVE_SQL_KEY_BOOLEAN) {
        bool value = false;
        bool isNull = false;
        return gxDecodeIndexBooleanKey(bytes, length, &value, &isNull) ==
            GxStatus::Ok;
    }
    return false;
}

bool validKeySpan(const uint8_t* bytes, size_t length, uint32_t keyType) {
    const size_t maxLength = keyType == GX_NATIVE_SQL_KEY_INT64 ? 9u :
        keyType == GX_NATIVE_SQL_KEY_INT32 ? 5u :
        keyType == GX_NATIVE_SQL_KEY_BOOLEAN ? 2u : 0u;
    return bytes != 0 && length != 0 && length <= maxLength &&
        validEncodedKey(bytes, length, keyType);
}

bool validRequest(const GxNativeSqlReadRequest& request,
                  GxNativeSqlReadStatus* failure) {
    if (request.operation != GX_NATIVE_SQL_READ_POINT &&
        request.operation != GX_NATIVE_SQL_READ_RANGE) {
        *failure = GX_NATIVE_SQL_READ_UNSUPPORTED_OPERATION;
        return false;
    }
    if (request.consistency != GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE) {
        *failure = request.consistency == GX_NATIVE_SQL_CONSISTENCY_LIVE_SINGLE_INSTANCE
            ? GX_NATIVE_SQL_READ_UNSUPPORTED_OPERATION
            : GX_NATIVE_SQL_READ_INVALID_REQUEST;
        return false;
    }
    IndexIdentity index = {};
    if (request.fixture != GX_NATIVE_SQL_FIXTURE_R3AH_MULTILEVEL ||
        !identityFor(request.index, &index) ||
        request.catalogRootPageId != kCatalogRootPageId ||
        request.rootPageId != index.rootPageId ||
        request.ownerIndexId != index.ownerIndexId) {
        *failure = GX_NATIVE_SQL_READ_INVALID_REQUEST;
        return false;
    }
    if (request.operation == GX_NATIVE_SQL_READ_POINT) {
        if (!validKeySpan(request.keyBytes, request.keyLength,
                          index.keyType) ||
            request.hasLower || request.lowerKeyBytes || request.lowerKeyLength ||
            request.lowerInclusive || request.hasUpper || request.upperKeyBytes ||
            request.upperKeyLength || request.upperInclusive ||
            (request.index != GX_NATIVE_SQL_INDEX_ROWS_K64 &&
             request.index != GX_NATIVE_SQL_INDEX_ROWS_KBOOL)) {
            *failure = GX_NATIVE_SQL_READ_INVALID_REQUEST;
            return false;
        }
        return true;
    }

    if (request.index != GX_NATIVE_SQL_INDEX_ROWS_K32 ||
        request.keyBytes || request.keyLength || request.hasLower > 1 ||
        request.hasUpper > 1 || request.lowerInclusive > 1 ||
        request.upperInclusive > 1) {
        *failure = GX_NATIVE_SQL_READ_INVALID_REQUEST;
        return false;
    }
    if ((request.hasLower && !validKeySpan(request.lowerKeyBytes,
                                            request.lowerKeyLength,
                                            GX_NATIVE_SQL_KEY_INT32)) ||
        (!request.hasLower && (request.lowerKeyBytes || request.lowerKeyLength ||
                               request.lowerInclusive)) ||
        (request.hasUpper && !validKeySpan(request.upperKeyBytes,
                                            request.upperKeyLength,
                                            GX_NATIVE_SQL_KEY_INT32)) ||
        (!request.hasUpper && (request.upperKeyBytes || request.upperKeyLength ||
                               request.upperInclusive))) {
        *failure = GX_NATIVE_SQL_READ_INVALID_REQUEST;
        return false;
    }
    return true;
}

bool headerMatches(const GxNativeSqlIndexPageProviderContext& context) {
    if (context.fileSize != kFixtureBytes || context.pageSize != kPageSize ||
        context.pageCount != kPageCount ||
        context.catalogRootPageId != kCatalogRootPageId) {
        return false;
    }
    const uint8_t* header = context.pageBuffer;
    if (gxos::db::loadLe16(header + gxos::db::header_offset::FormatMajor) !=
            gxos::db::kFormatMajor ||
        gxos::db::loadLe16(header + gxos::db::header_offset::FormatMinor) !=
            gxos::db::kFormatMinor ||
        gxos::db::loadLe64(header + gxos::db::header_offset::CreationTime) !=
            kCreationTimeUnixNanos) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kExpectedDatabaseId); ++i) {
        if (header[gxos::db::header_offset::DatabaseId + i] !=
            kExpectedDatabaseId[i]) return false;
    }
    return true;
}

GxNativeSqlReadStatus imageFailureStatus(GxNativeSqlImageError error) {
    switch (error) {
    case GX_NATIVE_SQL_IMAGE_HEADER_CORRUPT:
    case GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH:
    case GX_NATIVE_SQL_IMAGE_INVALID_ROOT:
    case GX_NATIVE_SQL_IMAGE_TRUNCATED:
    case GX_NATIVE_SQL_IMAGE_INVALID_PAGE:
    case GX_NATIVE_SQL_IMAGE_CORRUPT_PAGE:
    case GX_NATIVE_SQL_IMAGE_INVALID_RELEASE:
        return GX_NATIVE_SQL_READ_CORRUPT_INDEX;
    default:
        return GX_NATIVE_SQL_READ_STORAGE_FAILURE;
    }
}

GxNativeSqlReadStatus lookupFailureStatus(
    GxIndexLookupStatus status,
    const GxNativeSqlIndexPageProviderContext& context) {
    if (context.error != GX_NATIVE_SQL_IMAGE_OK) {
        return imageFailureStatus(context.error);
    }
    switch (status) {
    case GxIndexLookupStatus::InvalidPageId:
    case GxIndexLookupStatus::MissingPage:
    case GxIndexLookupStatus::CorruptPage:
    case GxIndexLookupStatus::Cycle:
    case GxIndexLookupStatus::TraversalLimit:
        return GX_NATIVE_SQL_READ_CORRUPT_INDEX;
    case GxIndexLookupStatus::InvalidArgument:
        return GX_NATIVE_SQL_READ_INVALID_REQUEST;
    default:
        return GX_NATIVE_SQL_READ_STORAGE_FAILURE;
    }
}

GxIndexLookupVisitControl collectPoint(void* opaque, uint64_t pageId,
                                        uint32_t slot) {
    Collector* collector = static_cast<Collector*>(opaque);
    if (!collector || collector->count >= GX_NATIVE_SQL_READ_MAX_RESULTS) {
        if (collector) collector->overflow = 1;
        return GxIndexLookupVisitControl::Error;
    }
    collector->rows[collector->count++] = {pageId, slot};
    return GxIndexLookupVisitControl::Continue;
}

GxIndexRangeScanVisitControl collectRange(void* opaque, const uint8_t*, size_t,
                                          uint64_t pageId, uint32_t slot) {
    Collector* collector = static_cast<Collector*>(opaque);
    if (!collector || collector->count >= GX_NATIVE_SQL_READ_MAX_RESULTS) {
        if (collector) collector->overflow = 1;
        return GxIndexRangeScanVisitControl::Error;
    }
    collector->rows[collector->count++] = {pageId, slot};
    return GxIndexRangeScanVisitControl::Continue;
}

bool selectedRootMatches(GxNativeSqlIndexPageProviderContext* context,
                         const IndexIdentity& index) {
    const GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(context);
    GxIndexPageView view = {};
    if (provider.acquire(provider.context, index.rootPageId, &view) !=
        GxIndexPageProviderStatus::Ok) return false;
    GxIndexInternalNodeCursor cursor = {};
    GxIndexInternalNodeError error = GxIndexInternalNodeError::None;
    const bool valid = view.pageId == index.rootPageId &&
        view.pageType == static_cast<uint16_t>(gxos::db::PageType::IndexInternal) &&
        gxIndexInternalNodeCursorInit(view.bytes, view.length, kPageCount,
            index.ownerIndexId, &cursor, &error) == GxStatus::Ok;
    provider.release(provider.context, index.rootPageId, &view);
    return valid && !context->borrowed &&
        context->error == GX_NATIVE_SQL_IMAGE_OK;
}

bool catalogRootMatches(GxNativeSqlIndexPageProviderContext* context) {
    uint64_t offset = 0;
    if (!gxNativeSqlIndexPageOffset(context->catalogRootPageId,
                                    context->pageSize, &offset)) {
        context->error = GX_NATIVE_SQL_IMAGE_OFFSET_OVERFLOW;
        return false;
    }
    context->nativeFileStatus = gx_db_file_read_at(
        context->fileHandle, offset, context->pageBuffer, context->pageSize);
    if (context->nativeFileStatus != GX_DB_FILE_OK) {
        context->error = context->nativeFileStatus == GX_DB_FILE_SHORT_READ
            ? GX_NATIVE_SQL_IMAGE_TRUNCATED
            : GX_NATIVE_SQL_IMAGE_READ_FAILURE;
        return false;
    }

    GxDatabasePageInfo pageInfo = {};
    if (gxDatabasePageInspect(context->pageBuffer, context->pageSize,
            context->pageSize, context->catalogRootPageId, &pageInfo) !=
            GxStatus::Ok ||
        pageInfo.pageType != static_cast<uint16_t>(gxos::db::PageType::Catalog) ||
        pageInfo.payloadSize < 8) {
        context->error = GX_NATIVE_SQL_IMAGE_CORRUPT_PAGE;
        return false;
    }

    const uint8_t* payload = context->pageBuffer + gxos::db::kPageHeaderSize;
    const uint16_t version = gxos::db::loadLe16(payload + 4);
    const uint32_t rootHeaderSize = version == gxos::db::kCatalogVersionV3
        ? gxos::db::kCatalogV3RootHeaderSize
        : version == gxos::db::kCatalogVersionV4
            ? gxos::db::kCatalogV4RootHeaderSize : 0;
    if (gxos::db::loadLe32(payload) != gxos::db::kCatalogMagic ||
        rootHeaderSize == 0 || pageInfo.payloadSize < rootHeaderSize ||
        gxos::db::loadLe16(payload + 6) != 0) {
        context->error = GX_NATIVE_SQL_IMAGE_CORRUPT_PAGE;
        return false;
    }

    const uint32_t continuationCount = gxos::db::loadLe32(payload + 16);
    const uint64_t firstContinuation = gxos::db::loadLe32(payload + 20);
    const uint32_t tableCount = gxos::db::loadLe32(payload + 8);
    const uint32_t rootRecordCount = gxos::db::loadLe32(payload + 24);
    const uint32_t indexCount = gxos::db::loadLe32(payload + 28);
    const uint32_t foreignKeyCount = version == gxos::db::kCatalogVersionV4
        ? gxos::db::loadLe32(payload + 36) : 0;
    if (tableCount > gxos::db::kMaxTables ||
        continuationCount > context->pageCount ||
        (continuationCount == 0 && firstContinuation != 0) ||
        (continuationCount != 0 &&
         (firstContinuation == 0 || firstContinuation >= context->pageCount)) ||
        indexCount < kIndexCount || indexCount > gxos::db::kMaxIndexes ||
        rootRecordCount > tableCount + indexCount + foreignKeyCount ||
        (version == gxos::db::kCatalogVersionV3 &&
         gxos::db::loadLe32(payload + 36) != 0) ||
        (version == gxos::db::kCatalogVersionV4 &&
         (foreignKeyCount > gxos::db::kMaxForeignKeys ||
          gxos::db::loadLe32(payload + 44) != 0))) {
        context->error = GX_NATIVE_SQL_IMAGE_CORRUPT_PAGE;
        return false;
    }
    return true;
}

GxNativeSqlReadStatus executeTraversal(
    const GxNativeSqlReadRequest& request,
    GxNativeSqlIndexPageProviderContext* context,
    Collector* collector,
    GxIndexLookupStatus* outLookupStatus) {
    uint64_t visited[kMaxDepth + 1] = {};
    uint8_t physicalLowerBound[9 + 12] = {};
    uint8_t previousPhysicalKey[kPageSize - gxos::db::kPageHeaderSize] = {};
    GxIndexLookupResult lookup = {};
    const GxIndexPageProvider provider = gxNativeSqlIndexPageProviderBind(context);
    GxIndexLookupStatus status;
    if (request.operation == GX_NATIVE_SQL_READ_POINT) {
        status = gxIndexLookupAll(&provider, request.rootPageId, kPageCount,
            request.ownerIndexId, request.keyBytes, request.keyLength,
            kMaxDepth, visited, sizeof(visited) / sizeof(visited[0]),
            kPageCount - 1, physicalLowerBound, sizeof(physicalLowerBound),
            previousPhysicalKey, sizeof(previousPhysicalKey), collectPoint,
            collector, &lookup);
    } else {
        status = gxIndexRangeScan(&provider, request.rootPageId, kPageCount,
            request.ownerIndexId, request.hasLower != 0, request.lowerKeyBytes,
            request.lowerKeyLength, request.lowerInclusive != 0,
            request.hasUpper != 0, request.upperKeyBytes,
            request.upperKeyLength, request.upperInclusive != 0, kMaxDepth,
            visited, sizeof(visited) / sizeof(visited[0]), kPageCount - 1,
            physicalLowerBound, sizeof(physicalLowerBound), previousPhysicalKey,
            sizeof(previousPhysicalKey), collectRange, collector, &lookup);
    }
    *outLookupStatus = status;
    if (collector->overflow) return GX_NATIVE_SQL_READ_RESOURCE_EXHAUSTED;
    if (status != GxIndexLookupStatus::Complete) {
        return lookupFailureStatus(status, *context);
    }
    if (context->borrowed || context->error != GX_NATIVE_SQL_IMAGE_OK) {
        return context->error == GX_NATIVE_SQL_IMAGE_OK
            ? GX_NATIVE_SQL_READ_CORRUPT_INDEX
            : imageFailureStatus(context->error);
    }
    return GX_NATIVE_SQL_READ_OK;
}

} // namespace

GxNativeSqlReadStatus gxNativeSqlExecuteRead(
    const GxNativeSqlReadRequest* request,
    GxNativeSqlReadLocator* resultRows,
    size_t resultCapacity,
    GxNativeSqlReadResponse* response) {
    if (!response) return GX_NATIVE_SQL_READ_INVALID_REQUEST;
    response->status = GX_NATIVE_SQL_READ_INVALID_REQUEST;
    response->rowCount = 0;
    response->imageError = GX_NATIVE_SQL_IMAGE_OK;
    response->cleanupError = GX_NATIVE_SQL_IMAGE_OK;
    response->lookupStatus = GxIndexLookupStatus::InvalidArgument;
    if (!request) return response->status;

    GxNativeSqlReadStatus failure = GX_NATIVE_SQL_READ_INVALID_REQUEST;
    if (!validRequest(*request, &failure)) {
        response->status = failure;
        return response->status;
    }
    if (!resultRows || resultCapacity == 0) {
        response->status = GX_NATIVE_SQL_READ_RESOURCE_EXHAUSTED;
        return response->status;
    }
    if (resultCapacity > GX_NATIVE_SQL_READ_MAX_RESULTS) {
        response->status = GX_NATIVE_SQL_READ_RESOURCE_EXHAUSTED;
        return response->status;
    }

    IndexIdentity index = {};
    (void)identityFor(request->index, &index);
    uint8_t pageBuffer[kPageSize] = {};
    GxNativeSqlIndexPageProviderContext context = {};
    gxNativeSqlIndexPageProviderInit(&context);
    GxNativeSqlIndexImageIdentity imageIdentity = {};
    imageIdentity.pageSize = kPageSize;
    imageIdentity.pageCount = kPageCount;
    imageIdentity.catalogRootPageId = kCatalogRootPageId;
    imageIdentity.indexRootPageId = index.rootPageId;
    imageIdentity.ownerIndexId = index.ownerIndexId;
    imageIdentity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;
    GxNativeSqlImageError openError = gxNativeSqlIndexPageProviderOpen(
        &context, kFixturePath, &imageIdentity, pageBuffer, sizeof(pageBuffer));
    if (openError != GX_NATIVE_SQL_IMAGE_OK) {
        response->imageError = openError;
        response->cleanupError = context.cleanupFileStatus == GX_DB_FILE_OK
            ? GX_NATIVE_SQL_IMAGE_OK : GX_NATIVE_SQL_IMAGE_CLOSE_FAILURE;
        response->status = response->cleanupError == GX_NATIVE_SQL_IMAGE_OK
            ? imageFailureStatus(openError) : GX_NATIVE_SQL_READ_STORAGE_FAILURE;
        return response->status;
    }

    uint8_t readOnlyMount = 0;
    const GxDbFileStatus mountStatus = gx_db_file_is_read_only(
        context.fileHandle, &readOnlyMount);
    if (mountStatus != GX_DB_FILE_OK || !readOnlyMount) {
        const GxNativeSqlImageError closeError =
            gxNativeSqlIndexPageProviderClose(&context);
        response->imageError = mountStatus == GX_DB_FILE_OK
            ? GX_NATIVE_SQL_IMAGE_UNSUPPORTED_MODE
            : GX_NATIVE_SQL_IMAGE_BAD_STATE;
        response->cleanupError = closeError;
        response->status = closeError == GX_NATIVE_SQL_IMAGE_OK
            ? GX_NATIVE_SQL_READ_UNSUPPORTED_OPERATION
            : GX_NATIVE_SQL_READ_STORAGE_FAILURE;
        return response->status;
    }

    Collector collector = {};
    GxNativeSqlReadStatus operationStatus = GX_NATIVE_SQL_READ_OK;
    if (!headerMatches(context)) {
        operationStatus = GX_NATIVE_SQL_READ_CORRUPT_INDEX;
        response->imageError = GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH;
    } else if (!catalogRootMatches(&context)) {
        response->imageError = context.error;
        operationStatus = imageFailureStatus(context.error);
    } else if (!selectedRootMatches(&context, index)) {
        response->imageError = context.error;
        operationStatus = context.error == GX_NATIVE_SQL_IMAGE_OK
            ? GX_NATIVE_SQL_READ_CORRUPT_INDEX : imageFailureStatus(context.error);
    } else {
        operationStatus = executeTraversal(*request, &context, &collector,
                                           &response->lookupStatus);
        response->imageError = context.error;
    }

    if (context.active) {
        const GxNativeSqlImageError closeError =
            gxNativeSqlIndexPageProviderClose(&context);
        if (closeError != GX_NATIVE_SQL_IMAGE_OK) {
            response->cleanupError = closeError;
            response->status = GX_NATIVE_SQL_READ_STORAGE_FAILURE;
            return response->status;
        }
    } else if (context.fileHandle != GX_DB_FILE_INVALID_HANDLE) {
        // A provider failure must never leave an owned handle behind.
        response->cleanupError = GX_NATIVE_SQL_IMAGE_BAD_STATE;
        response->status = GX_NATIVE_SQL_READ_STORAGE_FAILURE;
        return response->status;
    }

    if (operationStatus != GX_NATIVE_SQL_READ_OK) {
        response->status = operationStatus;
        return response->status;
    }
    if (collector.count == 0) {
        response->status = GX_NATIVE_SQL_READ_NO_MATCH;
        return response->status;
    }
    if (collector.count > resultCapacity) {
        response->status = GX_NATIVE_SQL_READ_RESOURCE_EXHAUSTED;
        return response->status;
    }

    for (size_t i = 0; i < collector.count; ++i) {
        resultRows[i].pageId = collector.rows[i].pageId;
        resultRows[i].slot = collector.rows[i].slot;
    }
    response->rowCount = collector.count;
    response->status = GX_NATIVE_SQL_READ_OK;
    return response->status;
}

GxNativeSqlReadStatus gxNativeSqlValidateDatabase(
    GxNativeSqlReadFixture fixture,
    GxNativeSqlImageError* imageError,
    GxNativeSqlImageError* cleanupError) {
    if (!imageError || !cleanupError) return GX_NATIVE_SQL_READ_INVALID_REQUEST;
    *imageError = GX_NATIVE_SQL_IMAGE_OK;
    *cleanupError = GX_NATIVE_SQL_IMAGE_OK;
    if (fixture != GX_NATIVE_SQL_FIXTURE_R3AH_MULTILEVEL) {
        return GX_NATIVE_SQL_READ_INVALID_REQUEST;
    }

    IndexIdentity index = {};
    if (!identityFor(GX_NATIVE_SQL_INDEX_ROWS_K64, &index)) {
        return GX_NATIVE_SQL_READ_INVALID_REQUEST;
    }

    uint8_t pageBuffer[kPageSize] = {};
    GxNativeSqlIndexPageProviderContext context = {};
    gxNativeSqlIndexPageProviderInit(&context);
    GxNativeSqlIndexImageIdentity imageIdentity = {};
    imageIdentity.pageSize = kPageSize;
    imageIdentity.pageCount = kPageCount;
    imageIdentity.catalogRootPageId = kCatalogRootPageId;
    imageIdentity.indexRootPageId = index.rootPageId;
    imageIdentity.ownerIndexId = index.ownerIndexId;
    imageIdentity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;

    *imageError = gxNativeSqlIndexPageProviderOpen(
        &context, kFixturePath, &imageIdentity, pageBuffer, sizeof(pageBuffer));
    if (*imageError != GX_NATIVE_SQL_IMAGE_OK) {
        GxNativeSqlReadStatus status = imageFailureStatus(*imageError);
        if (context.active) {
            *cleanupError = gxNativeSqlIndexPageProviderClose(&context);
        } else if (context.fileHandle != GX_DB_FILE_INVALID_HANDLE) {
            *cleanupError = GX_NATIVE_SQL_IMAGE_BAD_STATE;
        }
        return *cleanupError == GX_NATIVE_SQL_IMAGE_OK
            ? status : GX_NATIVE_SQL_READ_STORAGE_FAILURE;
    }

    uint8_t readOnlyMount = 0;
    const GxDbFileStatus mountStatus = gx_db_file_is_read_only(
        context.fileHandle, &readOnlyMount);
    GxNativeSqlReadStatus status = GX_NATIVE_SQL_READ_OK;
    if (mountStatus != GX_DB_FILE_OK) {
        *imageError = GX_NATIVE_SQL_IMAGE_BAD_STATE;
        status = GX_NATIVE_SQL_READ_STORAGE_FAILURE;
    } else if (!readOnlyMount) {
        *imageError = GX_NATIVE_SQL_IMAGE_UNSUPPORTED_MODE;
        status = GX_NATIVE_SQL_READ_UNSUPPORTED_OPERATION;
    } else if (!headerMatches(context)) {
        *imageError = GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH;
        status = GX_NATIVE_SQL_READ_CORRUPT_INDEX;
    } else if (!catalogRootMatches(&context)) {
        *imageError = context.error;
        status = imageFailureStatus(context.error);
    } else if (!selectedRootMatches(&context, index)) {
        if (context.error != GX_NATIVE_SQL_IMAGE_OK) {
            *imageError = context.error;
            status = imageFailureStatus(context.error);
        } else {
            *imageError = GX_NATIVE_SQL_IMAGE_INVALID_ROOT;
            status = GX_NATIVE_SQL_READ_CORRUPT_INDEX;
        }
    }

    if (context.active) {
        *cleanupError = gxNativeSqlIndexPageProviderClose(&context);
    } else if (context.fileHandle != GX_DB_FILE_INVALID_HANDLE) {
        *cleanupError = GX_NATIVE_SQL_IMAGE_BAD_STATE;
    }
    if (*cleanupError != GX_NATIVE_SQL_IMAGE_OK) {
        return GX_NATIVE_SQL_READ_STORAGE_FAILURE;
    }
    return status;
}

size_t gxNativeSqlGetSupportedIndexCount(void) {
    return kIndexCount;
}

const GxNativeSqlIndexCatalogEntry* gxNativeSqlGetSupportedIndexAt(
    size_t index) {
    return index < kIndexCount ? &kIndexes[index].catalog : 0;
}

int gxNativeSqlGetReadIndexIdentity(GxNativeSqlReadIndex index,
    uint64_t* outCatalogRootPageId, uint64_t* outRootPageId,
    uint32_t* outOwnerIndexId) {
    if (!outCatalogRootPageId || !outRootPageId || !outOwnerIndexId) return 0;
    *outCatalogRootPageId = 0;
    *outRootPageId = 0;
    *outOwnerIndexId = 0;
    IndexIdentity identity = {};
    if (!identityFor(index, &identity)) return 0;
    *outCatalogRootPageId = kCatalogRootPageId;
    *outRootPageId = identity.rootPageId;
    *outOwnerIndexId = identity.ownerIndexId;
    return 1;
}

GxNativeSqlReadStatus gxNativeSqlValidateCatalog(
    GxNativeSqlReadFixture fixture,
    GxNativeSqlImageError* imageError,
    GxNativeSqlImageError* cleanupError) {
    if (!imageError || !cleanupError) return GX_NATIVE_SQL_READ_INVALID_REQUEST;
    *imageError = GX_NATIVE_SQL_IMAGE_OK;
    *cleanupError = GX_NATIVE_SQL_IMAGE_OK;
    if (fixture != GX_NATIVE_SQL_FIXTURE_R3AH_MULTILEVEL || kIndexCount == 0) {
        return GX_NATIVE_SQL_READ_INVALID_REQUEST;
    }

    uint8_t pageBuffer[kPageSize] = {};
    GxNativeSqlIndexPageProviderContext context = {};
    gxNativeSqlIndexPageProviderInit(&context);
    GxNativeSqlIndexImageIdentity imageIdentity = {};
    imageIdentity.pageSize = kPageSize;
    imageIdentity.pageCount = kPageCount;
    imageIdentity.catalogRootPageId = kCatalogRootPageId;
    imageIdentity.indexRootPageId = kIndexes[0].rootPageId;
    imageIdentity.ownerIndexId = kIndexes[0].ownerIndexId;
    imageIdentity.consistency = GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE;

    *imageError = gxNativeSqlIndexPageProviderOpen(
        &context, kFixturePath, &imageIdentity, pageBuffer, sizeof(pageBuffer));
    if (*imageError != GX_NATIVE_SQL_IMAGE_OK) {
        const GxNativeSqlReadStatus status = imageFailureStatus(*imageError);
        if (context.active) {
            *cleanupError = gxNativeSqlIndexPageProviderClose(&context);
        } else if (context.fileHandle != GX_DB_FILE_INVALID_HANDLE) {
            *cleanupError = GX_NATIVE_SQL_IMAGE_BAD_STATE;
        }
        return *cleanupError == GX_NATIVE_SQL_IMAGE_OK
            ? status : GX_NATIVE_SQL_READ_STORAGE_FAILURE;
    }

    uint8_t readOnlyMount = 0;
    const GxDbFileStatus mountStatus = gx_db_file_is_read_only(
        context.fileHandle, &readOnlyMount);
    GxNativeSqlReadStatus status = GX_NATIVE_SQL_READ_OK;
    if (mountStatus != GX_DB_FILE_OK) {
        *imageError = GX_NATIVE_SQL_IMAGE_BAD_STATE;
        status = GX_NATIVE_SQL_READ_STORAGE_FAILURE;
    } else if (!readOnlyMount) {
        *imageError = GX_NATIVE_SQL_IMAGE_UNSUPPORTED_MODE;
        status = GX_NATIVE_SQL_READ_UNSUPPORTED_OPERATION;
    } else if (!headerMatches(context)) {
        *imageError = GX_NATIVE_SQL_IMAGE_METADATA_MISMATCH;
        status = GX_NATIVE_SQL_READ_CORRUPT_INDEX;
    } else if (!catalogRootMatches(&context)) {
        *imageError = context.error == GX_NATIVE_SQL_IMAGE_OK
            ? GX_NATIVE_SQL_IMAGE_CORRUPT_PAGE : context.error;
        status = imageFailureStatus(*imageError);
    } else {
        for (size_t i = 0; i < kIndexCount; ++i) {
            if (selectedRootMatches(&context, kIndexes[i])) continue;
            *imageError = context.error == GX_NATIVE_SQL_IMAGE_OK
                ? GX_NATIVE_SQL_IMAGE_INVALID_ROOT : context.error;
            status = imageFailureStatus(*imageError);
            break;
        }
    }

    if (context.active) {
        *cleanupError = gxNativeSqlIndexPageProviderClose(&context);
    } else if (context.fileHandle != GX_DB_FILE_INVALID_HANDLE) {
        *cleanupError = GX_NATIVE_SQL_IMAGE_BAD_STATE;
    }
    if (*cleanupError != GX_NATIVE_SQL_IMAGE_OK) {
        return GX_NATIVE_SQL_READ_STORAGE_FAILURE;
    }
    return status;
}
