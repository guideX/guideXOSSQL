#include "gx_native_sql_read_service.h"

#include "database_endian.h"
#include "database_format.h"
#include "gx_index_integer_key.h"
#include "gx_index_internal_node.h"
#include "gx_index_lookup.h"

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
const uint64_t kPointRootPageId = 234;
const uint32_t kPointOwnerIndexId = 1;
const uint64_t kRangeRootPageId = 148;
const uint32_t kRangeOwnerIndexId = 2;
const uint64_t kCreationTimeUnixNanos = 1793556264120817800ull;
const size_t kFixtureBytes = 250880;
const uint32_t kMaxDepth = gxos::db::kMaxIndexDepth;

struct IndexIdentity {
    uint64_t rootPageId;
    uint32_t ownerIndexId;
    uint8_t isInt64;
};

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
    if (index == GX_NATIVE_SQL_INDEX_ROWS_K64) {
        out->rootPageId = kPointRootPageId;
        out->ownerIndexId = kPointOwnerIndexId;
        out->isInt64 = 1;
        return true;
    }
    if (index == GX_NATIVE_SQL_INDEX_ROWS_K32) {
        out->rootPageId = kRangeRootPageId;
        out->ownerIndexId = kRangeOwnerIndexId;
        out->isInt64 = 0;
        return true;
    }
    return false;
}

bool validEncodedKey(const uint8_t* bytes, size_t length, bool isInt64) {
    if (isInt64) {
        int64_t value = 0;
        bool isNull = false;
        return gxDecodeIndexInt64Key(bytes, length, &value, &isNull) ==
            GxStatus::Ok;
    }
    int32_t value = 0;
    bool isNull = false;
    return gxDecodeIndexInt32Key(bytes, length, &value, &isNull) ==
        GxStatus::Ok;
}

bool validKeySpan(const uint8_t* bytes, size_t length, bool isInt64) {
    const size_t maxLength = isInt64 ? 9u : 5u;
    return bytes != 0 && length != 0 && length <= maxLength &&
        validEncodedKey(bytes, length, isInt64);
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
                          index.isInt64) ||
            request.hasLower || request.lowerKeyBytes || request.lowerKeyLength ||
            request.lowerInclusive || request.hasUpper || request.upperKeyBytes ||
            request.upperKeyLength || request.upperInclusive ||
            request.index != GX_NATIVE_SQL_INDEX_ROWS_K64) {
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
                                            request.lowerKeyLength, false)) ||
        (!request.hasLower && (request.lowerKeyBytes || request.lowerKeyLength ||
                               request.lowerInclusive)) ||
        (request.hasUpper && !validKeySpan(request.upperKeyBytes,
                                            request.upperKeyLength, false)) ||
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
