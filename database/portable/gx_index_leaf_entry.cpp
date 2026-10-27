#include "gx_index_leaf_entry.h"

#include "database_format.h"
#include "gx_index_physical_key.h"

namespace {

const size_t kLeafHeaderSize = 24;
const size_t kLeafSlotSize = 8;
const size_t kLeafEntryOverhead = 16;
const size_t kPhysicalLocatorBytes = 12;

void setError(GxIndexLeafError* outError, GxIndexLeafError error) {
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

GxStatus corrupt(GxIndexLeafError error, GxIndexLeafError* outError) {
    setError(outError, error);
    return GxStatus::CorruptData;
}

GxStatus cursorCorrupt(GxIndexLeafCursor* cursor, GxIndexLeafError error,
                       GxIndexLeafError* outError) {
    cursor->failed = true;
    cursor->lastError = error;
    return corrupt(error, outError);
}

bool intervalsOverlap(size_t leftStart, size_t leftLength,
                      size_t rightStart, size_t rightLength) {
    return leftStart < rightStart + rightLength &&
           rightStart < leftStart + leftLength;
}

bool memoryRangesOverlap(const void* left, size_t leftLength,
                         const void* right, size_t rightLength) {
    if (left == nullptr || right == nullptr || leftLength == 0 ||
        rightLength == 0) {
        return false;
    }
    const uintptr_t leftStart = reinterpret_cast<uintptr_t>(left);
    const uintptr_t rightStart = reinterpret_cast<uintptr_t>(right);
    const uintptr_t maximum = static_cast<uintptr_t>(-1);
    if (leftLength > static_cast<size_t>(maximum - leftStart) ||
        rightLength > static_cast<size_t>(maximum - rightStart)) {
        return true;
    }
    const uintptr_t leftEnd = leftStart + static_cast<uintptr_t>(leftLength);
    const uintptr_t rightEnd = rightStart + static_cast<uintptr_t>(rightLength);
    return leftStart < rightEnd && rightStart < leftEnd;
}

} // namespace

GxStatus gxIndexLeafCursorInit(const uint8_t* pageBytes, size_t pageLength,
                               GxIndexLeafCursor* outCursor,
                               GxIndexLeafError* outError) {
    if (outCursor != nullptr) {
        GxIndexLeafCursor empty = {};
        *outCursor = empty;
    }
    setError(outError, GxIndexLeafError::None);
    if (outCursor == nullptr || pageBytes == nullptr) {
        setError(outError, GxIndexLeafError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }
    if (pageLength < kLeafHeaderSize) {
        return corrupt(GxIndexLeafError::HeaderTruncated, outError);
    }

    const size_t minimumPayload =
        static_cast<size_t>(gxos::db::kMinPageSize - gxos::db::kPageHeaderSize);
    const size_t maximumPayload =
        static_cast<size_t>(gxos::db::kMaxPageSize - gxos::db::kPageHeaderSize);
    if (pageLength < minimumPayload || pageLength > maximumPayload ||
        pageLength > static_cast<size_t>(UINT32_MAX)) {
        return corrupt(GxIndexLeafError::PageSizeOutOfRange, outError);
    }

    if (loadLe32(pageBytes) != gxos::db::kIndexLeafMagic) {
        return corrupt(GxIndexLeafError::SignatureMismatch, outError);
    }
    if (loadLe16(pageBytes + 4) != gxos::db::kIndexPageVersion) {
        return corrupt(GxIndexLeafError::UnsupportedVersion, outError);
    }

    const uint32_t entryCount = loadLe32(pageBytes + 16);
    if (entryCount > (pageLength - kLeafHeaderSize) / kLeafSlotSize) {
        return corrupt(GxIndexLeafError::ImpossibleEntryCount, outError);
    }

    const uint32_t pageSize = static_cast<uint32_t>(
        pageLength + gxos::db::kPageHeaderSize);
    outCursor->pageBytes = pageBytes;
    outCursor->pageLength = pageLength;
    outCursor->position = 0;
    outCursor->maxKeyBytes = gxos::db::maxIndexKeyBytes(pageSize);
    outCursor->page.nextLeafPageId = loadLe64(pageBytes + 8);
    outCursor->page.entryCount = entryCount;
    outCursor->page.ownerIndexId = loadLe32(pageBytes + 20);
    outCursor->failed = false;
    outCursor->lastError = GxIndexLeafError::None;
    return GxStatus::Ok;
}

GxStatus gxIndexLeafCursorReadNext(GxIndexLeafCursor* cursor,
                                   GxIndexLeafEntryView* outEntry,
                                   bool* outHasEntry,
                                   GxIndexLeafError* outError) {
    setError(outError, GxIndexLeafError::None);
    if (outEntry != nullptr) {
        GxIndexLeafEntryView empty = {};
        *outEntry = empty;
    }
    if (outHasEntry != nullptr) *outHasEntry = false;
    if (cursor == nullptr || outEntry == nullptr || outHasEntry == nullptr) {
        setError(outError, GxIndexLeafError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }
    if (cursor->failed) {
        return corrupt(cursor->lastError, outError);
    }
    if (cursor->pageBytes == nullptr || cursor->position > cursor->page.entryCount ||
        cursor->pageLength < kLeafHeaderSize) {
        return cursorCorrupt(cursor, GxIndexLeafError::InvalidCursor, outError);
    }
    if (cursor->position == cursor->page.entryCount) return GxStatus::Ok;

    const uint64_t slotDistance =
        static_cast<uint64_t>(cursor->position + 1u) * kLeafSlotSize;
    if (slotDistance > cursor->pageLength ||
        cursor->pageLength - static_cast<size_t>(slotDistance) < kLeafHeaderSize) {
        return cursorCorrupt(cursor, GxIndexLeafError::SlotOutOfRange, outError);
    }
    const size_t slotOffset = cursor->pageLength -
        static_cast<size_t>(slotDistance);
    const size_t directoryStart = cursor->pageLength -
        static_cast<size_t>(cursor->page.entryCount) * kLeafSlotSize;
    const uint32_t entryOffset32 = loadLe32(cursor->pageBytes + slotOffset);
    const uint32_t entryLength32 = loadLe32(cursor->pageBytes + slotOffset + 4);
    const size_t entryOffset = entryOffset32;
    const size_t entryLength = entryLength32;
    if (entryOffset < kLeafHeaderSize || entryOffset > cursor->pageLength ||
        entryLength > cursor->pageLength - entryOffset ||
        entryOffset + entryLength > directoryStart) {
        return cursorCorrupt(cursor, GxIndexLeafError::EntryOutOfRange, outError);
    }
    if (entryLength < kLeafEntryOverhead) {
        return cursorCorrupt(cursor, GxIndexLeafError::EntryTooSmall, outError);
    }

    const uint8_t* encodedEntry = cursor->pageBytes + entryOffset;
    const uint32_t keyLength32 = loadLe32(encodedEntry);
    const size_t keyLength = keyLength32;
    if (keyLength > cursor->maxKeyBytes ||
        keyLength != entryLength - kLeafEntryOverhead) {
        return cursorCorrupt(cursor, GxIndexLeafError::KeyLengthInvalid, outError);
    }

    outEntry->keyBytes = encodedEntry + 4;
    outEntry->keyLength = keyLength;
    outEntry->locatorPageId = loadLe64(encodedEntry + 4 + keyLength);
    outEntry->locatorSlot = loadLe32(encodedEntry + 4 + keyLength + 8);
    outEntry->entryOffset = entryOffset32;
    outEntry->entryLength = entryLength32;
    ++cursor->position;
    *outHasEntry = true;
    return GxStatus::Ok;
}

GxStatus gxIndexLeafLookup(const uint8_t* pageBytes, size_t pageLength,
                           const uint8_t* physicalKeyBytes,
                           size_t physicalKeyLength,
                           GxIndexLeafLookupResult* outResult,
                           GxIndexLeafError* outError) {
    const bool resultOverlapsInput = outResult != nullptr &&
        (memoryRangesOverlap(outResult, sizeof(*outResult), pageBytes,
                             pageLength) ||
         memoryRangesOverlap(outResult, sizeof(*outResult),
                             physicalKeyBytes, physicalKeyLength));
    const bool outputsOverlap = outResult != nullptr && outError != nullptr &&
        memoryRangesOverlap(outResult, sizeof(*outResult), outError,
                            sizeof(*outError));
    const bool errorOverlapsInput = outError != nullptr &&
        (memoryRangesOverlap(outError, sizeof(*outError), pageBytes,
                             pageLength) ||
         memoryRangesOverlap(outError, sizeof(*outError),
                             physicalKeyBytes, physicalKeyLength));
    if (resultOverlapsInput || outputsOverlap || errorOverlapsInput) {
        if (outError != nullptr && !outputsOverlap && !errorOverlapsInput) {
            setError(outError, GxIndexLeafError::InvalidArgument);
        }
        return GxStatus::InvalidArgument;
    }

    if (outResult != nullptr) {
        GxIndexLeafLookupResult empty = {};
        *outResult = empty;
    }
    setError(outError, GxIndexLeafError::None);
    if (outResult == nullptr || pageBytes == nullptr ||
        (physicalKeyBytes == nullptr && physicalKeyLength != 0) ||
        physicalKeyLength < kPhysicalLocatorBytes) {
        setError(outError, GxIndexLeafError::InvalidArgument);
        return GxStatus::InvalidArgument;
    }

    GxIndexLeafError error = GxIndexLeafError::None;
    GxStatus status = gxIndexLeafValidate(pageBytes, pageLength, &error);
    if (status != GxStatus::Ok) {
        setError(outError, error);
        return status;
    }

    GxIndexLeafLookupResult result = {};
    status = gxIndexLeafCursorInit(pageBytes, pageLength,
                                   &result.continuation, &error);
    if (status != GxStatus::Ok) {
        setError(outError, error);
        return status;
    }

    for (;;) {
        GxIndexLeafEntryView entry = {};
        bool hasEntry = false;
        status = gxIndexLeafCursorReadNext(&result.continuation, &entry,
                                           &hasEntry, &error);
        if (status != GxStatus::Ok) {
            setError(outError, error);
            return status;
        }
        if (!hasEntry) {
            result.entryPosition = result.continuation.page.entryCount;
            *outResult = result;
            return GxStatus::Ok;
        }

        int comparison = 0;
        status = gxCompareIndexPhysicalKeyViews(
            entry.keyBytes, entry.keyLength, true,
            entry.locatorPageId, entry.locatorSlot,
            physicalKeyBytes, physicalKeyLength, false, 0, 0,
            &comparison);
        if (status != GxStatus::Ok) {
            setError(outError, GxIndexLeafError::PhysicalKeyComparisonFailed);
            return GxStatus::CorruptData;
        }
        if (comparison >= 0) {
            result.entry = entry;
            result.entryPosition = result.continuation.position - 1u;
            result.exactMatch = comparison == 0;
            result.hasEntry = true;
            *outResult = result;
            return GxStatus::Ok;
        }
    }
}

GxStatus gxIndexLeafValidate(const uint8_t* pageBytes, size_t pageLength,
                             GxIndexLeafError* outError) {
    setError(outError, GxIndexLeafError::None);
    GxIndexLeafCursor cursor = {};
    GxIndexLeafError error = GxIndexLeafError::None;
    GxStatus status = gxIndexLeafCursorInit(pageBytes, pageLength, &cursor,
                                           &error);
    if (status != GxStatus::Ok) {
        setError(outError, error);
        return status;
    }

    GxIndexLeafEntryView previous = {};
    bool hasPrevious = false;
    uint32_t visited = 0;
    for (;;) {
        GxIndexLeafEntryView current = {};
        bool hasEntry = false;
        status = gxIndexLeafCursorReadNext(&cursor, &current, &hasEntry,
                                          &error);
        if (status != GxStatus::Ok) {
            setError(outError, error);
            return status;
        }
        if (!hasEntry) return GxStatus::Ok;

        for (uint32_t i = 0; i < visited; ++i) {
            const uint64_t priorSlotDistance =
                static_cast<uint64_t>(i + 1u) * kLeafSlotSize;
            if (priorSlotDistance > pageLength ||
                pageLength - static_cast<size_t>(priorSlotDistance) <
                    kLeafHeaderSize) {
                return corrupt(GxIndexLeafError::SlotOutOfRange, outError);
            }
            const size_t priorSlotOffset = pageLength -
                static_cast<size_t>(priorSlotDistance);
            const size_t priorOffset =
                loadLe32(pageBytes + priorSlotOffset);
            const size_t priorLength =
                loadLe32(pageBytes + priorSlotOffset + 4);
            if (intervalsOverlap(current.entryOffset, current.entryLength,
                                 priorOffset, priorLength)) {
                return corrupt(GxIndexLeafError::EntryRegionsOverlap, outError);
            }
        }

        if (hasPrevious) {
            int comparison = 0;
            status = gxCompareIndexPhysicalKeyViews(
                previous.keyBytes, previous.keyLength, true,
                previous.locatorPageId, previous.locatorSlot,
                current.keyBytes, current.keyLength, true,
                current.locatorPageId, current.locatorSlot, &comparison);
            if (status != GxStatus::Ok) {
                return corrupt(GxIndexLeafError::PhysicalKeyComparisonFailed,
                               outError);
            }
            if (comparison >= 0) {
                return corrupt(GxIndexLeafError::PhysicalKeysNotIncreasing,
                               outError);
            }
        }
        previous = current;
        hasPrevious = true;
        ++visited;
    }
}
