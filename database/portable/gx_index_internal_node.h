#ifndef GX_PORTABLE_INDEX_INTERNAL_NODE_H
#define GX_PORTABLE_INDEX_INTERNAL_NODE_H

#include <stddef.h>
#include <stdint.h>

#include "gx_status.h"

enum class GxIndexInternalNodeError {
    None = 0,
    InvalidArgument,
    PageSizeOutOfRange,
    HeaderTruncated,
    SignatureMismatch,
    UnsupportedVersion,
    OwnerIndexMismatch,
    ImpossibleChildCount,
    SlotOutOfRange,
    EntryOutOfRange,
    EntryTooSmall,
    SeparatorLengthInvalid,
    SeparatorMissingPhysicalLocator,
    ChildPageIdInvalid,
    EntryRegionsOverlap,
    DuplicateChildPage,
    SeparatorsNotIncreasing,
    PhysicalKeyComparisonFailed
};

struct GxIndexInternalNodeInfo {
    uint32_t childCount;
    uint32_t ownerIndexId;
};

// Borrowed record view. separatorBytes is empty only for the existing
// leftmost -infinity child bound. Any nonempty separator is the complete
// physical key, including its 12-byte row-locator suffix.
struct GxIndexInternalChildView {
    uint64_t childPageId;
    const uint8_t* separatorBytes;
    size_t separatorLength;
    uint32_t entryOffset;
    uint32_t entryLength;
};

// A non-owning cursor over the existing SQL6 internal-node payload. Init
// validates the complete node before exposing any entry. The source bytes must
// stay alive and unchanged until the cursor and all returned views are
// discarded.
struct GxIndexInternalNodeCursor {
    const uint8_t* pageBytes;
    size_t pageLength;
    uint64_t pageCount;
    uint32_t position;
    uint32_t maxPhysicalKeyBytes;
    GxIndexInternalNodeInfo page;
    bool failed;
    GxIndexInternalNodeError lastError;
};

// Opens and fully validates the production little-endian internal-node format.
// pageLength is the payload span without the ordinary 32-byte page header.
// Child page IDs must be in [1, pageCount), there must be at least two child
// records, and their count may not exceed
// (pageLength - 24) / (8 + 12), the existing minimum slot-plus-record bound.
// Separator bounds must be strictly increasing. The first bound may be empty
// (-infinity) or a finite physical key, preserving non-root right-half nodes
// produced by legacy splits. Reserved header bytes retain the existing
// hosted-reader tolerance.
GxStatus gxIndexInternalNodeCursorInit(
    const uint8_t* pageBytes, size_t pageLength, uint64_t pageCount,
    uint32_t expectedOwnerIndexId, GxIndexInternalNodeCursor* outCursor,
    GxIndexInternalNodeError* outError);

// Reads one validated child record. Successful end-of-node is Ok with
// outHasChild=false. Child and separator views borrow the source page.
GxStatus gxIndexInternalNodeCursorReadNext(
    GxIndexInternalNodeCursor* cursor, GxIndexInternalChildView* outChild,
    bool* outHasChild, GxIndexInternalNodeError* outError);

// Applies the production routing rule: choose the rightmost child whose
// separator is <= the complete physical search key; equality routes right.
// The first child is the default, including when its finite lower bound is
// greater than the probe, matching IndexTree::chooseChild exactly. Outputs are
// written only on success and must not overlap the input spans.
GxStatus gxIndexInternalNodeSelectChild(
    const uint8_t* pageBytes, size_t pageLength, uint64_t pageCount,
    uint32_t expectedOwnerIndexId, const uint8_t* physicalSearchKey,
    size_t physicalSearchKeyLength, uint64_t* outChildPageId,
    uint32_t* outChildIndex, GxIndexInternalNodeError* outError);

#endif
