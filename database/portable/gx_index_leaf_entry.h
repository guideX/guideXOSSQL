#ifndef GX_PORTABLE_INDEX_LEAF_ENTRY_H
#define GX_PORTABLE_INDEX_LEAF_ENTRY_H

#include <stddef.h>
#include <stdint.h>

#include "gx_status.h"

// More specific diagnostics for the existing SQL index-leaf byte format.
// GxStatus remains the control-flow result; this value explains corrupt data.
enum class GxIndexLeafError {
    None = 0,
    InvalidArgument,
    InvalidCursor,
    PageSizeOutOfRange,
    HeaderTruncated,
    SignatureMismatch,
    UnsupportedVersion,
    ImpossibleEntryCount,
    SlotOutOfRange,
    EntryOutOfRange,
    EntryTooSmall,
    KeyLengthInvalid,
    EntryRegionsOverlap,
    PhysicalKeysNotIncreasing,
    PhysicalKeyComparisonFailed
};

struct GxIndexLeafPageInfo {
    uint64_t nextLeafPageId;
    uint32_t entryCount;
    uint32_t ownerIndexId;
};

// A non-owning cursor over one existing leaf-page payload. The byte span is
// borrowed: the caller must keep it alive and unchanged until it discards the
// cursor and every GxIndexLeafEntryView returned from it. No pointer is retained
// beyond that span's lifetime, and this type never owns or allocates storage.
struct GxIndexLeafCursor {
    const uint8_t* pageBytes;
    size_t pageLength;
    uint32_t position;
    uint32_t maxKeyBytes;
    GxIndexLeafPageInfo page;
    bool failed;
    GxIndexLeafError lastError;
};

// Borrowed fields from a validated inline leaf entry. keyBytes is not
// null-terminated. All fields remain valid only while the source page bytes
// remain alive and unchanged.
struct GxIndexLeafEntryView {
    const uint8_t* keyBytes;
    size_t keyLength;
    uint64_t locatorPageId;
    uint32_t locatorSlot;
    uint32_t entryOffset;
    uint32_t entryLength;
};

// Result of a leaf-local physical-key lower-bound lookup. When hasEntry is
// true, entry is the first physical key >= the requested key and entryPosition
// is its zero-based slot position. exactMatch compares the complete physical
// key, including the 12-byte row-locator suffix. continuation is positioned
// immediately after entry and can be passed to gxIndexLeafCursorReadNext to
// continue in slot order. When hasEntry is false, entryPosition is the valid
// end-of-leaf insertion position and continuation is at end-of-leaf.
//
// The entry view and continuation borrow the supplied page bytes. Keep the
// page alive and unchanged until they are discarded. On failure, the result
// is cleared. The result and optional error output must not overlap either
// input span or each other.
struct GxIndexLeafLookupResult {
    GxIndexLeafCursor continuation;
    GxIndexLeafEntryView entry;
    uint32_t entryPosition;
    bool exactMatch;
    bool hasEntry;
};

// Opens the existing little-endian SQL index-leaf payload format. pageLength
// is the complete page payload length (the outer 32-byte database page header
// is not included). It must correspond to an existing supported database page
// size. This validates the leaf signature, version, and bounded slot count;
// page type, owner-index identity, and sibling page-count context remain the
// caller's responsibility. The reserved header flags are intentionally
// preserved as tolerated by the hosted reader. The output cursor and error
// value must not overlap pageBytes; treat cursor fields as opaque and do not
// modify them after initialization.
GxStatus gxIndexLeafCursorInit(const uint8_t* pageBytes, size_t pageLength,
                               GxIndexLeafCursor* outCursor,
                               GxIndexLeafError* outError);

// Returns the next validated entry in slot order. GxStatus::Ok with
// outHasEntry=false is the successful end-of-page result. A malformed entry
// returns GxStatus::CorruptData; callers should stop using that cursor. Output
// values must not overlap the borrowed page bytes.
GxStatus gxIndexLeafCursorReadNext(GxIndexLeafCursor* cursor,
                                   GxIndexLeafEntryView* outEntry,
                                   bool* outHasEntry,
                                   GxIndexLeafError* outError);

// Finds the leaf-local lower-bound candidate for a complete physical key.
// The page is fully validated first, including entry bounds, non-overlap, and
// strictly increasing physical order. physicalKeyBytes contains the complete
// logical-key bytes followed by the existing 12-byte big-endian row locator;
// it is not null-terminated and its length must include at least those 12
// locator bytes. A successful non-exact result with hasEntry=true
// is a valid insertion position, while hasEntry=false is valid end-of-leaf.
// A lower bound from one leaf is not a tree-wide first match unless the
// caller's B+ tree descent guarantees that property. No bytes are modified or
// retained outside the returned borrowed view and continuation cursor.
GxStatus gxIndexLeafLookup(const uint8_t* pageBytes, size_t pageLength,
                           const uint8_t* physicalKeyBytes,
                           size_t physicalKeyLength,
                           GxIndexLeafLookupResult* outResult,
                           GxIndexLeafError* outError);

// Validates all entries and the existing physical-key ordering contract
// without modifying the page or allocating. Unlike simple traversal, this
// full validation also rejects overlapping entry regions and keys that are
// not strictly increasing by logical-key bytes plus the established
// big-endian row-locator suffix. Sibling and owner IDs require page-store
// context and are not validated here. outError must not overlap pageBytes.
GxStatus gxIndexLeafValidate(const uint8_t* pageBytes, size_t pageLength,
                             GxIndexLeafError* outError);

#endif
