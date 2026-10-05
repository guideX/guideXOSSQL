#pragma once
// guideXOS SQL -- Phase SQL1
// On-disk format constants for the native .gxdb database file.
//
// Byte order: little-endian for every multi-byte integer field. Fields are
// serialized explicitly (never by dumping a C++ struct) so layout, packing and
// byte order are controlled and stable across compilers and architectures.
//
// File layout (append-only, fixed-size pages):
//
//   offset 0                         pageSize
//   +-----------------------------+-----------------------------+
//   | Page 0: File Header         | Page 1: Root/Catalog Page    | ...
//   +-----------------------------+-----------------------------+
//
// Page 0 is the database header page. Pages 1..pageCount-1 are ordinary pages
// carrying a fixed page header followed by an opaque payload.

#include <cstdint>

namespace gxos {
namespace db {

// Recognizable 8-byte signature: "GXDB" followed by CR LF SUB LF. The control
// bytes make accidental text collisions extremely unlikely and survive naive
// text-mode transfers.
extern const uint8_t kFileMagic[8];

const uint16_t kFormatMajor = 1;
const uint16_t kFormatMinor = 0;

// Page size policy. 4096 matches the existing guideXOS server allocator page
// granularity (gxos::PageSize) and common storage/cluster sizes. Larger sizes
// are accepted for future flexibility.
const uint32_t kDefaultPageSize = 4096;
const uint32_t kMinPageSize = 512;
const uint32_t kMaxPageSize = 65536;

// Bytes occupied by the serialized database header at the start of page 0.
// The remainder of page 0 is reserved and written as zero.
const uint32_t kHeaderSize = 128;

// Fixed page header at the start of every non-header page.
const uint32_t kPageHeaderSize = 32;

// Well-known page identifiers.
const uint64_t kHeaderPageId = 0;
const uint64_t kBootstrapPageId = 1;

// Hard bound on total allocated pages. Keeps offsets representable in 64 bits
// and rejects absurd on-disk allocation metadata before any arithmetic.
const uint64_t kMaxPageCount = (static_cast<uint64_t>(1) << 32);

// Catalog/bootstrap payload markers.
const uint32_t kCatalogMagic = 0x54435847u; // 'G','X','C','T' little-endian
const uint16_t kCatalogVersion = 2;         // SQL2: relational catalog (tables)
const uint16_t kCatalogVersionLegacy = 1;   // SQL1: empty catalog (no tables)
const uint16_t kCatalogVersionV3 = 3;       // SQL6: tables + indexes/constraints
const uint16_t kCatalogVersionV4 = 4;       // SQL8: + defaults, foreign keys, ownership
const uint32_t kCatalogPayloadSize = 16;    // SQL1 minimum payload size

// ---- SQL2 catalog record layout -------------------------------------------
// Root catalog page payload header (catalog v2).
const uint32_t kCatalogRootHeaderSize = 32;
// Catalog continuation page payload header.
const uint32_t kCatalogContHeaderSize = 24;

// ---- SQL6 catalog v3 layout -----------------------------------------------
// The v3 root header extends the v2 header with index-allocation metadata. The
// continuation header is byte-identical to v2 (the version field distinguishes
// them). v3 records are type-tagged so table and index records can share one
// record stream:
//
//   u8 recordType (1 = table, 2 = index), then the record payload.
const uint32_t kCatalogV3RootHeaderSize = 40;
const uint32_t kCatalogV3ContHeaderSize = kCatalogContHeaderSize;
const uint8_t kCatalogRecordTable = 1;
const uint8_t kCatalogRecordIndex = 2;

// ---- SQL8 catalog v4 layout -----------------------------------------------
// The v4 root header extends the v3 header with foreign-key allocation metadata.
// The continuation header is byte-identical to v3. v4 records are type-tagged:
//
//   u8 recordType (1 = table, 2 = index, 3 = foreign key), then the payload.
//
// v4 table records embed column records that now carry DEFAULT metadata:
//   u32 ordinal, u16 type, u16 nullable, u32 flags, u32 nameLength, name,
//   u8 hasDefault, [u8 defaultType, u8 defaultNull, default payload].
//
// v4 index records carry ownership:
//   u32 indexId, u32 tableId, u32 columnOrdinal, u64 rootPageId,
//   u16 flags (bit0 unique, bit1 PK, bit2 system-owned), u16 formatVersion,
//   u64 entryCount, u32 ownerForeignKeyId, u32 nameLength, name bytes.
//
// v4 foreign-key record:
//   u32 foreignKeyId, u32 childTableId, u32 childColumnOrdinal,
//   u32 parentTableId, u32 parentColumnOrdinal,
//   u32 referencedIndexId, u32 supportIndexId, u16 flags, u16 formatVersion.
const uint32_t kCatalogV4RootHeaderSize = 48;
const uint32_t kCatalogV4ContHeaderSize = kCatalogContHeaderSize;
const uint8_t kCatalogRecordForeignKey = 3;

// ---- SQL8 foreign-key limits ----------------------------------------------
const uint32_t kMaxForeignKeysPerTable = 64;
const uint32_t kMaxForeignKeys = 2048;

// ---- SQL2 heap page layout ------------------------------------------------
const uint32_t kHeapMagic = 0x50485847u; // 'G','X','H','P' little-endian
const uint16_t kHeapVersion = 1;
const uint32_t kHeapHeaderSize = 24;

// ---- SQL2 identifier and schema limits ------------------------------------
const uint32_t kMaxTableNameBytes = 64;
const uint32_t kMaxColumnNameBytes = 64;
const uint32_t kMaxColumnsPerTable = 64;
const uint32_t kMaxTables = 512;

// ---- SQL6 index limits ----------------------------------------------------
const uint32_t kMaxIndexNameBytes = 64;
const uint32_t kMaxIndexes = 2048;
const uint32_t kMaxIndexesPerTable = 64;

// ---- SQL2 value limits ----------------------------------------------------
const uint32_t kMaxTextBytes = 65536;
const uint32_t kMaxBlobBytes = 65536;
const uint32_t kMaxRowBytes = 65536;

// ---- SQL6 B+ tree index page layout ---------------------------------------
// Every index page keeps the ordinary SQL1 page header (and CRC) and adds a
// small index header followed by a slot directory that grows backward from the
// end of the payload, exactly like a heap page.
const uint32_t kIndexLeafMagic = 0x4C495847u;   // 'G','X','I','L'
const uint32_t kIndexInternalMagic = 0x4E495847u; // 'G','X','I','N'
const uint16_t kIndexPageVersion = 1;
const uint32_t kIndexLeafHeaderSize = 24;
const uint32_t kIndexInternalHeaderSize = 24;
const uint32_t kIndexSlotSize = 8;              // {u32 offset, u32 length}
// Entry payload: u32 keyLength, key bytes, u64 pageId, u32 slot.
const uint32_t kIndexLeafEntryOverhead = 4 + 12;      // keyLength + locator
// Child payload: u64 childPageId, u32 keyLength, key bytes.
const uint32_t kIndexInternalEntryOverhead = 8 + 4;

// Maximum tree depth accepted by any traversal. A defensive bound independent
// of the catalog's recorded height.
const uint32_t kMaxIndexDepth = 32;

// Minimum entries that must be able to coexist in one leaf page. Used to derive
// the maximum encoded index-key size for a given page size.
const uint32_t kIndexMinEntriesPerLeaf = 4;
// Absolute cap on an encoded index key, independent of page size.
const uint32_t kMaxIndexKeyBytesCap = 1024;

// Returns the largest encoded index key (in bytes) that still guarantees
// kIndexMinEntriesPerLeaf entries fit in a leaf page of `pageSize`.
inline uint32_t maxIndexKeyBytes(uint32_t pageSize) {
    const uint32_t capacity = pageSize > kPageHeaderSize ? pageSize - kPageHeaderSize : 0;
    const uint32_t perEntry = kIndexSlotSize + kIndexLeafEntryOverhead;
    const uint32_t reserved =
        kIndexLeafHeaderSize + kIndexMinEntriesPerLeaf * perEntry;
    if (capacity <= reserved) {
        return 0;
    }
    uint32_t limit = (capacity - reserved) / kIndexMinEntriesPerLeaf;
    if (limit > kMaxIndexKeyBytesCap) {
        limit = kMaxIndexKeyBytesCap;
    }
    return limit;
}

// ---- Database header field offsets (within page 0) ------------------------
namespace header_offset {
enum : uint32_t {
    Magic = 0,            // 8 bytes
    FormatMajor = 8,      // u16
    FormatMinor = 10,     // u16
    PageSize = 12,        // u32
    DatabaseId = 16,      // 16 bytes (RFC 4122 layout, raw bytes)
    CreationTime = 32,    // u64, nanoseconds since Unix epoch (0 = unknown)
    PageCount = 40,       // u64, total allocated pages incl. header page
    RootPageId = 48,      // u64, page id of the bootstrap/catalog page
    Flags = 56,           // u32
    HeaderSize = 60,      // u32, must equal kHeaderSize
    HeaderCrc32 = 64,     // u32, CRC32 over [0,kHeaderSize) with this field zeroed
    Reserved = 68         // zero-filled through kHeaderSize
};
} // namespace header_offset

// ---- Page header field offsets (within a non-header page) -----------------
namespace page_offset {
enum : uint32_t {
    PageId = 0,           // u64, must equal the page's index
    PageType = 8,         // u16, see PageType
    Flags = 10,           // u16
    PayloadSize = 12,     // u32, used bytes in the payload area
    Generation = 16,      // u32
    Reserved0 = 20,       // u32
    PageCrc32 = 24,       // u32, CRC32 over the whole page with this field zeroed
    Reserved1 = 28        // u32
};
} // namespace page_offset

enum class PageType : uint16_t {
    Unknown = 0,
    Catalog = 1,       // bootstrap/root page and catalog continuation pages
    Data = 2,          // SQL2 heap pages carry structured row data
    IndexLeaf = 3,     // SQL6 B+ tree leaf page
    IndexInternal = 4  // SQL6 B+ tree internal page
};

inline const char* pageTypeName(PageType type) {
    switch (type) {
    case PageType::Unknown: return "Unknown";
    case PageType::Catalog: return "Catalog";
    case PageType::Data: return "Data";
    case PageType::IndexLeaf: return "IndexLeaf";
    case PageType::IndexInternal: return "IndexInternal";
    }
    return "Unknown";
}

inline bool isIndexPageType(PageType type) {
    return type == PageType::IndexLeaf || type == PageType::IndexInternal;
}

// Header flags (reserved for forward-compatible use).
namespace header_flags {
enum : uint32_t {
    None = 0
};
} // namespace header_flags

} // namespace db
} // namespace gxos
