#include "database_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "database_endian.h"
#include "database_transaction.h"
#include "portable/gx_array.h"
#include "portable/gx_index_key.h"
#include "portable/gx_index_leaf_entry.h"
#include "portable/gx_index_physical_key.h"
#include "portable/gx_page_visit_set.h"

namespace gxos {
namespace db {

namespace {

const uint32_t kIndexPhysicalLocatorBytes = 12;

uint32_t maxPhysicalIndexKeyBytes(uint32_t pageSize) {
    const uint32_t logicalLimit = maxIndexKeyBytes(pageSize);
    return logicalLimit > UINT32_MAX - kIndexPhysicalLocatorBytes
        ? UINT32_MAX : logicalLimit + kIndexPhysicalLocatorBytes;
}

void appendU32Big(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void appendU64Big(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

uint64_t readU64Big(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(bytes[i]);
    }
    return value;
}

int comparePhysicalKeyParts(const uint8_t* leftKey, size_t leftKeyLength,
                            uint64_t leftPageId, uint32_t leftSlot,
                            const uint8_t* rightKey, size_t rightKeyLength,
                            uint64_t rightPageId, uint32_t rightSlot) {
    int comparison = 0;
    const GxStatus status = gxCompareIndexPhysicalKeyViews(
        leftKey, leftKeyLength, true, leftPageId, leftSlot,
        rightKey, rightKeyLength, true, rightPageId, rightSlot,
        &comparison);
    // Page-derived keys are bounded by the page format, and these borrowed
    // spans are validated before the planner reaches its ordering step.
    return status == GxStatus::Ok ? comparison : 0;
}

int compareStoredBoundToInsertKey(const std::vector<uint8_t>& bound,
                                 const std::vector<uint8_t>& logicalKey,
                                 const RowLocator& locator) {
    int comparison = 0;
    const GxStatus status = gxCompareIndexPhysicalKeyViews(
        bound.empty() ? nullptr : bound.data(), bound.size(), false, 0, 0,
        logicalKey.empty() ? nullptr : logicalKey.data(), logicalKey.size(),
        true, locator.pageId, locator.slot, &comparison);
    // Bounds read from validated pages and the caller's logical key are both
    // within the existing index page-size limit.
    return status == GxStatus::Ok ? comparison : 0;
}

DbResult leafPreparationStatus(GxStatus status) {
    switch (status) {
    case GxStatus::Ok:
        return DbResult::ok();
    case GxStatus::OutOfMemory:
        return DbResult::error(DbStatus::NoSpace,
                               "index leaf preparation allocation failed");
    case GxStatus::ResourceLimit:
    case GxStatus::Overflow:
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "index leaf preparation exceeds its bound");
    case GxStatus::CorruptData:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf preparation produced invalid data");
    case GxStatus::InvalidArgument:
    case GxStatus::UnsupportedAlignment:
        return DbResult::error(DbStatus::Internal,
                               "index leaf preparation allocator is invalid");
    }
    return DbResult::error(DbStatus::Internal,
                           "index leaf preparation failed unexpectedly");
}

DbResult internalPreparationStatus(GxStatus status) {
    switch (status) {
    case GxStatus::Ok:
        return DbResult::ok();
    case GxStatus::OutOfMemory:
        return DbResult::error(DbStatus::NoSpace,
                               "index internal preparation allocation failed");
    case GxStatus::ResourceLimit:
    case GxStatus::Overflow:
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "index internal preparation exceeds its bound");
    case GxStatus::CorruptData:
        return DbResult::error(DbStatus::CorruptPage,
                               "index internal preparation produced invalid data");
    case GxStatus::InvalidArgument:
    case GxStatus::UnsupportedAlignment:
        return DbResult::error(DbStatus::Internal,
                               "index internal preparation allocator is invalid");
    }
    return DbResult::error(DbStatus::Internal,
                           "index internal preparation failed unexpectedly");
}

DbResult leafEntryDataError(GxIndexLeafError error) {
    switch (error) {
    case GxIndexLeafError::HeaderTruncated:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf header truncated");
    case GxIndexLeafError::SignatureMismatch:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf signature mismatch");
    case GxIndexLeafError::UnsupportedVersion:
        return DbResult::error(DbStatus::CorruptPage,
                               "unsupported index leaf version");
    case GxIndexLeafError::ImpossibleEntryCount:
        return DbResult::error(DbStatus::CorruptPage,
                               "impossible index leaf entry count");
    case GxIndexLeafError::SlotOutOfRange:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf slot out of range");
    case GxIndexLeafError::EntryOutOfRange:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf entry out of range");
    case GxIndexLeafError::EntryTooSmall:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf entry too small");
    case GxIndexLeafError::KeyLengthInvalid:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf key length invalid");
    case GxIndexLeafError::PageSizeOutOfRange:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf payload size mismatch");
    case GxIndexLeafError::EntryRegionsOverlap:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf entry regions overlap");
    case GxIndexLeafError::PhysicalKeysNotIncreasing:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf entries not strictly ordered");
    case GxIndexLeafError::PhysicalKeyComparisonFailed:
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf physical key comparison failed");
    case GxIndexLeafError::InvalidArgument:
    case GxIndexLeafError::InvalidCursor:
    case GxIndexLeafError::None:
        break;
    }
    return DbResult::error(DbStatus::CorruptPage,
                           "index leaf entry data is invalid");
}

struct ScopedIndexPayload {
    GxArray bytes;

    ScopedIndexPayload() : bytes() {}
    ~ScopedIndexPayload() { gxArrayDestroy(&bytes); }

    ScopedIndexPayload(const ScopedIndexPayload&) = delete;
    ScopedIndexPayload& operator=(const ScopedIndexPayload&) = delete;
};

// Leaf split entries borrow key bytes from the validated source leaf or from
// the live INSERT argument. The scalar locator is copied into the record.
// Records are moved and stored as bytes because GxArray does not manage C++
// object lifetimes.
struct PlannedLeafEntry {
    const uint8_t* key;
    uint64_t locatorPageId;
    uint32_t keyLength;
    uint32_t locatorSlot;
};

// The split planner stores only fixed-width references here. Existing bounds
// point into validated source page images; the inserted physical separator
// points into a transaction-allocator-owned GxArray kept alive by the plan.
// Records are copied as bytes because GxArray deliberately does not construct
// C++ object lifetimes.
struct PlannedChildRef {
    uint64_t pageId;
    const uint8_t* lowerBound;
    uint32_t lowerBoundSize;
};

struct ScopedIndexPlan {
    GxArray records;

    ScopedIndexPlan() : records() {}
    ~ScopedIndexPlan() { gxArrayDestroy(&records); }

    ScopedIndexPlan(const ScopedIndexPlan&) = delete;
    ScopedIndexPlan& operator=(const ScopedIndexPlan&) = delete;
};

DbResult splitPlanAllocationStatus(GxStatus status,
                                   const char* allocationFailure) {
    switch (status) {
    case GxStatus::Ok:
        return DbResult::ok();
    case GxStatus::OutOfMemory:
        return DbResult::error(DbStatus::NoSpace, allocationFailure);
    case GxStatus::ResourceLimit:
    case GxStatus::Overflow:
        return DbResult::error(DbStatus::TransactionTooLarge,
                               "index split plan exceeds its checked bound");
    case GxStatus::CorruptData:
        return DbResult::error(DbStatus::CorruptPage,
                               "index split plan contains invalid data");
    case GxStatus::InvalidArgument:
    case GxStatus::UnsupportedAlignment:
        return DbResult::error(DbStatus::Internal,
                               "index split plan allocator is invalid");
    }
    return DbResult::error(DbStatus::Internal,
                           "index split plan allocation failed unexpectedly");
}

bool maximumLeafPlanEntryCount(uint32_t pageCapacity,
                               size_t& outMaximumEntries) {
    outMaximumEntries = 0;
    const uint64_t minimumEntryBytes =
        static_cast<uint64_t>(kIndexLeafEntryOverhead) + 1u +
        kIndexSlotSize;
    if (pageCapacity < kIndexLeafHeaderSize || minimumEntryBytes == 0) {
        return false;
    }
    outMaximumEntries = static_cast<size_t>(
        (pageCapacity - kIndexLeafHeaderSize) / minimumEntryBytes);
    return true;
}

PlannedLeafEntry loadPlannedLeafEntry(const GxArray& array, size_t index) {
    PlannedLeafEntry entry = {};
    const uint8_t* bytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&array, index));
    if (bytes != nullptr) std::memcpy(&entry, bytes, sizeof(entry));
    return entry;
}

void storePlannedLeafEntry(GxArray& array, size_t index,
                           const PlannedLeafEntry& entry) {
    uint8_t* bytes = static_cast<uint8_t*>(gxArrayAt(&array, index));
    if (bytes != nullptr) std::memcpy(bytes, &entry, sizeof(entry));
}

int comparePlannedLeafEntries(const PlannedLeafEntry& left,
                              const PlannedLeafEntry& right) {
    return comparePhysicalKeyParts(left.key, left.keyLength,
                                   left.locatorPageId, left.locatorSlot,
                                   right.key, right.keyLength,
                                   right.locatorPageId, right.locatorSlot);
}

void siftPlannedLeafEntries(GxArray& entries, size_t root, size_t count) {
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count &&
            comparePlannedLeafEntries(loadPlannedLeafEntry(entries, child),
                                      loadPlannedLeafEntry(entries, child + 1)) < 0) {
            ++child;
        }
        if (comparePlannedLeafEntries(loadPlannedLeafEntry(entries, root),
                                      loadPlannedLeafEntry(entries, child)) >= 0) {
            return;
        }
        const PlannedLeafEntry parent = loadPlannedLeafEntry(entries, root);
        const PlannedLeafEntry childEntry = loadPlannedLeafEntry(entries, child);
        storePlannedLeafEntry(entries, root, childEntry);
        storePlannedLeafEntry(entries, child, parent);
        root = child;
    }
}

void sortPlannedLeafEntries(GxArray& entries) {
    const size_t count = gxArraySize(&entries);
    for (size_t start = count / 2; start != 0; --start) {
        siftPlannedLeafEntries(entries, start - 1, count);
    }
    for (size_t end = count; end > 1; --end) {
        const PlannedLeafEntry first = loadPlannedLeafEntry(entries, 0);
        const PlannedLeafEntry last = loadPlannedLeafEntry(entries, end - 1);
        storePlannedLeafEntry(entries, 0, last);
        storePlannedLeafEntry(entries, end - 1, first);
        siftPlannedLeafEntries(entries, 0, end - 1);
    }
}

bool leafPlanRangeFits(const GxArray& entries, size_t first, size_t count,
                       uint32_t pageCapacity, uint32_t maxKeyBytes,
                       uint64_t& outUsed) {
    outUsed = 0;
    if (!entries.initialized ||
        entries.elementSize != sizeof(PlannedLeafEntry) ||
        first > gxArraySize(&entries) ||
        count > gxArraySize(&entries) - first ||
        pageCapacity < kIndexLeafHeaderSize ||
        count > (pageCapacity - kIndexLeafHeaderSize) / kIndexSlotSize) {
        return false;
    }
    uint64_t used = kIndexLeafHeaderSize +
        static_cast<uint64_t>(count) * kIndexSlotSize;
    for (size_t i = 0; i < count; ++i) {
        const PlannedLeafEntry entry = loadPlannedLeafEntry(entries, first + i);
        const uint64_t keyLength = entry.keyLength;
        if ((keyLength != 0 && entry.key == nullptr) ||
            keyLength > maxKeyBytes || keyLength > pageCapacity ||
            kIndexLeafEntryOverhead > pageCapacity - keyLength) {
            return false;
        }
        const uint64_t entryLength = kIndexLeafEntryOverhead + keyLength;
        if (used > pageCapacity || entryLength > pageCapacity - used) {
            return false;
        }
        used += entryLength;
    }
    outUsed = used;
    return true;
}

DbResult appendLeafPlanEntry(GxArray& entries,
                             const PlannedLeafEntry& entry) {
    const GxStatus status = gxArrayAppend(&entries, &entry, 1);
    if (status != GxStatus::Ok) {
        return splitPlanAllocationStatus(
            status, "index leaf planning allocation failed");
    }
    return DbResult::ok();
}

GxStatus initSplitPlanArray(GxArray& array, const GxAllocator& allocator,
                            size_t elementSize, size_t maximumElements) {
    GxStatus status = gxArrayInit(&array, &allocator, elementSize,
                                  maximumElements);
    if (status == GxStatus::Ok) {
        // Reserve the complete proven capacity before decoding or insertion.
        // Later list edits therefore cannot allocate partway through planning.
        status = gxArrayReserve(&array, maximumElements);
    }
    if (status != GxStatus::Ok) gxArrayDestroy(&array);
    return status;
}

PlannedChildRef loadPlannedChild(const GxArray& array, size_t index) {
    PlannedChildRef child = {};
    const uint8_t* bytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&array, index));
    if (bytes != nullptr) std::memcpy(&child, bytes, sizeof(child));
    return child;
}

void storePlannedChild(GxArray& array, size_t index,
                       const PlannedChildRef& child) {
    uint8_t* bytes = static_cast<uint8_t*>(gxArrayAt(&array, index));
    if (bytes != nullptr) std::memcpy(bytes, &child, sizeof(child));
}

DbResult preparePhysicalLeafSeparator(const GxAllocator& allocator,
                                      uint32_t pageSize,
                                      const PlannedLeafEntry& entry,
                                      ScopedIndexPayload& out,
                                      const char* allocationFailure) {
    const uint32_t maxLogicalKey = maxIndexKeyBytes(pageSize);
    const uint32_t maxPhysicalKey = maxPhysicalIndexKeyBytes(pageSize);
    if (entry.keyLength == 0 || entry.keyLength > maxLogicalKey ||
        entry.keyLength > UINT32_MAX - kIndexPhysicalLocatorBytes ||
        entry.key == nullptr) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split separator key exceeds its bound");
    }
    const size_t separatorLength =
        static_cast<size_t>(entry.keyLength) + kIndexPhysicalLocatorBytes;
    if (separatorLength > maxPhysicalKey) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split separator exceeds its physical bound");
    }
    GxStatus status = initSplitPlanArray(out.bytes, allocator,
                                         sizeof(uint8_t), separatorLength);
    if (status == GxStatus::Ok) {
        status = gxArrayResize(&out.bytes, separatorLength);
    }
    if (status != GxStatus::Ok) {
        return splitPlanAllocationStatus(status, allocationFailure);
    }
    uint8_t* separator = static_cast<uint8_t*>(gxArrayAt(&out.bytes, 0));
    if (separator == nullptr || gxArraySize(&out.bytes) != separatorLength) {
        return DbResult::error(DbStatus::Internal,
                               "index split separator storage is inconsistent");
    }
    std::memcpy(separator, entry.key, entry.keyLength);
    for (size_t i = 0; i < 8; ++i) {
        separator[entry.keyLength + i] = static_cast<uint8_t>(
            entry.locatorPageId >> (56 - i * 8));
    }
    for (size_t i = 0; i < 4; ++i) {
        separator[entry.keyLength + 8 + i] = static_cast<uint8_t>(
            entry.locatorSlot >> (24 - i * 8));
    }
    return DbResult::ok();
}

size_t maxInternalChildCount(uint32_t pageCapacity) {
    const uint32_t minimumPerChild =
        kIndexInternalEntryOverhead + kIndexSlotSize;
    if (pageCapacity < kIndexInternalHeaderSize) return 0;
    return (pageCapacity - kIndexInternalHeaderSize) / minimumPerChild;
}

template <typename StoreChild>
DbResult decodeInternalChildRecords(const DatabasePage& page,
                                    uint32_t childCount,
                                    uint32_t pageCapacity,
                                    uint32_t maxKeyBytes,
                                    uint64_t pageCount,
                                    StoreChild storeChild) {
    if (page.payload.size() != pageCapacity ||
        childCount > maxInternalChildCount(pageCapacity)) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index internal child count is impossible");
    }
    for (uint32_t i = 0; i < childCount; ++i) {
        const uint32_t slotOffset = pageCapacity - kIndexSlotSize * (i + 1);
        if (slotOffset < kIndexInternalHeaderSize) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal slot out of range");
        }
        const uint32_t childOffset = loadLe32(page.payload.data() + slotOffset);
        const uint32_t childLength = loadLe32(page.payload.data() + slotOffset + 4);
        if (childOffset < kIndexInternalHeaderSize || childOffset > pageCapacity ||
            childLength > pageCapacity - childOffset) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal child out of range");
        }
        if (childLength < kIndexInternalEntryOverhead) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal child too small");
        }
        const uint8_t* childBytes = page.payload.data() + childOffset;
        const uint64_t childPageId = loadLe64(childBytes);
        const uint32_t keyLength = loadLe32(childBytes + 8);
        if (keyLength > maxKeyBytes ||
            keyLength + kIndexInternalEntryOverhead != childLength) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal key length invalid");
        }
        if (childPageId >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal child page out of range");
        }
        if (!storeChild(i, childPageId,
                        childBytes + kIndexInternalEntryOverhead, keyLength)) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal child output is invalid");
        }
    }
    return DbResult::ok();
}

template <typename ChildAt>
DbResult prepareInternalPayloadBytes(const GxAllocator& allocator,
                                     uint32_t pageCapacity,
                                     uint32_t maxKeyBytes,
                                     uint32_t indexId,
                                     uint64_t candidatePageCount,
                                     size_t childCount, ChildAt childAt,
                                     GxArray& outPayload) {
    if (outPayload.initialized) {
        return DbResult::error(DbStatus::Internal,
                               "index internal candidate output is already owned");
    }
    if (candidatePageCount == 0 || childCount > UINT32_MAX || childCount < 2 ||
        pageCapacity < kIndexInternalHeaderSize ||
        childCount > (pageCapacity - kIndexInternalHeaderSize) / kIndexSlotSize) {
        return DbResult::error(DbStatus::NoSpace,
                               "index internal children do not fit");
    }

    uint64_t used = kIndexInternalHeaderSize +
        static_cast<uint64_t>(childCount) * kIndexSlotSize;
    PlannedChildRef previous = {};
    for (size_t i = 0; i < childCount; ++i) {
        PlannedChildRef child = {};
        if (!childAt(i, child) ||
            (child.lowerBoundSize != 0 && child.lowerBound == nullptr)) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal child representation is invalid");
        }
        const size_t keyLength = child.lowerBoundSize;
        if (keyLength > maxKeyBytes || keyLength > pageCapacity ||
            kIndexInternalEntryOverhead > pageCapacity - keyLength ||
            used > pageCapacity -
                (kIndexInternalEntryOverhead + keyLength)) {
            return DbResult::error(DbStatus::NoSpace,
                                   "index internal children do not fit");
        }
        used += kIndexInternalEntryOverhead + keyLength;
        if (child.pageId == 0 || child.pageId >= candidatePageCount) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal candidate child is out of range");
        }
        if (i != 0 && (child.lowerBoundSize == 0 ||
            compareEncodedKeys(previous.lowerBound, previous.lowerBoundSize,
                               child.lowerBound, child.lowerBoundSize) >= 0)) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal candidate bounds are not increasing");
        }
        for (size_t j = 0; j < i; ++j) {
            PlannedChildRef earlier = {};
            if (!childAt(j, earlier)) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "index internal child representation is invalid");
            }
            if (earlier.pageId == child.pageId) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "index internal candidate repeats a child");
            }
        }
        previous = child;
    }

    GxArray candidate = {};
    GxStatus status = gxArrayInit(&candidate, &allocator, sizeof(uint8_t),
                                  pageCapacity);
    if (status == GxStatus::Ok) status = gxArrayResize(&candidate, pageCapacity);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&candidate);
        return internalPreparationStatus(status);
    }
    uint8_t* bytes = static_cast<uint8_t*>(gxArrayAt(&candidate, 0));
    if (bytes == nullptr || gxArraySize(&candidate) != pageCapacity) {
        gxArrayDestroy(&candidate);
        return DbResult::error(DbStatus::Internal,
                               "index internal candidate storage is inconsistent");
    }
    std::memset(bytes, 0, pageCapacity);
    storeLe32(bytes + 0, kIndexInternalMagic);
    storeLe16(bytes + 4, kIndexPageVersion);
    storeLe16(bytes + 6, 0);
    storeLe64(bytes + 8, 0);
    storeLe32(bytes + 16, static_cast<uint32_t>(childCount));
    storeLe32(bytes + 20, indexId);

    uint32_t offset = kIndexInternalHeaderSize;
    for (size_t i = 0; i < childCount; ++i) {
        PlannedChildRef child = {};
        childAt(i, child);
        const uint32_t entryLength =
            kIndexInternalEntryOverhead + child.lowerBoundSize;
        storeLe64(bytes + offset, child.pageId);
        storeLe32(bytes + offset + 8, child.lowerBoundSize);
        if (child.lowerBoundSize != 0) {
            std::memcpy(bytes + offset + 12, child.lowerBound,
                        child.lowerBoundSize);
        }
        const uint32_t slotOffset = pageCapacity -
            kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        storeLe32(bytes + slotOffset, offset);
        storeLe32(bytes + slotOffset + 4, entryLength);
        offset += entryLength;
    }

    bool valid = offset + childCount * kIndexSlotSize == used &&
        loadLe32(bytes) == kIndexInternalMagic &&
        loadLe16(bytes + 4) == kIndexPageVersion &&
        loadLe16(bytes + 6) == 0 && loadLe64(bytes + 8) == 0 &&
        loadLe32(bytes + 16) == childCount && loadLe32(bytes + 20) == indexId;
    uint32_t expectedOffset = kIndexInternalHeaderSize;
    for (size_t i = 0; valid && i < childCount; ++i) {
        const uint32_t slotOffset = pageCapacity -
            kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        const uint32_t entryOffset = loadLe32(bytes + slotOffset);
        const uint32_t entryLength = loadLe32(bytes + slotOffset + 4);
        PlannedChildRef child = {};
        childAt(i, child);
        valid = entryOffset == expectedOffset && entryOffset <= pageCapacity &&
            entryLength >= kIndexInternalEntryOverhead &&
            entryLength <= pageCapacity - entryOffset &&
            entryOffset <= slotOffset && entryLength <= slotOffset - entryOffset &&
            loadLe64(bytes + entryOffset) == child.pageId &&
            loadLe32(bytes + entryOffset + 8) == child.lowerBoundSize &&
            child.lowerBoundSize + kIndexInternalEntryOverhead == entryLength &&
            (child.lowerBoundSize == 0 ||
             std::memcmp(bytes + entryOffset + 12, child.lowerBound,
                         child.lowerBoundSize) == 0);
        expectedOffset += entryLength;
    }
    valid = valid && expectedOffset <= pageCapacity - childCount * kIndexSlotSize;
    if (!valid) {
        gxArrayDestroy(&candidate);
        return DbResult::error(DbStatus::CorruptPage,
                               "index internal candidate validation failed");
    }
    outPayload = candidate;
    candidate = {};
    return DbResult::ok();
}

template <typename LeafAt>
bool validatePreparedLeaf(const uint8_t* bytes, uint32_t capacity,
                          uint64_t nextLeaf, uint32_t indexId,
                          size_t entryCount, LeafAt leafAt,
                          uint32_t maxKeyBytes, uint64_t pageCount) {
    if (bytes == nullptr || capacity < kIndexLeafHeaderSize ||
        loadLe32(bytes) != kIndexLeafMagic ||
        loadLe16(bytes + 4) != kIndexPageVersion ||
        loadLe16(bytes + 6) != 0 || loadLe64(bytes + 8) != nextLeaf ||
        (nextLeaf != 0 && nextLeaf >= pageCount) ||
        entryCount > UINT32_MAX || loadLe32(bytes + 16) != entryCount ||
        loadLe32(bytes + 20) != indexId ||
        entryCount > (capacity - kIndexLeafHeaderSize) / kIndexSlotSize) {
        return false;
    }

    uint32_t expectedOffset = kIndexLeafHeaderSize;
    PlannedLeafEntry previous = {};
    for (size_t i = 0; i < entryCount; ++i) {
        PlannedLeafEntry planned = {};
        if (!leafAt(i, planned) ||
            (planned.keyLength != 0 && planned.key == nullptr)) {
            return false;
        }
        const uint64_t slotDistance =
            static_cast<uint64_t>(kIndexSlotSize) * (i + 1);
        if (slotDistance > capacity) return false;
        const uint32_t slotOffset = capacity - static_cast<uint32_t>(slotDistance);
        const uint32_t entryOffset = loadLe32(bytes + slotOffset);
        const uint32_t entryLength = loadLe32(bytes + slotOffset + 4);
        if (entryOffset != expectedOffset || entryOffset > capacity ||
            entryLength < kIndexLeafEntryOverhead ||
            entryLength > capacity - entryOffset) {
            return false;
        }
        const uint8_t* entry = bytes + entryOffset;
        const uint32_t keyLength = loadLe32(entry);
        if (keyLength > maxKeyBytes || keyLength != planned.keyLength ||
            keyLength > UINT32_MAX - kIndexLeafEntryOverhead ||
            keyLength + kIndexLeafEntryOverhead != entryLength ||
            entryOffset > slotOffset ||
            entryLength > slotOffset - entryOffset ||
            (keyLength != 0 &&
             std::memcmp(entry + 4, planned.key, keyLength) != 0)) {
            return false;
        }
        const uint64_t locatorPage = loadLe64(entry + 4 + keyLength);
        const uint32_t locatorSlot = loadLe32(entry + 4 + keyLength + 8);
        if (locatorPage != planned.locatorPageId ||
            locatorSlot != planned.locatorSlot || locatorPage == 0 ||
            locatorPage >= pageCount) {
            return false;
        }
        if (i != 0 && comparePlannedLeafEntries(previous, planned) >= 0) {
            return false;
        }
        expectedOffset += entryLength;
        previous = planned;
    }
    return expectedOffset <= capacity - entryCount * kIndexSlotSize;
}

template <typename LeafAt>
DbResult prepareLeafPayloadBytes(const GxAllocator& allocator,
                                 uint32_t pageCapacity,
                                 uint32_t maxKeyBytes,
                                 uint32_t indexId, uint64_t nextLeaf,
                                 uint64_t candidatePageCount,
                                 size_t entryCount, LeafAt leafAt,
                                 GxArray& outPayload) {
    if (outPayload.initialized) {
        return DbResult::error(DbStatus::Internal,
                               "index leaf candidate output is already owned");
    }
    if (candidatePageCount == 0 || entryCount > UINT32_MAX ||
        pageCapacity < kIndexLeafHeaderSize ||
        entryCount > (pageCapacity - kIndexLeafHeaderSize) / kIndexSlotSize) {
        return DbResult::error(DbStatus::NoSpace,
                               "index leaf entries do not fit");
    }
    if (nextLeaf != 0 && nextLeaf >= candidatePageCount) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf candidate sibling is out of range");
    }

    uint64_t used = kIndexLeafHeaderSize +
        static_cast<uint64_t>(entryCount) * kIndexSlotSize;
    PlannedLeafEntry previous = {};
    for (size_t i = 0; i < entryCount; ++i) {
        PlannedLeafEntry entry = {};
        if (!leafAt(i, entry) ||
            (entry.keyLength != 0 && entry.key == nullptr)) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index leaf entry representation is invalid");
        }
        const uint64_t keyLength = entry.keyLength;
        if (keyLength > maxKeyBytes || keyLength > pageCapacity ||
            kIndexLeafEntryOverhead > pageCapacity - keyLength) {
            return DbResult::error(DbStatus::NoSpace,
                                   "index leaf entries do not fit");
        }
        const uint64_t entryLength = kIndexLeafEntryOverhead + keyLength;
        if (used > pageCapacity || entryLength > pageCapacity - used) {
            return DbResult::error(DbStatus::NoSpace,
                                   "index leaf entries do not fit");
        }
        used += entryLength;
        if (entry.locatorPageId == 0 ||
            entry.locatorPageId >= candidatePageCount) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index leaf candidate locator is out of range");
        }
        if (i != 0 && comparePlannedLeafEntries(previous, entry) >= 0) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index leaf candidate entries are not increasing");
        }
        previous = entry;
    }

    GxArray candidate = {};
    GxStatus status = gxArrayInit(&candidate, &allocator, sizeof(uint8_t),
                                  pageCapacity);
    if (status == GxStatus::Ok) status = gxArrayResize(&candidate, pageCapacity);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&candidate);
        return leafPreparationStatus(status);
    }

    uint8_t* bytes = static_cast<uint8_t*>(gxArrayAt(&candidate, 0));
    if (bytes == nullptr || gxArraySize(&candidate) != pageCapacity) {
        gxArrayDestroy(&candidate);
        return DbResult::error(DbStatus::Internal,
                               "index leaf candidate storage is inconsistent");
    }
    std::memset(bytes, 0, pageCapacity);
    storeLe32(bytes + 0, kIndexLeafMagic);
    storeLe16(bytes + 4, kIndexPageVersion);
    storeLe16(bytes + 6, 0);
    storeLe64(bytes + 8, nextLeaf);
    storeLe32(bytes + 16, static_cast<uint32_t>(entryCount));
    storeLe32(bytes + 20, indexId);

    uint32_t offset = kIndexLeafHeaderSize;
    for (size_t i = 0; i < entryCount; ++i) {
        PlannedLeafEntry entry = {};
        leafAt(i, entry);
        const uint32_t entryLength =
            kIndexLeafEntryOverhead + entry.keyLength;
        storeLe32(bytes + offset, entry.keyLength);
        if (entry.keyLength != 0) {
            std::memcpy(bytes + offset + 4, entry.key, entry.keyLength);
        }
        storeLe64(bytes + offset + 4 + entry.keyLength,
                  entry.locatorPageId);
        storeLe32(bytes + offset + 4 + entry.keyLength + 8,
                  entry.locatorSlot);
        const uint32_t slotOffset = pageCapacity -
            kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        storeLe32(bytes + slotOffset, offset);
        storeLe32(bytes + slotOffset + 4, entryLength);
        offset += entryLength;
    }

    if (offset + entryCount * kIndexSlotSize != used ||
        !validatePreparedLeaf(bytes, pageCapacity, nextLeaf, indexId,
                              entryCount, leafAt, maxKeyBytes,
                              candidatePageCount)) {
        gxArrayDestroy(&candidate);
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf candidate validation failed");
    }
    outPayload = candidate;
    candidate = {};
    return DbResult::ok();
}

// Order-preserving Float64 transform for finite values. -0.0 is normalized to
// +0.0 before this is called so the two zeroes encode identically.
uint64_t floatToOrdered(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (bits & 0x8000000000000000ull) {
        return ~bits;
    }
    return bits | 0x8000000000000000ull;
}

} // namespace

bool encodeIndexKey(DbType type, const DbValue& value, std::vector<uint8_t>& out) {
    out.clear();

    // Keep DbValue as the hosted adapter; fixed-width integer bytes have one
    // production implementation in the portable scalar codec.
    if (type == DbType::Int32 || type == DbType::Int64) {
        uint8_t encoded[9] = {};
        size_t encodedLength = 0;
        GxStatus status = GxStatus::InvalidArgument;
        if (type == DbType::Int32) {
            if (!value.isNull() && value.type() != DbType::Int32) {
                out.push_back(0x01u);
                return false;
            }
            status = gxEncodeIndexInt32Key(
                value.isNull(), value.int32Value(), encoded, sizeof(encoded),
                &encodedLength);
        } else {
            if (!value.isNull() && value.type() != DbType::Int64) {
                out.push_back(0x01u);
                return false;
            }
            status = gxEncodeIndexInt64Key(
                value.isNull(), value.int64Value(), encoded, sizeof(encoded),
                &encodedLength);
        }
        if (status != GxStatus::Ok) {
            out.push_back(value.isNull() ? 0x00u : 0x01u);
            return false;
        }
        out.assign(encoded, encoded + encodedLength);
        return true;
    }

    // Boolean keys use the same hosted-adapter boundary as the integer keys;
    // keep the byte format in the freestanding production codec.
    if (type == DbType::Boolean) {
        if (!value.isNull() && value.type() != DbType::Boolean) {
            out.push_back(0x01u);
            return false;
        }
        uint8_t encoded[2] = {};
        size_t encodedLength = 0;
        const GxStatus status = gxEncodeIndexBooleanKey(
            value.isNull(), value.booleanValue(), encoded, sizeof(encoded),
            &encodedLength);
        if (status != GxStatus::Ok) {
            out.push_back(value.isNull() ? 0x00u : 0x01u);
            return false;
        }
        out.assign(encoded, encoded + encodedLength);
        return true;
    }

    if (value.isNull()) {
        out.push_back(0x00);
        return true;
    }
    out.push_back(0x01);
    switch (type) {
    case DbType::Boolean:
        if (value.type() != DbType::Boolean) {
            return false;
        }
        out.push_back(value.booleanValue() ? 1u : 0u);
        return true;
    case DbType::Float64: {
        if (value.type() != DbType::Float64) {
            return false;
        }
        double d = value.float64Value();
        if (!std::isfinite(d)) {
            return false;
        }
        if (d == 0.0) {
            d = 0.0; // collapse -0.0 to +0.0
        }
        appendU64Big(out, floatToOrdered(d));
        return true;
    }
    case DbType::Text: {
        if (value.type() != DbType::Text) {
            return false;
        }
        const std::string& text = value.textValue();
        size_t encodedLength = 0;
        if (gxIndexTextKeyEncodedSize(text.size(), &encodedLength) !=
            GxStatus::Ok) {
            return false;
        }
        if (encodedLength > out.max_size()) {
            return false;
        }
        out.resize(encodedLength);
        size_t written = 0;
        const GxStatus status = gxEncodeIndexTextKey(
            reinterpret_cast<const uint8_t*>(text.data()), text.size(),
            out.data(), encodedLength, &written);
        if (status != GxStatus::Ok || written != encodedLength) {
            out.resize(1);
            out[0] = 0x01u;
            return false;
        }
        return true;
    }
    default:
        return false;
    }
}

bool encodedKeyIsNull(const uint8_t* key, size_t length) {
    bool isNull = false;
    return gxIndexKeyIsNull(key, length, &isNull) == GxStatus::Ok && isNull;
}

int compareEncodedKeys(const uint8_t* a, size_t aLength, const uint8_t* b,
                       size_t bLength) {
    int comparison = 0;
    (void)gxCompareIndexKeys(a, aLength, b, bLength, &comparison);
    return comparison;
}

bool decodeIndexKey(DbType type, const uint8_t* key, size_t length, DbValue& out) {
    if (type == DbType::Boolean) {
        bool value = false;
        bool isNull = false;
        if (gxDecodeIndexBooleanKey(key, length, &value, &isNull) !=
            GxStatus::Ok) {
            return false;
        }
        out = isNull ? DbValue::null() : DbValue::boolean(value);
        return true;
    }
    if (type == DbType::Int32) {
        int32_t value = 0;
        bool isNull = false;
        if (gxDecodeIndexInt32Key(key, length, &value, &isNull) !=
            GxStatus::Ok) {
            return false;
        }
        out = isNull ? DbValue::null() : DbValue::int32(value);
        return true;
    }
    if (type == DbType::Int64) {
        int64_t value = 0;
        bool isNull = false;
        if (gxDecodeIndexInt64Key(key, length, &value, &isNull) !=
            GxStatus::Ok) {
            return false;
        }
        out = isNull ? DbValue::null() : DbValue::int64(value);
        return true;
    }

    if (type == DbType::Text) {
        GxTextByteView text = {nullptr, 0};
        bool isNull = false;
        const GxStatus status =
            gxDecodeIndexTextKey(key, length, &text, &isNull);
        if (status != GxStatus::Ok) return false;
        if (isNull) {
            out = DbValue::null();
        } else {
            // The portable result borrows the encoded bytes. DbValue owns its
            // Text, so copy only at this hosted API boundary.
            const std::string ownedText(
                reinterpret_cast<const char*>(text.data), text.length);
            out = DbValue::text(ownedText);
        }
        return true;
    }

    if (!key || length == 0) {
        return false;
    }
    if (key[0] == 0x00) {
        if (length != 1) {
            return false;
        }
        out = DbValue::null();
        return true;
    }
    if (key[0] != 0x01) {
        return false;
    }
    switch (type) {
    case DbType::Float64: {
        if (length != 9) {
            return false;
        }
        uint64_t bits = readU64Big(key + 1);
        if (bits & 0x8000000000000000ull) {
            bits &= 0x7FFFFFFFFFFFFFFFull;
        } else {
            bits = ~bits;
        }
        double d = 0.0;
        std::memcpy(&d, &bits, sizeof(d));
        out = DbValue::float64(d);
        return true;
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// IndexTree.

IndexTree::IndexTree(PageAccess& pages, DbType keyType, uint32_t indexId,
                     Transaction* transaction)
    : _pages(pages), _keyType(keyType), _indexId(indexId),
      _transaction(transaction), _traversalAllocator(nullptr) {}

IndexTree::IndexTree(PageAccess& pages, DbType keyType, uint32_t indexId,
                     Transaction* transaction,
                     const GxAllocator* traversalAllocator)
    : _pages(pages), _keyType(keyType), _indexId(indexId),
      _transaction(transaction), _traversalAllocator(traversalAllocator) {}

namespace {

DbResult pageVisitTrackerError(GxStatus status) {
    if (status == GxStatus::CorruptData) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index page visit id out of range");
    }
    if (status == GxStatus::OutOfMemory || status == GxStatus::ResourceLimit ||
        status == GxStatus::Overflow) {
        return DbResult::error(DbStatus::NoSpace,
                               "index page visit bookkeeping unavailable");
    }
    return DbResult::error(DbStatus::Internal,
                           "index page visit tracker contract failed");
}

DbResult initializePageVisitTracker(::GxPageVisitSet& tracker,
                                    const GxAllocator* allocator,
                                    uint64_t pageCount) {
    if (pageCount == 0 || pageCount > kMaxPageCount) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index logical page count out of range");
    }
    if (allocator == nullptr) {
        return DbResult::error(DbStatus::NoSpace,
                               "index page visit allocator unavailable");
    }
    const GxStatus status = tracker.init(allocator, pageCount, pageCount - 1);
    return status == GxStatus::Ok ? DbResult::ok()
                                  : pageVisitTrackerError(status);
}

} // namespace

uint32_t IndexTree::capacity() const {
    return DatabasePage::payloadCapacity(_pages.pageSize());
}

uint64_t IndexTree::leafUsed(const std::vector<IndexEntry>& entries) {
    uint64_t used = kIndexLeafHeaderSize + kIndexSlotSize * entries.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        used += kIndexLeafEntryOverhead + entries[i].key.size();
    }
    return used;
}

uint64_t IndexTree::internalUsed(const std::vector<Child>& children) const {
    uint64_t used = kIndexInternalHeaderSize + kIndexSlotSize * children.size();
    for (size_t i = 0; i < children.size(); ++i) {
        used += kIndexInternalEntryOverhead + children[i].lowerBound.size();
    }
    return used;
}

std::vector<uint8_t> IndexTree::makePhysicalKey(const std::vector<uint8_t>& logicalKey,
                                                const RowLocator& locator) {
    std::vector<uint8_t> key = logicalKey;
    appendLocator(key, locator);
    return key;
}

void IndexTree::appendLocator(std::vector<uint8_t>& out, const RowLocator& locator) {
    appendU64Big(out, locator.pageId);
    appendU32Big(out, locator.slot);
}

DbResult IndexTree::readLeaf(uint64_t pageId, DatabasePage& page, LeafInfo& info) const {
    DbResult result = _pages.readPage(pageId, page);
    if (!result.isOk()) {
        return result;
    }
    if (page.type != PageType::IndexLeaf) {
        return DbResult::error(DbStatus::CorruptPage, "index page is not a leaf");
    }
    const uint32_t cap = capacity();
    if (page.payload.size() != cap) {
        return DbResult::error(DbStatus::CorruptPage, "index leaf payload size mismatch");
    }
    GxIndexLeafCursor cursor = {};
    GxIndexLeafError leafError = GxIndexLeafError::None;
    const GxStatus cursorStatus = gxIndexLeafCursorInit(
        page.payload.data(), page.payload.size(), &cursor, &leafError);
    if (cursorStatus != GxStatus::Ok) return leafEntryDataError(leafError);
    info.nextLeaf = cursor.page.nextLeafPageId;
    info.entryCount = cursor.page.entryCount;
    info.ownerIndexId = cursor.page.ownerIndexId;
    if (info.ownerIndexId != _indexId) {
        return DbResult::error(DbStatus::CorruptPage, "index leaf belongs to another index");
    }
    return DbResult::ok();
}

DbResult IndexTree::readLeafEntries(const DatabasePage& page, const LeafInfo& info,
                                    std::vector<IndexEntry>& out) const {
    out.clear();
    out.reserve(info.entryCount);
    GxIndexLeafCursor cursor = {};
    GxIndexLeafError leafError = GxIndexLeafError::None;
    GxStatus status = gxIndexLeafCursorInit(
        page.payload.data(), page.payload.size(), &cursor, &leafError);
    if (status != GxStatus::Ok) return leafEntryDataError(leafError);
    if (cursor.page.entryCount != info.entryCount ||
        cursor.page.nextLeafPageId != info.nextLeaf ||
        cursor.page.ownerIndexId != info.ownerIndexId) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf metadata changed during read");
    }
    for (;;) {
        GxIndexLeafEntryView entry = {};
        bool hasEntry = false;
        status = gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                          &leafError);
        if (status != GxStatus::Ok) return leafEntryDataError(leafError);
        if (!hasEntry) break;
        IndexEntry result;
        result.key.assign(entry.keyBytes, entry.keyBytes + entry.keyLength);
        result.locator.pageId = entry.locatorPageId;
        result.locator.slot = entry.locatorSlot;
        out.push_back(std::move(result));
    }
    return DbResult::ok();
}

DbResult IndexTree::readLeafPlan(const DatabasePage& page, const LeafInfo& info,
                                GxArray& out, size_t& outCount) const {
    outCount = 0;
    const uint32_t cap = capacity();
    size_t maximumEntries = 0;
    if (!out.initialized ||
        out.elementSize != sizeof(PlannedLeafEntry) ||
        !maximumLeafPlanEntryCount(cap, maximumEntries) ||
        maximumEntries == static_cast<size_t>(-1) ||
        out.maximum < maximumEntries + 1 ||
        page.payload.size() != cap || info.entryCount > maximumEntries) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf plan capacity or count is invalid");
    }

    GxIndexLeafCursor cursor = {};
    GxIndexLeafError leafError = GxIndexLeafError::None;
    GxStatus status = gxIndexLeafCursorInit(
        page.payload.data(), page.payload.size(), &cursor, &leafError);
    if (status != GxStatus::Ok || cursor.page.entryCount != info.entryCount ||
        cursor.page.nextLeafPageId != info.nextLeaf ||
        cursor.page.ownerIndexId != info.ownerIndexId) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf plan capacity or count is invalid");
    }
    for (;;) {
        GxIndexLeafEntryView leafEntry = {};
        bool hasEntry = false;
        status = gxIndexLeafCursorReadNext(&cursor, &leafEntry, &hasEntry,
                                          &leafError);
        if (status != GxStatus::Ok) {
            return leafError == GxIndexLeafError::KeyLengthInvalid
                ? DbResult::error(DbStatus::CorruptPage,
                                  "index leaf plan key length is invalid")
                : DbResult::error(DbStatus::CorruptPage,
                                  "index leaf plan entry is out of range");
        }
        if (!hasEntry) break;
        if (leafEntry.keyLength == 0) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index leaf plan key length is invalid");
        }
        PlannedLeafEntry entry = {};
        entry.key = leafEntry.keyBytes;
        entry.locatorPageId = leafEntry.locatorPageId;
        entry.keyLength = static_cast<uint32_t>(leafEntry.keyLength);
        entry.locatorSlot = leafEntry.locatorSlot;
        DbResult appended = appendLeafPlanEntry(out, entry);
        if (!appended.isOk()) return appended;
    }
    outCount = info.entryCount;
    return DbResult::ok();
}

DbResult IndexTree::writeLeaf(uint64_t pageId, uint64_t nextLeaf,
                              const std::vector<IndexEntry>& entries) {
    const uint32_t cap = capacity();
    uint64_t used = kIndexLeafHeaderSize + kIndexSlotSize * entries.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        used += kIndexLeafEntryOverhead + entries[i].key.size();
    }
    if (used > cap) {
        return DbResult::error(DbStatus::NoSpace, "index leaf entries do not fit");
    }
    DatabasePage page;
    DbResult result = _pages.readPage(pageId, page);
    if (!result.isOk()) {
        return result;
    }
    page.pageId = pageId;
    page.type = PageType::IndexLeaf;
    try {
        page.payload.assign(cap, 0);
    } catch (const std::bad_alloc&) {
        return DbResult::error(DbStatus::NoSpace,
                               "index leaf payload allocation failed");
    }
    storeLe32(page.payload.data() + 0, kIndexLeafMagic);
    storeLe16(page.payload.data() + 4, kIndexPageVersion);
    storeLe16(page.payload.data() + 6, 0);
    storeLe64(page.payload.data() + 8, nextLeaf);
    storeLe32(page.payload.data() + 16, static_cast<uint32_t>(entries.size()));
    storeLe32(page.payload.data() + 20, _indexId);
    uint32_t offset = kIndexLeafHeaderSize;
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint32_t keyLength = static_cast<uint32_t>(entries[i].key.size());
        const uint32_t entryLength = kIndexLeafEntryOverhead + keyLength;
        storeLe32(page.payload.data() + offset, keyLength);
        if (keyLength > 0) {
            std::memcpy(page.payload.data() + offset + 4, entries[i].key.data(),
                        keyLength);
        }
        storeLe64(page.payload.data() + offset + 4 + keyLength,
                  entries[i].locator.pageId);
        storeLe32(page.payload.data() + offset + 4 + keyLength + 8,
                  entries[i].locator.slot);
        const uint32_t slotOffset = cap - kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        storeLe32(page.payload.data() + slotOffset, offset);
        storeLe32(page.payload.data() + slotOffset + 4, entryLength);
        offset += entryLength;
    }
    page.payloadSize = cap;
    return _pages.writePage(page);
}

DbResult IndexTree::stagePreparedLeaf(uint64_t pageId, uint64_t nextLeaf,
                                      uint16_t flags, uint32_t generation,
                                      const std::vector<IndexEntry>& entries) {
    if (_transaction == nullptr) {
        return DbResult::error(DbStatus::Internal,
                               "checked index leaf staging requires a transaction");
    }
    if (static_cast<PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "index leaf transaction does not own its page view");
    }
    GxArray candidate = {};
    DbResult prepared = prepareLeafPayload(nextLeaf, entries,
                                           _transaction->pageCount(), candidate);
    if (!prepared.isOk()) {
        gxArrayDestroy(&candidate);
        return prepared;
    }

    const uint32_t cap = capacity();
    const uint8_t* bytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&candidate, 0));
    DbResult staged = _transaction->writePagePayload(
        pageId, PageType::IndexLeaf, flags, generation, bytes, cap);
    gxArrayDestroy(&candidate);
    return staged;
}

DbResult IndexTree::stagePreparedLeafPlan(
    uint64_t pageId, uint64_t nextLeaf, uint16_t flags,
    uint32_t generation, const GxArray& entries, size_t count) {
    if (_transaction == nullptr) {
        return DbResult::error(DbStatus::Internal,
                               "checked index leaf staging requires a transaction");
    }
    if (static_cast<PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "index leaf transaction does not own its page view");
    }
    ScopedIndexPayload candidate;
    DbResult prepared = prepareLeafPlanPayload(
        nextLeaf, entries, 0, count, _transaction->pageCount(),
        candidate.bytes);
    if (!prepared.isOk()) return prepared;

    const uint8_t* bytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&candidate.bytes, 0));
    return _transaction->writePagePayload(
        pageId, PageType::IndexLeaf, flags, generation, bytes, capacity());
}

DbResult IndexTree::prepareLeafPayload(
    uint64_t nextLeaf, const std::vector<IndexEntry>& entries,
    uint64_t candidatePageCount, GxArray& outPayload) const {
    if (_transaction == nullptr ||
        static_cast<const PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "checked index leaf preparation requires its transaction");
    }
    const uint32_t cap = capacity();
    const uint32_t maxKey = maxIndexKeyBytes(_pages.pageSize());
    return prepareLeafPayloadBytes(
        _transaction->pageAllocator(), cap, maxKey, _indexId, nextLeaf,
        candidatePageCount, entries.size(),
        [&entries](size_t index, PlannedLeafEntry& planned) -> bool {
            if (index >= entries.size() ||
                entries[index].key.size() > UINT32_MAX) {
                return false;
            }
            planned.key = entries[index].key.empty()
                ? nullptr : entries[index].key.data();
            planned.locatorPageId = entries[index].locator.pageId;
            planned.keyLength = static_cast<uint32_t>(entries[index].key.size());
            planned.locatorSlot = entries[index].locator.slot;
            return true;
        }, outPayload);
}

DbResult IndexTree::prepareLeafPlanPayload(
    uint64_t nextLeaf, const GxArray& entries, size_t first, size_t count,
    uint64_t candidatePageCount, GxArray& outPayload) const {
    if (_transaction == nullptr ||
        static_cast<const PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "checked index leaf preparation requires its transaction");
    }
    if (!entries.initialized ||
        entries.elementSize != sizeof(PlannedLeafEntry) ||
        first > gxArraySize(&entries) ||
        count > gxArraySize(&entries) - first) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf plan range is invalid");
    }
    const uint32_t cap = capacity();
    const uint32_t maxKey = maxIndexKeyBytes(_pages.pageSize());
    uint64_t used = 0;
    if (!leafPlanRangeFits(entries, first, count, cap, maxKey, used)) {
        return DbResult::error(DbStatus::NoSpace,
                               "index leaf entries do not fit");
    }
    return prepareLeafPayloadBytes(
        _transaction->pageAllocator(), cap, maxKey, _indexId, nextLeaf,
        candidatePageCount, count,
        [&entries, first](size_t index, PlannedLeafEntry& planned) -> bool {
            if (index >= gxArraySize(&entries) - first) return false;
            planned = loadPlannedLeafEntry(entries, first + index);
            return true;
        }, outPayload);
}

DbResult IndexTree::allocateLeaf(uint64_t nextLeaf, uint64_t& outPageId) {
    uint64_t pageId = 0;
    DbResult result = _pages.allocatePage(PageType::IndexLeaf, pageId);
    if (!result.isOk()) {
        return result;
    }
    std::vector<IndexEntry> empty;
    result = writeLeaf(pageId, nextLeaf, empty);
    if (!result.isOk()) {
        return result;
    }
    outPageId = pageId;
    return DbResult::ok();
}

DbResult IndexTree::readInternal(uint64_t pageId, DatabasePage& page,
                                 InternalInfo& info) const {
    DbResult result = _pages.readPage(pageId, page);
    if (!result.isOk()) {
        return result;
    }
    if (page.type != PageType::IndexInternal) {
        return DbResult::error(DbStatus::CorruptPage, "index page is not internal");
    }
    const uint32_t cap = capacity();
    if (page.payload.size() != cap) {
        return DbResult::error(DbStatus::CorruptPage, "index internal payload size mismatch");
    }
    if (page.payload.size() < kIndexInternalHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "index internal header truncated");
    }
    const uint8_t* p = page.payload.data();
    if (loadLe32(p + 0) != kIndexInternalMagic) {
        return DbResult::error(DbStatus::CorruptPage, "index internal signature mismatch");
    }
    if (loadLe16(p + 4) != kIndexPageVersion) {
        return DbResult::error(DbStatus::CorruptPage, "unsupported index internal version");
    }
    info.childCount = loadLe32(p + 16);
    info.ownerIndexId = loadLe32(p + 20);
    if (info.ownerIndexId != _indexId) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index internal page belongs to another index");
    }
    if (kIndexInternalHeaderSize +
            kIndexSlotSize * static_cast<uint64_t>(info.childCount) >
        cap) {
        return DbResult::error(DbStatus::CorruptPage,
                               "impossible index internal child count");
    }
    return DbResult::ok();
}

DbResult IndexTree::readInternalChildren(const DatabasePage& page,
                                         const InternalInfo& info,
                                         std::vector<Child>& out) const {
    out.clear();
    const uint32_t cap = capacity();
    if (info.childCount > maxInternalChildCount(cap)) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index internal child count is impossible");
    }
    out.reserve(info.childCount);
    return decodeInternalChildRecords(
        page, info.childCount, cap,
        maxPhysicalIndexKeyBytes(_pages.pageSize()), _pages.pageCount(),
        [&out](size_t, uint64_t pageId, const uint8_t* lowerBound,
               uint32_t lowerBoundSize) -> bool {
            Child child;
            child.pageId = pageId;
            child.lowerBound.assign(lowerBound, lowerBound + lowerBoundSize);
            out.push_back(std::move(child));
            return true;
        });
}

DbResult IndexTree::readInternalChildPlan(const DatabasePage& page,
                                          const InternalInfo& info,
                                          GxArray& out,
                                          size_t& outCount) const {
    outCount = 0;
    const uint32_t cap = capacity();
    if (!out.initialized || out.elementSize != sizeof(PlannedChildRef) ||
        out.capacity < info.childCount ||
        info.childCount > maxInternalChildCount(cap)) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split child plan capacity is invalid");
    }
    GxStatus status = gxArrayResize(&out, info.childCount);
    if (status != GxStatus::Ok) {
        return splitPlanAllocationStatus(
            status, "index parent split planning allocation failed");
    }
    DbResult result = decodeInternalChildRecords(
        page, info.childCount, cap,
        maxPhysicalIndexKeyBytes(_pages.pageSize()), _pages.pageCount(),
        [&out](size_t index, uint64_t pageId, const uint8_t* lowerBound,
               uint32_t lowerBoundSize) -> bool {
            if (index >= gxArraySize(&out)) return false;
            PlannedChildRef child = {};
            child.pageId = pageId;
            child.lowerBound = lowerBound;
            child.lowerBoundSize = lowerBoundSize;
            storePlannedChild(out, index, child);
            return true;
        });
    if (!result.isOk()) return result;
    outCount = info.childCount;
    return DbResult::ok();
}
DbResult IndexTree::writeInternal(uint64_t pageId, const std::vector<Child>& children) {
    const uint32_t cap = capacity();
    uint64_t used = kIndexInternalHeaderSize + kIndexSlotSize * children.size();
    for (size_t i = 0; i < children.size(); ++i) {
        used += kIndexInternalEntryOverhead + children[i].lowerBound.size();
    }
    if (used > cap) {
        return DbResult::error(DbStatus::NoSpace, "index internal children do not fit");
    }
    DatabasePage page;
    DbResult result = _pages.readPage(pageId, page);
    if (!result.isOk()) {
        return result;
    }
    page.pageId = pageId;
    page.type = PageType::IndexInternal;
    try {
        page.payload.assign(cap, 0);
    } catch (const std::bad_alloc&) {
        return DbResult::error(DbStatus::NoSpace,
                               "index internal payload allocation failed");
    }
    storeLe32(page.payload.data() + 0, kIndexInternalMagic);
    storeLe16(page.payload.data() + 4, kIndexPageVersion);
    storeLe16(page.payload.data() + 6, 0);
    storeLe64(page.payload.data() + 8, 0);
    storeLe32(page.payload.data() + 16, static_cast<uint32_t>(children.size()));
    storeLe32(page.payload.data() + 20, _indexId);
    uint32_t offset = kIndexInternalHeaderSize;
    for (size_t i = 0; i < children.size(); ++i) {
        const uint32_t keyLength = static_cast<uint32_t>(children[i].lowerBound.size());
        const uint32_t childLength = kIndexInternalEntryOverhead + keyLength;
        storeLe64(page.payload.data() + offset, children[i].pageId);
        storeLe32(page.payload.data() + offset + 8, keyLength);
        if (keyLength > 0) {
            std::memcpy(page.payload.data() + offset + 12,
                        children[i].lowerBound.data(), keyLength);
        }
        const uint32_t slotOffset = cap - kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        storeLe32(page.payload.data() + slotOffset, offset);
        storeLe32(page.payload.data() + slotOffset + 4, childLength);
        offset += childLength;
    }
    page.payloadSize = cap;
    return _pages.writePage(page);
}

bool IndexTree::internalChildrenFit(const std::vector<Child>& children,
                                   uint32_t pageCapacity,
                                   uint32_t maxKeyBytes,
                                   uint64_t& outUsed) const {
    outUsed = 0;
    if (pageCapacity < kIndexInternalHeaderSize ||
        children.size() < 2 ||
        children.size() >
            (pageCapacity - kIndexInternalHeaderSize) / kIndexSlotSize) {
        return false;
    }
    uint64_t used = kIndexInternalHeaderSize +
        static_cast<uint64_t>(children.size()) * kIndexSlotSize;
    for (size_t i = 0; i < children.size(); ++i) {
        const size_t keyLength = children[i].lowerBound.size();
        if (keyLength > maxKeyBytes || keyLength > UINT32_MAX ||
            keyLength > pageCapacity ||
            kIndexInternalEntryOverhead > pageCapacity - keyLength) {
            return false;
        }
        const uint64_t entryLength =
            kIndexInternalEntryOverhead + static_cast<uint64_t>(keyLength);
        if (used > pageCapacity || entryLength > pageCapacity - used) {
            return false;
        }
        used += entryLength;
    }
    outUsed = used;
    return true;
}

bool IndexTree::internalChildPlanFits(const GxArray& children, size_t first,
                                      size_t count, uint32_t pageCapacity,
                                      uint32_t maxKeyBytes,
                                      uint64_t& outUsed) const {
    outUsed = 0;
    if (!children.initialized ||
        children.elementSize != sizeof(PlannedChildRef) ||
        first > gxArraySize(&children) ||
        count > gxArraySize(&children) - first ||
        pageCapacity < kIndexInternalHeaderSize || count < 2 ||
        count > (pageCapacity - kIndexInternalHeaderSize) / kIndexSlotSize) {
        return false;
    }
    uint64_t used = kIndexInternalHeaderSize +
        static_cast<uint64_t>(count) * kIndexSlotSize;
    for (size_t i = 0; i < count; ++i) {
        const PlannedChildRef child = loadPlannedChild(children, first + i);
        const size_t keyLength = child.lowerBoundSize;
        if (keyLength > maxKeyBytes || keyLength > UINT32_MAX ||
            keyLength > pageCapacity ||
            kIndexInternalEntryOverhead > pageCapacity - keyLength) {
            return false;
        }
        const uint64_t entryLength =
            kIndexInternalEntryOverhead + static_cast<uint64_t>(keyLength);
        if (used > pageCapacity || entryLength > pageCapacity - used) {
            return false;
        }
        used += entryLength;
    }
    outUsed = used;
    return true;
}

DbResult IndexTree::prepareInternalPayload(
    const std::vector<Child>& children, uint64_t candidatePageCount,
    GxArray& outPayload) const {
    if (_transaction == nullptr ||
        static_cast<const PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "checked index parent preparation requires its transaction");
    }
    const uint32_t maxKey = maxPhysicalIndexKeyBytes(_pages.pageSize());
    return prepareInternalPayloadBytes(
        _transaction->pageAllocator(), capacity(), maxKey, _indexId,
        candidatePageCount, children.size(),
        [&children](size_t index, PlannedChildRef& child) -> bool {
            if (index >= children.size() ||
                children[index].lowerBound.size() > UINT32_MAX) {
                return false;
            }
            child.pageId = children[index].pageId;
            child.lowerBound = children[index].lowerBound.empty()
                ? nullptr : children[index].lowerBound.data();
            child.lowerBoundSize = static_cast<uint32_t>(
                children[index].lowerBound.size());
            return true;
        },
        outPayload);
}

DbResult IndexTree::prepareInternalPlanPayload(
    const GxArray& children, size_t first, size_t count,
    uint64_t candidatePageCount, GxArray& outPayload) const {
    if (_transaction == nullptr ||
        static_cast<const PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "checked index split plan requires its transaction");
    }
    if (!children.initialized ||
        children.elementSize != sizeof(PlannedChildRef) ||
        first > gxArraySize(&children) ||
        count > gxArraySize(&children) - first) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split child plan range is invalid");
    }
    const uint32_t maxKey = maxPhysicalIndexKeyBytes(_pages.pageSize());
    return prepareInternalPayloadBytes(
        _transaction->pageAllocator(), capacity(), maxKey, _indexId,
        candidatePageCount, count,
        [&children, first](size_t index, PlannedChildRef& child) -> bool {
            if (index >= gxArraySize(&children)) return false;
            child = loadPlannedChild(children, first + index);
            return true;
        },
        outPayload);
}
DbResult IndexTree::stageNonCascadingLeafSplit(
    uint64_t leafPageId, const DatabasePage& leafPage,
    const LeafInfo& leafInfo,
    const std::vector<uint64_t>& path,
    const GxArray& entries, size_t splitPoint, size_t entryCount,
    uint64_t& outNewLeafPageId, bool& outHandled) {
    outHandled = false;
    if (_transaction == nullptr || path.empty()) return DbResult::ok();
    if (static_cast<PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "index split transaction does not own its page view");
    }
    if (_transaction->pageCount() >= kMaxPageCount) {
        return DbResult::error(DbStatus::NoSpace,
                               "database reached the page count limit");
    }

    const uint64_t plannedNewLeafId = _transaction->pageCount();
    const uint64_t plannedPageCount = plannedNewLeafId + 1;
    const uint64_t parentId = path.back();
    if (!entries.initialized ||
        entries.elementSize != sizeof(PlannedLeafEntry) ||
        entryCount != gxArraySize(&entries) || splitPoint == 0 ||
        splitPoint >= entryCount) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split leaf plan range is invalid");
    }
    DatabasePage parentPage;
    InternalInfo parentInfo;
    DbResult result = readInternal(parentId, parentPage, parentInfo);
    if (!result.isOk()) return result;
    const uint32_t pageCapacity = capacity();
    const size_t maximumChildren = maxInternalChildCount(pageCapacity);
    if (parentInfo.childCount < 2 ||
        parentInfo.childCount > maximumChildren) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split parent child count is invalid");
    }

    ScopedIndexPlan children;
    GxStatus planStatus = initSplitPlanArray(
        children.records, _transaction->pageAllocator(),
        sizeof(PlannedChildRef),
        static_cast<size_t>(parentInfo.childCount) + 1);
    if (planStatus != GxStatus::Ok) {
        return splitPlanAllocationStatus(
            planStatus, "index parent split planning allocation failed");
    }
    size_t childCount = 0;
    result = readInternalChildPlan(parentPage, parentInfo, children.records,
                                   childCount);
    if (!result.isOk()) return result;
    if (childCount < 2) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split parent has too few children");
    }
    size_t position = childCount;
    for (size_t i = 0; i < childCount; ++i) {
        if (loadPlannedChild(children.records, i).pageId == leafPageId) {
            position = i;
            break;
        }
    }
    if (position == childCount) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index parent does not reference split child");
    }
    const PlannedLeafEntry rightFirst =
        loadPlannedLeafEntry(entries, splitPoint);
    ScopedIndexPayload separator;
    result = preparePhysicalLeafSeparator(
        _transaction->pageAllocator(), _pages.pageSize(), rightFirst,
        separator, "index split separator allocation failed");
    if (!result.isOk()) return result;
    const uint8_t* separatorBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&separator.bytes, 0));
    PlannedChildRef inserted = {};
    inserted.pageId = plannedNewLeafId;
    inserted.lowerBound = separatorBytes;
    inserted.lowerBoundSize = static_cast<uint32_t>(
        gxArraySize(&separator.bytes));
    planStatus = gxArrayInsert(&children.records, position + 1, &inserted, 1);
    if (planStatus != GxStatus::Ok) {
        return splitPlanAllocationStatus(
            planStatus, "index parent split planning allocation failed");
    }
    childCount = gxArraySize(&children.records);

    uint64_t internalUsedBytes = 0;
    if (!internalChildPlanFits(children.records, 0, childCount, pageCapacity,
                               maxPhysicalIndexKeyBytes(_pages.pageSize()),
                               internalUsedBytes)) {
        // Parent recursion remains on the existing path. Nothing has been
        // staged or reserved, so the legacy cascading split can proceed.
        return DbResult::ok();
    }

    ScopedIndexPayload leftPayload;
    ScopedIndexPayload rightPayload;
    ScopedIndexPayload parentPayload;
    result = prepareLeafPlanPayload(plannedNewLeafId, entries, 0, splitPoint,
                                    plannedPageCount, leftPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareLeafPlanPayload(leafInfo.nextLeaf, entries, splitPoint,
                                    entryCount - splitPoint, plannedPageCount,
                                    rightPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareInternalPlanPayload(children.records, 0, childCount,
                                        plannedPageCount,
                                        parentPayload.bytes);
    if (!result.isOk()) return result;

    const uint8_t* rightBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&rightPayload.bytes, 0));
    uint64_t allocatedLeafId = 0;
    result = _transaction->allocatePagePayload(
        PageType::IndexLeaf, 0, 0, rightBytes, capacity(), allocatedLeafId);
    if (!result.isOk()) return result;
    if (allocatedLeafId != plannedNewLeafId) {
        return DbResult::error(DbStatus::Internal,
                               "index split page reservation changed unexpectedly");
    }

    // These calls are individually checked. A later failure is restored as a
    // complete logical group by executeInsert's active statement savepoint.
    const uint8_t* leftBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&leftPayload.bytes, 0));
    result = _transaction->writePagePayload(
        leafPageId, PageType::IndexLeaf, leafPage.flags,
        leafPage.generation, leftBytes, capacity());
    if (!result.isOk()) return result;

    const uint8_t* parentBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&parentPayload.bytes, 0));
    result = _transaction->writePagePayload(
        parentId, PageType::IndexInternal, parentPage.flags,
        parentPage.generation, parentBytes, capacity());
    if (!result.isOk()) return result;

    outNewLeafPageId = allocatedLeafId;
    outHandled = true;
    return DbResult::ok();
}

DbResult IndexTree::stageInternalParentSplit(
    uint64_t rootPageId, uint64_t leafPageId, const DatabasePage& leafPage,
    const LeafInfo& leafInfo, const std::vector<uint64_t>& path,
    const GxArray& entries, size_t leafSplitPoint, size_t entryCount,
    uint64_t& outNewRootPageId, bool& outHandled) {
    outHandled = false;
    if (_transaction == nullptr ||
        (path.size() != 1 && path.size() != 2)) {
        return DbResult::ok();
    }
    if (static_cast<PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "index parent split transaction does not own its page view");
    }

    const bool splitRoot = path.size() == 1;
    const uint64_t parentPageId = path.back();
    if (path[0] != rootPageId ||
        (path.size() == 2 && path[0] == path[1])) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split path does not begin at its root");
    }
    if (!entries.initialized ||
        entries.elementSize != sizeof(PlannedLeafEntry) ||
        entryCount != gxArraySize(&entries) || leafSplitPoint == 0 ||
        leafSplitPoint >= entryCount) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split leaf plan range is invalid");
    }
    const uint64_t newPageCount = splitRoot ? 3 : 2;
    if (_transaction->pageCount() >= kMaxPageCount) {
        return DbResult::error(DbStatus::NoSpace,
                               "database reached the page count limit");
    }

    const uint64_t plannedNewLeafId = _transaction->pageCount();
    const uint64_t plannedRightInternalId = plannedNewLeafId + 1;
    const uint64_t plannedNewRootId = splitRoot
        ? plannedNewLeafId + 2 : 0;
    const uint64_t plannedPageCount =
        plannedNewLeafId + newPageCount;

    DatabasePage parentPage;
    InternalInfo parentInfo;
    DbResult result = readInternal(parentPageId, parentPage, parentInfo);
    if (!result.isOk()) return result;
    const uint32_t pageCapacity = capacity();
    const size_t parentChildLimit = maxInternalChildCount(pageCapacity);
    if (parentInfo.childCount < 2 ||
        parentInfo.childCount > parentChildLimit) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split parent child count is invalid");
    }
    const uint32_t maxPhysicalKey =
        maxPhysicalIndexKeyBytes(_pages.pageSize());

    ScopedIndexPlan allChildren;
    GxStatus planStatus = initSplitPlanArray(
        allChildren.records, _transaction->pageAllocator(),
        sizeof(PlannedChildRef), static_cast<size_t>(parentInfo.childCount) + 1);
    if (planStatus != GxStatus::Ok) {
        return splitPlanAllocationStatus(
            planStatus, "index parent split planning allocation failed");
    }
    size_t allChildCount = 0;
    result = readInternalChildPlan(parentPage, parentInfo,
                                   allChildren.records, allChildCount);
    if (!result.isOk()) return result;
    if (allChildCount < 2) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split parent has too few children");
    }

    size_t position = allChildCount;
    for (size_t i = 0; i < allChildCount; ++i) {
        if (loadPlannedChild(allChildren.records, i).pageId == leafPageId) {
            position = i;
            break;
        }
    }
    if (position == allChildCount) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index parent does not reference split child");
    }
    const PlannedLeafEntry rightFirst =
        loadPlannedLeafEntry(entries, leafSplitPoint);
    ScopedIndexPayload separator;
    result = preparePhysicalLeafSeparator(
        _transaction->pageAllocator(), _pages.pageSize(), rightFirst,
        separator, "index split separator allocation failed");
    if (!result.isOk()) return result;
    const uint8_t* separatorBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&separator.bytes, 0));

    PlannedChildRef newLeafChild = {};
    newLeafChild.pageId = plannedNewLeafId;
    newLeafChild.lowerBound = separatorBytes;
    newLeafChild.lowerBoundSize = static_cast<uint32_t>(
        gxArraySize(&separator.bytes));
    planStatus = gxArrayInsert(&allChildren.records, position + 1,
                               &newLeafChild, 1);
    if (planStatus != GxStatus::Ok) {
        return splitPlanAllocationStatus(
            planStatus, "index parent split planning allocation failed");
    }
    allChildCount = gxArraySize(&allChildren.records);

    uint64_t combinedUsed = 0;
    if (internalChildPlanFits(allChildren.records, 0, allChildCount,
                              pageCapacity, maxPhysicalKey, combinedUsed)) {
        // The R3L fitting-parent case consumes the same portable leaf plan and
        // child plan as the R3M/R3N parent split cases.
        const uint64_t directPageCount = plannedNewLeafId + 1;
        ScopedIndexPayload leftLeafPayload;
        ScopedIndexPayload rightLeafPayload;
        ScopedIndexPayload parentPayload;
        result = prepareLeafPlanPayload(plannedNewLeafId, entries, 0,
                                        leafSplitPoint, directPageCount,
                                        leftLeafPayload.bytes);
        if (!result.isOk()) return result;
        result = prepareLeafPlanPayload(leafInfo.nextLeaf, entries,
                                        leafSplitPoint,
                                        entryCount - leafSplitPoint,
                                        directPageCount,
                                        rightLeafPayload.bytes);
        if (!result.isOk()) return result;
        result = prepareInternalPlanPayload(
            allChildren.records, 0, allChildCount, directPageCount,
            parentPayload.bytes);
        if (!result.isOk()) return result;

        uint64_t allocatedLeafId = 0;
        const uint8_t* rightBytes = static_cast<const uint8_t*>(
            gxArrayAtConst(&rightLeafPayload.bytes, 0));
        result = _transaction->allocatePagePayload(
            PageType::IndexLeaf, 0, 0, rightBytes, capacity(), allocatedLeafId);
        if (!result.isOk()) return result;
        if (allocatedLeafId != plannedNewLeafId) {
            return DbResult::error(DbStatus::Internal,
                                   "index split page reservation changed unexpectedly");
        }
        const uint8_t* leftBytes = static_cast<const uint8_t*>(
            gxArrayAtConst(&leftLeafPayload.bytes, 0));
        result = _transaction->writePagePayload(
            leafPageId, PageType::IndexLeaf, leafPage.flags,
            leafPage.generation, leftBytes, capacity());
        if (!result.isOk()) return result;
        const uint8_t* parentBytes = static_cast<const uint8_t*>(
            gxArrayAtConst(&parentPayload.bytes, 0));
        result = _transaction->writePagePayload(
            parentPageId, PageType::IndexInternal, parentPage.flags,
            parentPage.generation, parentBytes, capacity());
        if (!result.isOk()) return result;

        outNewRootPageId = rootPageId;
        outHandled = true;
        return DbResult::ok();
    }

    if (_transaction->pageCount() > kMaxPageCount - newPageCount) {
        return DbResult::error(DbStatus::NoSpace,
                               "database reached the page count limit");
    }

    size_t splitPoint = 0;
    bool foundSplit = false;
    for (size_t candidate = 2; candidate + 1 < allChildCount; ++candidate) {
        uint64_t leftUsed = 0;
        uint64_t rightUsed = 0;
        if (internalChildPlanFits(allChildren.records, 0, candidate,
                                  pageCapacity, maxPhysicalKey, leftUsed) &&
            internalChildPlanFits(allChildren.records, candidate,
                                  allChildCount - candidate, pageCapacity,
                                  maxPhysicalKey, rightUsed)) {
            splitPoint = candidate;
            foundSplit = true;
            break;
        }
    }
    if (!foundSplit) {
        return DbResult::error(DbStatus::NoSpace,
                               "index internal parent cannot be split");
    }

    const size_t rightChildCount = allChildCount - splitPoint;
    const PlannedChildRef promotedChild =
        loadPlannedChild(allChildren.records, splitPoint);
    if (rightChildCount == 0 || promotedChild.lowerBoundSize == 0 ||
        promotedChild.lowerBound == nullptr) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index promoted parent separator is empty");
    }

    DatabasePage grandparentPage;
    InternalInfo grandparentInfo;
    ScopedIndexPlan grandparentChildren;
    size_t grandparentChildCount = 0;
    size_t grandparentPosition = 0;
    if (splitRoot) {
        planStatus = initSplitPlanArray(
            grandparentChildren.records, _transaction->pageAllocator(),
            sizeof(PlannedChildRef), 2);
        if (planStatus == GxStatus::Ok) {
            planStatus = gxArrayResize(&grandparentChildren.records, 2);
        }
        if (planStatus != GxStatus::Ok) {
            return splitPlanAllocationStatus(
                planStatus, "index root split planning allocation failed");
        }
        PlannedChildRef rootLeft = {};
        rootLeft.pageId = parentPageId;
        PlannedChildRef rootRight = {};
        rootRight.pageId = plannedRightInternalId;
        rootRight.lowerBound = promotedChild.lowerBound;
        rootRight.lowerBoundSize = promotedChild.lowerBoundSize;
        storePlannedChild(grandparentChildren.records, 0, rootLeft);
        storePlannedChild(grandparentChildren.records, 1, rootRight);
        grandparentChildCount = 2;
    } else {
        const uint64_t grandparentPageId = path[path.size() - 2];
        result = readInternal(grandparentPageId, grandparentPage,
                              grandparentInfo);
        if (!result.isOk()) return result;
        const size_t grandparentChildLimit = maxInternalChildCount(pageCapacity);
        if (grandparentInfo.childCount < 2 ||
            grandparentInfo.childCount > grandparentChildLimit) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index split grandparent child count is invalid");
        }
        planStatus = initSplitPlanArray(
            grandparentChildren.records, _transaction->pageAllocator(),
            sizeof(PlannedChildRef),
            static_cast<size_t>(grandparentInfo.childCount) + 1);
        if (planStatus != GxStatus::Ok) {
            return splitPlanAllocationStatus(
                planStatus, "index grandparent split planning allocation failed");
        }
        result = readInternalChildPlan(grandparentPage, grandparentInfo,
                                       grandparentChildren.records,
                                       grandparentChildCount);
        if (!result.isOk()) return result;
        grandparentPosition = grandparentChildCount;
        for (size_t i = 0; i < grandparentChildCount; ++i) {
            if (loadPlannedChild(grandparentChildren.records, i).pageId ==
                parentPageId) {
                grandparentPosition = i;
                break;
            }
        }
        if (grandparentPosition == grandparentChildCount) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index grandparent does not reference split parent");
        }
        PlannedChildRef rightParent = {};
        rightParent.pageId = plannedRightInternalId;
        rightParent.lowerBound = promotedChild.lowerBound;
        rightParent.lowerBoundSize = promotedChild.lowerBoundSize;
        planStatus = gxArrayInsert(&grandparentChildren.records,
                                   grandparentPosition + 1,
                                   &rightParent, 1);
        if (planStatus != GxStatus::Ok) {
            return splitPlanAllocationStatus(
                planStatus, "index grandparent split planning allocation failed");
        }
        grandparentChildCount = gxArraySize(&grandparentChildren.records);
        uint64_t grandparentUsed = 0;
        if (!internalChildPlanFits(grandparentChildren.records, 0,
                                   grandparentChildCount, pageCapacity,
                                   maxPhysicalKey, grandparentUsed)) {
            // A second internal split is outside this bounded conversion.
            // Leave the transaction untouched for the existing fallback.
            return DbResult::ok();
        }
    }

    ScopedIndexPayload leftLeafPayload;
    ScopedIndexPayload rightLeafPayload;
    ScopedIndexPayload leftInternalPayload;
    ScopedIndexPayload rightInternalPayload;
    ScopedIndexPayload grandparentPayload;
    result = prepareLeafPlanPayload(plannedNewLeafId, entries, 0, leafSplitPoint,
                                    plannedPageCount,
                                    leftLeafPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareLeafPlanPayload(leafInfo.nextLeaf, entries, leafSplitPoint,
                                    entryCount - leafSplitPoint,
                                    plannedPageCount,
                                    rightLeafPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareInternalPlanPayload(allChildren.records, 0, splitPoint,
                                        plannedPageCount,
                                        leftInternalPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareInternalPlanPayload(allChildren.records, splitPoint,
                                        rightChildCount, plannedPageCount,
                                        rightInternalPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareInternalPlanPayload(grandparentChildren.records, 0,
                                        grandparentChildCount,
                                        plannedPageCount,
                                        grandparentPayload.bytes);
    if (!result.isOk()) return result;

    // All final images are complete and validated before the first transaction
    // page is staged. The active INSERT savepoint owns recovery from any later
    // P7/P8 failure in this multi-page group.
    uint64_t allocatedPageId = 0;
    const uint8_t* rightLeafBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&rightLeafPayload.bytes, 0));
    result = _transaction->allocatePagePayload(
        PageType::IndexLeaf, 0, 0, rightLeafBytes, capacity(), allocatedPageId);
    if (!result.isOk()) return result;
    if (allocatedPageId != plannedNewLeafId) {
        return DbResult::error(DbStatus::Internal,
                               "index split leaf reservation changed unexpectedly");
    }

    const uint8_t* rightInternalBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&rightInternalPayload.bytes, 0));
    result = _transaction->allocatePagePayload(
        PageType::IndexInternal, 0, 0, rightInternalBytes, capacity(),
        allocatedPageId);
    if (!result.isOk()) return result;
    if (allocatedPageId != plannedRightInternalId) {
        return DbResult::error(DbStatus::Internal,
                               "index split internal reservation changed unexpectedly");
    }

    if (splitRoot) {
        const uint8_t* newRootBytes = static_cast<const uint8_t*>(
            gxArrayAtConst(&grandparentPayload.bytes, 0));
        result = _transaction->allocatePagePayload(
            PageType::IndexInternal, 0, 0, newRootBytes, capacity(),
            allocatedPageId);
        if (!result.isOk()) return result;
        if (allocatedPageId != plannedNewRootId) {
            return DbResult::error(DbStatus::Internal,
                                   "index new root reservation changed unexpectedly");
        }
    }

    const uint8_t* leftLeafBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&leftLeafPayload.bytes, 0));
    result = _transaction->writePagePayload(
        leafPageId, PageType::IndexLeaf, leafPage.flags,
        leafPage.generation, leftLeafBytes, capacity());
    if (!result.isOk()) return result;

    const uint8_t* leftInternalBytes = static_cast<const uint8_t*>(
        gxArrayAtConst(&leftInternalPayload.bytes, 0));
    result = _transaction->writePagePayload(
        parentPageId, PageType::IndexInternal, parentPage.flags,
        parentPage.generation, leftInternalBytes, capacity());
    if (!result.isOk()) return result;

    if (!splitRoot) {
        const uint8_t* grandparentBytes = static_cast<const uint8_t*>(
            gxArrayAtConst(&grandparentPayload.bytes, 0));
        result = _transaction->writePagePayload(
            path[path.size() - 2], PageType::IndexInternal,
            grandparentPage.flags, grandparentPage.generation,
            grandparentBytes, capacity());
        if (!result.isOk()) return result;
    } else {
        outNewRootPageId = plannedNewRootId;
    }
    outHandled = true;
    return DbResult::ok();
}

DbResult IndexTree::allocateInternal(uint64_t& outPageId) {
    uint64_t pageId = 0;
    DbResult result = _pages.allocatePage(PageType::IndexInternal, pageId);
    if (!result.isOk()) {
        return result;
    }
    std::vector<Child> children;
    result = writeInternal(pageId, children);
    if (!result.isOk()) {
        return result;
    }
    outPageId = pageId;
    return DbResult::ok();
}

size_t IndexTree::chooseChild(const std::vector<Child>& children,
                              const std::vector<uint8_t>& physicalKey) {
    size_t best = 0;
    for (size_t i = 1; i < children.size(); ++i) {
        if (children[i].lowerBound.empty()) {
            best = i;
            continue;
        }
        const int cmp = compareEncodedKeys(children[i].lowerBound.data(),
                                           children[i].lowerBound.size(),
                                           physicalKey.data(), physicalKey.size());
        if (cmp <= 0) {
            best = i;
        } else {
            break;
        }
    }
    return best;
}

size_t IndexTree::chooseChildForInsert(
    const std::vector<Child>& children,
    const std::vector<uint8_t>& logicalKey, const RowLocator& locator) {
    size_t best = 0;
    for (size_t i = 1; i < children.size(); ++i) {
        if (children[i].lowerBound.empty()) {
            best = i;
            continue;
        }
        if (compareStoredBoundToInsertKey(children[i].lowerBound, logicalKey,
                                          locator) <= 0) {
            best = i;
        } else {
            break;
        }
    }
    return best;
}

DbResult IndexTree::findLeaf(uint64_t rootPageId, const std::vector<uint8_t>& physicalKey,
                             uint64_t& outLeafPageId, std::vector<uint64_t>& outPath) const {
    outPath.clear();
    uint64_t current = rootPageId;
    const uint64_t pageCount = _pages.pageCount();
    for (uint32_t depth = 0; depth <= kMaxIndexDepth; ++depth) {
        if (current == 0 || current >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "index root page out of range");
        }
        DatabasePage page;
        DbResult result = _pages.readPage(current, page);
        if (!result.isOk()) {
            return result;
        }
        if (page.type == PageType::IndexLeaf) {
            LeafInfo info;
            result = readLeaf(current, page, info);
            if (!result.isOk()) {
                return result;
            }
            outLeafPageId = current;
            return DbResult::ok();
        }
        if (page.type != PageType::IndexInternal) {
            return DbResult::error(DbStatus::CorruptPage, "index page has unknown type");
        }
        InternalInfo info;
        result = readInternal(current, page, info);
        if (!result.isOk()) {
            return result;
        }
        if (info.childCount == 0) {
            return DbResult::error(DbStatus::CorruptPage, "index internal node has no children");
        }
        std::vector<Child> children;
        result = readInternalChildren(page, info, children);
        if (!result.isOk()) {
            return result;
        }
        outPath.push_back(current);
        current = children[chooseChild(children, physicalKey)].pageId;
    }
    return DbResult::error(DbStatus::CorruptPage, "index tree exceeds maximum depth");
}

DbResult IndexTree::findLeafForInsert(
    uint64_t rootPageId, const std::vector<uint8_t>& logicalKey,
    const RowLocator& locator, uint64_t& outLeafPageId,
    std::vector<uint64_t>& outPath) const {
    outPath.clear();
    uint64_t current = rootPageId;
    const uint64_t pageCount = _pages.pageCount();
    for (uint32_t depth = 0; depth <= kMaxIndexDepth; ++depth) {
        if (current == 0 || current >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index root page out of range");
        }
        DatabasePage page;
        DbResult result = _pages.readPage(current, page);
        if (!result.isOk()) return result;
        if (page.type == PageType::IndexLeaf) {
            LeafInfo info;
            result = readLeaf(current, page, info);
            if (!result.isOk()) return result;
            outLeafPageId = current;
            return DbResult::ok();
        }
        if (page.type != PageType::IndexInternal) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index page has unknown type");
        }
        InternalInfo info;
        result = readInternal(current, page, info);
        if (!result.isOk()) return result;
        if (info.childCount == 0) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal node has no children");
        }
        std::vector<Child> children;
        result = readInternalChildren(page, info, children);
        if (!result.isOk()) return result;
        outPath.push_back(current);
        current = children[chooseChildForInsert(children, logicalKey,
                                                locator)].pageId;
    }
    return DbResult::error(DbStatus::CorruptPage,
                           "index tree exceeds maximum depth");
}

DbResult IndexTree::createEmpty(uint64_t& outRootPageId) {
    return allocateLeaf(0, outRootPageId);
}

DbResult IndexTree::lookupAll(const std::vector<uint8_t>& logicalKey, uint64_t rootPageId,
                              std::vector<IndexEntry>& out) const {
    out.clear();
    std::vector<uint8_t> probe = logicalKey;
    RowLocator minLocator(0, 0);
    appendLocator(probe, minLocator);

    uint64_t leaf = 0;
    std::vector<uint64_t> path;
    DbResult result = findLeaf(rootPageId, probe, leaf, path);
    if (!result.isOk()) {
        return result;
    }

    const uint64_t pageCount = _pages.pageCount();
    ::GxPageVisitSet visited;
    result = initializePageVisitTracker(visited, _traversalAllocator,
                                        pageCount);
    if (!result.isOk()) return result;
    while (leaf != 0) {
        if (leaf >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf chain out of range");
        }
        bool revisited = false;
        const GxStatus visitStatus = visited.visit(leaf, revisited);
        if (visitStatus != GxStatus::Ok) {
            return pageVisitTrackerError(visitStatus);
        }
        if (revisited) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf chain cycle");
        }
        DatabasePage page;
        result = _pages.readPage(leaf, page);
        if (!result.isOk()) {
            return result;
        }
        LeafInfo info;
        result = readLeaf(leaf, page, info);
        if (!result.isOk()) {
            return result;
        }
        std::vector<IndexEntry> entries;
        result = readLeafEntries(page, info, entries);
        if (!result.isOk()) {
            return result;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            const int cmp = compareEncodedKeys(entries[i].key.data(), entries[i].key.size(),
                                               logicalKey.data(), logicalKey.size());
            if (cmp == 0) {
                out.push_back(entries[i]);
            } else if (cmp > 0) {
                return DbResult::ok();
            }
        }
        leaf = info.nextLeaf;
    }
    return DbResult::ok();
}

DbResult IndexTree::lookupFirst(const std::vector<uint8_t>& logicalKey, uint64_t rootPageId,
                                bool& found, IndexEntry& out) const {
    found = false;
    std::vector<IndexEntry> entries;
    DbResult result = lookupAll(logicalKey, rootPageId, entries);
    if (!result.isOk()) {
        return result;
    }
    if (!entries.empty()) {
        found = true;
        out = entries[0];
    }
    return DbResult::ok();
}

DbResult IndexTree::splitRootLeaf(uint64_t oldLeaf, uint64_t newLeaf,
                                  const std::vector<uint8_t>& separator,
                                  uint64_t& outRootPageId) {
    uint64_t rootId = 0;
    DbResult result = allocateInternal(rootId);
    if (!result.isOk()) {
        return result;
    }
    std::vector<Child> children;
    Child left;
    left.pageId = oldLeaf;
    Child right;
    right.pageId = newLeaf;
    right.lowerBound = separator;
    children.push_back(left);
    children.push_back(right);
    result = writeInternal(rootId, children);
    if (!result.isOk()) {
        return result;
    }
    outRootPageId = rootId;
    return DbResult::ok();
}

DbResult IndexTree::insert(const std::vector<uint8_t>& logicalKey,
                           const RowLocator& locator, uint64_t rootPageId,
                           bool unique, uint64_t& outRootPageId,
                           bool& outInserted) {
    if (_transaction == nullptr) {
        return insertCompatibility(logicalKey, locator, rootPageId, unique,
                                   outRootPageId, outInserted);
    }
    outRootPageId = rootPageId;
    outInserted = false;
    if (static_cast<PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "index insert transaction does not own its page view");
    }
    const uint32_t maxKey = maxIndexKeyBytes(_pages.pageSize());
    if (logicalKey.size() > maxKey || logicalKey.size() > UINT32_MAX) {
        return DbResult::error(DbStatus::NoSpace,
                               "index leaf entries do not fit");
    }

    // Inspect the validated leaf without allocating an entry collection. Most
    // inserts fit in place and keep using the compatibility mutation path.
    // A split enters the portable descriptor planner before any hosted entry
    // vectors or redistribution copies are created.
    uint64_t leaf = 0;
    std::vector<uint64_t> path;
    DbResult result = findLeafForInsert(rootPageId, logicalKey, locator,
                                        leaf, path);
    if (!result.isOk()) return result;
    DatabasePage leafPage;
    LeafInfo leafInfo;
    result = readLeaf(leaf, leafPage, leafInfo);
    if (!result.isOk()) return result;

    const uint32_t cap = capacity();
    uint64_t used = kIndexLeafHeaderSize +
        static_cast<uint64_t>(leafInfo.entryCount) * kIndexSlotSize;
    bool exactDuplicate = false;
    bool uniqueConflict = false;
    GxIndexLeafCursor cursor = {};
    GxIndexLeafError leafError = GxIndexLeafError::None;
    GxStatus cursorStatus = gxIndexLeafCursorInit(
        leafPage.payload.data(), leafPage.payload.size(), &cursor, &leafError);
    if (cursorStatus != GxStatus::Ok ||
        cursor.page.entryCount != leafInfo.entryCount ||
        cursor.page.nextLeafPageId != leafInfo.nextLeaf ||
        cursor.page.ownerIndexId != leafInfo.ownerIndexId) {
        return cursorStatus == GxStatus::Ok
            ? DbResult::error(DbStatus::CorruptPage,
                              "index leaf metadata changed during read")
            : leafEntryDataError(leafError);
    }
    for (;;) {
        GxIndexLeafEntryView leafEntry = {};
        bool hasEntry = false;
        cursorStatus = gxIndexLeafCursorReadNext(&cursor, &leafEntry,
                                                &hasEntry, &leafError);
        if (cursorStatus != GxStatus::Ok) {
            return leafEntryDataError(leafError);
        }
        if (!hasEntry) break;
        const uint64_t entryBytes =
            static_cast<uint64_t>(kIndexLeafEntryOverhead) +
            leafEntry.keyLength;
        if (used > cap || entryBytes > cap - used) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index leaf entries exceed page capacity");
        }
        used += entryBytes;
        if (leafEntry.locatorPageId == locator.pageId &&
            leafEntry.locatorSlot == locator.slot &&
            compareEncodedKeys(leafEntry.keyBytes, leafEntry.keyLength,
                               logicalKey.data(),
                               logicalKey.size()) == 0) {
            exactDuplicate = true;
        }
        if (unique && !encodedKeyIsNull(logicalKey.data(), logicalKey.size()) &&
            compareEncodedKeys(leafEntry.keyBytes, leafEntry.keyLength,
                               logicalKey.data(),
                               logicalKey.size()) == 0 &&
            (leafEntry.locatorPageId != locator.pageId ||
             leafEntry.locatorSlot != locator.slot)) {
            uniqueConflict = true;
        }
    }
    if (exactDuplicate) return DbResult::ok();
    if (uniqueConflict) {
        return DbResult::error(DbStatus::AlreadyExists,
                               "unique index constraint violation");
    }

    const uint64_t addedBytes =
        static_cast<uint64_t>(kIndexLeafEntryOverhead) + logicalKey.size() +
        kIndexSlotSize;
    const bool slotFits =
        leafInfo.entryCount < (cap - kIndexLeafHeaderSize) / kIndexSlotSize;
    const bool needsSplit = !slotFits || used > cap || addedBytes > cap - used;
    if (!needsSplit) {
        return insertCompatibility(logicalKey, locator, rootPageId, unique,
                                   outRootPageId, outInserted);
    }
    return insertTransactionPlan(logicalKey, locator, rootPageId, unique,
                                 outRootPageId, outInserted);
}

DbResult IndexTree::insertTransactionPlan(
    const std::vector<uint8_t>& logicalKey, const RowLocator& locator,
    uint64_t rootPageId, bool unique, uint64_t& outRootPageId,
    bool& outInserted) {
    outRootPageId = rootPageId;
    outInserted = false;
    if (_transaction == nullptr ||
        static_cast<PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "portable index leaf planning requires its transaction");
    }

    uint64_t leaf = 0;
    std::vector<uint64_t> path;
    DbResult result = findLeafForInsert(rootPageId, logicalKey, locator,
                                        leaf, path);
    if (!result.isOk()) return result;
    DatabasePage leafPage;
    LeafInfo leafInfo;
    result = readLeaf(leaf, leafPage, leafInfo);
    if (!result.isOk()) return result;

    size_t maximumLeafEntries = 0;
    if (!maximumLeafPlanEntryCount(capacity(), maximumLeafEntries) ||
        maximumLeafEntries == static_cast<size_t>(-1)) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf planning bound is invalid");
    }
    ScopedIndexPlan leafEntries;
    GxStatus planStatus = gxArrayInit(
        &leafEntries.records, &_transaction->pageAllocator(),
        sizeof(PlannedLeafEntry), maximumLeafEntries + 1);
    if (planStatus != GxStatus::Ok) {
        return splitPlanAllocationStatus(
            planStatus, "index leaf planning allocation failed");
    }
    size_t entryCount = 0;
    result = readLeafPlan(leafPage, leafInfo, leafEntries.records, entryCount);
    if (!result.isOk()) return result;

    for (size_t i = 0; i < entryCount; ++i) {
        const PlannedLeafEntry existing =
            loadPlannedLeafEntry(leafEntries.records, i);
        if (existing.locatorPageId == locator.pageId &&
            existing.locatorSlot == locator.slot &&
            compareEncodedKeys(existing.key, existing.keyLength,
                               logicalKey.data(), logicalKey.size()) == 0) {
            return DbResult::ok();
        }
        if (unique && !encodedKeyIsNull(logicalKey.data(), logicalKey.size()) &&
            compareEncodedKeys(existing.key, existing.keyLength,
                               logicalKey.data(), logicalKey.size()) == 0) {
            return DbResult::error(DbStatus::AlreadyExists,
                                   "unique index constraint violation");
        }
    }
    if (logicalKey.size() == 0 || logicalKey.size() > UINT32_MAX) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index inserted key length is invalid");
    }
    PlannedLeafEntry inserted = {};
    inserted.key = logicalKey.data();
    inserted.locatorPageId = locator.pageId;
    inserted.keyLength = static_cast<uint32_t>(logicalKey.size());
    inserted.locatorSlot = locator.slot;
    result = appendLeafPlanEntry(leafEntries.records, inserted);
    if (!result.isOk()) return result;
    entryCount = gxArraySize(&leafEntries.records);
    sortPlannedLeafEntries(leafEntries.records);

    uint64_t fullUsed = 0;
    if (leafPlanRangeFits(leafEntries.records, 0, entryCount, capacity(),
                          maxIndexKeyBytes(_pages.pageSize()), fullUsed)) {
        result = stagePreparedLeafPlan(
            leaf, leafInfo.nextLeaf, leafPage.flags, leafPage.generation,
            leafEntries.records, entryCount);
        if (!result.isOk()) return result;
        outInserted = true;
        return DbResult::ok();
    }

    size_t splitPoint = 0;
    bool foundSplit = false;
    for (size_t candidate = 1; candidate < entryCount; ++candidate) {
        uint64_t leftUsed = 0;
        uint64_t rightUsed = 0;
        if (leafPlanRangeFits(leafEntries.records, 0, candidate, capacity(),
                              maxIndexKeyBytes(_pages.pageSize()), leftUsed) &&
            leafPlanRangeFits(leafEntries.records, candidate,
                              entryCount - candidate, capacity(),
                              maxIndexKeyBytes(_pages.pageSize()), rightUsed)) {
            splitPoint = candidate;
            foundSplit = true;
            break;
        }
    }
    if (!foundSplit) {
        return DbResult::error(DbStatus::NoSpace,
                               "index leaf cannot be split");
    }

    if (path.size() > 2) {
        uint64_t checkedNewLeaf = 0;
        bool checkedSplit = false;
        result = stageNonCascadingLeafSplit(
            leaf, leafPage, leafInfo, path, leafEntries.records, splitPoint,
            entryCount, checkedNewLeaf, checkedSplit);
        if (!result.isOk()) return result;
        if (checkedSplit) {
            outInserted = true;
            return DbResult::ok();
        }
    }
    if (path.size() == 1 || path.size() == 2) {
        uint64_t checkedNewRoot = 0;
        bool checkedParentSplit = false;
        result = stageInternalParentSplit(
            rootPageId, leaf, leafPage, leafInfo, path,
            leafEntries.records, splitPoint, entryCount,
            checkedNewRoot, checkedParentSplit);
        if (!result.isOk()) return result;
        if (checkedParentSplit) {
            if (path.size() == 1) outRootPageId = checkedNewRoot;
            outInserted = true;
            return DbResult::ok();
        }
    }

    // Deep cascading and height-one root fallback remains on its existing
    // hosted compatibility path. The selected R3L/R3M/R3N split cases return
    // above after consuming this descriptor plan directly.
    std::vector<IndexEntry> left;
    std::vector<IndexEntry> right;
    left.reserve(splitPoint);
    right.reserve(entryCount - splitPoint);
    for (size_t i = 0; i < entryCount; ++i) {
        const PlannedLeafEntry planned =
            loadPlannedLeafEntry(leafEntries.records, i);
        IndexEntry entry;
        if (planned.keyLength != 0) {
            entry.key.assign(planned.key, planned.key + planned.keyLength);
        }
        entry.locator.pageId = planned.locatorPageId;
        entry.locator.slot = planned.locatorSlot;
        (i < splitPoint ? left : right).push_back(entry);
    }

    uint64_t newLeaf = 0;
    result = allocateLeaf(leafInfo.nextLeaf, newLeaf);
    if (!result.isOk()) return result;
    result = writeLeaf(leaf, newLeaf, left);
    if (!result.isOk()) return result;
    result = writeLeaf(newLeaf, leafInfo.nextLeaf, right);
    if (!result.isOk()) return result;
    const std::vector<uint8_t> separator =
        makePhysicalKey(right[0].key, right[0].locator);
    if (path.empty()) {
        result = splitRootLeaf(leaf, newLeaf, separator, outRootPageId);
        if (!result.isOk()) return result;
        outInserted = true;
        return DbResult::ok();
    }

    uint64_t childToInsert = newLeaf;
    uint64_t splitChild = leaf;
    std::vector<uint8_t> separatorKey = separator;
    for (size_t level = path.size(); level-- > 0;) {
        const uint64_t parentId = path[level];
        DatabasePage parentPage;
        InternalInfo parentInfo;
        result = readInternal(parentId, parentPage, parentInfo);
        if (!result.isOk()) return result;
        std::vector<Child> children;
        result = readInternalChildren(parentPage, parentInfo, children);
        if (!result.isOk()) return result;
        size_t position = children.size();
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i].pageId == splitChild) {
                position = i;
                break;
            }
        }
        if (position == children.size()) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index parent does not reference split child");
        }
        Child insertedChild;
        insertedChild.pageId = childToInsert;
        insertedChild.lowerBound = separatorKey;
        children.insert(children.begin() + static_cast<long>(position + 1),
                        insertedChild);
        result = writeInternal(parentId, children);
        if (result.isOk()) {
            outInserted = true;
            return DbResult::ok();
        }
        if (result.status() != DbStatus::NoSpace ||
            internalUsed(children) <= capacity()) {
            return result;
        }
        size_t internalSplit = 0;
        bool foundInternalSplit = false;
        for (size_t candidate = 2; candidate + 1 < children.size(); ++candidate) {
            std::vector<Child> leftTry(
                children.begin(), children.begin() + static_cast<long>(candidate));
            std::vector<Child> rightTry(
                children.begin() + static_cast<long>(candidate), children.end());
            if (internalUsed(leftTry) <= capacity() &&
                internalUsed(rightTry) <= capacity()) {
                internalSplit = candidate;
                foundInternalSplit = true;
                break;
            }
        }
        if (!foundInternalSplit) {
            return DbResult::error(DbStatus::NoSpace,
                                   "index internal node cannot be split");
        }
        std::vector<Child> leftChildren(
            children.begin(), children.begin() + static_cast<long>(internalSplit));
        std::vector<Child> rightChildren(
            children.begin() + static_cast<long>(internalSplit), children.end());
        uint64_t newInternal = 0;
        result = allocateInternal(newInternal);
        if (!result.isOk()) return result;
        result = writeInternal(parentId, leftChildren);
        if (!result.isOk()) return result;
        result = writeInternal(newInternal, rightChildren);
        if (!result.isOk()) return result;
        childToInsert = newInternal;
        splitChild = parentId;
        separatorKey = rightChildren[0].lowerBound;
    }
    result = splitRootLeaf(splitChild, childToInsert, separatorKey,
                           outRootPageId);
    if (!result.isOk()) return result;
    outInserted = true;
    return DbResult::ok();
}

DbResult IndexTree::insertCompatibility(
    const std::vector<uint8_t>& logicalKey, const RowLocator& locator,
    uint64_t rootPageId, bool unique, uint64_t& outRootPageId,
    bool& outInserted) {
    outRootPageId = rootPageId;
    outInserted = false;

    std::vector<uint8_t> physicalKey = makePhysicalKey(logicalKey, locator);
    uint64_t leaf = 0;
    std::vector<uint64_t> path;
    DbResult result = findLeaf(rootPageId, physicalKey, leaf, path);
    if (!result.isOk()) {
        return result;
    }
    DatabasePage leafPage;
    LeafInfo leafInfo;
    result = readLeaf(leaf, leafPage, leafInfo);
    if (!result.isOk()) {
        return result;
    }
    std::vector<IndexEntry> entries;
    result = readLeafEntries(leafPage, leafInfo, entries);
    if (!result.isOk()) {
        return result;
    }

    // Exact physical duplicate: idempotent no-op.
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].locator.pageId == locator.pageId &&
            entries[i].locator.slot == locator.slot &&
            compareEncodedKeys(entries[i].key.data(), entries[i].key.size(),
                               logicalKey.data(), logicalKey.size()) == 0) {
            return DbResult::ok();
        }
    }
    // Uniqueness: any other row with the same non-NULL logical key conflicts.
    if (unique && !encodedKeyIsNull(logicalKey.data(), logicalKey.size())) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (compareEncodedKeys(entries[i].key.data(), entries[i].key.size(),
                                   logicalKey.data(), logicalKey.size()) == 0) {
                return DbResult::error(DbStatus::AlreadyExists,
                                       "unique index constraint violation");
            }
        }
    }

    IndexEntry entry;
    entry.key = logicalKey;
    entry.locator = locator;
    entries.push_back(entry);
    std::sort(entries.begin(), entries.end(),
              [](const IndexEntry& a, const IndexEntry& b) {
                  const std::vector<uint8_t> pa =
                      IndexTree::makePhysicalKey(a.key, a.locator);
                  const std::vector<uint8_t> pb =
                      IndexTree::makePhysicalKey(b.key, b.locator);
                  return compareEncodedKeys(pa.data(), pa.size(), pb.data(), pb.size()) < 0;
              });

    if (leafUsed(entries) <= capacity()) {
        result = _transaction != nullptr
            ? stagePreparedLeaf(leaf, leafInfo.nextLeaf, leafPage.flags,
                                leafPage.generation, entries)
            : writeLeaf(leaf, leafInfo.nextLeaf, entries);
        if (!result.isOk()) return result;
        outInserted = true;
        return DbResult::ok();
    }

    result = writeLeaf(leaf, leafInfo.nextLeaf, entries);
    if (result.isOk()) {
        outInserted = true;
        return DbResult::ok();
    }
    if (result.status() != DbStatus::NoSpace ||
        leafUsed(entries) <= capacity()) {
        return result;
    }

    // Split the leaf: find a split point where both halves fit.
    size_t mid = 0;
    bool found = false;
    for (size_t candidate = 1; candidate < entries.size(); ++candidate) {
        std::vector<IndexEntry> left(entries.begin(), entries.begin() + candidate);
        std::vector<IndexEntry> right(entries.begin() + candidate, entries.end());
        if (leafUsed(left) <= capacity() && leafUsed(right) <= capacity()) {
            mid = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return DbResult::error(DbStatus::NoSpace, "index leaf cannot be split");
    }
    std::vector<IndexEntry> left(entries.begin(), entries.begin() + mid);
    std::vector<IndexEntry> right(entries.begin() + mid, entries.end());

    uint64_t newLeaf = 0;
    result = allocateLeaf(leafInfo.nextLeaf, newLeaf);
    if (!result.isOk()) {
        return result;
    }
    result = writeLeaf(leaf, newLeaf, left);
    if (!result.isOk()) {
        return result;
    }
    result = writeLeaf(newLeaf, leafInfo.nextLeaf, right);
    if (!result.isOk()) {
        return result;
    }
    const std::vector<uint8_t> separator =
        makePhysicalKey(right[0].key, right[0].locator);

    if (path.empty()) {
        result = splitRootLeaf(leaf, newLeaf, separator, outRootPageId);
        if (!result.isOk()) {
            return result;
        }
        outInserted = true;
        return DbResult::ok();
    }

    uint64_t childToInsert = newLeaf;
    uint64_t splitChild = leaf;
    std::vector<uint8_t> separatorKey = separator;
    for (size_t level = path.size(); level-- > 0;) {
        const uint64_t parentId = path[level];
        DatabasePage parentPage;
        InternalInfo parentInfo;
        result = readInternal(parentId, parentPage, parentInfo);
        if (!result.isOk()) {
            return result;
        }
        std::vector<Child> children;
        result = readInternalChildren(parentPage, parentInfo, children);
        if (!result.isOk()) {
            return result;
        }
        size_t position = children.size();
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i].pageId == splitChild) {
                position = i;
                break;
            }
        }
        if (position == children.size()) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index parent does not reference split child");
        }
        Child inserted;
        inserted.pageId = childToInsert;
        inserted.lowerBound = separatorKey;
        children.insert(children.begin() + static_cast<long>(position + 1), inserted);

        result = writeInternal(parentId, children);
        if (result.isOk()) {
            outInserted = true;
            return DbResult::ok();
        }
        if (result.status() != DbStatus::NoSpace ||
            internalUsed(children) <= capacity()) {
            return result;
        }

        // Split the internal node. Both halves must keep at least two children
        // so no internal node ever degenerates to a single child.
        size_t midInternal = 0;
        bool internalFound = false;
        for (size_t candidate = 2; candidate + 1 < children.size(); ++candidate) {
            std::vector<Child> leftTry(children.begin(),
                                       children.begin() + static_cast<long>(candidate));
            std::vector<Child> rightTry(children.begin() + static_cast<long>(candidate),
                                        children.end());
            if (internalUsed(leftTry) <= capacity() &&
                internalUsed(rightTry) <= capacity()) {
                midInternal = candidate;
                internalFound = true;
                break;
            }
        }
        if (!internalFound) {
            return DbResult::error(DbStatus::NoSpace, "index internal node cannot be split");
        }
        std::vector<Child> leftChildren(children.begin(),
                                        children.begin() + static_cast<long>(midInternal));
        std::vector<Child> rightChildren(children.begin() + static_cast<long>(midInternal),
                                         children.end());
        uint64_t newInternal = 0;
        result = allocateInternal(newInternal);
        if (!result.isOk()) {
            return result;
        }
        result = writeInternal(parentId, leftChildren);
        if (!result.isOk()) {
            return result;
        }
        result = writeInternal(newInternal, rightChildren);
        if (!result.isOk()) {
            return result;
        }
        childToInsert = newInternal;
        splitChild = parentId;
        separatorKey = rightChildren[0].lowerBound;
    }

    // The old root split: create a new root above it.
    result = splitRootLeaf(splitChild, childToInsert, separatorKey, outRootPageId);
    if (!result.isOk()) {
        return result;
    }
    outInserted = true;
    return DbResult::ok();
}

DbResult IndexTree::remove(const std::vector<uint8_t>& logicalKey,
                           const RowLocator& locator, uint64_t rootPageId,
                           uint64_t& outRootPageId, bool& outRemoved) {
    outRootPageId = rootPageId;
    outRemoved = false;
    std::vector<uint8_t> physicalKey = makePhysicalKey(logicalKey, locator);
    uint64_t leaf = 0;
    std::vector<uint64_t> path;
    DbResult result = findLeaf(rootPageId, physicalKey, leaf, path);
    if (!result.isOk()) {
        return result;
    }
    DatabasePage leafPage;
    LeafInfo leafInfo;
    result = readLeaf(leaf, leafPage, leafInfo);
    if (!result.isOk()) {
        return result;
    }
    std::vector<IndexEntry> entries;
    result = readLeafEntries(leafPage, leafInfo, entries);
    if (!result.isOk()) {
        return result;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].locator.pageId == locator.pageId &&
            entries[i].locator.slot == locator.slot &&
            compareEncodedKeys(entries[i].key.data(), entries[i].key.size(),
                               logicalKey.data(), logicalKey.size()) == 0) {
            entries.erase(entries.begin() + static_cast<long>(i));
            result = writeLeaf(leaf, leafInfo.nextLeaf, entries);
            if (!result.isOk()) {
                return result;
            }
            outRemoved = true;
            return DbResult::ok();
        }
    }
    return DbResult::ok();
}

DbResult IndexTree::rangeScan(const IndexRangeBound& bound, uint64_t rootPageId,
                              std::vector<IndexEntry>& out) const {
    out.clear();
    std::vector<uint8_t> lowerEncoded;
    std::vector<uint8_t> upperEncoded;
    if (bound.hasLower) {
        if (!encodeIndexKey(_keyType, bound.lower, lowerEncoded)) {
            return DbResult::error(DbStatus::InvalidArgument, "invalid lower bound value");
        }
    }
    if (bound.hasUpper) {
        if (!encodeIndexKey(_keyType, bound.upper, upperEncoded)) {
            return DbResult::error(DbStatus::InvalidArgument, "invalid upper bound value");
        }
    }

    uint64_t leaf = 0;
    if (bound.hasLower) {
        std::vector<uint8_t> probe = lowerEncoded;
        appendLocator(probe, RowLocator(0, 0));
        std::vector<uint64_t> path;
        DbResult result = findLeaf(rootPageId, probe, leaf, path);
        if (!result.isOk()) {
            return result;
        }
    } else {
        // Leftmost leaf: follow child 0 from the root.
        uint64_t current = rootPageId;
        const uint64_t pageCount = _pages.pageCount();
        for (uint32_t depth = 0; depth <= kMaxIndexDepth; ++depth) {
            if (current == 0 || current >= pageCount) {
                return DbResult::error(DbStatus::CorruptPage, "index root page out of range");
            }
            DatabasePage page;
            DbResult result = _pages.readPage(current, page);
            if (!result.isOk()) {
                return result;
            }
            if (page.type == PageType::IndexLeaf) {
                leaf = current;
                break;
            }
            if (page.type != PageType::IndexInternal) {
                return DbResult::error(DbStatus::CorruptPage, "index page has unknown type");
            }
            InternalInfo info;
            result = readInternal(current, page, info);
            if (!result.isOk()) {
                return result;
            }
            if (info.childCount == 0) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "index internal node has no children");
            }
            std::vector<Child> children;
            result = readInternalChildren(page, info, children);
            if (!result.isOk()) {
                return result;
            }
            current = children[0].pageId;
        }
        if (leaf == 0) {
            return DbResult::error(DbStatus::CorruptPage, "index tree exceeds maximum depth");
        }
    }

    const uint64_t pageCount = _pages.pageCount();
    ::GxPageVisitSet visited;
    DbResult trackerResult = initializePageVisitTracker(visited,
                                                          _traversalAllocator,
                                                          pageCount);
    if (!trackerResult.isOk()) return trackerResult;
    while (leaf != 0) {
        if (leaf >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf chain out of range");
        }
        bool revisited = false;
        const GxStatus visitStatus = visited.visit(leaf, revisited);
        if (visitStatus != GxStatus::Ok) {
            return pageVisitTrackerError(visitStatus);
        }
        if (revisited) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf chain cycle");
        }
        DatabasePage page;
        DbResult result = _pages.readPage(leaf, page);
        if (!result.isOk()) {
            return result;
        }
        LeafInfo info;
        result = readLeaf(leaf, page, info);
        if (!result.isOk()) {
            return result;
        }
        std::vector<IndexEntry> entries;
        result = readLeafEntries(page, info, entries);
        if (!result.isOk()) {
            return result;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            const std::vector<uint8_t>& logical = entries[i].key;
            if (bound.hasLower) {
                std::vector<uint8_t> probe = lowerEncoded;
                appendLocator(probe, RowLocator(0, 0));
                const std::vector<uint8_t> physical =
                    makePhysicalKey(logical, entries[i].locator);
                if (compareEncodedKeys(physical.data(), physical.size(), probe.data(),
                                       probe.size()) < 0) {
                    continue;
                }
                if (!bound.lowerInclusive &&
                    compareEncodedKeys(logical.data(), logical.size(),
                                       lowerEncoded.data(), lowerEncoded.size()) == 0) {
                    continue;
                }
            }
            if (bound.hasUpper) {
                const int cmp = compareEncodedKeys(logical.data(), logical.size(),
                                                   upperEncoded.data(), upperEncoded.size());
                if (bound.upperInclusive ? cmp > 0 : cmp >= 0) {
                    return DbResult::ok();
                }
            }
            out.push_back(entries[i]);
        }
        leaf = info.nextLeaf;
    }
    return DbResult::ok();
}

DbResult IndexTree::validate(uint64_t rootPageId, IndexValidation& out) const {
    out = IndexValidation();
    if (rootPageId == 0 || rootPageId >= _pages.pageCount()) {
        out.message = "index root page out of range";
        return DbResult::error(DbStatus::CorruptPage, out.message);
    }

    ::GxPageVisitSet visited;
    DbResult result = initializePageVisitTracker(visited,
                                                  _traversalAllocator,
                                                  _pages.pageCount());
    if (!result.isOk()) {
        out.message = result.message();
        return result;
    }
    uint64_t entryCount = 0;
    uint32_t height = 0;
    uint32_t pagesVisited = 0;
    std::string error;
    result = validateNode(rootPageId, 0, visited, entryCount, height,
                          pagesVisited, error);
    out.pagesVisited = pagesVisited;
    if (!result.isOk()) {
        out.ok = false;
        out.message = error.empty() ? result.message() : error;
        if (result.status() == DbStatus::NoSpace) return result;
        return DbResult::error(DbStatus::CorruptPage, out.message);
    }
    const GxStatus resetStatus = visited.reset();
    if (resetStatus != GxStatus::Ok) {
        out.message = "index page visit tracker reset failed";
        return DbResult::error(DbStatus::Internal, out.message);
    }
    // Walk the leaf chain to verify it is acyclic and ordered.
    uint64_t leaf = 0;
    {
        uint64_t current = rootPageId;
        for (uint32_t depth = 0; depth <= kMaxIndexDepth; ++depth) {
            DatabasePage page;
            DbResult r = _pages.readPage(current, page);
            if (!r.isOk()) {
                out.message = "index page unreadable";
                return DbResult::error(DbStatus::CorruptPage, out.message);
            }
            if (page.type == PageType::IndexLeaf) {
                leaf = current;
                break;
            }
            InternalInfo info;
            r = readInternal(current, page, info);
            if (!r.isOk()) {
                out.message = r.message();
                return DbResult::error(DbStatus::CorruptPage, out.message);
            }
            std::vector<Child> children;
            r = readInternalChildren(page, info, children);
            if (!r.isOk() || children.empty()) {
                out.message = "index internal node malformed";
                return DbResult::error(DbStatus::CorruptPage, out.message);
            }
            current = children[0].pageId;
        }
    }
    while (leaf != 0) {
        if (leaf >= _pages.pageCount()) {
            out.message = "index leaf chain out of range";
            return DbResult::error(DbStatus::CorruptPage, out.message);
        }
        bool revisited = false;
        const GxStatus visitStatus = visited.visit(leaf, revisited);
        if (visitStatus != GxStatus::Ok) {
            const DbResult visitResult = pageVisitTrackerError(visitStatus);
            out.message = visitResult.message();
            return visitResult;
        }
        if (revisited) {
            out.message = "index leaf chain cycle";
            return DbResult::error(DbStatus::CorruptPage, out.message);
        }
        DatabasePage page;
        DbResult r = _pages.readPage(leaf, page);
        if (!r.isOk()) {
            if (r.status() == DbStatus::NoSpace) {
                out.message = r.message();
                return r;
            }
            out.message = "index leaf unreadable";
            return DbResult::error(DbStatus::CorruptPage, out.message);
        }
        LeafInfo info;
        r = readLeaf(leaf, page, info);
        if (!r.isOk()) {
            out.message = r.message();
            return DbResult::error(DbStatus::CorruptPage, out.message);
        }
        leaf = info.nextLeaf;
    }

    out.ok = true;
    out.entryCount = entryCount;
    out.height = height;
    return DbResult::ok();
}

// Recursive validator. Defined here (outside the class body) so it can use the
// private parsing helpers. Returns the minimum physical key of the subtree via
// `outMinKey` and the subtree height via `outHeight`.
DbResult IndexTree::validateNode(uint64_t pageId, uint32_t depth,
                                 ::GxPageVisitSet& visited, uint64_t& entryCount,
                                 uint32_t& outHeight, uint32_t& pagesVisited,
                                 std::string& error) const {
    if (depth > kMaxIndexDepth) {
        error = "index tree exceeds maximum depth";
        return DbResult::error(DbStatus::CorruptPage, error);
    }
    if (pageId == 0 || pageId >= _pages.pageCount()) {
        error = "index page id out of range";
        return DbResult::error(DbStatus::CorruptPage, error);
    }
    bool revisited = false;
    const GxStatus visitStatus = visited.visit(pageId, revisited);
    if (visitStatus != GxStatus::Ok) {
        const DbResult visitResult = pageVisitTrackerError(visitStatus);
        error = visitResult.message();
        return visitResult;
    }
    if (revisited) {
        error = "index tree cycle";
        return DbResult::error(DbStatus::CorruptPage, error);
    }
    ++pagesVisited;
    DatabasePage page;
    DbResult result = _pages.readPage(pageId, page);
    if (!result.isOk()) {
        error = result.message();
        return result;
    }
    if (page.type == PageType::IndexLeaf) {
        LeafInfo info;
        result = readLeaf(pageId, page, info);
        if (!result.isOk()) {
            error = result.message();
            return result;
        }
        std::vector<IndexEntry> entries;
        result = readLeafEntries(page, info, entries);
        if (!result.isOk()) {
            error = result.message();
            return result;
        }
        GxIndexLeafError leafError = GxIndexLeafError::None;
        const GxStatus leafValidation = gxIndexLeafValidate(
            page.payload.data(), page.payload.size(), &leafError);
        if (leafValidation != GxStatus::Ok) {
            const DbResult validationResult = leafEntryDataError(leafError);
            error = validationResult.message();
            return validationResult;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].locator.pageId >= _pages.pageCount()) {
                error = "index entry locator out of range";
                return DbResult::error(DbStatus::CorruptPage, error);
            }
        }
        entryCount += entries.size();
        outHeight = 1;
        return DbResult::ok();
    }
    if (page.type != PageType::IndexInternal) {
        error = "index page has unknown type";
        return DbResult::error(DbStatus::CorruptPage, error);
    }
    InternalInfo info;
    result = readInternal(pageId, page, info);
    if (!result.isOk()) {
        error = result.message();
        return result;
    }
    if (info.childCount < 2) {
        error = "index internal node has too few children";
        return DbResult::error(DbStatus::CorruptPage, error);
    }
    std::vector<Child> children;
    result = readInternalChildren(page, info, children);
    if (!result.isOk()) {
        error = result.message();
        return result;
    }
    // A non-root internal node may have a finite lower bound on its first
    // child (it may be the right half of a split). Lower bounds must simply be
    // strictly increasing, with an empty first bound representing -infinity.
    for (size_t i = 1; i < children.size(); ++i) {
        if (children[i].lowerBound.empty()) {
            error = "index internal child missing lower bound";
            return DbResult::error(DbStatus::CorruptPage, error);
        }
        const int cmp = compareEncodedKeys(children[i - 1].lowerBound.data(),
                                           children[i - 1].lowerBound.size(),
                                           children[i].lowerBound.data(),
                                           children[i].lowerBound.size());
        if (cmp >= 0) {
            error = "index internal lower bounds not increasing";
            return DbResult::error(DbStatus::CorruptPage, error);
        }
    }
    uint32_t childHeight = 0;
    for (size_t i = 0; i < children.size(); ++i) {
        uint64_t childEntries = 0;
        uint32_t height = 0;
        result = validateNode(children[i].pageId, depth + 1, visited, childEntries,
                              height, pagesVisited, error);
        if (!result.isOk()) {
            return result;
        }
        if (i == 0) {
            childHeight = height;
        } else if (height != childHeight) {
            error = "index subtrees have differing heights";
            return DbResult::error(DbStatus::CorruptPage, error);
        }
        entryCount += childEntries;
    }
    outHeight = childHeight + 1;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
