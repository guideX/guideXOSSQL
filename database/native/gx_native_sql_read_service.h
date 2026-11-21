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
    GX_NATIVE_SQL_INDEX_ROWS_K32 = 2,
    GX_NATIVE_SQL_INDEX_ROWS_KBOOL = 3
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

enum GxNativeSqlIndexKeyType {
    GX_NATIVE_SQL_KEY_INT32 = 1,
    GX_NATIVE_SQL_KEY_INT64 = 2,
    GX_NATIVE_SQL_KEY_BOOLEAN = 3
};

enum GxNativeSqlIndexCapability {
    GX_NATIVE_SQL_INDEX_CAP_POINT = 1u,
    GX_NATIVE_SQL_INDEX_CAP_RANGE = 2u
};

enum GxNativeSqlIndexComparison {
    GX_NATIVE_SQL_COMPARE_SIGNED_ASCENDING_NULLS_FIRST = 1u,
    GX_NATIVE_SQL_COMPARE_BOOLEAN_ASCENDING_NULLS_FIRST = 2u
};

struct GxNativeSqlIndexCatalogEntry {
    uint32_t identity;
    const char* name;
    uint32_t keyType;
    uint32_t capabilities;
    uint32_t comparisonSemantics;
    uint32_t maximumResults;
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
    // Int64 (K64), Int32 (K32), or Boolean (KBool) logical-key formats.
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

// Verifies the one approved immutable fixture using the production read-only
// provider, header checks, and a trusted catalog index root. The provider is
// always closed before this function returns; no VFS/SQL pointer is retained.
GxNativeSqlReadStatus gxNativeSqlValidateDatabase(
    GxNativeSqlReadFixture fixture,
    GxNativeSqlImageError* imageError,
    GxNativeSqlImageError* cleanupError);

// Returns the compile-time trusted metadata used by identityFor() and query
// dispatch. The entries are ordered by stable identity and do not expose page
// roots. Call gxNativeSqlValidateCatalog before publishing them to an app.
size_t gxNativeSqlGetSupportedIndexCount(void);
const GxNativeSqlIndexCatalogEntry* gxNativeSqlGetSupportedIndexAt(
    size_t index);
int gxNativeSqlGetReadIndexIdentity(GxNativeSqlReadIndex index,
    uint64_t* outCatalogRootPageId, uint64_t* outRootPageId,
    uint32_t* outOwnerIndexId);

// Validates the approved immutable image and every index the reader advertises
// using the production VFS/page provider. This is an application catalog
// preflight, not a general on-disk catalog parser.
GxNativeSqlReadStatus gxNativeSqlValidateCatalog(
    GxNativeSqlReadFixture fixture,
    GxNativeSqlImageError* imageError,
    GxNativeSqlImageError* cleanupError);

#endif
