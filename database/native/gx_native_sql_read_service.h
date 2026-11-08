#ifndef GX_NATIVE_SQL_READ_SERVICE_H
#define GX_NATIVE_SQL_READ_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "gx_native_sql_index_page_provider.h"
#include "gx_index_lookup.h"

// This interface deliberately exposes one trusted immutable fixture. It does
// not accept a database path, arbitrary catalog roots, or live consistency.
enum GxNativeSqlReadFixture {
    GX_NATIVE_SQL_FIXTURE_R3AH_MULTILEVEL = 1
};

enum GxNativeSqlReadIndex {
    GX_NATIVE_SQL_INDEX_ROWS_K64 = 1,
    GX_NATIVE_SQL_INDEX_ROWS_K32 = 2
};

enum GxNativeSqlReadOperation {
    GX_NATIVE_SQL_READ_POINT = 1,
    GX_NATIVE_SQL_READ_RANGE = 2
};

enum GxNativeSqlReadStatus {
    GX_NATIVE_SQL_READ_OK = 0,
    GX_NATIVE_SQL_READ_NO_MATCH,
    GX_NATIVE_SQL_READ_INVALID_REQUEST,
    GX_NATIVE_SQL_READ_UNSUPPORTED_OPERATION,
    GX_NATIVE_SQL_READ_STORAGE_FAILURE,
    GX_NATIVE_SQL_READ_CORRUPT_INDEX,
    GX_NATIVE_SQL_READ_RESOURCE_EXHAUSTED
};

static const size_t GX_NATIVE_SQL_READ_MAX_RESULTS = 73;

struct GxNativeSqlReadRequest {
    GxNativeSqlReadOperation operation;
    GxNativeSqlReadFixture fixture;
    GxNativeSqlReadIndex index;
    GxNativeSqlReadConsistency consistency;
    uint64_t catalogRootPageId;
    uint64_t rootPageId;
    uint32_t ownerIndexId;

    // Point lookup uses keyBytes/keyLength. Range scan uses the lower/upper
    // encoded key spans and endpoint flags. Encodings are the existing tagged
    // Int64 (K64) or Int32 (K32) logical-key formats.
    const uint8_t* keyBytes;
    size_t keyLength;
    uint8_t hasLower;
    const uint8_t* lowerKeyBytes;
    size_t lowerKeyLength;
    uint8_t lowerInclusive;
    uint8_t hasUpper;
    const uint8_t* upperKeyBytes;
    size_t upperKeyLength;
    uint8_t upperInclusive;
};

struct GxNativeSqlReadLocator {
    uint64_t pageId;
    uint32_t slot;
};

struct GxNativeSqlReadResponse {
    GxNativeSqlReadStatus status;
    size_t rowCount;
    GxNativeSqlImageError imageError;
    GxNativeSqlImageError cleanupError;
    GxIndexLookupStatus lookupStatus;
};

// Executes one request against /system/Apps/R3AI/MULTI.gxdb using the
// production VFS bridge and production portable index reader. Results are
// staged in bounded stack storage and copied to resultRows only after a
// complete traversal and successful file close. On every non-OK status the
// caller's resultRows bytes are unchanged and response.rowCount is zero.
// resultRows and response must be valid caller-owned, disjoint storage; the
// result capacity is checked before any row is published. No heap allocation
// or persistent query state is used.
GxNativeSqlReadStatus gxNativeSqlExecuteRead(
    const GxNativeSqlReadRequest* request,
    GxNativeSqlReadLocator* resultRows,
    size_t resultCapacity,
    GxNativeSqlReadResponse* response);

#endif
