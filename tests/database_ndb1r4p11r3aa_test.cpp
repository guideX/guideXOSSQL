#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "database_format.h"
#include "database_index.h"
#include "database_pagestore.h"
#include "gx_allocator_hosted.h"
#include "gx_index_internal_node.h"
#include "gx_index_leaf_entry.h"
#include "gx_index_leaf_traversal.h"
#include "gx_index_tree_descent.h"

using namespace gxos::db;

namespace {

int gPass = 0;
int gFail = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) ++gPass;
    else {
        ++gFail;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

void storeLe16(uint8_t* bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void storeLe32(uint8_t* bytes, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

void storeLe64(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
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

bool byteLess(const std::vector<uint8_t>& left,
              const std::vector<uint8_t>& right) {
    const size_t shared = left.size() < right.size()
        ? left.size() : right.size();
    for (size_t i = 0; i < shared; ++i) {
        if (left[i] != right[i]) return left[i] < right[i];
    }
    return left.size() < right.size();
}

int byteCompare(const uint8_t* left, size_t leftLength,
                const uint8_t* right, size_t rightLength) {
    const size_t shared = leftLength < rightLength ? leftLength : rightLength;
    for (size_t i = 0; i < shared; ++i) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    if (leftLength == rightLength) return 0;
    return leftLength < rightLength ? -1 : 1;
}

std::vector<uint8_t> encodedText(const std::string& text) {
    std::vector<uint8_t> result(1, 0x01u);
    result.insert(result.end(), text.begin(), text.end());
    return result;
}

std::vector<uint8_t> physicalKey(const std::vector<uint8_t>& key,
                                 uint64_t pageId, uint32_t slot) {
    std::vector<uint8_t> result = key;
    for (size_t i = 0; i < 8; ++i) {
        result.push_back(static_cast<uint8_t>(pageId >> (56u - 8u * i)));
    }
    for (size_t i = 0; i < 4; ++i) {
        result.push_back(static_cast<uint8_t>(slot >> (24u - 8u * i)));
    }
    return result;
}

struct ChildBytes {
    uint64_t pageId;
    std::vector<uint8_t> lowerBound;
};

std::vector<uint8_t> makeInternalPayload(
    size_t payloadSize, uint32_t owner, const std::vector<ChildBytes>& children) {
    std::vector<uint8_t> bytes(payloadSize, 0);
    storeLe32(bytes.data(), kIndexInternalMagic);
    storeLe16(bytes.data() + 4, kIndexPageVersion);
    storeLe32(bytes.data() + 16, static_cast<uint32_t>(children.size()));
    storeLe32(bytes.data() + 20, owner);
    uint32_t offset = kIndexInternalHeaderSize;
    for (size_t i = 0; i < children.size(); ++i) {
        const uint32_t entryLength = static_cast<uint32_t>(
            kIndexInternalEntryOverhead + children[i].lowerBound.size());
        if (offset > bytes.size() || entryLength > bytes.size() - offset) {
            return std::vector<uint8_t>();
        }
        storeLe64(bytes.data() + offset, children[i].pageId);
        storeLe32(bytes.data() + offset + 8,
                  static_cast<uint32_t>(children[i].lowerBound.size()));
        if (!children[i].lowerBound.empty()) {
            std::memcpy(bytes.data() + offset + kIndexInternalEntryOverhead,
                        children[i].lowerBound.data(),
                        children[i].lowerBound.size());
        }
        const size_t slotOffset = bytes.size() -
            kIndexSlotSize * (i + 1u);
        storeLe32(bytes.data() + slotOffset, offset);
        storeLe32(bytes.data() + slotOffset + 4, entryLength);
        offset += entryLength;
    }
    return bytes;
}

std::vector<uint8_t> makeEmptyLeafPayload(size_t payloadSize, uint32_t owner) {
    std::vector<uint8_t> bytes(payloadSize, 0);
    storeLe32(bytes.data(), kIndexLeafMagic);
    storeLe16(bytes.data() + 4, kIndexPageVersion);
    storeLe64(bytes.data() + 8, 0);
    storeLe32(bytes.data() + 16, 0);
    storeLe32(bytes.data() + 20, owner);
    return bytes;
}

class MemoryPageAccess : public PageAccess {
public:
    explicit MemoryPageAccess(uint32_t pageBytes = 512)
        : _pageSize(pageBytes), _pages(1), _writes(0) {
        _pages[0].pageId = 0;
    }

    uint32_t pageSize() const override { return _pageSize; }
    uint64_t pageCount() const override { return _pages.size(); }
    bool isReadOnly() const override { return false; }

    DbResult readPage(uint64_t pageId, DatabasePage& out) override {
        if (pageId == 0 || pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "R3AA page id is out of range");
        }
        out = _pages[static_cast<size_t>(pageId)];
        return DbResult::ok();
    }

    DbResult writePage(const DatabasePage& page) override {
        if (page.pageId == 0 || page.pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "R3AA page id is out of range");
        }
        _pages[static_cast<size_t>(page.pageId)] = page;
        ++_writes;
        return DbResult::ok();
    }

    DbResult allocatePage(PageType type, uint64_t& outPageId) override {
        DatabasePage page;
        page.pageId = _pages.size();
        page.type = type;
        page.payload.assign(DatabasePage::payloadCapacity(_pageSize), 0);
        page.payloadSize = static_cast<uint32_t>(page.payload.size());
        _pages.push_back(page);
        outPageId = page.pageId;
        return DbResult::ok();
    }

    const DatabasePage* get(uint64_t pageId) const {
        return pageId < _pages.size()
            ? &_pages[static_cast<size_t>(pageId)] : nullptr;
    }

    uint64_t writeCount() const { return _writes; }

private:
    uint32_t _pageSize;
    std::vector<DatabasePage> _pages;
    uint64_t _writes;
};

struct ReusingProvider {
    const MemoryPageAccess& pages;
    std::vector<uint8_t> buffer;
    uint64_t acquireCount;
    uint64_t releaseCount;
    uint64_t failOnAcquire;
    uint64_t missingPageId;
    uint64_t overrideViewPageId;
    uint16_t overridePageType;
    bool overridePageId;
    bool overrideType;
    bool active;
    bool releaseMismatch;

    explicit ReusingProvider(const MemoryPageAccess& source)
        : pages(source), buffer(DatabasePage::payloadCapacity(source.pageSize()), 0),
          acquireCount(0), releaseCount(0), failOnAcquire(0), missingPageId(0),
          overrideViewPageId(0), overridePageType(0), overridePageId(false),
          overrideType(false), active(false), releaseMismatch(false) {}
};

GxIndexPageProviderStatus acquirePage(
    void* opaque, uint64_t pageId, GxIndexPageView* outView) {
    ReusingProvider* provider = static_cast<ReusingProvider*>(opaque);
    if (provider == nullptr || outView == nullptr || provider->active) {
        return GxIndexPageProviderStatus::Error;
    }
    ++provider->acquireCount;
    *outView = GxIndexPageView();
    if (provider->failOnAcquire != 0 &&
        provider->acquireCount == provider->failOnAcquire) {
        return GxIndexPageProviderStatus::Error;
    }
    if (provider->missingPageId == pageId) {
        return GxIndexPageProviderStatus::MissingPage;
    }
    const DatabasePage* page = provider->pages.get(pageId);
    if (page == nullptr) return GxIndexPageProviderStatus::MissingPage;
    if (page->payload.size() > provider->buffer.size()) {
        return GxIndexPageProviderStatus::Error;
    }
    std::copy(page->payload.begin(), page->payload.end(), provider->buffer.begin());
    outView->bytes = provider->buffer.data();
    outView->length = page->payload.size();
    outView->pageId = provider->overridePageId
        ? provider->overrideViewPageId : page->pageId;
    outView->pageType = provider->overrideType
        ? provider->overridePageType : static_cast<uint16_t>(page->type);
    outView->token = provider;
    provider->active = true;
    return GxIndexPageProviderStatus::Ok;
}

void releasePage(void* opaque, uint64_t pageId,
                 const GxIndexPageView* view) {
    ReusingProvider* provider = static_cast<ReusingProvider*>(opaque);
    if (provider == nullptr) return;
    if (!provider->active || view == nullptr || view->token != provider ||
        view->bytes != provider->buffer.data() || view->pageId != pageId) {
        provider->releaseMismatch = true;
    }
    provider->active = false;
    ++provider->releaseCount;
    std::fill(provider->buffer.begin(), provider->buffer.end(), 0xDDu);
}

GxIndexPageProvider providerApi(ReusingProvider& provider) {
    GxIndexPageProvider result = {&provider, acquirePage, releasePage};
    return result;
}

GxIndexTreeDescentStatus descend(
    ReusingProvider& source, uint64_t root, const std::vector<uint8_t>& key,
    uint32_t maxDepth, GxIndexTreeDescentResult& result) {
    GxIndexPageProvider provider = providerApi(source);
    uint64_t visited[kMaxIndexDepth + 1] = {};
    return gxIndexTreeDescendToLeaf(
        &provider, root, source.pages.pageCount(), 37, key.data(), key.size(),
        maxDepth, visited, sizeof(visited) / sizeof(visited[0]), &result);
}

uint64_t appendPage(MemoryPageAccess& pages, PageType type,
                    const std::vector<uint8_t>& payload) {
    uint64_t pageId = 0;
    if (!pages.allocatePage(type, pageId).isOk()) return 0;
    DatabasePage page;
    if (!pages.readPage(pageId, page).isOk()) return 0;
    page.payload = payload;
    page.payloadSize = static_cast<uint32_t>(payload.size());
    if (!pages.writePage(page).isOk()) return 0;
    return pageId;
}

void testInternalReaderAndRouting() {
    std::printf("R3AA internal reader and exact separator routing\n");
    const size_t payloadSize = 512 - kPageHeaderSize;
    const std::vector<uint8_t> sepB = physicalKey(encodedText("b"), 1, 10);
    const std::vector<uint8_t> sepD = physicalKey(encodedText("d"), 1, 10);
    const std::vector<ChildBytes> children = {
        {2, std::vector<uint8_t>()}, {3, sepB}, {4, sepD}
    };
    std::vector<uint8_t> bytes = makeInternalPayload(payloadSize, 37, children);
    GxIndexInternalNodeCursor cursor = {};
    GxIndexInternalNodeError error = GxIndexInternalNodeError::None;
    CHECK(gxIndexInternalNodeCursorInit(bytes.data(), bytes.size(), 8, 37,
                                        &cursor, &error) == GxStatus::Ok);
    CHECK(cursor.page.childCount == 3 && cursor.page.ownerIndexId == 37);
    for (size_t i = 0; i < children.size(); ++i) {
        GxIndexInternalChildView child = {};
        bool hasChild = false;
        CHECK(gxIndexInternalNodeCursorReadNext(&cursor, &child, &hasChild,
                                                &error) == GxStatus::Ok);
        CHECK(hasChild && child.childPageId == children[i].pageId);
        CHECK(child.separatorLength == children[i].lowerBound.size());
        CHECK(child.separatorLength == 0 ||
              std::memcmp(child.separatorBytes, children[i].lowerBound.data(),
                          child.separatorLength) == 0);
    }
    GxIndexInternalChildView endChild = {};
    bool hasChild = true;
    CHECK(gxIndexInternalNodeCursorReadNext(&cursor, &endChild, &hasChild,
                                            &error) == GxStatus::Ok);
    CHECK(!hasChild);

    const std::vector<uint8_t> keyBeforeB = physicalKey(encodedText("b"), 1, 9);
    const std::vector<uint8_t> keyAtB = physicalKey(encodedText("b"), 1, 10);
    const std::vector<uint8_t> keyAfterB = physicalKey(encodedText("b"), 1, 11);
    const std::vector<uint8_t> keyBeforeD = physicalKey(encodedText("d"), 1, 9);
    const std::vector<uint8_t> keyAtD = physicalKey(encodedText("d"), 1, 10);
    const std::vector<uint8_t> keyAfterD = physicalKey(encodedText("d"), 1, 11);
    const std::vector<uint8_t> keyLow = physicalKey(encodedText("a"), 1, 99);
    const std::vector<uint8_t> keyHigh = physicalKey(encodedText("z"), 1, 1);
    struct RouteCase { const std::vector<uint8_t>* key; uint64_t child; };
    const RouteCase routes[] = {
        {&keyLow, 2}, {&keyBeforeB, 2}, {&keyAtB, 3},
        {&keyAfterB, 3}, {&keyBeforeD, 3}, {&keyAtD, 4},
        {&keyAfterD, 4}, {&keyHigh, 4}
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        uint64_t selected = 0;
        uint32_t selectedIndex = UINT32_MAX;
        CHECK(gxIndexInternalNodeSelectChild(
                  bytes.data(), bytes.size(), 8, 37, routes[i].key->data(),
                  routes[i].key->size(), &selected, &selectedIndex, &error) ==
              GxStatus::Ok);
        CHECK(selected == routes[i].child);
        CHECK(selectedIndex == selected - 2);
    }

    // The parser treats all supported logical key encodings as opaque bytes;
    // physical locator suffixes retain ordering and the production route.
    const std::vector<uint8_t> nullKey(1, 0x00u);
    const std::vector<uint8_t> falseKey = {0x01u, 0x00u};
    const std::vector<uint8_t> trueKey = {0x01u, 0x01u};
    const std::vector<uint8_t> embeddedZero = {0x01u, 'a', 0, 'b'};
    const std::vector<uint8_t> int32Min = {0x01u, 0x00u, 0x00u, 0x00u, 0x00u};
    const std::vector<uint8_t> int32Max = {0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
    const std::vector<uint8_t> int64Min = {
        0x01u, 0, 0, 0, 0, 0, 0, 0, 0
    };
    const std::vector<uint8_t> int64Max = {
        0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
    };
    const std::vector<std::vector<uint8_t> > codecKeys = {
        nullKey, falseKey, trueKey, int32Min, int32Max, int64Min, int64Max,
        embeddedZero
    };
    std::vector<uint8_t> orderedBytes = makeInternalPayload(
        payloadSize, 37, {{2, std::vector<uint8_t>()},
                          {3, physicalKey(falseKey, 1, 1)},
                          {4, physicalKey(trueKey, 1, 1)}});
    for (size_t i = 0; i < codecKeys.size(); ++i) {
        std::vector<uint8_t> query = physicalKey(codecKeys[i], 1, 0);
        uint64_t selected = 0;
        GxStatus status = gxIndexInternalNodeSelectChild(
            orderedBytes.data(), orderedBytes.size(), 8, 37, query.data(),
            query.size(), &selected, nullptr, &error);
        CHECK(status == GxStatus::Ok);
        const std::vector<uint8_t> falseSeparator =
            physicalKey(falseKey, 1, 1);
        const std::vector<uint8_t> trueSeparator =
            physicalKey(trueKey, 1, 1);
        const uint64_t expected = byteCompare(
            query.data(), query.size(), trueSeparator.data(),
            trueSeparator.size()) >= 0 ? 4u :
            (byteCompare(query.data(), query.size(), falseSeparator.data(),
                         falseSeparator.size()) >= 0 ? 3u : 2u);
        CHECK(selected == expected);
    }

    uint64_t selected = 91;
    const uint8_t shortPhysical[11] = {};
    CHECK(gxIndexInternalNodeSelectChild(
              bytes.data(), bytes.size(), 8, 37, shortPhysical,
              sizeof(shortPhysical), &selected, nullptr, &error) ==
          GxStatus::InvalidArgument);
    CHECK(selected == 91);
    CHECK(error == GxIndexInternalNodeError::InvalidArgument);
    CHECK(gxIndexInternalNodeSelectChild(
              bytes.data(), bytes.size(), 8, 37, nullptr, 12, &selected,
              nullptr, &error) == GxStatus::InvalidArgument);
    CHECK(gxIndexInternalNodeCursorInit(bytes.data(), bytes.size(), 8, 99,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::OwnerIndexMismatch);
}

void testInternalCorruption() {
    std::printf("R3AA internal-node malformed inputs and bounds\n");
    const size_t payloadSize = 480;
    const std::vector<uint8_t> sep = physicalKey(encodedText("m"), 1, 5);
    const std::vector<ChildBytes> children = {
        {2, std::vector<uint8_t>()}, {3, sep}
    };
    const std::vector<uint8_t> key = physicalKey(encodedText("m"), 1, 0);
    std::vector<uint8_t> valid = makeInternalPayload(payloadSize, 37, children);
    GxIndexInternalNodeCursor cursor = {};
    GxIndexInternalNodeError error = GxIndexInternalNodeError::None;
    CHECK(gxIndexInternalNodeCursorInit(valid.data(), 23, 8, 37, &cursor,
                                        &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::HeaderTruncated);

    std::vector<uint8_t> damaged = valid;
    storeLe32(damaged.data() + 16, 1);
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::ImpossibleChildCount);

    damaged = valid;
    storeLe32(damaged.data() + 16,
              static_cast<uint32_t>((payloadSize - kIndexInternalHeaderSize) /
                                    (kIndexSlotSize +
                                     kIndexInternalEntryOverhead) + 1));
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::ImpossibleChildCount);

    damaged = valid;
    const size_t firstSlot = damaged.size() - kIndexSlotSize;
    const uint32_t firstOffset = loadLe32(damaged.data() + firstSlot);
    storeLe64(damaged.data() + firstOffset, 0);
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::ChildPageIdInvalid);

    damaged = valid;
    const size_t secondSlot = damaged.size() - 2u * kIndexSlotSize;
    const uint32_t secondOffset = loadLe32(damaged.data() + secondSlot);
    storeLe32(damaged.data() + secondOffset + 8, 0xFFFFFFFFu);
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::SeparatorLengthInvalid);

    damaged = valid;
    storeLe32(damaged.data() + secondOffset + 8, 2);
    storeLe32(damaged.data() + secondSlot + 4,
              static_cast<uint32_t>(kIndexInternalEntryOverhead + 2));
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::SeparatorMissingPhysicalLocator);

    damaged = makeInternalPayload(
        payloadSize, 37,
        {{2, std::vector<uint8_t>()}, {3, sep},
         {4, physicalKey(encodedText("t"), 1, 5)}});
    const size_t orderedSecondSlot = damaged.size() - 2u * kIndexSlotSize;
    const uint32_t orderedSecondOffset = loadLe32(
        damaged.data() + orderedSecondSlot);
    std::vector<uint8_t> reverse = physicalKey(encodedText("z"), 1, 5);
    std::memcpy(damaged.data() + orderedSecondOffset +
                    kIndexInternalEntryOverhead,
                reverse.data(), reverse.size());
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::SeparatorsNotIncreasing);

    damaged = valid;
    const size_t firstEntrySlot = damaged.size() - kIndexSlotSize;
    const size_t secondEntrySlot = damaged.size() - 2u * kIndexSlotSize;
    storeLe32(damaged.data() + secondEntrySlot,
              loadLe32(damaged.data() + firstEntrySlot));
    storeLe32(damaged.data() + secondEntrySlot + 4,
              loadLe32(damaged.data() + firstEntrySlot + 4));
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::EntryRegionsOverlap);

    const std::vector<uint8_t> duplicateChildren = makeInternalPayload(
        payloadSize, 37, {{2, std::vector<uint8_t>()}, {2, sep}});
    CHECK(gxIndexInternalNodeCursorInit(duplicateChildren.data(),
                                        duplicateChildren.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::DuplicateChildPage);

    CHECK(gxIndexInternalNodeSelectChild(
              valid.data(), valid.size(), 3, 37, key.data(), key.size(),
              reinterpret_cast<uint64_t*>(valid.data() + 4), nullptr,
              &error) == GxStatus::InvalidArgument);

    damaged = valid;
    const size_t secondChildSlot = damaged.size() - 2u * kIndexSlotSize;
    storeLe32(damaged.data() + secondChildSlot, 0xFFFFFFFFu);
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::EntryOutOfRange);

    damaged = valid;
    storeLe32(damaged.data(), 0);
    CHECK(gxIndexInternalNodeCursorInit(damaged.data(), damaged.size(), 8, 37,
                                        &cursor, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexInternalNodeError::SignatureMismatch);
}

struct ManualTree {
    MemoryPageAccess pages;
    uint64_t root;
    uint64_t leftLeaf;
    uint64_t middleLeaf;
    uint64_t rightLeaf;
    uint64_t leftInternal;
    uint64_t middleInternal;
    uint64_t rightInternal;

    ManualTree() : pages(), root(0), leftLeaf(0), middleLeaf(0), rightLeaf(0),
                   leftInternal(0), middleInternal(0), rightInternal(0) {
        uint64_t ignored = 0;
        pages.allocatePage(PageType::Data, ignored);
        leftLeaf = appendPage(pages, PageType::IndexLeaf,
                              makeEmptyLeafPayload(480, 37));
        middleLeaf = appendPage(pages, PageType::IndexLeaf,
                                makeEmptyLeafPayload(480, 37));
        rightLeaf = appendPage(pages, PageType::IndexLeaf,
                               makeEmptyLeafPayload(480, 37));
        const std::vector<uint8_t> sepF = physicalKey(encodedText("f"), 1, 0);
        const std::vector<uint8_t> sepP = physicalKey(encodedText("p"), 1, 0);
        const std::vector<uint8_t> sepX = physicalKey(encodedText("x"), 1, 0);
        leftInternal = appendPage(
            pages, PageType::IndexInternal,
            makeInternalPayload(480, 37, {{leftLeaf, std::vector<uint8_t>()},
                                          {middleLeaf, sepF}}));
        middleInternal = appendPage(
            pages, PageType::IndexInternal,
            makeInternalPayload(480, 37, {{middleLeaf, std::vector<uint8_t>()},
                                          {rightLeaf, sepP}}));
        rightInternal = appendPage(
            pages, PageType::IndexInternal,
            makeInternalPayload(480, 37, {{rightLeaf, std::vector<uint8_t>()},
                                          {leftLeaf, sepX}}));
        const std::vector<uint8_t> sepM = physicalKey(encodedText("m"), 1, 0);
        const std::vector<uint8_t> sepT = physicalKey(encodedText("t"), 1, 0);
        root = appendPage(
            pages, PageType::IndexInternal,
            makeInternalPayload(480, 37, {{leftInternal, std::vector<uint8_t>()},
                                          {middleInternal, sepM},
                                          {rightInternal, sepT}}));
    }
};

void testBoundedDescentAndProviderOwnership() {
    std::printf("R3AA bounded multi-level descent and provider ownership\n");
    ManualTree tree;
    ReusingProvider source(tree.pages);
    GxIndexTreeDescentResult result = {};
    const std::vector<uint8_t> keyA = physicalKey(encodedText("a"), 1, 0);
    const std::vector<uint8_t> keyM = physicalKey(encodedText("m"), 1, 0);
    const std::vector<uint8_t> keyR = physicalKey(encodedText("r"), 1, 0);
    const std::vector<uint8_t> keyZ = physicalKey(encodedText("z"), 1, 0);
    struct Case { const std::vector<uint8_t>* key; uint64_t leaf; };
    const Case cases[] = {
        {&keyA, tree.leftLeaf}, {&keyM, tree.middleLeaf},
        {&keyR, tree.rightLeaf}, {&keyZ, tree.leftLeaf}
    };
    const DatabasePage* rootPage = tree.pages.get(tree.root);
    CHECK(rootPage != nullptr);
    const std::vector<uint8_t> rootBefore = rootPage->payload;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        source.acquireCount = 0;
        source.releaseCount = 0;
        CHECK(descend(source, tree.root, *cases[i].key, kMaxIndexDepth,
                      result) == GxIndexTreeDescentStatus::Success);
        CHECK(result.leafPageId == cases[i].leaf);
        CHECK(result.depth == 2 && result.pagesVisited == 3);
        CHECK(source.releaseCount == result.pagesVisited && !source.active);
        CHECK(!source.releaseMismatch);
    }
    CHECK(rootPage->payload == rootBefore);

    std::vector<std::vector<uint8_t> > pageImages;
    for (uint64_t pageId = 1; pageId < tree.pages.pageCount(); ++pageId) {
        const DatabasePage* page = tree.pages.get(pageId);
        pageImages.push_back(page == nullptr ? std::vector<uint8_t>()
                                              : page->payload);
    }
    for (uint64_t pageId = 1; pageId < tree.pages.pageCount(); ++pageId) {
        const DatabasePage* page = tree.pages.get(pageId);
        CHECK(page != nullptr && page->payload == pageImages[pageId - 1]);
    }

    source.acquireCount = 0;
    source.releaseCount = 0;
    CHECK(descend(source, tree.root, keyA, 0, result) ==
          GxIndexTreeDescentStatus::TraversalLimit);
    CHECK(source.releaseCount == 1 && !source.active);
    CHECK(result.leafPageId == 0);
    CHECK(descend(source, tree.root, keyA, 1, result) ==
          GxIndexTreeDescentStatus::TraversalLimit);
    CHECK(result.pagesVisited == 2 && source.releaseCount == 3);

    source.acquireCount = 0;
    source.releaseCount = 0;
    source.failOnAcquire = 2;
    CHECK(descend(source, tree.root, keyA, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::ProviderFailure);
    CHECK(result.providerStatus == GxIndexPageProviderStatus::Error);
    CHECK(source.releaseCount == 1 && !source.active);
    source.failOnAcquire = 0;

    source.acquireCount = 0;
    source.releaseCount = 0;
    source.missingPageId = tree.leftInternal;
    CHECK(descend(source, tree.root, keyA, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::MissingPage);
    CHECK(source.releaseCount == 1 && !source.active);
    source.missingPageId = 0;

    source.acquireCount = 0;
    source.releaseCount = 0;
    source.overridePageId = true;
    source.overrideViewPageId = tree.root + 1;
    CHECK(descend(source, tree.root, keyA, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::CorruptPage);
    CHECK(result.error == GxIndexTreeDescentError::PageIdentityMismatch);
    CHECK(source.releaseCount == 1 && !source.active);
    source.overridePageId = false;

    source.acquireCount = 0;
    source.releaseCount = 0;
    source.overrideType = true;
    source.overridePageType = static_cast<uint16_t>(PageType::Data);
    CHECK(descend(source, tree.root, keyA, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::CorruptPage);
    CHECK(result.error == GxIndexTreeDescentError::InvalidPageType);
    CHECK(source.releaseCount == 1 && !source.active);
    source.overrideType = false;

    const uint64_t beforeWrites = tree.pages.writeCount();
    CHECK(tree.pages.writeCount() == beforeWrites);
}

void testLeafRootAndOneLevelDescent() {
    std::printf("R3AA leaf-root and single-level descent\n");
    MemoryPageAccess leafPages;
    uint64_t rowPage = 0;
    CHECK(leafPages.allocatePage(PageType::Data, rowPage).isOk());
    IndexTree emptyTree(leafPages, DbType::Text, 37);
    uint64_t emptyRoot = 0;
    CHECK(emptyTree.createEmpty(emptyRoot).isOk());
    ReusingProvider leafProvider(leafPages);
    GxIndexPageProvider provider = providerApi(leafProvider);
    const uint8_t emptyLogicalPhysicalKey[12] = {};
    uint64_t visited[kMaxIndexDepth + 1] = {};
    GxIndexTreeDescentResult result = {};
    CHECK(gxIndexTreeDescendToLeaf(
              &provider, emptyRoot, leafPages.pageCount(), 37,
              emptyLogicalPhysicalKey, sizeof(emptyLogicalPhysicalKey), 0,
              visited, sizeof(visited) / sizeof(visited[0]), &result) ==
          GxIndexTreeDescentStatus::Success);
    CHECK(result.leafPageId == emptyRoot && result.depth == 0 &&
          result.pagesVisited == 1);
    CHECK(leafProvider.releaseCount == 1 && !leafProvider.active);
    leafProvider.acquireCount = 0;
    leafProvider.releaseCount = 0;
    CHECK(gxIndexTreeDescendToLeaf(
              &provider, emptyRoot, leafPages.pageCount(), 37,
              emptyLogicalPhysicalKey, sizeof(emptyLogicalPhysicalKey), 0,
              visited, sizeof(visited) / sizeof(visited[0]), &result) ==
          GxIndexTreeDescentStatus::Success);
    CHECK(leafProvider.releaseCount == 1 && !leafProvider.active);

    GxIndexTreeDescentResult unchanged = {};
    unchanged.leafPageId = 77;
    const uint8_t invalidPhysicalKey[11] = {};
    CHECK(gxIndexTreeDescendToLeaf(
              &provider, emptyRoot, leafPages.pageCount(), 37,
              invalidPhysicalKey, sizeof(invalidPhysicalKey), 0, visited,
              sizeof(visited) / sizeof(visited[0]), &unchanged) ==
          GxIndexTreeDescentStatus::InvalidArgument);
    CHECK(unchanged.leafPageId == 77);

    MemoryPageAccess oneLevelPages;
    CHECK(oneLevelPages.allocatePage(PageType::Data, rowPage).isOk());
    const uint64_t left = appendPage(
        oneLevelPages, PageType::IndexLeaf, makeEmptyLeafPayload(480, 37));
    const uint64_t right = appendPage(
        oneLevelPages, PageType::IndexLeaf, makeEmptyLeafPayload(480, 37));
    const std::vector<uint8_t> middle = physicalKey(encodedText("m"), 1, 0);
    const uint64_t root = appendPage(
        oneLevelPages, PageType::IndexInternal,
        makeInternalPayload(480, 37, {{left, std::vector<uint8_t>()},
                                     {right, middle}}));
    ReusingProvider oneLevelProvider(oneLevelPages);
    const std::vector<uint8_t> before = physicalKey(encodedText("a"), 1, 0);
    CHECK(descend(oneLevelProvider, root, before, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::Success);
    CHECK(result.leafPageId == left && result.depth == 1 &&
          result.pagesVisited == 2);
    oneLevelProvider.acquireCount = 0;
    oneLevelProvider.releaseCount = 0;
    const std::vector<uint8_t> after = physicalKey(encodedText("z"), 1, 0);
    CHECK(descend(oneLevelProvider, root, after, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::Success);
    CHECK(result.leafPageId == right && result.depth == 1 &&
          result.pagesVisited == 2);
}

void testCyclesAndMalformedDescentPages() {
    std::printf("R3AA cycle, malformed-page, and invalid-child descent\n");
    MemoryPageAccess pages;
    uint64_t data = 0;
    pages.allocatePage(PageType::Data, data);
    const uint64_t leaf = appendPage(pages, PageType::IndexLeaf,
                                     makeEmptyLeafPayload(480, 37));
    const std::vector<uint8_t> sep = physicalKey(encodedText("m"), 1, 1);
    const uint64_t root = appendPage(
        pages, PageType::IndexInternal,
        makeInternalPayload(480, 37, {{leaf, std::vector<uint8_t>()},
                                      {data, sep}}));
    const std::vector<uint8_t> key = physicalKey(encodedText("a"), 1, 0);
    ReusingProvider source(pages);
    GxIndexTreeDescentResult result = {};

    DatabasePage rootCopy = *pages.get(root);
    std::vector<uint8_t> malformed = rootCopy.payload;
    storeLe64(malformed.data() + kIndexInternalHeaderSize, root);
    rootCopy.payload = malformed;
    pages.writePage(rootCopy);
    CHECK(descend(source, root, key, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::Cycle);
    CHECK(result.pagesVisited == 1 && source.releaseCount == 1);

    // Rebuild a two-node cycle; both page payloads remain valid local nodes.
    uint64_t other = appendPage(
        pages, PageType::IndexInternal,
        makeInternalPayload(480, 37, {{root, std::vector<uint8_t>()},
                                      {leaf, sep}}));
    rootCopy = *pages.get(root);
    rootCopy.payload = makeInternalPayload(
        480, 37, {{other, std::vector<uint8_t>()}, {data, sep}});
    pages.writePage(rootCopy);
    source.acquireCount = 0;
    source.releaseCount = 0;
    CHECK(descend(source, root, key, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::Cycle);
    CHECK(result.pagesVisited == 2 && source.releaseCount == 2);
    CHECK(!source.active);

    rootCopy = *pages.get(root);
    rootCopy.payload = makeInternalPayload(
        480, 37, {{pages.pageCount(), std::vector<uint8_t>()}, {leaf, sep}});
    pages.writePage(rootCopy);
    source.acquireCount = 0;
    source.releaseCount = 0;
    CHECK(descend(source, root, key, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::CorruptPage);
    CHECK(result.internalNodeError ==
          GxIndexInternalNodeError::ChildPageIdInvalid);
    CHECK(source.releaseCount == 1 && !source.active);

    rootCopy = *pages.get(root);
    rootCopy.payload = makeInternalPayload(
        480, 37, {{leaf, std::vector<uint8_t>()}, {other, sep}});
    storeLe32(rootCopy.payload.data() + 16, 0xFFFFFFFFu);
    pages.writePage(rootCopy);
    source.acquireCount = 0;
    source.releaseCount = 0;
    CHECK(descend(source, root, key, kMaxIndexDepth, result) ==
          GxIndexTreeDescentStatus::CorruptPage);
    CHECK(result.internalNodeError ==
          GxIndexInternalNodeError::ImpossibleChildCount);
    CHECK(source.releaseCount == 1 && !source.active);

    DatabasePage badLeaf = *pages.get(leaf);
    storeLe32(badLeaf.payload.data() + 20, 99);
    pages.writePage(badLeaf);
    GxIndexTreeDescentResult leafResult = {};
    ReusingProvider leafProvider(pages);
    const std::vector<uint8_t> leafProbe = physicalKey(encodedText("a"), 1, 0);
    CHECK(descend(leafProvider, leaf, leafProbe, 0, leafResult) ==
          GxIndexTreeDescentStatus::CorruptPage);
    CHECK(leafResult.error == GxIndexTreeDescentError::LeafOwnerMismatch);
    CHECK(leafProvider.releaseCount == 1 && !leafProvider.active);
}

uint64_t legacyFindLeaf(const MemoryPageAccess& pages, uint64_t root,
                        const std::vector<uint8_t>& physicalSearchKey) {
    uint64_t current = root;
    for (uint32_t depth = 0; depth <= kMaxIndexDepth; ++depth) {
        const DatabasePage* page = pages.get(current);
        if (page == nullptr) return 0;
        if (page->type == PageType::IndexLeaf) return current;
        if (page->type != PageType::IndexInternal ||
            page->payload.size() < kIndexInternalHeaderSize) return 0;
        const uint32_t childCount = loadLe32(page->payload.data() + 16);
        if (childCount == 0) return 0;
        size_t best = 0;
        for (uint32_t i = 1; i < childCount; ++i) {
            const size_t slot = page->payload.size() -
                kIndexSlotSize * (static_cast<size_t>(i) + 1u);
            const uint32_t offset = loadLe32(page->payload.data() + slot);
            const uint32_t length = loadLe32(page->payload.data() + slot + 4);
            if (offset > page->payload.size() ||
                length > page->payload.size() - offset || length < 12) return 0;
            const uint8_t* record = page->payload.data() + offset;
            const uint32_t separatorLength = loadLe32(record + 8);
            if (separatorLength > length - 12) return 0;
            if (separatorLength == 0) {
                best = i;
                continue;
            }
            if (byteCompare(record + 12, separatorLength,
                            physicalSearchKey.data(),
                            physicalSearchKey.size()) <= 0) {
                best = i;
            } else {
                break;
            }
        }
        const size_t slot = page->payload.size() -
            kIndexSlotSize * (best + 1u);
        const uint32_t offset = loadLe32(page->payload.data() + slot);
        current = loadLe64(page->payload.data() + offset);
    }
    return 0;
}

std::vector<IndexEntry> legacyLookupAll(
    const MemoryPageAccess& pages, uint64_t root,
    const std::vector<uint8_t>& logicalKey) {
    const uint64_t leaf = legacyFindLeaf(
        pages, root, physicalKey(logicalKey, 0, 0));
    std::vector<IndexEntry> entries;
    uint64_t current = leaf;
    uint64_t hops = 0;
    while (current != 0 && hops++ < pages.pageCount()) {
        const DatabasePage* page = pages.get(current);
        if (page == nullptr || page->type != PageType::IndexLeaf) return {};
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(page->payload.data(), page->payload.size(),
                                  &cursor, &error) != GxStatus::Ok) return {};
        GxIndexLeafEntryView entry = {};
        bool hasEntry = false;
        for (;;) {
            if (gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                          &error) != GxStatus::Ok) return {};
            if (!hasEntry) break;
            if (entry.keyLength == logicalKey.size() &&
                (entry.keyLength == 0 ||
                 std::memcmp(entry.keyBytes, logicalKey.data(),
                             entry.keyLength) == 0)) {
                IndexEntry found;
                found.key.assign(entry.keyBytes,
                                 entry.keyBytes + entry.keyLength);
                found.locator.pageId = entry.locatorPageId;
                found.locator.slot = entry.locatorSlot;
                entries.push_back(found);
            }
        }
        current = cursor.page.nextLeafPageId;
    }
    return entries;
}

std::vector<uint64_t> matchingLeafPages(
    const MemoryPageAccess& pages, uint64_t first,
    const std::vector<uint8_t>& key) {
    std::vector<uint64_t> result;
    uint64_t current = first;
    uint64_t hops = 0;
    while (current != 0 && hops++ < pages.pageCount()) {
        const DatabasePage* page = pages.get(current);
        if (page == nullptr) break;
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(page->payload.data(), page->payload.size(),
                                  &cursor, &error) != GxStatus::Ok) break;
        GxIndexLeafEntryView entry = {};
        bool hasEntry = false;
        for (;;) {
            if (gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                          &error) != GxStatus::Ok || !hasEntry) {
                break;
            }
            if (entry.keyLength == key.size() &&
                std::memcmp(entry.keyBytes, key.data(), key.size()) == 0) {
                result.push_back(current);
                break;
            }
        }
        current = cursor.page.nextLeafPageId;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct SeedEntry {
    std::vector<uint8_t> key;
    uint32_t slot;
};

bool createProductionTree(MemoryPageAccess& pages, uint64_t& root,
                          std::vector<uint8_t>& duplicateKey,
                          uint32_t duplicateCount) {
    uint64_t rowPage = 0;
    if (!pages.allocatePage(PageType::Data, rowPage).isOk()) return false;
    GxAllocator allocator = {};
    if (gxHostedAllocatorInit(&allocator, 65536) != GxStatus::Ok) return false;
    IndexTree tree(pages, DbType::Text, 37, nullptr, &allocator);
    if (!tree.createEmpty(root).isOk()) return false;
    duplicateKey = encodedText("m-dup");
    std::vector<SeedEntry> seeds;
    for (uint32_t i = 0; i < 160; ++i) {
        char value[16];
        std::snprintf(value, sizeof(value), "a%05u", i);
        seeds.push_back({encodedText(value), 1u + i});
    }
    for (uint32_t i = 0; i < duplicateCount; ++i) {
        seeds.push_back({duplicateKey, 1000u + i});
    }
    for (uint32_t i = 0; i < 160; ++i) {
        char value[16];
        std::snprintf(value, sizeof(value), "z%05u", i);
        seeds.push_back({encodedText(value), 2000u + i});
    }
    std::stable_sort(seeds.begin(), seeds.end(), [](const SeedEntry& left,
                                                    const SeedEntry& right) {
        if (byteLess(left.key, right.key)) return true;
        if (byteLess(right.key, left.key)) return false;
        return left.slot < right.slot;
    });
    for (size_t i = 0; i < seeds.size(); ++i) {
        uint64_t newRoot = 0;
        bool inserted = false;
        const DbResult result = tree.insert(
            seeds[i].key, RowLocator(rowPage, seeds[i].slot), root, false,
            newRoot, inserted);
        if (!result.isOk() || !inserted) return false;
        root = newRoot;
    }
    IndexValidation validation;
    return tree.validate(root, validation).isOk() && validation.ok &&
           validation.height >= 3;
}

bool sameEntries(const std::vector<IndexEntry>& left,
                 const std::vector<IndexEntry>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].key != right[i].key ||
            left[i].locator.pageId != right[i].locator.pageId ||
            left[i].locator.slot != right[i].locator.slot) return false;
    }
    return true;
}

bool writeProductionFixture(const char* path) {
    MemoryPageAccess pages;
    uint64_t root = 0;
    std::vector<uint8_t> duplicateKey;
    const uint32_t duplicateCount = 100;
    if (!createProductionTree(pages, root, duplicateKey, duplicateCount)) {
        return false;
    }
    std::vector<uint64_t> treePageIds;
    const std::vector<uint8_t> physicalProbe =
        physicalKey(duplicateKey, 0, 0);
    ReusingProvider source(pages);
    GxIndexPageProvider provider = providerApi(source);
    uint64_t visitedPath[kMaxIndexDepth + 1] = {};
    GxIndexTreeDescentResult descentResult = {};
    if (gxIndexTreeDescendToLeaf(
            &provider, root, pages.pageCount(), 37, physicalProbe.data(),
            physicalProbe.size(), kMaxIndexDepth, visitedPath,
            sizeof(visitedPath) / sizeof(visitedPath[0]), &descentResult) !=
            GxIndexTreeDescentStatus::Success || descentResult.depth < 2) {
        return false;
    }
    for (uint64_t i = 0; i < descentResult.pagesVisited; ++i) {
        treePageIds.push_back(visitedPath[i]);
    }
    uint64_t currentLeaf = descentResult.leafPageId;
    for (uint64_t hop = 0; currentLeaf != 0 && hop < pages.pageCount(); ++hop) {
        const DatabasePage* page = pages.get(currentLeaf);
        if (page == nullptr || page->type != PageType::IndexLeaf) return false;
        if (std::find(treePageIds.begin(), treePageIds.end(), currentLeaf) ==
            treePageIds.end()) treePageIds.push_back(currentLeaf);
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError leafError = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(page->payload.data(), page->payload.size(),
                                  &cursor, &leafError) != GxStatus::Ok) {
            return false;
        }
        bool reachedKeyBoundary = false;
        for (;;) {
            GxIndexLeafEntryView entry = {};
            bool hasEntry = false;
            if (gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                          &leafError) != GxStatus::Ok) {
                return false;
            }
            if (!hasEntry) break;
            if (byteCompare(entry.keyBytes, entry.keyLength,
                            duplicateKey.data(), duplicateKey.size()) > 0) {
                reachedKeyBoundary = true;
                break;
            }
        }
        if (reachedKeyBoundary) break;
        currentLeaf = cursor.page.nextLeafPageId;
    }
    if (currentLeaf == 0) return false;
    std::sort(treePageIds.begin(), treePageIds.end());
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) return false;
    output << pages.pageSize() << ' ' << pages.pageCount() << ' ' << 37 << ' '
           << root << ' ' << duplicateCount << ' ' << treePageIds.size() << '\n';
    const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < treePageIds.size(); ++i) {
        const uint64_t pageId = treePageIds[i];
        const DatabasePage* page = pages.get(pageId);
        if (page == nullptr || page->payload.size() != 480) return false;
        output << pageId << ' ' << static_cast<uint16_t>(page->type) << ' ';
        for (size_t i = 0; i < page->payload.size(); ++i) {
            const uint8_t byte = page->payload[i];
            output << digits[byte >> 4] << digits[byte & 0x0Fu];
        }
        output << '\n';
    }
    return output.good();
}

void testProductionTreeEquivalence() {
    std::printf("R3AA production-written multi-level tree equivalence\n");
    MemoryPageAccess pages;
    uint64_t root = 0;
    std::vector<uint8_t> duplicateKey;
    const uint32_t duplicateCount = 100;
    CHECK(createProductionTree(pages, root, duplicateKey, duplicateCount));
    if (root == 0) return;

    GxAllocator traversalAllocator = {};
    CHECK(gxHostedAllocatorInit(&traversalAllocator, 65536) == GxStatus::Ok);
    IndexTree tree(pages, DbType::Text, 37, nullptr, &traversalAllocator);
    std::vector<uint8_t> duplicateProbe = physicalKey(duplicateKey, 0, 0);
    ReusingProvider source(pages);
    GxIndexTreeDescentResult descentResult = {};
    const uint64_t legacyDuplicateLeaf =
        legacyFindLeaf(pages, root, duplicateProbe);
    CHECK(legacyDuplicateLeaf != 0);
    CHECK(descend(source, root, duplicateProbe, kMaxIndexDepth,
                  descentResult) == GxIndexTreeDescentStatus::Success);
    CHECK(descentResult.leafPageId == legacyDuplicateLeaf);
    CHECK(descentResult.depth >= 2 && descentResult.pagesVisited >= 3);
    CHECK(source.releaseCount == descentResult.pagesVisited && !source.active);
    const std::vector<uint64_t> duplicatePages = matchingLeafPages(
        pages, descentResult.leafPageId, duplicateKey);
    CHECK(duplicatePages.size() > 1);

    std::vector<IndexEntry> actual;
    DbResult lookupResult = tree.lookupAll(duplicateKey, root, actual);
    if (!lookupResult.isOk()) {
        std::printf("  lookupAll duplicate returned %s: %s\n",
                    dbStatusName(lookupResult.status()),
                    lookupResult.message().c_str());
    }
    CHECK(lookupResult.isOk());
    const std::vector<IndexEntry> legacy =
        legacyLookupAll(pages, root, duplicateKey);
    CHECK(actual.size() == duplicateCount);
    CHECK(sameEntries(actual, legacy));
    for (uint32_t i = 0; i < duplicateCount && i < actual.size(); ++i) {
        CHECK(actual[i].locator.pageId == 1);
        CHECK(actual[i].locator.slot == 1000u + i);
    }

    const std::vector<std::vector<uint8_t> > lookupKeys = {
        encodedText("a00000"), encodedText("a00083"), duplicateKey,
        encodedText("z00071"), encodedText("z00159"), encodedText("m-miss")
    };
    for (size_t i = 0; i < lookupKeys.size(); ++i) {
        actual.clear();
        lookupResult = tree.lookupAll(lookupKeys[i], root, actual);
        if (!lookupResult.isOk()) {
            std::printf("  lookupAll query %zu returned %s: %s\n", i,
                        dbStatusName(lookupResult.status()),
                        lookupResult.message().c_str());
        }
        CHECK(lookupResult.isOk());
        const std::vector<IndexEntry> beforeExtraction =
            legacyLookupAll(pages, root, lookupKeys[i]);
        CHECK(sameEntries(actual, beforeExtraction));
    }

    // Compare the exact hosted pre-extraction decision at every separator in
    // the root and in the next internal layer, including equality and adjacent
    // locator positions.
    std::vector<uint64_t> pending(1, root);
    std::vector<uint64_t> seen;
    while (!pending.empty() && seen.size() < pages.pageCount()) {
        const uint64_t current = pending.back();
        pending.pop_back();
        if (std::find(seen.begin(), seen.end(), current) != seen.end()) continue;
        seen.push_back(current);
        const DatabasePage* page = pages.get(current);
        if (page == nullptr || page->type != PageType::IndexInternal) continue;
        const uint32_t count = loadLe32(page->payload.data() + 16);
        for (uint32_t i = 0; i < count; ++i) {
            const size_t slot = page->payload.size() -
                kIndexSlotSize * (static_cast<size_t>(i) + 1u);
            const uint32_t offset = loadLe32(page->payload.data() + slot);
            const uint32_t length = loadLe32(page->payload.data() + slot + 4);
            const uint8_t* record = page->payload.data() + offset;
            const uint64_t childId = loadLe64(record);
            pending.push_back(childId);
            if (i == 0) continue;
            const uint32_t boundLength = loadLe32(record + 8);
            if (boundLength < 12 || boundLength > length - 12) continue;
            std::vector<uint8_t> boundary(
                record + kIndexInternalEntryOverhead,
                record + kIndexInternalEntryOverhead + boundLength);
            std::vector<std::vector<uint8_t> > probes;
            probes.push_back(boundary);
            std::vector<uint8_t> before = boundary;
            if (before.back() != 0) {
                --before.back();
                probes.push_back(before);
            }
            std::vector<uint8_t> after = boundary;
            if (after.back() != 0xFFu) {
                ++after.back();
                probes.push_back(after);
            }
            for (size_t p = 0; p < probes.size(); ++p) {
                const uint64_t prior = legacyFindLeaf(pages, root, probes[p]);
                source.acquireCount = 0;
                source.releaseCount = 0;
                CHECK(descend(source, root, probes[p], kMaxIndexDepth,
                              descentResult) ==
                      GxIndexTreeDescentStatus::Success);
                CHECK(descentResult.leafPageId == prior);
                CHECK(source.releaseCount == descentResult.pagesVisited &&
                      !source.active);
            }
        }
    }

    std::vector<IndexEntry> notFound;
    const std::vector<uint8_t> beyond = encodedText("zzzzzz");
    lookupResult = tree.lookupAll(beyond, root, notFound);
    if (!lookupResult.isOk()) {
        std::printf("  lookupAll miss returned %s: %s\n",
                    dbStatusName(lookupResult.status()),
                    lookupResult.message().c_str());
    }
    CHECK(lookupResult.isOk());
    CHECK(notFound.empty());
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 &&
        std::strcmp(argv[1], "--write-freestanding-fixture") == 0) {
        if (!writeProductionFixture(argv[2])) {
            std::printf("failed to write R3AA production tree fixture\n");
            return 2;
        }
        return 0;
    }
    std::printf("NDB1R4P11R3AA portable internal-node descent tests\n");
    testInternalReaderAndRouting();
    testInternalCorruption();
    testBoundedDescentAndProviderOwnership();
    testLeafRootAndOneLevelDescent();
    testCyclesAndMalformedDescentPages();
    testProductionTreeEquivalence();
    std::printf("NDB1R4P11R3AA: %d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
