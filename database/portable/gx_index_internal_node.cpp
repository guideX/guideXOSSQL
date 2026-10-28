#include "gx_index_internal_node.h"

#include "database_format.h"
#include "database_endian.h"
#include "gx_index_physical_key.h"

namespace {

const size_t kInternalHeaderSize = 24;
const size_t kInternalSlotSize = 8;
const size_t kInternalEntryOverhead = 12;
const size_t kInternalMinimumChildFootprint =
    kInternalSlotSize + kInternalEntryOverhead;
const size_t kPhysicalLocatorSize = 12;

void setError(GxIndexInternalNodeError* outError,
              GxIndexInternalNodeError error) {
    if (outError != nullptr) *outError = error;
}

uint16_t loadLe16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t loadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t loadLe64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
    }
    return value;
}

bool memoryRangesOverlap(const void* left, size_t leftLength,
                         const void* right, size_t rightLength) {
    if (left == nullptr || right == nullptr || leftLength == 0 ||
        rightLength == 0) return false;
    const uintptr_t leftStart = reinterpret_cast<uintptr_t>(left);
    const uintptr_t rightStart = reinterpret_cast<uintptr_t>(right);
    const uintptr_t maximum = static_cast<uintptr_t>(-1);
    if (leftLength > static_cast<size_t>(maximum - leftStart) ||
        rightLength > static_cast<size_t>(maximum - rightStart)) return true;
    const uintptr_t leftEnd = leftStart + static_cast<uintptr_t>(leftLength);
    const uintptr_t rightEnd = rightStart + static_cast<uintptr_t>(rightLength);
    return leftStart < rightEnd && rightStart < leftEnd;
}

GxStatus corrupt(GxIndexInternalNodeError error,
                 GxIndexInternalNodeError* outError) {
    setError(outError, error);
    return GxStatus::CorruptData;
}

bool intervalsOverlap(size_t leftStart, size_t leftLength,
                      size_t rightStart, size_t rightLength) {
    return leftStart < rightStart + rightLength &&
           rightStart < leftStart + leftLength;
}

GxStatus readChildAt(const uint8_t* pageBytes, size_t pageLength,
                     uint32_t childCount, uint32_t index,
                     GxIndexInternalChildView* outChild,
                     GxIndexInternalNodeError* outError) {
    if (pageBytes == nullptr || outChild == nullptr || index >= childCount) {
        setError(outError, GxIndexInternalNodeError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }
    if (pageLength < kInternalHeaderSize || childCount < 2 ||
        childCount > (pageLength - kInternalHeaderSize) /
                         kInternalMinimumChildFootprint) {
        return corrupt(GxIndexInternalNodeError::ImpossibleChildCount,
                       outError);
    }
    const uint64_t slotDistance =
        static_cast<uint64_t>(kInternalSlotSize) * (index + 1u);
    if (slotDistance > pageLength ||
        pageLength - static_cast<size_t>(slotDistance) < kInternalHeaderSize) {
        return corrupt(GxIndexInternalNodeError::SlotOutOfRange, outError);
    }
    const size_t slotOffset = pageLength - static_cast<size_t>(slotDistance);
    const uint32_t entryOffset32 = loadLe32(pageBytes + slotOffset);
    const uint32_t entryLength32 = loadLe32(pageBytes + slotOffset + 4);
    const size_t entryOffset = entryOffset32;
    const size_t entryLength = entryLength32;
    if (entryOffset < kInternalHeaderSize || entryOffset > pageLength ||
        entryLength > pageLength - entryOffset) {
        return corrupt(GxIndexInternalNodeError::EntryOutOfRange, outError);
    }
    if (entryLength < kInternalEntryOverhead) {
        return corrupt(GxIndexInternalNodeError::EntryTooSmall, outError);
    }
    const size_t directoryStart = pageLength -
        static_cast<size_t>(childCount) * kInternalSlotSize;
    if (entryOffset + entryLength > directoryStart) {
        return corrupt(GxIndexInternalNodeError::EntryOutOfRange, outError);
    }
    const uint8_t* record = pageBytes + entryOffset;
    const uint32_t separatorLength32 = loadLe32(record + 8);
    const size_t separatorLength = separatorLength32;
    if (separatorLength != entryLength - kInternalEntryOverhead) {
        return corrupt(GxIndexInternalNodeError::SeparatorLengthInvalid,
                       outError);
    }
    if (separatorLength != 0 && separatorLength < kPhysicalLocatorSize) {
        return corrupt(
            GxIndexInternalNodeError::SeparatorMissingPhysicalLocator,
            outError);
    }

    outChild->childPageId = loadLe64(record);
    outChild->separatorBytes = record + kInternalEntryOverhead;
    outChild->separatorLength = separatorLength;
    outChild->entryOffset = entryOffset32;
    outChild->entryLength = entryLength32;
    return GxStatus::Ok;
}

GxStatus validateNodeBytes(const uint8_t* pageBytes, size_t pageLength,
                           uint64_t pageCount, uint32_t expectedOwnerIndexId,
                           GxIndexInternalNodeInfo* outInfo,
                           uint32_t* outMaxKeyBytes,
                           GxIndexInternalNodeError* outError) {
    if (pageBytes == nullptr || outInfo == nullptr || outMaxKeyBytes == nullptr ||
        pageCount < 2 || pageLength > static_cast<size_t>(UINT32_MAX) ||
        pageLength > static_cast<size_t>(-1) - gxos::db::kPageHeaderSize) {
        setError(outError, GxIndexInternalNodeError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }
    if (pageLength < kInternalHeaderSize) {
        return corrupt(GxIndexInternalNodeError::HeaderTruncated, outError);
    }
    const size_t pageSizeValue = pageLength + gxos::db::kPageHeaderSize;
    if (pageSizeValue < gxos::db::kMinPageSize ||
        pageSizeValue > gxos::db::kMaxPageSize ||
        pageSizeValue > static_cast<size_t>(UINT32_MAX) ||
        !gxos::db::isSupportedPageSize(static_cast<uint32_t>(pageSizeValue))) {
        return corrupt(GxIndexInternalNodeError::PageSizeOutOfRange, outError);
    }
    if (loadLe32(pageBytes) != gxos::db::kIndexInternalMagic) {
        return corrupt(GxIndexInternalNodeError::SignatureMismatch, outError);
    }
    if (loadLe16(pageBytes + 4) != gxos::db::kIndexPageVersion) {
        return corrupt(GxIndexInternalNodeError::UnsupportedVersion, outError);
    }

    GxIndexInternalNodeInfo info = {};
    info.childCount = loadLe32(pageBytes + 16);
    info.ownerIndexId = loadLe32(pageBytes + 20);
    if (info.ownerIndexId != expectedOwnerIndexId) {
        return corrupt(GxIndexInternalNodeError::OwnerIndexMismatch, outError);
    }
    if (info.childCount < 2 ||
        info.childCount > (pageLength - kInternalHeaderSize) /
                              kInternalMinimumChildFootprint) {
        return corrupt(GxIndexInternalNodeError::ImpossibleChildCount,
                       outError);
    }

    const uint32_t pageSize = static_cast<uint32_t>(pageSizeValue);
    const uint32_t logicalLimit = gxos::db::maxIndexKeyBytes(pageSize);
    if (logicalLimit > UINT32_MAX - kPhysicalLocatorSize) {
        return corrupt(GxIndexInternalNodeError::SeparatorLengthInvalid,
                       outError);
    }
    const uint32_t maxKeyBytes = logicalLimit +
        static_cast<uint32_t>(kPhysicalLocatorSize);

    GxIndexInternalChildView previous = {};
    for (uint32_t i = 0; i < info.childCount; ++i) {
        GxIndexInternalChildView child = {};
        GxStatus status = readChildAt(pageBytes, pageLength, info.childCount,
                                      i, &child, outError);
        if (status != GxStatus::Ok) return status;
        if (child.separatorLength > maxKeyBytes) {
            return corrupt(GxIndexInternalNodeError::SeparatorLengthInvalid,
                           outError);
        }
        if (child.childPageId == 0 || child.childPageId >= pageCount) {
            return corrupt(GxIndexInternalNodeError::ChildPageIdInvalid,
                           outError);
        }
        for (uint32_t j = 0; j < i; ++j) {
            GxIndexInternalChildView earlier = {};
            status = readChildAt(pageBytes, pageLength, info.childCount, j,
                                 &earlier, outError);
            if (status != GxStatus::Ok) return status;
            if (intervalsOverlap(child.entryOffset, child.entryLength,
                                 earlier.entryOffset, earlier.entryLength)) {
                return corrupt(GxIndexInternalNodeError::EntryRegionsOverlap,
                               outError);
            }
            if (earlier.childPageId == child.childPageId) {
                return corrupt(GxIndexInternalNodeError::DuplicateChildPage,
                               outError);
            }
        }
        if (i != 0) {
            int comparison = 0;
            status = gxCompareIndexPhysicalKeyViews(
                previous.separatorBytes, previous.separatorLength, false,
                0, 0, child.separatorBytes, child.separatorLength, false,
                0, 0, &comparison);
            if (status != GxStatus::Ok) {
                return corrupt(
                    GxIndexInternalNodeError::PhysicalKeyComparisonFailed,
                    outError);
            }
            if (comparison >= 0) {
                return corrupt(GxIndexInternalNodeError::SeparatorsNotIncreasing,
                               outError);
            }
        }
        previous = child;
    }
    *outInfo = info;
    *outMaxKeyBytes = maxKeyBytes;
    return GxStatus::Ok;
}

} // namespace

GxStatus gxIndexInternalNodeCursorInit(
    const uint8_t* pageBytes, size_t pageLength, uint64_t pageCount,
    uint32_t expectedOwnerIndexId, GxIndexInternalNodeCursor* outCursor,
    GxIndexInternalNodeError* outError) {
    if ((outCursor != nullptr &&
         memoryRangesOverlap(outCursor, sizeof(*outCursor), pageBytes,
                             pageLength)) ||
        (outError != nullptr &&
         (memoryRangesOverlap(outError, sizeof(*outError), pageBytes,
                              pageLength) ||
          (outCursor != nullptr &&
           memoryRangesOverlap(outError, sizeof(*outError), outCursor,
                               sizeof(*outCursor)))))) {
        return GxStatus::InvalidArgument;
    }
    if (outCursor != nullptr) {
        GxIndexInternalNodeCursor empty = {};
        *outCursor = empty;
    }
    setError(outError, GxIndexInternalNodeError::None);
    if (outCursor == nullptr) {
        setError(outError, GxIndexInternalNodeError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }
    GxIndexInternalNodeInfo info = {};
    uint32_t maxKeyBytes = 0;
    const GxStatus status = validateNodeBytes(
        pageBytes, pageLength, pageCount, expectedOwnerIndexId, &info,
        &maxKeyBytes, outError);
    if (status != GxStatus::Ok) return status;
    outCursor->pageBytes = pageBytes;
    outCursor->pageLength = pageLength;
    outCursor->pageCount = pageCount;
    outCursor->position = 0;
    outCursor->maxPhysicalKeyBytes = maxKeyBytes;
    outCursor->page = info;
    outCursor->failed = false;
    outCursor->lastError = GxIndexInternalNodeError::None;
    return GxStatus::Ok;
}

GxStatus gxIndexInternalNodeCursorReadNext(
    GxIndexInternalNodeCursor* cursor, GxIndexInternalChildView* outChild,
    bool* outHasChild, GxIndexInternalNodeError* outError) {
    if (cursor != nullptr &&
        ((outChild != nullptr &&
          memoryRangesOverlap(outChild, sizeof(*outChild), cursor->pageBytes,
                              cursor->pageLength)) ||
         (outHasChild != nullptr &&
          memoryRangesOverlap(outHasChild, sizeof(*outHasChild),
                              cursor->pageBytes, cursor->pageLength)) ||
         (outError != nullptr &&
          memoryRangesOverlap(outError, sizeof(*outError), cursor->pageBytes,
                              cursor->pageLength)))) {
        return GxStatus::InvalidArgument;
    }
    if (outChild != nullptr && outHasChild != nullptr &&
        memoryRangesOverlap(outChild, sizeof(*outChild), outHasChild,
                            sizeof(*outHasChild))) {
        return GxStatus::InvalidArgument;
    }
    if (outError != nullptr &&
        ((outChild != nullptr &&
          memoryRangesOverlap(outError, sizeof(*outError), outChild,
                              sizeof(*outChild))) ||
         (outHasChild != nullptr &&
          memoryRangesOverlap(outError, sizeof(*outError), outHasChild,
                              sizeof(*outHasChild))))) {
        return GxStatus::InvalidArgument;
    }
    setError(outError, GxIndexInternalNodeError::None);
    if (outChild != nullptr) {
        GxIndexInternalChildView empty = {};
        *outChild = empty;
    }
    if (outHasChild != nullptr) *outHasChild = false;
    if (cursor == nullptr || outChild == nullptr || outHasChild == nullptr) {
        setError(outError, GxIndexInternalNodeError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }
    if (cursor->failed || cursor->pageBytes == nullptr ||
        cursor->position > cursor->page.childCount) {
        setError(outError, GxIndexInternalNodeError::InvalidArgument);
        return GxStatus::CorruptData;
    }
    if (cursor->position == cursor->page.childCount) return GxStatus::Ok;
    const GxStatus status = readChildAt(
        cursor->pageBytes, cursor->pageLength, cursor->page.childCount,
        cursor->position, outChild, outError);
    if (status != GxStatus::Ok) {
        cursor->failed = true;
        if (outError != nullptr) cursor->lastError = *outError;
        return status;
    }
    if (outChild->separatorLength > cursor->maxPhysicalKeyBytes ||
        outChild->childPageId == 0 ||
        outChild->childPageId >= cursor->pageCount) {
        cursor->failed = true;
        cursor->lastError = outChild->separatorLength > cursor->maxPhysicalKeyBytes
            ? GxIndexInternalNodeError::SeparatorLengthInvalid
            : GxIndexInternalNodeError::ChildPageIdInvalid;
        return corrupt(cursor->lastError, outError);
    }
    ++cursor->position;
    *outHasChild = true;
    return GxStatus::Ok;
}

GxStatus gxIndexInternalNodeSelectChild(
    const uint8_t* pageBytes, size_t pageLength, uint64_t pageCount,
    uint32_t expectedOwnerIndexId, const uint8_t* physicalSearchKey,
    size_t physicalSearchKeyLength, uint64_t* outChildPageId,
    uint32_t* outChildIndex, GxIndexInternalNodeError* outError) {
    if (pageBytes == nullptr || physicalSearchKey == nullptr ||
        outChildPageId == nullptr || physicalSearchKeyLength < kPhysicalLocatorSize) {
        setError(outError, GxIndexInternalNodeError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }
    if (memoryRangesOverlap(outChildPageId, sizeof(*outChildPageId), pageBytes,
                           pageLength) ||
        memoryRangesOverlap(outChildPageId, sizeof(*outChildPageId),
                           physicalSearchKey, physicalSearchKeyLength) ||
        (outChildIndex != nullptr &&
         (memoryRangesOverlap(outChildIndex, sizeof(*outChildIndex), pageBytes,
                              pageLength) ||
          memoryRangesOverlap(outChildIndex, sizeof(*outChildIndex),
                              physicalSearchKey,
                              physicalSearchKeyLength) ||
          memoryRangesOverlap(outChildIndex, sizeof(*outChildIndex),
                              outChildPageId, sizeof(*outChildPageId)))) ||
        (outError != nullptr &&
         (memoryRangesOverlap(outError, sizeof(*outError), pageBytes,
                              pageLength) ||
          memoryRangesOverlap(outError, sizeof(*outError),
                              physicalSearchKey,
                              physicalSearchKeyLength) ||
          memoryRangesOverlap(outError, sizeof(*outError), outChildPageId,
                              sizeof(*outChildPageId)) ||
           (outChildIndex != nullptr &&
           memoryRangesOverlap(outError, sizeof(*outError), outChildIndex,
                               sizeof(*outChildIndex)))))) {
        return GxStatus::InvalidArgument;
    }
    setError(outError, GxIndexInternalNodeError::None);
    GxIndexInternalNodeCursor cursor = {};
    GxStatus status = gxIndexInternalNodeCursorInit(
        pageBytes, pageLength, pageCount, expectedOwnerIndexId, &cursor,
        outError);
    if (status != GxStatus::Ok) return status;
    if (physicalSearchKeyLength > cursor.maxPhysicalKeyBytes) {
        setError(outError, GxIndexInternalNodeError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }

    uint64_t selectedPageId = 0;
    uint32_t selectedIndex = 0;
    for (;;) {
        GxIndexInternalChildView child = {};
        bool hasChild = false;
        status = gxIndexInternalNodeCursorReadNext(&cursor, &child,
                                                   &hasChild, outError);
        if (status != GxStatus::Ok) return status;
        if (!hasChild) break;
        const uint32_t index = cursor.position - 1u;
        if (index == 0) {
            selectedPageId = child.childPageId;
            selectedIndex = 0;
            continue;
        }
        int comparison = 0;
        status = gxCompareIndexPhysicalKeyViews(
            child.separatorBytes, child.separatorLength, false, 0, 0,
            physicalSearchKey, physicalSearchKeyLength, false, 0, 0,
            &comparison);
        if (status != GxStatus::Ok) {
            setError(outError,
                     GxIndexInternalNodeError::PhysicalKeyComparisonFailed);
            return GxStatus::CorruptData;
        }
        if (comparison <= 0) {
            selectedPageId = child.childPageId;
            selectedIndex = index;
        } else {
            break;
        }
    }
    if (selectedPageId == 0) {
        setError(outError, GxIndexInternalNodeError::ImpossibleChildCount);
        return GxStatus::CorruptData;
    }
    *outChildPageId = selectedPageId;
    if (outChildIndex != nullptr) *outChildIndex = selectedIndex;
    return GxStatus::Ok;
}
