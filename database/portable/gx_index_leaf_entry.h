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

// Validates all entries and the existing physical-key ordering contract
// without modifying the page or allocating. Unlike simple traversal, this
// full validation also rejects overlapping entry regions and keys that are
// not strictly increasing by logical-key bytes plus the established
// big-endian row-locator suffix. Sibling and owner IDs require page-store
// context and are not validated here. outError must not overlap pageBytes.
GxStatus gxIndexLeafValidate(const uint8_t* pageBytes, size_t pageLength,
                             GxIndexLeafError* outError);

#endif
