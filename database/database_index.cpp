#include "database_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <set>

#include "database_endian.h"
#include "database_transaction.h"
#include "portable/gx_array.h"

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

uint32_t readU32Big(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint64_t readU64Big(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(bytes[i]);
    }
    return value;
}

uint8_t physicalEntryByte(const IndexEntry& entry, size_t offset) {
    if (offset < entry.key.size()) return entry.key[offset];
    offset -= entry.key.size();
    if (offset < 8) {
        return static_cast<uint8_t>(entry.locator.pageId >> (56 - offset * 8));
    }
    offset -= 8;
    return static_cast<uint8_t>(entry.locator.slot >> (24 - offset * 8));
}

int comparePhysicalEntries(const IndexEntry& left, const IndexEntry& right) {
    const size_t leftLength = left.key.size() + 12;
    const size_t rightLength = right.key.size() + 12;
    const size_t compareLength = leftLength < rightLength ? leftLength : rightLength;
    for (size_t i = 0; i < compareLength; ++i) {
        const uint8_t a = physicalEntryByte(left, i);
        const uint8_t b = physicalEntryByte(right, i);
        if (a != b) return a < b ? -1 : 1;
    }
    if (leftLength == rightLength) return 0;
    return leftLength < rightLength ? -1 : 1;
}

bool leafEntriesFit(const std::vector<IndexEntry>& entries, uint32_t capacity,
                    uint32_t maxKeyBytes, uint64_t& outUsed) {
    if (capacity < kIndexLeafHeaderSize ||
        entries.size() > (capacity - kIndexLeafHeaderSize) / kIndexSlotSize) {
        return false;
    }
    uint64_t used = kIndexLeafHeaderSize +
        static_cast<uint64_t>(entries.size()) * kIndexSlotSize;
    for (size_t i = 0; i < entries.size(); ++i) {
        const size_t keyLength = entries[i].key.size();
        if (keyLength > maxKeyBytes || keyLength > UINT32_MAX ||
            keyLength > capacity ||
            kIndexLeafEntryOverhead > capacity - keyLength ||
            used > capacity - (kIndexLeafEntryOverhead + keyLength)) {
            return false;
        }
        used += kIndexLeafEntryOverhead + keyLength;
    }
    outUsed = used;
    return true;
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

struct ScopedIndexPayload {
    GxArray bytes;

    ScopedIndexPayload() : bytes() {}
    ~ScopedIndexPayload() { gxArrayDestroy(&bytes); }

    ScopedIndexPayload(const ScopedIndexPayload&) = delete;
    ScopedIndexPayload& operator=(const ScopedIndexPayload&) = delete;
};

bool validatePreparedLeaf(const uint8_t* bytes, uint32_t capacity,
                           uint64_t nextLeaf, uint32_t indexId,
                           const std::vector<IndexEntry>& entries,
                           uint32_t maxKeyBytes, uint64_t pageCount) {
    if (bytes == nullptr || capacity < kIndexLeafHeaderSize ||
        loadLe32(bytes) != kIndexLeafMagic ||
        loadLe16(bytes + 4) != kIndexPageVersion ||
        loadLe16(bytes + 6) != 0 || loadLe64(bytes + 8) != nextLeaf ||
        (nextLeaf != 0 && nextLeaf >= pageCount) ||
        loadLe32(bytes + 16) != entries.size() ||
        loadLe32(bytes + 20) != indexId ||
        entries.size() > (capacity - kIndexLeafHeaderSize) / kIndexSlotSize) {
        return false;
    }

    uint32_t expectedOffset = kIndexLeafHeaderSize;
    for (size_t i = 0; i < entries.size(); ++i) {
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
        if (keyLength > maxKeyBytes || keyLength != entries[i].key.size() ||
            keyLength > UINT32_MAX - kIndexLeafEntryOverhead ||
            keyLength + kIndexLeafEntryOverhead != entryLength ||
            entryOffset > slotOffset ||
            entryLength > slotOffset - entryOffset ||
            (keyLength != 0 &&
             std::memcmp(entry + 4, entries[i].key.data(), keyLength) != 0)) {
            return false;
        }
        const uint64_t locatorPage = loadLe64(entry + 4 + keyLength);
        const uint32_t locatorSlot = loadLe32(entry + 4 + keyLength + 8);
        if (locatorPage != entries[i].locator.pageId ||
            locatorSlot != entries[i].locator.slot || locatorPage == 0 ||
            locatorPage >= pageCount) {
            return false;
        }
        if (i != 0 && comparePhysicalEntries(entries[i - 1], entries[i]) >= 0) {
            return false;
        }
        expectedOffset += entryLength;
    }
    return expectedOffset <= capacity - entries.size() * kIndexSlotSize;
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
    case DbType::Int32: {
        if (value.type() != DbType::Int32) {
            return false;
        }
        const uint32_t ordered =
            static_cast<uint32_t>(value.int32Value()) ^ 0x80000000u;
        appendU32Big(out, ordered);
        return true;
    }
    case DbType::Int64: {
        if (value.type() != DbType::Int64) {
            return false;
        }
        const uint64_t ordered =
            static_cast<uint64_t>(value.int64Value()) ^ 0x8000000000000000ull;
        appendU64Big(out, ordered);
        return true;
    }
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
        out.insert(out.end(), text.begin(), text.end());
        return true;
    }
    default:
        return false;
    }
}

bool encodedKeyIsNull(const uint8_t* key, size_t length) {
    return length == 1 && key != nullptr && key[0] == 0x00;
}

int compareEncodedKeys(const uint8_t* a, size_t aLength, const uint8_t* b,
                       size_t bLength) {
    const size_t count = aLength < bLength ? aLength : bLength;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    if (aLength == bLength) {
        return 0;
    }
    return aLength < bLength ? -1 : 1;
}

bool decodeIndexKey(DbType type, const uint8_t* key, size_t length, DbValue& out) {
    if (length == 0) {
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
    case DbType::Boolean:
        if (length != 2 || key[1] > 1) {
            return false;
        }
        out = DbValue::boolean(key[1] != 0);
        return true;
    case DbType::Int32:
        if (length != 5) {
            return false;
        }
        out = DbValue::int32(static_cast<int32_t>(readU32Big(key + 1) ^ 0x80000000u));
        return true;
    case DbType::Int64:
        if (length != 9) {
            return false;
        }
        out = DbValue::int64(
            static_cast<int64_t>(readU64Big(key + 1) ^ 0x8000000000000000ull));
        return true;
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
    case DbType::Text:
        out = DbValue::text(std::string(reinterpret_cast<const char*>(key + 1),
                                        length - 1));
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// IndexTree.

IndexTree::IndexTree(PageAccess& pages, DbType keyType, uint32_t indexId,
                     Transaction* transaction)
    : _pages(pages), _keyType(keyType), _indexId(indexId),
      _transaction(transaction) {}

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
    if (page.payload.size() < kIndexLeafHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "index leaf header truncated");
    }
    const uint8_t* p = page.payload.data();
    if (loadLe32(p + 0) != kIndexLeafMagic) {
        return DbResult::error(DbStatus::CorruptPage, "index leaf signature mismatch");
    }
    if (loadLe16(p + 4) != kIndexPageVersion) {
        return DbResult::error(DbStatus::CorruptPage, "unsupported index leaf version");
    }
    info.nextLeaf = loadLe64(p + 8);
    info.entryCount = loadLe32(p + 16);
    info.ownerIndexId = loadLe32(p + 20);
    if (info.ownerIndexId != _indexId) {
        return DbResult::error(DbStatus::CorruptPage, "index leaf belongs to another index");
    }
    if (kIndexLeafHeaderSize + kIndexSlotSize * static_cast<uint64_t>(info.entryCount) >
        cap) {
        return DbResult::error(DbStatus::CorruptPage, "impossible index leaf entry count");
    }
    return DbResult::ok();
}

DbResult IndexTree::readLeafEntries(const DatabasePage& page, const LeafInfo& info,
                                    std::vector<IndexEntry>& out) const {
    out.clear();
    out.reserve(info.entryCount);
    const uint32_t cap = capacity();
    const uint32_t maxKey = maxIndexKeyBytes(_pages.pageSize());
    for (uint32_t i = 0; i < info.entryCount; ++i) {
        const uint32_t slotOffset = cap - kIndexSlotSize * (i + 1);
        if (slotOffset < kIndexLeafHeaderSize) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf slot out of range");
        }
        const uint32_t entryOffset = loadLe32(page.payload.data() + slotOffset);
        const uint32_t entryLength = loadLe32(page.payload.data() + slotOffset + 4);
        if (entryOffset < kIndexLeafHeaderSize || entryOffset > cap ||
            entryLength > cap - entryOffset) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf entry out of range");
        }
        if (entryLength < kIndexLeafEntryOverhead) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf entry too small");
        }
        const uint8_t* entry = page.payload.data() + entryOffset;
        const uint32_t keyLength = loadLe32(entry);
        if (keyLength > maxKey || keyLength + kIndexLeafEntryOverhead != entryLength) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf key length invalid");
        }
        IndexEntry result;
        result.key.assign(entry + 4, entry + 4 + keyLength);
        result.locator.pageId = loadLe64(entry + 4 + keyLength);
        result.locator.slot = loadLe32(entry + 4 + keyLength + 8);
        out.push_back(std::move(result));
    }
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

DbResult IndexTree::prepareLeafPayload(
    uint64_t nextLeaf, const std::vector<IndexEntry>& entries,
    uint64_t candidatePageCount, GxArray& outPayload) const {
    if (_transaction == nullptr ||
        static_cast<const PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "checked index leaf preparation requires its transaction");
    }
    if (outPayload.initialized) {
        return DbResult::error(DbStatus::Internal,
                               "index leaf candidate output is already owned");
    }
    const uint32_t cap = capacity();
    const uint32_t maxKey = maxIndexKeyBytes(_pages.pageSize());
    uint64_t used = 0;
    if (candidatePageCount == 0 ||
        (nextLeaf != 0 && nextLeaf >= candidatePageCount)) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf candidate sibling is out of range");
    }
    if (entries.size() > UINT32_MAX ||
        !leafEntriesFit(entries, cap, maxKey, used)) {
        return DbResult::error(DbStatus::NoSpace,
                               "index leaf entries do not fit");
    }

    GxArray candidate = {};
    GxStatus status = gxArrayInit(&candidate, &_transaction->pageAllocator(),
                                  sizeof(uint8_t), cap);
    if (status == GxStatus::Ok) status = gxArrayResize(&candidate, cap);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&candidate);
        return leafPreparationStatus(status);
    }

    uint8_t* bytes = static_cast<uint8_t*>(gxArrayAt(&candidate, 0));
    if (bytes == nullptr || gxArraySize(&candidate) != cap) {
        gxArrayDestroy(&candidate);
        return DbResult::error(DbStatus::Internal,
                               "index leaf candidate storage is inconsistent");
    }
    std::memset(bytes, 0, cap);
    storeLe32(bytes + 0, kIndexLeafMagic);
    storeLe16(bytes + 4, kIndexPageVersion);
    storeLe16(bytes + 6, 0);
    storeLe64(bytes + 8, nextLeaf);
    storeLe32(bytes + 16, static_cast<uint32_t>(entries.size()));
    storeLe32(bytes + 20, _indexId);

    uint32_t offset = kIndexLeafHeaderSize;
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint32_t keyLength = static_cast<uint32_t>(entries[i].key.size());
        const uint32_t entryLength = kIndexLeafEntryOverhead + keyLength;
        storeLe32(bytes + offset, keyLength);
        if (keyLength != 0) {
            std::memcpy(bytes + offset + 4, entries[i].key.data(), keyLength);
        }
        storeLe64(bytes + offset + 4 + keyLength,
                  entries[i].locator.pageId);
        storeLe32(bytes + offset + 4 + keyLength + 8,
                  entries[i].locator.slot);
        const uint32_t slotOffset =
            cap - kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        storeLe32(bytes + slotOffset, offset);
        storeLe32(bytes + slotOffset + 4, entryLength);
        offset += entryLength;
    }

    if (offset + entries.size() * kIndexSlotSize != used ||
        !validatePreparedLeaf(bytes, cap, nextLeaf, _indexId, entries,
                              maxKey, candidatePageCount)) {
        gxArrayDestroy(&candidate);
        return DbResult::error(DbStatus::CorruptPage,
                               "index leaf candidate validation failed");
    }
    outPayload = candidate;
    candidate = {};
    return DbResult::ok();
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
    out.reserve(info.childCount);
    const uint32_t cap = capacity();
    const uint32_t maxKey = maxPhysicalIndexKeyBytes(_pages.pageSize());
    const uint64_t pageCount = _pages.pageCount();
    for (uint32_t i = 0; i < info.childCount; ++i) {
        const uint32_t slotOffset = cap - kIndexSlotSize * (i + 1);
        if (slotOffset < kIndexInternalHeaderSize) {
            return DbResult::error(DbStatus::CorruptPage, "index internal slot out of range");
        }
        const uint32_t childOffset = loadLe32(page.payload.data() + slotOffset);
        const uint32_t childLength = loadLe32(page.payload.data() + slotOffset + 4);
        if (childOffset < kIndexInternalHeaderSize || childOffset > cap ||
            childLength > cap - childOffset) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal child out of range");
        }
        if (childLength < kIndexInternalEntryOverhead) {
            return DbResult::error(DbStatus::CorruptPage, "index internal child too small");
        }
        const uint8_t* child = page.payload.data() + childOffset;
        Child result;
        result.pageId = loadLe64(child);
        const uint32_t keyLength = loadLe32(child + 8);
        if (keyLength > maxKey ||
            keyLength + kIndexInternalEntryOverhead != childLength) {
            return DbResult::error(DbStatus::CorruptPage, "index internal key length invalid");
        }
        if (result.pageId >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal child page out of range");
        }
        result.lowerBound.assign(child + 12, child + 12 + keyLength);
        out.push_back(std::move(result));
    }
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

DbResult IndexTree::prepareInternalPayload(
    const std::vector<Child>& children, uint64_t candidatePageCount,
    GxArray& outPayload) const {
    if (_transaction == nullptr ||
        static_cast<const PageAccess*>(_transaction) != &_pages) {
        return DbResult::error(DbStatus::Internal,
                               "checked index parent preparation requires its transaction");
    }
    if (outPayload.initialized) {
        return DbResult::error(DbStatus::Internal,
                               "index parent candidate output is already owned");
    }
    const uint32_t cap = capacity();
    const uint32_t maxKey = maxPhysicalIndexKeyBytes(_pages.pageSize());
    uint64_t used = 0;
    if (candidatePageCount == 0 ||
        !internalChildrenFit(children, cap, maxKey, used)) {
        return DbResult::error(DbStatus::NoSpace,
                               "index internal children do not fit");
    }
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i].pageId == 0 ||
            children[i].pageId >= candidatePageCount) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal candidate child is out of range");
        }
        if (i != 0 && (children[i].lowerBound.empty() ||
            compareEncodedKeys(children[i - 1].lowerBound.data(),
                               children[i - 1].lowerBound.size(),
                               children[i].lowerBound.data(),
                               children[i].lowerBound.size()) >= 0)) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index internal candidate bounds are not increasing");
        }
        for (size_t j = 0; j < i; ++j) {
            if (children[j].pageId == children[i].pageId) {
                return DbResult::error(DbStatus::CorruptPage,
                                       "index internal candidate repeats a child");
            }
        }
    }

    GxArray candidate = {};
    GxStatus status = gxArrayInit(&candidate, &_transaction->pageAllocator(),
                                  sizeof(uint8_t), cap);
    if (status == GxStatus::Ok) status = gxArrayResize(&candidate, cap);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&candidate);
        return internalPreparationStatus(status);
    }
    uint8_t* bytes = static_cast<uint8_t*>(gxArrayAt(&candidate, 0));
    if (bytes == nullptr || gxArraySize(&candidate) != cap) {
        gxArrayDestroy(&candidate);
        return DbResult::error(DbStatus::Internal,
                               "index internal candidate storage is inconsistent");
    }
    std::memset(bytes, 0, cap);
    storeLe32(bytes + 0, kIndexInternalMagic);
    storeLe16(bytes + 4, kIndexPageVersion);
    storeLe16(bytes + 6, 0);
    storeLe64(bytes + 8, 0);
    storeLe32(bytes + 16, static_cast<uint32_t>(children.size()));
    storeLe32(bytes + 20, _indexId);

    uint32_t offset = kIndexInternalHeaderSize;
    for (size_t i = 0; i < children.size(); ++i) {
        const uint32_t keyLength =
            static_cast<uint32_t>(children[i].lowerBound.size());
        const uint32_t entryLength = kIndexInternalEntryOverhead + keyLength;
        storeLe64(bytes + offset, children[i].pageId);
        storeLe32(bytes + offset + 8, keyLength);
        if (keyLength != 0) {
            std::memcpy(bytes + offset + 12, children[i].lowerBound.data(),
                        keyLength);
        }
        const uint32_t slotOffset =
            cap - kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        storeLe32(bytes + slotOffset, offset);
        storeLe32(bytes + slotOffset + 4, entryLength);
        offset += entryLength;
    }

    bool valid = offset + children.size() * kIndexSlotSize == used &&
        loadLe32(bytes) == kIndexInternalMagic &&
        loadLe16(bytes + 4) == kIndexPageVersion &&
        loadLe16(bytes + 6) == 0 && loadLe64(bytes + 8) == 0 &&
        loadLe32(bytes + 16) == children.size() &&
        loadLe32(bytes + 20) == _indexId;
    uint32_t expectedOffset = kIndexInternalHeaderSize;
    for (size_t i = 0; valid && i < children.size(); ++i) {
        const uint32_t slotOffset =
            cap - kIndexSlotSize * (static_cast<uint32_t>(i) + 1);
        const uint32_t entryOffset = loadLe32(bytes + slotOffset);
        const uint32_t entryLength = loadLe32(bytes + slotOffset + 4);
        const uint32_t keyLength = static_cast<uint32_t>(
            children[i].lowerBound.size());
        valid = entryOffset == expectedOffset && entryOffset <= cap &&
            entryLength >= kIndexInternalEntryOverhead &&
            entryLength <= cap - entryOffset &&
            entryOffset <= slotOffset && entryLength <= slotOffset - entryOffset &&
            loadLe64(bytes + entryOffset) == children[i].pageId &&
            loadLe32(bytes + entryOffset + 8) == keyLength &&
            keyLength + kIndexInternalEntryOverhead == entryLength &&
            (keyLength == 0 ||
             std::memcmp(bytes + entryOffset + 12,
                         children[i].lowerBound.data(), keyLength) == 0);
        expectedOffset += entryLength;
    }
    valid = valid && expectedOffset <= cap - children.size() * kIndexSlotSize;
    if (!valid) {
        gxArrayDestroy(&candidate);
        return DbResult::error(DbStatus::CorruptPage,
                               "index internal candidate validation failed");
    }
    outPayload = candidate;
    candidate = {};
    return DbResult::ok();
}

DbResult IndexTree::stageNonCascadingLeafSplit(
    uint64_t leafPageId, const DatabasePage& leafPage,
    const LeafInfo& leafInfo,
    const std::vector<uint64_t>& path,
    const std::vector<IndexEntry>& left,
    const std::vector<IndexEntry>& right,
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
    DatabasePage parentPage;
    InternalInfo parentInfo;
    DbResult result = readInternal(parentId, parentPage, parentInfo);
    if (!result.isOk()) return result;
    std::vector<Child> children;
    result = readInternalChildren(parentPage, parentInfo, children);
    if (!result.isOk()) return result;
    if (children.size() < 2) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split parent has too few children");
    }

    size_t position = children.size();
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i].pageId == leafPageId) {
            position = i;
            break;
        }
    }
    if (position == children.size()) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index parent does not reference split child");
    }
    if (right.empty()) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split produced an empty right leaf");
    }
    Child inserted;
    inserted.pageId = plannedNewLeafId;
    inserted.lowerBound = makePhysicalKey(right[0].key, right[0].locator);
    children.insert(children.begin() + static_cast<long>(position + 1), inserted);

    uint64_t internalUsedBytes = 0;
    if (!internalChildrenFit(children, capacity(),
                             maxPhysicalIndexKeyBytes(_pages.pageSize()),
                             internalUsedBytes)) {
        // Parent recursion remains on the existing path. Nothing has been
        // staged or reserved, so the legacy cascading split can proceed.
        return DbResult::ok();
    }

    ScopedIndexPayload leftPayload;
    ScopedIndexPayload rightPayload;
    ScopedIndexPayload parentPayload;
    result = prepareLeafPayload(plannedNewLeafId, left, plannedPageCount,
                                leftPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareLeafPayload(leafInfo.nextLeaf, right, plannedPageCount,
                                rightPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareInternalPayload(children, plannedPageCount,
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
    const std::vector<IndexEntry>& left, const std::vector<IndexEntry>& right,
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
    const uint64_t newPageCount = splitRoot ? 3 : 2;
    if (_transaction->pageCount() > kMaxPageCount - newPageCount) {
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
    std::vector<Child> allChildren;
    result = readInternalChildren(parentPage, parentInfo, allChildren);
    if (!result.isOk()) return result;
    if (allChildren.size() < 2) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split parent has too few children");
    }

    size_t position = allChildren.size();
    for (size_t i = 0; i < allChildren.size(); ++i) {
        if (allChildren[i].pageId == leafPageId) {
            position = i;
            break;
        }
    }
    if (position == allChildren.size()) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index parent does not reference split child");
    }
    if (right.empty()) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index split produced an empty right leaf");
    }

    Child newLeafChild;
    newLeafChild.pageId = plannedNewLeafId;
    newLeafChild.lowerBound = makePhysicalKey(right[0].key, right[0].locator);
    allChildren.insert(allChildren.begin() + static_cast<long>(position + 1),
                       newLeafChild);

    uint64_t combinedUsed = 0;
    if (internalChildrenFit(allChildren, capacity(),
                            maxPhysicalIndexKeyBytes(_pages.pageSize()),
                            combinedUsed)) {
        // The fitting-parent case belongs to R3L. Leave it untouched here.
        return DbResult::ok();
    }

    size_t splitPoint = 0;
    bool foundSplit = false;
    const uint32_t maxKeyBytes = maxPhysicalIndexKeyBytes(_pages.pageSize());
    for (size_t candidate = 2; candidate + 1 < allChildren.size(); ++candidate) {
        std::vector<Child> leftTry(
            allChildren.begin(), allChildren.begin() + static_cast<long>(candidate));
        std::vector<Child> rightTry(
            allChildren.begin() + static_cast<long>(candidate), allChildren.end());
        uint64_t leftUsed = 0;
        uint64_t rightUsed = 0;
        if (internalChildrenFit(leftTry, capacity(), maxKeyBytes, leftUsed) &&
            internalChildrenFit(rightTry, capacity(), maxKeyBytes, rightUsed)) {
            splitPoint = candidate;
            foundSplit = true;
            break;
        }
    }
    if (!foundSplit) {
        return DbResult::error(DbStatus::NoSpace,
                               "index internal parent cannot be split");
    }

    std::vector<Child> leftChildren(
        allChildren.begin(), allChildren.begin() + static_cast<long>(splitPoint));
    std::vector<Child> rightChildren(
        allChildren.begin() + static_cast<long>(splitPoint), allChildren.end());
    if (rightChildren.empty() || rightChildren[0].lowerBound.empty()) {
        return DbResult::error(DbStatus::CorruptPage,
                               "index promoted parent separator is empty");
    }

    DatabasePage grandparentPage;
    InternalInfo grandparentInfo;
    std::vector<Child> grandparentChildren;
    size_t grandparentPosition = 0;
    if (splitRoot) {
        Child rootLeft;
        rootLeft.pageId = parentPageId;
        Child rootRight;
        rootRight.pageId = plannedRightInternalId;
        rootRight.lowerBound = rightChildren[0].lowerBound;
        grandparentChildren.push_back(rootLeft);
        grandparentChildren.push_back(rootRight);
    } else {
        const uint64_t grandparentPageId = path[path.size() - 2];
        result = readInternal(grandparentPageId, grandparentPage,
                              grandparentInfo);
        if (!result.isOk()) return result;
        result = readInternalChildren(grandparentPage, grandparentInfo,
                                      grandparentChildren);
        if (!result.isOk()) return result;
        grandparentPosition = grandparentChildren.size();
        for (size_t i = 0; i < grandparentChildren.size(); ++i) {
            if (grandparentChildren[i].pageId == parentPageId) {
                grandparentPosition = i;
                break;
            }
        }
        if (grandparentPosition == grandparentChildren.size()) {
            return DbResult::error(DbStatus::CorruptPage,
                                   "index grandparent does not reference split parent");
        }
        Child rightParent;
        rightParent.pageId = plannedRightInternalId;
        rightParent.lowerBound = rightChildren[0].lowerBound;
        grandparentChildren.insert(
            grandparentChildren.begin() +
                static_cast<long>(grandparentPosition + 1),
            rightParent);
        uint64_t grandparentUsed = 0;
        if (!internalChildrenFit(grandparentChildren, capacity(),
                                 maxPhysicalIndexKeyBytes(_pages.pageSize()),
                                 grandparentUsed)) {
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
    result = prepareLeafPayload(plannedNewLeafId, left, plannedPageCount,
                                leftLeafPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareLeafPayload(leafInfo.nextLeaf, right, plannedPageCount,
                                rightLeafPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareInternalPayload(leftChildren, plannedPageCount,
                                    leftInternalPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareInternalPayload(rightChildren, plannedPageCount,
                                    rightInternalPayload.bytes);
    if (!result.isOk()) return result;
    result = prepareInternalPayload(grandparentChildren, plannedPageCount,
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
    std::set<uint64_t> visited;
    while (leaf != 0) {
        if (leaf >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf chain out of range");
        }
        if (!visited.insert(leaf).second) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf chain cycle");
        }
        if (visited.size() > pageCount) {
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
                           const RowLocator& locator, uint64_t rootPageId, bool unique,
                           uint64_t& outRootPageId, bool& outInserted) {
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

    if (_transaction != nullptr && !path.empty()) {
        uint64_t checkedNewLeaf = 0;
        bool checkedSplit = false;
        result = stageNonCascadingLeafSplit(leaf, leafPage, leafInfo, path,
                                            left, right, checkedNewLeaf,
                                            checkedSplit);
        if (!result.isOk()) return result;
        if (checkedSplit) {
            outInserted = true;
            return DbResult::ok();
        }
    }

    if (_transaction != nullptr &&
        (path.size() == 1 || path.size() == 2)) {
        uint64_t checkedNewRoot = 0;
        bool checkedParentSplit = false;
        result = stageInternalParentSplit(rootPageId, leaf, leafPage, leafInfo,
                                          path, left, right, checkedNewRoot,
                                          checkedParentSplit);
        if (!result.isOk()) return result;
        if (checkedParentSplit) {
            if (path.size() == 1) outRootPageId = checkedNewRoot;
            outInserted = true;
            return DbResult::ok();
        }
    }

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
    std::set<uint64_t> visited;
    while (leaf != 0) {
        if (leaf >= pageCount) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf chain out of range");
        }
        if (!visited.insert(leaf).second) {
            return DbResult::error(DbStatus::CorruptPage, "index leaf chain cycle");
        }
        if (visited.size() > pageCount) {
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

    std::set<uint64_t> visited;
    uint64_t entryCount = 0;
    uint32_t height = 0;
    uint32_t pagesVisited = 0;
    std::string error;
    DbResult result = validateNode(rootPageId, 0, visited, entryCount, height,
                                   pagesVisited, error);
    out.pagesVisited = pagesVisited;
    if (!result.isOk()) {
        out.ok = false;
        out.message = error.empty() ? result.message() : error;
        return DbResult::error(DbStatus::CorruptPage, out.message);
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
    std::set<uint64_t> chainVisited;
    while (leaf != 0) {
        if (leaf >= _pages.pageCount()) {
            out.message = "index leaf chain out of range";
            return DbResult::error(DbStatus::CorruptPage, out.message);
        }
        if (!chainVisited.insert(leaf).second) {
            out.message = "index leaf chain cycle";
            return DbResult::error(DbStatus::CorruptPage, out.message);
        }
        if (chainVisited.size() > _pages.pageCount()) {
            out.message = "index leaf chain cycle";
            return DbResult::error(DbStatus::CorruptPage, out.message);
        }
        DatabasePage page;
        DbResult r = _pages.readPage(leaf, page);
        if (!r.isOk()) {
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
                                 std::set<uint64_t>& visited, uint64_t& entryCount,
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
    if (!visited.insert(pageId).second) {
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
        for (size_t i = 1; i < entries.size(); ++i) {
            const std::vector<uint8_t> prev =
                makePhysicalKey(entries[i - 1].key, entries[i - 1].locator);
            const std::vector<uint8_t> cur =
                makePhysicalKey(entries[i].key, entries[i].locator);
            if (compareEncodedKeys(prev.data(), prev.size(), cur.data(), cur.size()) >= 0) {
                error = "index leaf entries not strictly ordered";
                return DbResult::error(DbStatus::CorruptPage, error);
            }
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
