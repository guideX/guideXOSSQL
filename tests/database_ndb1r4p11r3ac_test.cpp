#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "database_index.h"
#include "portable/gx_allocator_hosted.h"
#include "portable/gx_index_internal_node.h"
#include "portable/gx_index_leaf_entry.h"
#include "portable/gx_index_lookup.h"

using namespace gxos::db;

namespace {

int gPass = 0;
int gFail = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) {
        ++gPass;
    } else {
        ++gFail;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

uint32_t loadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

void storeLe64(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

bool parseHex(const std::string& encoded, std::vector<uint8_t>& bytes) {
    if ((encoded.size() & 1u) != 0) return false;
    bytes.clear();
    bytes.reserve(encoded.size() / 2u);
    for (size_t i = 0; i < encoded.size(); i += 2u) {
        const char digits[] = "0123456789abcdef";
        const char highChar = static_cast<char>(
            std::tolower(static_cast<unsigned char>(encoded[i])));
        const char lowChar = static_cast<char>(
            std::tolower(static_cast<unsigned char>(encoded[i + 1u])));
        const char* high = std::find(digits, digits + 16, highChar);
        const char* low = std::find(digits, digits + 16, lowChar);
        if (high == digits + 16 || low == digits + 16) return false;
        bytes.push_back(static_cast<uint8_t>(
            ((high - digits) << 4) | (low - digits)));
    }
    return true;
}

class MemoryPageAccess : public PageAccess {
public:
    explicit MemoryPageAccess(uint32_t pageSize = 512)
        : _pageSize(pageSize), _pages(1), _present(1, true),
          _reads(0), _failOnRead(0) {
        _pages[0].pageId = 0;
    }

    uint32_t pageSize() const override { return _pageSize; }
    uint64_t pageCount() const override { return _pages.size(); }
    bool isReadOnly() const override { return false; }

    DbResult readPage(uint64_t pageId, DatabasePage& out) override {
        ++_reads;
        if (_failOnRead != 0 && _reads == _failOnRead) {
            return DbResult::error(DbStatus::IoError, "R3AC injected read failure");
        }
        if (pageId == 0 || pageId >= _pages.size() ||
            !_present[static_cast<size_t>(pageId)]) {
            return DbResult::error(DbStatus::OutOfBounds, "R3AC page missing");
        }
        out = _pages[static_cast<size_t>(pageId)];
        return DbResult::ok();
    }

    DbResult writePage(const DatabasePage& page) override {
        if (page.pageId == 0 || page.pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds, "R3AC page out of range");
        }
        _pages[static_cast<size_t>(page.pageId)] = page;
        _present[static_cast<size_t>(page.pageId)] = true;
        return DbResult::ok();
    }

    DbResult allocatePage(PageType type, uint64_t& outPageId) override {
        DatabasePage page;
        page.pageId = _pages.size();
        page.type = type;
        page.payload.assign(DatabasePage::payloadCapacity(_pageSize), 0);
        page.payloadSize = static_cast<uint32_t>(page.payload.size());
        _pages.push_back(page);
        _present.push_back(true);
        outPageId = page.pageId;
        return DbResult::ok();
    }

    void initializeSparse(uint64_t count) {
        _pages.assign(static_cast<size_t>(count), DatabasePage());
        _present.assign(static_cast<size_t>(count), false);
        _pages[0].pageId = 0;
        _present[0] = true;
        _reads = 0;
    }

    void put(const DatabasePage& page) {
        _pages[static_cast<size_t>(page.pageId)] = page;
        _present[static_cast<size_t>(page.pageId)] = true;
    }

    void markMissing(uint64_t pageId) {
        if (pageId < _present.size()) _present[static_cast<size_t>(pageId)] = false;
    }

    const DatabasePage* get(uint64_t pageId) const {
        return pageId < _pages.size() && _present[static_cast<size_t>(pageId)]
            ? &_pages[static_cast<size_t>(pageId)] : nullptr;
    }

    DatabasePage* getMutable(uint64_t pageId) {
        return pageId < _pages.size() && _present[static_cast<size_t>(pageId)]
            ? &_pages[static_cast<size_t>(pageId)] : nullptr;
    }

    void failOnRead(uint64_t call) { _reads = 0; _failOnRead = call; }
    uint64_t reads() const { return _reads; }

private:
    uint32_t _pageSize;
    std::vector<DatabasePage> _pages;
    std::vector<bool> _present;
    uint64_t _reads;
    uint64_t _failOnRead;
};

bool loadFixture(MemoryPageAccess& pages, uint64_t& root, uint32_t& owner,
                 uint32_t& duplicateCount) {
    std::ifstream input(GX_R3AC_FIXTURE_PATH);
    uint32_t pageSize = 0;
    uint64_t pageCount = 0;
    size_t pageTotal = 0;
    if (!input || !(input >> pageSize >> pageCount >> owner >> root >>
                    duplicateCount >> pageTotal) ||
        pageSize != 512 || pageCount < 2 || pageTotal < 3 ||
        root == 0 || root >= pageCount || duplicateCount < 2) return false;
    pages.initializeSparse(pageCount);
    for (size_t i = 0; i < pageTotal; ++i) {
        DatabasePage page;
        uint16_t type = 0;
        std::string hex;
        if (!(input >> page.pageId >> type >> hex) || page.pageId == 0 ||
            page.pageId >= pageCount || !parseHex(hex, page.payload) ||
            page.payload.size() != 480) return false;
        page.type = static_cast<PageType>(type);
        page.payloadSize = static_cast<uint32_t>(page.payload.size());
        pages.put(page);
    }
    std::string extra;
    return !(input >> extra);
}

std::vector<uint8_t> textKey(const std::string& value) {
    std::vector<uint8_t> key(1, 1);
    key.insert(key.end(), value.begin(), value.end());
    return key;
}

struct Seed {
    std::vector<uint8_t> key;
    uint32_t slot;
};

bool buildMultiLevelTextTree(MemoryPageAccess& pages, GxAllocator& allocator,
                             uint64_t& root, uint32_t& height) {
    uint64_t rowPage = 0;
    if (!pages.allocatePage(PageType::Data, rowPage).isOk()) return false;
    IndexTree tree(pages, DbType::Text, 37, nullptr, &allocator);
    if (!tree.createEmpty(root).isOk()) return false;
    const std::vector<uint8_t> duplicate = textKey("m-dup");
    std::vector<Seed> seeds;
    for (uint32_t i = 0; i < 160; ++i) {
        char value[16];
        std::snprintf(value, sizeof(value), "a%05u", i);
        seeds.push_back(Seed{textKey(value), 1u + i});
    }
    for (uint32_t i = 0; i < 100; ++i) {
        seeds.push_back(Seed{duplicate, 1000u + i});
    }
    for (uint32_t i = 0; i < 160; ++i) {
        char value[16];
        std::snprintf(value, sizeof(value), "z%05u", i);
        seeds.push_back(Seed{textKey(value), 2000u + i});
    }
    std::stable_sort(seeds.begin(), seeds.end(),
        [](const Seed& left, const Seed& right) {
            if (left.key != right.key) return left.key < right.key;
            return left.slot < right.slot;
        });
    for (size_t i = 0; i < seeds.size(); ++i) {
        uint64_t updated = 0;
        bool inserted = false;
        if (!tree.insert(seeds[i].key, RowLocator(rowPage, seeds[i].slot),
                         root, false, updated, inserted).isOk() || !inserted) {
            return false;
        }
        root = updated;
    }
    IndexValidation validation;
    if (!tree.validate(root, validation).isOk() || !validation.ok) return false;
    height = validation.height;
    return true;
}

bool writeRangeFixture(const char* path, const MemoryPageAccess& pages,
                       uint64_t root, uint32_t duplicateCount) {
    if (path == nullptr || root == 0 || pages.pageCount() < 2) return false;
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) return false;
    const size_t pageTotal = static_cast<size_t>(pages.pageCount() - 1);
    output << pages.pageSize() << ' ' << pages.pageCount() << ' ' << 37 << ' '
           << root << ' ' << duplicateCount << ' ' << pageTotal << '\n';
    static const char digits[] = "0123456789abcdef";
    for (uint64_t pageId = 1; pageId < pages.pageCount(); ++pageId) {
        const DatabasePage* page = pages.get(pageId);
        if (page == nullptr || page->payload.size() != 480) return false;
        output << pageId << ' ' << static_cast<uint16_t>(page->type) << ' ';
        for (size_t i = 0; i < page->payload.size(); ++i) {
            output << digits[page->payload[i] >> 4]
                   << digits[page->payload[i] & 0x0Fu];
        }
        output << '\n';
    }
    output.flush();
    return output.good();
}

struct RangeProvider {
    const MemoryPageAccess& pages;
    std::vector<uint8_t> buffer;
    uint64_t acquireCount;
    uint64_t successCount;
    uint64_t releaseCount;
    uint64_t failOnAcquire;
    uint64_t missingPageId;
    uint64_t shortPageId;
    size_t shortLength;
    const uint64_t* visitorCount;
    bool failAfterVisitor;
    bool active;
    bool releaseMismatch;
    uint64_t requestedPageId;
    const uint8_t* activeBytes;

    explicit RangeProvider(const MemoryPageAccess& source)
        : pages(source), buffer(DatabasePage::payloadCapacity(source.pageSize()), 0),
          acquireCount(0), successCount(0), releaseCount(0), failOnAcquire(0),
          missingPageId(0), shortPageId(0), shortLength(0), visitorCount(nullptr),
          failAfterVisitor(false), active(false), releaseMismatch(false),
          requestedPageId(0), activeBytes(nullptr) {}
};

GxIndexPageProviderStatus acquirePage(void* opaque, uint64_t pageId,
                                      GxIndexPageView* out) {
    RangeProvider* provider = static_cast<RangeProvider*>(opaque);
    if (provider == nullptr || out == nullptr || provider->active) {
        return GxIndexPageProviderStatus::Error;
    }
    ++provider->acquireCount;
    *out = GxIndexPageView();
    if (provider->failOnAcquire != 0 &&
        provider->acquireCount == provider->failOnAcquire) {
        return GxIndexPageProviderStatus::Error;
    }
    if (provider->failAfterVisitor && provider->visitorCount != nullptr &&
        *provider->visitorCount != 0) {
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
    size_t length = page->payload.size();
    if (provider->shortPageId == pageId && provider->shortLength < length) {
        length = provider->shortLength;
    }
    out->bytes = provider->buffer.data();
    out->length = length;
    out->pageId = page->pageId;
    out->pageType = static_cast<uint16_t>(page->type);
    out->token = provider;
    provider->active = true;
    provider->requestedPageId = pageId;
    provider->activeBytes = out->bytes;
    ++provider->successCount;
    return GxIndexPageProviderStatus::Ok;
}

void releasePage(void* opaque, uint64_t pageId, const GxIndexPageView* view) {
    RangeProvider* provider = static_cast<RangeProvider*>(opaque);
    if (provider == nullptr) return;
    if (!provider->active || view == nullptr || view->token != provider ||
        pageId != provider->requestedPageId || view->bytes != provider->activeBytes) {
        provider->releaseMismatch = true;
    } else {
        ++provider->releaseCount;
    }
    provider->active = false;
}

struct CollectedRange {
    std::vector<IndexEntry> entries;
    uint32_t stopAfter;
    uint32_t failAfter;
    bool fail;

    CollectedRange() : entries(), stopAfter(0), failAfter(0), fail(false) {}
};

GxIndexRangeScanVisitControl collectRange(void* opaque, const uint8_t* key,
                                          size_t keyLength, uint64_t pageId,
                                          uint32_t slot) {
    CollectedRange* output = static_cast<CollectedRange*>(opaque);
    if (output == nullptr || (key == nullptr && keyLength != 0)) {
        return GxIndexRangeScanVisitControl::Error;
    }
    IndexEntry entry;
    if (keyLength != 0) entry.key.assign(key, key + keyLength);
    entry.locator = RowLocator(pageId, slot);
    output->entries.push_back(entry);
    if (output->fail &&
        (output->failAfter == 0 || output->entries.size() >= output->failAfter)) {
        return GxIndexRangeScanVisitControl::Error;
    }
    return output->stopAfter != 0 && output->entries.size() >= output->stopAfter
        ? GxIndexRangeScanVisitControl::Stop
        : GxIndexRangeScanVisitControl::Continue;
}

GxIndexLookupStatus portableScan(
    RangeProvider& state, uint64_t root, uint64_t pageCount, uint32_t owner,
    bool hasLower, const std::vector<uint8_t>& lower, bool lowerInclusive,
    bool hasUpper, const std::vector<uint8_t>& upper, bool upperInclusive,
    uint64_t maxLeafPages, CollectedRange& output, GxIndexLookupResult& result) {
    GxIndexPageProvider provider = {&state, acquirePage, releasePage};
    uint64_t path[kMaxIndexDepth + 1] = {};
    uint8_t probe[512] = {};
    uint8_t previous[480] = {};
    return gxIndexRangeScan(
        &provider, root, pageCount, owner,
        hasLower, hasLower ? lower.data() : nullptr, lower.size(), lowerInclusive,
        hasUpper, hasUpper ? upper.data() : nullptr, upper.size(), upperInclusive,
        kMaxIndexDepth, path, sizeof(path) / sizeof(path[0]), maxLeafPages,
        probe, sizeof(probe), previous, sizeof(previous), collectRange,
        &output, &result);
}

bool equalRows(const std::vector<IndexEntry>& left,
               const std::vector<IndexEntry>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].key != right[i].key ||
            left[i].locator.pageId != right[i].locator.pageId ||
            left[i].locator.slot != right[i].locator.slot) return false;
    }
    return true;
}

void testProductionTreeRanges() {
    std::printf("R3AC production-written multi-level tree and hosted range adapter\n");
    MemoryPageAccess fixture;
    uint64_t fixtureRoot = 0;
    uint32_t fixtureOwner = 0;
    uint32_t fixtureDuplicates = 0;
    CHECK(loadFixture(fixture, fixtureRoot, fixtureOwner, fixtureDuplicates));
    CHECK(fixtureOwner == 37 && fixtureDuplicates == 100);

    MemoryPageAccess pages;
    uint64_t root = 0;
    uint32_t height = 0;
    const uint32_t owner = 37;
    const uint32_t duplicateCount = 100;
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 131072) == GxStatus::Ok);
    CHECK(buildMultiLevelTextTree(pages, allocator, root, height));
    CHECK(height >= 3);
    if (root == 0) return;
    IndexTree tree(pages, DbType::Text, owner, nullptr, &allocator);
    IndexRangeBound bound;
    std::vector<IndexEntry> hosted;

    bound.hasLower = true;
    bound.lowerInclusive = true;
    bound.lower = DbValue::text("m-dup");
    bound.hasUpper = true;
    bound.upperInclusive = true;
    bound.upper = DbValue::text("m-dup");
    const DbResult firstHostedRange = tree.rangeScan(bound, root, hosted);
    if (!firstHostedRange.isOk()) {
        std::printf("  first hosted range error %d: %s\n",
                    static_cast<int>(firstHostedRange.status()),
                    firstHostedRange.message().c_str());
    }
    CHECK(firstHostedRange.isOk());
    CHECK(hosted.size() == duplicateCount);
    for (size_t i = 0; i < hosted.size(); ++i) {
        CHECK(hosted[i].key == textKey("m-dup"));
        CHECK(hosted[i].locator.pageId == 1);
        CHECK(hosted[i].locator.slot == 1000u + i);
    }

    RangeProvider provider(pages);
    CollectedRange portable;
    GxIndexLookupResult result = {};
    const std::vector<uint8_t> duplicateKey = textKey("m-dup");
    const GxIndexLookupStatus firstPortableStatus = portableScan(
        provider, root, pages.pageCount(), owner, true, duplicateKey, true,
        true, duplicateKey, true, pages.pageCount() - 1, portable, result);
    if (firstPortableStatus != GxIndexLookupStatus::Complete) {
        std::printf("  first portable range status %d, error %d, leaf %d, provider %d\n",
                    static_cast<int>(firstPortableStatus),
                    static_cast<int>(result.error),
                    static_cast<int>(result.leafTraversalStatus),
                    static_cast<int>(result.providerStatus));
    }
    CHECK(firstPortableStatus == GxIndexLookupStatus::Complete);
    CHECK(equalRows(portable.entries, hosted));
    CHECK(result.depth >= 2 && result.descentPagesVisited >= 3);
    CHECK(result.leafPagesVisited >= 2);
    CHECK(provider.releaseCount == provider.successCount && !provider.active);

    bound.lowerInclusive = false;
    CHECK(tree.rangeScan(bound, root, hosted).isOk());
    CHECK(hosted.empty());
    bound.lowerInclusive = true;
    bound.upperInclusive = false;
    CHECK(tree.rangeScan(bound, root, hosted).isOk());
    CHECK(hosted.empty());

    bound.upperInclusive = true;
    bound.lower = DbValue::text("z00005");
    bound.upper = DbValue::text("a00000");
    CHECK(tree.rangeScan(bound, root, hosted).isOk());
    CHECK(hosted.empty());

    bound.hasLower = false;
    bound.hasUpper = false;
    const DbResult unboundedResult = tree.rangeScan(bound, root, hosted);
    if (!unboundedResult.isOk()) {
        std::printf("  unbounded hosted range error %d: %s\n",
                    static_cast<int>(unboundedResult.status()),
                    unboundedResult.message().c_str());
    }
    CHECK(unboundedResult.isOk());
    CHECK(hosted.size() == 420);
    for (size_t i = 1; i < hosted.size(); ++i) {
        CHECK(hosted[i - 1].key <= hosted[i].key);
        if (hosted[i - 1].key == hosted[i].key) {
            CHECK(hosted[i - 1].locator.slot < hosted[i].locator.slot);
        }
    }

    bound.hasLower = true;
    bound.lower = DbValue::text("m-dup");
    bound.lowerInclusive = true;
    bound.hasUpper = true;
    bound.upper = DbValue::text("z00005");
    bound.upperInclusive = true;
    const DbResult lowerToUpperResult = tree.rangeScan(bound, root, hosted);
    if (!lowerToUpperResult.isOk()) {
        std::printf("  bounded hosted range error %d: %s\n",
                    static_cast<int>(lowerToUpperResult.status()),
                    lowerToUpperResult.message().c_str());
    }
    CHECK(lowerToUpperResult.isOk());
    CHECK(hosted.size() == duplicateCount + 6);
    std::vector<IndexEntry> repeated;
    CHECK(tree.rangeScan(bound, root, repeated).isOk());
    CHECK(equalRows(hosted, repeated));
    bound.lowerInclusive = false;
    CHECK(tree.rangeScan(bound, root, hosted).isOk());
    CHECK(hosted.size() == 6);

    bound.lower = DbValue::text("x");
    bound.upper = DbValue::text("y");
    bound.lowerInclusive = true;
    CHECK(tree.rangeScan(bound, root, hosted).isOk());
    CHECK(hosted.empty());

    // A page-provider error after the first leaf has delivered rows must not
    // publish the staged partial collection to the caller.
    MemoryPageAccess failingPages;
    uint64_t failingRoot = 0;
    uint32_t failingHeight = 0;
    GxAllocator failingAllocator = {};
    CHECK(gxHostedAllocatorInit(&failingAllocator, 131072) == GxStatus::Ok);
    CHECK(buildMultiLevelTextTree(failingPages, failingAllocator,
                                  failingRoot, failingHeight));
    if (failingRoot != 0) {
        IndexTree failingTree(failingPages, DbType::Text, owner,
                              nullptr, &failingAllocator);
        IndexRangeBound unbounded;
        std::vector<IndexEntry> partial(1);
        failingPages.failOnRead(failingHeight + 2u);
        const DbResult failure = failingTree.rangeScan(unbounded, failingRoot, partial);
        CHECK(!failure.isOk());
        CHECK(partial.empty());
        CHECK(failingPages.reads() == failingHeight + 2u);
    }
}

void testEmptyAndLeafOnlyTrees() {
    std::printf("R3AC empty and leaf-only production trees\n");
    MemoryPageAccess pages;
    uint64_t rowPage = 0;
    CHECK(pages.allocatePage(PageType::Data, rowPage).isOk());
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 32768) == GxStatus::Ok);
    IndexTree tree(pages, DbType::Text, 37, nullptr, &allocator);
    uint64_t root = 0;
    CHECK(tree.createEmpty(root).isOk());
    CHECK(pages.get(root) != nullptr &&
          pages.get(root)->type == PageType::IndexLeaf);

    IndexRangeBound unbounded;
    std::vector<IndexEntry> found(1);
    CHECK(tree.rangeScan(unbounded, root, found).isOk());
    CHECK(found.empty());

    uint64_t updated = 0;
    bool inserted = false;
    CHECK(tree.insert(textKey("b"), RowLocator(rowPage, 20), root, false,
                      updated, inserted).isOk() && inserted);
    root = updated;
    IndexRangeBound single;
    single.hasLower = true;
    single.lower = DbValue::text("b");
    single.hasUpper = true;
    single.upper = DbValue::text("b");
    CHECK(tree.rangeScan(single, root, found).isOk());
    CHECK(found.size() == 1 && found[0].key == textKey("b") &&
          found[0].locator.slot == 20);

    const char* keys[] = {"a", "b", "b", "c"};
    const uint32_t slots[] = {10, 21, 22, 30};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(tree.insert(textKey(keys[i]), RowLocator(rowPage, slots[i]),
                          root, false, updated, inserted).isOk() && inserted);
        root = updated;
    }
    CHECK(pages.get(root) != nullptr &&
          pages.get(root)->type == PageType::IndexLeaf);

    IndexRangeBound firstToLast;
    firstToLast.hasLower = true;
    firstToLast.lower = DbValue::text("a");
    firstToLast.hasUpper = true;
    firstToLast.upper = DbValue::text("c");
    CHECK(tree.rangeScan(firstToLast, root, found).isOk());
    CHECK(found.size() == 5);
    const uint32_t expected[] = {10, 20, 21, 22, 30};
    for (size_t i = 0; i < found.size(); ++i) {
        CHECK(found[i].locator.slot == expected[i]);
    }
    IndexRangeBound withinLeaf;
    withinLeaf.hasLower = true;
    withinLeaf.lower = DbValue::text("b");
    withinLeaf.hasUpper = true;
    withinLeaf.upper = DbValue::text("b");
    CHECK(tree.rangeScan(withinLeaf, root, found).isOk());
    CHECK(found.size() == 3 && found[0].locator.slot == 20 &&
          found[1].locator.slot == 21 && found[2].locator.slot == 22);
}

void testHostedCycleAndCorruptionHandling() {
    std::printf("R3AC hosted cycle and malformed-page handling\n");
    MemoryPageAccess pages;
    uint64_t root = 0;
    uint32_t height = 0;
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 131072) == GxStatus::Ok);
    CHECK(buildMultiLevelTextTree(pages, allocator, root, height));
    if (root == 0) return;
    IndexRangeBound unbounded;

    MemoryPageAccess invalidChildPages = pages;
    DatabasePage* invalidRoot = invalidChildPages.getMutable(root);
    CHECK(invalidRoot != nullptr && invalidRoot->type == PageType::IndexInternal);
    if (invalidRoot != nullptr) {
        const uint32_t childCount = loadLe32(invalidRoot->payload.data() + 16);
        const size_t slotOffset = invalidRoot->payload.size() - 8u;
        const uint32_t entryOffset = loadLe32(
            invalidRoot->payload.data() + slotOffset);
        CHECK(childCount >= 2 && entryOffset < invalidRoot->payload.size());
        if (childCount >= 2 && entryOffset < invalidRoot->payload.size()) {
            storeLe64(invalidRoot->payload.data() + entryOffset,
                      invalidChildPages.pageCount());
            GxAllocator invalidAllocator = {};
            CHECK(gxHostedAllocatorInit(&invalidAllocator, 32768) == GxStatus::Ok);
            IndexTree invalidTree(invalidChildPages, DbType::Text, 37,
                                  nullptr, &invalidAllocator);
            std::vector<IndexEntry> output(1);
            CHECK(invalidTree.rangeScan(unbounded, root, output).status() ==
                  DbStatus::CorruptPage);
            CHECK(output.empty());
        }
    }

    MemoryPageAccess internalCyclePages = pages;
    DatabasePage* cycleRoot = internalCyclePages.getMutable(root);
    CHECK(cycleRoot != nullptr && cycleRoot->type == PageType::IndexInternal);
    if (cycleRoot != nullptr) {
        const std::vector<uint8_t> duplicate = textKey("m-dup");
        uint8_t searchKey[64] = {};
        std::copy(duplicate.begin(), duplicate.end(), searchKey);
        uint64_t routedPage = 0;
        uint32_t routedChild = 0;
        GxIndexInternalNodeError nodeError = GxIndexInternalNodeError::None;
        CHECK(gxIndexInternalNodeSelectChild(
            cycleRoot->payload.data(), cycleRoot->payload.size(),
            internalCyclePages.pageCount(), 37, searchKey,
            duplicate.size() + 12u, &routedPage, &routedChild,
            &nodeError) == GxStatus::Ok);
        GxIndexInternalNodeCursor cursor = {};
        CHECK(gxIndexInternalNodeCursorInit(
            cycleRoot->payload.data(), cycleRoot->payload.size(),
            internalCyclePages.pageCount(), 37, &cursor,
            &nodeError) == GxStatus::Ok);
        GxIndexInternalChildView child = {};
        bool hasChild = false;
        for (uint32_t i = 0; i <= routedChild; ++i) {
            CHECK(gxIndexInternalNodeCursorReadNext(
                &cursor, &child, &hasChild, &nodeError) == GxStatus::Ok &&
                hasChild);
        }
        CHECK(hasChild && child.childPageId == routedPage);
        if (hasChild) {
            storeLe64(cycleRoot->payload.data() + child.entryOffset, root);
            GxAllocator cycleAllocator = {};
            CHECK(gxHostedAllocatorInit(&cycleAllocator, 32768) == GxStatus::Ok);
            IndexTree cycleTree(internalCyclePages, DbType::Text, 37,
                                nullptr, &cycleAllocator);
            IndexRangeBound cycleBound;
            cycleBound.hasLower = true;
            cycleBound.lower = DbValue::text("m-dup");
            std::vector<IndexEntry> output(1);
            CHECK(cycleTree.rangeScan(cycleBound, root, output).status() ==
                  DbStatus::CorruptPage);
            CHECK(output.empty());
        }
    }

    MemoryPageAccess malformedLeafPages = pages;
    uint64_t firstLeafId = 0;
    for (uint64_t pageId = 1; pageId < malformedLeafPages.pageCount(); ++pageId) {
        const DatabasePage* page = malformedLeafPages.get(pageId);
        if (page != nullptr && page->type == PageType::IndexLeaf) {
            firstLeafId = pageId;
            break;
        }
    }
    DatabasePage* malformedLeaf = malformedLeafPages.getMutable(firstLeafId);
    CHECK(malformedLeaf != nullptr && firstLeafId != 0);
    if (malformedLeaf != nullptr) {
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError leafError = GxIndexLeafError::None;
        GxIndexLeafEntryView entry = {};
        bool hasEntry = false;
        CHECK(gxIndexLeafCursorInit(malformedLeaf->payload.data(),
                                    malformedLeaf->payload.size(), &cursor,
                                    &leafError) == GxStatus::Ok);
        CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                        &leafError) == GxStatus::Ok && hasEntry);
        if (hasEntry) {
            const size_t keyOffset = static_cast<size_t>(
                entry.keyBytes - malformedLeaf->payload.data());
            malformedLeaf->payload[keyOffset] = 0xFFu;
            GxAllocator malformedAllocator = {};
            CHECK(gxHostedAllocatorInit(&malformedAllocator, 32768) == GxStatus::Ok);
            IndexTree malformedTree(malformedLeafPages, DbType::Text, 37,
                                    nullptr, &malformedAllocator);
            std::vector<IndexEntry> output(1);
            CHECK(malformedTree.rangeScan(unbounded, root, output).status() ==
                  DbStatus::CorruptPage);
            CHECK(output.empty());
        }
    }

    MemoryPageAccess leafCyclePages = pages;
    uint64_t firstLeaf = 0;
    uint64_t leaf = 0;
    for (uint64_t pageId = 1; pageId < leafCyclePages.pageCount(); ++pageId) {
        const DatabasePage* page = leafCyclePages.get(pageId);
        if (page != nullptr && page->type == PageType::IndexLeaf) {
            leaf = pageId;
            break;
        }
    }
    firstLeaf = leaf;
    uint64_t lastLeaf = 0;
    for (uint64_t hops = 0; leaf != 0 && hops < leafCyclePages.pageCount(); ++hops) {
        const DatabasePage* page = leafCyclePages.get(leaf);
        if (page == nullptr) break;
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError leafError = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(page->payload.data(), page->payload.size(),
                                  &cursor, &leafError) != GxStatus::Ok) break;
        lastLeaf = leaf;
        leaf = cursor.page.nextLeafPageId;
    }
    CHECK(firstLeaf != 0 && lastLeaf != 0 && leaf == 0);
    DatabasePage* terminal = leafCyclePages.getMutable(lastLeaf);
    CHECK(terminal != nullptr);
    if (terminal != nullptr) {
        storeLe64(terminal->payload.data() + 8, firstLeaf);
        GxAllocator cycleAllocator = {};
        CHECK(gxHostedAllocatorInit(&cycleAllocator, 131072) == GxStatus::Ok);
        IndexTree cycleTree(leafCyclePages, DbType::Text, 37,
                            nullptr, &cycleAllocator);
        std::vector<IndexEntry> output(1);
        CHECK(cycleTree.rangeScan(unbounded, root, output).status() ==
              DbStatus::CorruptPage);
        CHECK(output.empty());
    }
}

void testPortableRangeVisitorAndFailures() {
    std::printf("R3AC portable bounds, visitor outcomes, and provider ownership\n");
    MemoryPageAccess pages;
    uint64_t root = 0;
    uint32_t height = 0;
    const uint32_t owner = 37;
    const uint32_t duplicateCount = 100;
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 131072) == GxStatus::Ok);
    CHECK(buildMultiLevelTextTree(pages, allocator, root, height));
    CHECK(height >= 3 && duplicateCount == 100);
    if (root == 0) return;
    const std::vector<uint8_t> duplicate = textKey("m-dup");
    RangeProvider provider(pages);
    CollectedRange output;
    GxIndexLookupResult result = {};
    const GxIndexLookupStatus upperOnlyStatus = portableScan(
        provider, root, pages.pageCount(), owner, false, std::vector<uint8_t>(),
        true, true, duplicate, true, pages.pageCount() - 1, output, result);
    if (upperOnlyStatus != GxIndexLookupStatus::Complete) {
        std::printf("  upper-only range status %d, error %d, leaf %d, provider %d\n",
                    static_cast<int>(upperOnlyStatus),
                    static_cast<int>(result.error),
                    static_cast<int>(result.leafTraversalStatus),
                    static_cast<int>(result.providerStatus));
    }
    CHECK(upperOnlyStatus == GxIndexLookupStatus::Complete);
    CHECK(output.entries.size() == 260);
    CHECK(provider.releaseCount == provider.successCount && !provider.active);

    RangeProvider exclusiveProvider(pages);
    CollectedRange exclusive;
    const GxIndexLookupStatus lowerOnlyStatus = portableScan(
        exclusiveProvider, root, pages.pageCount(), owner, true, duplicate,
        false, false, std::vector<uint8_t>(), true, pages.pageCount() - 1,
        exclusive, result);
    if (lowerOnlyStatus != GxIndexLookupStatus::Complete) {
        std::printf("  lower-only range status %d, error %d, leaf %d, provider %d\n",
                    static_cast<int>(lowerOnlyStatus),
                    static_cast<int>(result.error),
                    static_cast<int>(result.leafTraversalStatus),
                    static_cast<int>(result.providerStatus));
    }
    CHECK(lowerOnlyStatus == GxIndexLookupStatus::Complete);
    CHECK(exclusive.entries.size() == 160);
    CHECK(exclusive.entries[0].key == textKey("z00000"));

    RangeProvider stoppedProvider(pages);
    CollectedRange stopped;
    stopped.stopAfter = 1;
    CHECK(portableScan(stoppedProvider, root, pages.pageCount(), owner, true,
                       duplicate, true, false, std::vector<uint8_t>(), true,
                       pages.pageCount() - 1, stopped, result) ==
          GxIndexLookupStatus::VisitorStopped);
    CHECK(stopped.entries.size() == 1 && result.matchesVisited == 1);
    CHECK(stoppedProvider.releaseCount == stoppedProvider.successCount &&
          !stoppedProvider.active);

    RangeProvider stoppedManyProvider(pages);
    CollectedRange stoppedMany;
    stoppedMany.stopAfter = 5;
    CHECK(portableScan(stoppedManyProvider, root, pages.pageCount(), owner,
                       true, duplicate, true, true, duplicate, true,
                       pages.pageCount() - 1, stoppedMany, result) ==
          GxIndexLookupStatus::VisitorStopped);
    CHECK(stoppedMany.entries.size() == 5 && result.matchesVisited == 5);
    CHECK(stoppedManyProvider.releaseCount == stoppedManyProvider.successCount &&
          !stoppedManyProvider.active);

    RangeProvider failedVisitorProvider(pages);
    CollectedRange visitorFailure;
    visitorFailure.stopAfter = 3;
    visitorFailure.fail = true;
    visitorFailure.failAfter = 3;
    CHECK(portableScan(failedVisitorProvider, root, pages.pageCount(), owner,
                       true, duplicate, true, true, duplicate, true,
                       pages.pageCount() - 1, visitorFailure, result) ==
          GxIndexLookupStatus::VisitorFailure);
    CHECK(visitorFailure.entries.size() == 3);
    CHECK(failedVisitorProvider.releaseCount ==
          failedVisitorProvider.successCount && !failedVisitorProvider.active);

    RangeProvider providerFailure(pages);
    CollectedRange partial;
    providerFailure.visitorCount = nullptr;
    providerFailure.failAfterVisitor = false;
    // Force a missing successor after the first matching leaf has returned.
    // Locate the first duplicate leaf and follow its encoded next-page link.
    uint8_t probe[64] = {};
    const size_t probeLen = duplicate.size() + 12;
    std::copy(duplicate.begin(), duplicate.end(), probe);
    uint64_t path[kMaxIndexDepth + 1] = {};
    GxIndexPageProvider pageApi = {&providerFailure, acquirePage, releasePage};
    GxIndexTreeDescentResult descent = {};
    CHECK(gxIndexTreeDescendToLeaf(
        &pageApi, root, pages.pageCount(), owner, probe, probeLen,
        kMaxIndexDepth, path, sizeof(path) / sizeof(path[0]), &descent) ==
        GxIndexTreeDescentStatus::Success);
    uint64_t currentLeaf = descent.leafPageId;
    uint64_t nextLeaf = 0;
    for (uint64_t hops = 0; currentLeaf != 0 && hops < pages.pageCount(); ++hops) {
        const DatabasePage* currentPage = pages.get(currentLeaf);
        if (currentPage == nullptr) break;
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(currentPage->payload.data(), currentPage->payload.size(),
                                  &cursor, &error) == GxStatus::Ok) {
            bool containsDuplicate = false;
            for (;;) {
                GxIndexLeafEntryView entry = {};
                bool hasEntry = false;
                if (gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                              &error) != GxStatus::Ok ||
                    !hasEntry) break;
                if (entry.keyLength == duplicate.size() &&
                    std::equal(entry.keyBytes, entry.keyBytes + entry.keyLength,
                               duplicate.begin())) {
                    containsDuplicate = true;
                }
            }
            if (containsDuplicate) {
                nextLeaf = cursor.page.nextLeafPageId;
                break;
            }
            currentLeaf = cursor.page.nextLeafPageId;
        }
    }
    if (nextLeaf != 0) {
        providerFailure.missingPageId = nextLeaf;
        CHECK(portableScan(providerFailure, root, pages.pageCount(), owner,
                           true, duplicate, true, true, duplicate, true,
                           pages.pageCount() - 1, partial, result) ==
              GxIndexLookupStatus::MissingPage);
        if (partial.entries.empty()) {
            std::printf("  missing-page range delivered %llu callbacks; acquire=%llu start=%llu next=%llu leafPages=%llu\n",
                        static_cast<unsigned long long>(result.matchesVisited),
                        static_cast<unsigned long long>(providerFailure.acquireCount),
                        static_cast<unsigned long long>(descent.leafPageId),
                        static_cast<unsigned long long>(nextLeaf),
                        static_cast<unsigned long long>(result.leafPagesVisited));
        }
        CHECK(!partial.entries.empty());
        CHECK(providerFailure.releaseCount == providerFailure.successCount &&
              !providerFailure.active);
    }

    MemoryPageAccess badBoundaryPages = pages;
    const std::vector<uint8_t> firstZKey = textKey("z00000");
    uint64_t zLeafPageId = 0;
    for (uint64_t pageId = 1; pageId < pages.pageCount(); ++pageId) {
        const DatabasePage* page = pages.get(pageId);
        if (page == nullptr || page->type != PageType::IndexLeaf) continue;
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(page->payload.data(), page->payload.size(),
                                  &cursor, &error) != GxStatus::Ok) continue;
        for (;;) {
            GxIndexLeafEntryView entry = {};
            bool hasEntry = false;
            if (gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                          &error) != GxStatus::Ok || !hasEntry) break;
            if (entry.keyLength == firstZKey.size() &&
                std::equal(entry.keyBytes, entry.keyBytes + entry.keyLength,
                           firstZKey.begin())) {
                zLeafPageId = pageId;
                break;
            }
        }
        if (zLeafPageId != 0) break;
    }
    const DatabasePage* zLeaf = pages.get(zLeafPageId);
    uint64_t successorOfZ = 0;
    if (zLeaf != nullptr) {
        GxIndexLeafCursor zCursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(zLeaf->payload.data(), zLeaf->payload.size(),
                                  &zCursor, &error) == GxStatus::Ok) {
            successorOfZ = zCursor.page.nextLeafPageId;
        }
    }
    CHECK(zLeafPageId != 0 && successorOfZ != 0);
    DatabasePage* badSuccessor = badBoundaryPages.getMutable(successorOfZ);
    if (badSuccessor != nullptr) {
        GxIndexLeafCursor badCursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        GxIndexLeafEntryView firstEntry = {};
        bool hasEntry = false;
        CHECK(gxIndexLeafCursorInit(badSuccessor->payload.data(),
                                    badSuccessor->payload.size(), &badCursor,
                                    &error) == GxStatus::Ok);
        CHECK(gxIndexLeafCursorReadNext(&badCursor, &firstEntry, &hasEntry,
                                        &error) == GxStatus::Ok && hasEntry);
        if (hasEntry && firstEntry.keyLength > 1) {
            const size_t keyOffset = static_cast<size_t>(
                firstEntry.keyBytes - badSuccessor->payload.data());
            badSuccessor->payload[keyOffset + 1] = static_cast<uint8_t>('a');
            RangeProvider corruptOrderProvider(badBoundaryPages);
            CollectedRange corruptOrder;
            CHECK(portableScan(corruptOrderProvider, root, pages.pageCount(),
                               owner, true, duplicate, true, true, duplicate,
                               true, pages.pageCount() - 1, corruptOrder,
                               result) == GxIndexLookupStatus::CorruptPage);
            CHECK(!corruptOrder.entries.empty());
            CHECK(corruptOrderProvider.releaseCount ==
                  corruptOrderProvider.successCount &&
                  !corruptOrderProvider.active);

            GxAllocator corruptAllocator = {};
            CHECK(gxHostedAllocatorInit(&corruptAllocator, 131072) == GxStatus::Ok);
            IndexTree corruptTree(badBoundaryPages, DbType::Text, owner,
                                  nullptr, &corruptAllocator);
            IndexRangeBound duplicateRange;
            duplicateRange.hasLower = true;
            duplicateRange.lower = DbValue::text("m-dup");
            duplicateRange.hasUpper = true;
            duplicateRange.upper = DbValue::text("m-dup");
            std::vector<IndexEntry> unpublished(1);
            CHECK(!corruptTree.rangeScan(duplicateRange, root,
                                         unpublished).isOk());
            CHECK(unpublished.empty());
        }
    }

    RangeProvider budgetProvider(pages);
    CollectedRange budget;
    CHECK(portableScan(budgetProvider, root, pages.pageCount(), owner, true,
                       duplicate, true, true, duplicate, true, 1, budget,
                       result) == GxIndexLookupStatus::TraversalLimit);
    CHECK(budgetProvider.releaseCount == budgetProvider.successCount &&
          !budgetProvider.active);

    RangeProvider emptyProvider(pages);
    CollectedRange empty;
    CHECK(portableScan(emptyProvider, root, pages.pageCount(), owner, true,
                       textKey("z"), true, true, textKey("a"), true,
                       pages.pageCount() - 1, empty, result) ==
          GxIndexLookupStatus::Complete);
    CHECK(empty.entries.empty() && emptyProvider.acquireCount == 0);

    RangeProvider equalExclusiveProvider(pages);
    CollectedRange equalExclusive;
    CHECK(portableScan(equalExclusiveProvider, root, pages.pageCount(), owner,
                       true, duplicate, false, true, duplicate, true,
                       pages.pageCount() - 1, equalExclusive, result) ==
          GxIndexLookupStatus::Complete);
    CHECK(equalExclusive.entries.empty() &&
          equalExclusiveProvider.acquireCount == 0);

    RangeProvider shortProvider(pages);
    shortProvider.shortPageId = root;
    shortProvider.shortLength = 16;
    CollectedRange shortOutput;
    CHECK(portableScan(shortProvider, root, pages.pageCount(), owner, true,
                       duplicate, true, true, duplicate, true,
                       pages.pageCount() - 1, shortOutput, result) ==
          GxIndexLookupStatus::CorruptPage);
    CHECK(shortProvider.releaseCount == shortProvider.successCount &&
          !shortProvider.active);

    GxIndexPageProvider rawProvider = {&emptyProvider, acquirePage, releasePage};
    uint64_t invalidPath[kMaxIndexDepth + 1] = {};
    uint8_t invalidProbe[64] = {};
    uint8_t invalidPrevious[480] = {};
    const uint8_t invalidKey[] = {0xFFu};
    GxIndexLookupResult untouched = {};
    untouched.leafPageId = 987;
    CHECK(gxIndexRangeScan(
        &rawProvider, root, pages.pageCount(), owner, true, invalidKey,
        sizeof(invalidKey), true, false, nullptr, 0, true, kMaxIndexDepth,
        invalidPath, sizeof(invalidPath) / sizeof(invalidPath[0]),
        pages.pageCount() - 1, invalidProbe, sizeof(invalidProbe),
        invalidPrevious, sizeof(invalidPrevious), collectRange, &empty,
        &untouched) == GxIndexLookupStatus::InvalidArgument);
    CHECK(untouched.leafPageId == 987);

    const uint8_t oneByteKey[] = {0x01u};
    CHECK(gxIndexRangeScan(
        &rawProvider, root, pages.pageCount(), owner, true, oneByteKey,
        static_cast<size_t>(-1), true, false, nullptr, 0, true,
        kMaxIndexDepth, invalidPath,
        sizeof(invalidPath) / sizeof(invalidPath[0]), pages.pageCount() - 1,
        invalidProbe, sizeof(invalidProbe), invalidPrevious,
        sizeof(invalidPrevious), collectRange, &empty, &untouched) ==
        GxIndexLookupStatus::InvalidArgument);
    CHECK(untouched.leafPageId == 987);

    CHECK(gxIndexRangeScan(
        &rawProvider, pages.pageCount(), pages.pageCount(), owner, false,
        nullptr, 0, true, false, nullptr, 0, true, kMaxIndexDepth,
        invalidPath, sizeof(invalidPath) / sizeof(invalidPath[0]),
        pages.pageCount() - 1, invalidProbe, sizeof(invalidProbe),
        invalidPrevious, sizeof(invalidPrevious), collectRange, &empty,
        &result) == GxIndexLookupStatus::InvalidPageId);
}

void checkTypedRange(
    DbType type, const std::vector<std::pair<DbValue, uint32_t> >& values,
    const IndexRangeBound& bound, const std::vector<uint32_t>& expectedSlots) {
    MemoryPageAccess pages;
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 131072) == GxStatus::Ok);
    uint64_t rowPage = 0;
    CHECK(pages.allocatePage(PageType::Data, rowPage).isOk());
    IndexTree tree(pages, type, 37, nullptr, &allocator);
    uint64_t root = 0;
    CHECK(tree.createEmpty(root).isOk());
    for (size_t i = 0; i < values.size(); ++i) {
        std::vector<uint8_t> key;
        CHECK(encodeIndexKey(type, values[i].first, key));
        uint64_t updated = 0;
        bool inserted = false;
        CHECK(tree.insert(key, RowLocator(rowPage, values[i].second), root,
                          false, updated, inserted).isOk() && inserted);
        root = updated;
    }
    std::vector<IndexEntry> found;
    CHECK(tree.rangeScan(bound, root, found).isOk());
    CHECK(found.size() == expectedSlots.size());
    for (size_t i = 0; i < found.size() && i < expectedSlots.size(); ++i) {
        CHECK(found[i].locator.slot == expectedSlots[i]);
    }
}

void testHostedTypedAndTextEdges() {
    std::printf("R3AC hosted typed bounds and embedded-zero text\n");
    MemoryPageAccess pages;
    uint64_t rowPage = 0;
    CHECK(pages.allocatePage(PageType::Data, rowPage).isOk());
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 131072) == GxStatus::Ok);
    IndexTree tree(pages, DbType::Text, 37, nullptr, &allocator);
    uint64_t root = 0;
    CHECK(tree.createEmpty(root).isOk());
    const std::string values[] = {"", std::string("a\0b", 3), "b", "c"};
    for (uint32_t i = 0; i < 4; ++i) {
        std::vector<uint8_t> key = textKey(values[i]);
        uint64_t updated = 0;
        bool inserted = false;
        CHECK(tree.insert(key, RowLocator(rowPage, i + 1), root, false,
                          updated, inserted).isOk() && inserted);
        root = updated;
    }
    IndexRangeBound exactEmbedded;
    exactEmbedded.hasLower = true;
    exactEmbedded.lower = DbValue::text(std::string("a\0b", 3));
    exactEmbedded.hasUpper = true;
    exactEmbedded.upper = DbValue::text(std::string("a\0b", 3));
    std::vector<IndexEntry> found;
    CHECK(tree.rangeScan(exactEmbedded, root, found).isOk());
    CHECK(found.size() == 1 && found[0].key == textKey(std::string("a\0b", 3)));

    IndexRangeBound badTyped;
    badTyped.hasLower = true;
    badTyped.lower = DbValue::null();
    badTyped.hasUpper = true;
    const uint8_t blobByte = 1;
    badTyped.upper = DbValue::blob(&blobByte, 1);
    CHECK(tree.rangeScan(badTyped, root, found).status() == DbStatus::InvalidArgument);
    CHECK(found.empty());

    std::vector<std::pair<DbValue, uint32_t> > booleans;
    booleans.push_back(std::make_pair(DbValue::null(), 10u));
    booleans.push_back(std::make_pair(DbValue::boolean(false), 20u));
    booleans.push_back(std::make_pair(DbValue::boolean(true), 30u));
    IndexRangeBound booleanRange;
    booleanRange.hasLower = true;
    booleanRange.lower = DbValue::boolean(false);
    booleanRange.lowerInclusive = true;
    booleanRange.hasUpper = true;
    booleanRange.upper = DbValue::boolean(true);
    booleanRange.upperInclusive = false;
    checkTypedRange(DbType::Boolean, booleans, booleanRange,
                    std::vector<uint32_t>{20u});
    booleanRange.lower = DbValue::null();
    booleanRange.upper = DbValue::boolean(false);
    booleanRange.upperInclusive = true;
    checkTypedRange(DbType::Boolean, booleans, booleanRange,
                    std::vector<uint32_t>{10u, 20u});

    std::vector<std::pair<DbValue, uint32_t> > int32Values;
    int32Values.push_back(std::make_pair(
        DbValue::int32(std::numeric_limits<int32_t>::min()), 1u));
    int32Values.push_back(std::make_pair(DbValue::int32(0), 2u));
    int32Values.push_back(std::make_pair(
        DbValue::int32(std::numeric_limits<int32_t>::max()), 3u));
    IndexRangeBound int32Range;
    int32Range.hasLower = true;
    int32Range.lower = DbValue::int32(0);
    int32Range.lowerInclusive = true;
    int32Range.hasUpper = true;
    int32Range.upper = DbValue::int32(std::numeric_limits<int32_t>::max());
    int32Range.upperInclusive = true;
    checkTypedRange(DbType::Int32, int32Values, int32Range,
                    std::vector<uint32_t>{2u, 3u});

    std::vector<std::pair<DbValue, uint32_t> > int64Values;
    int64Values.push_back(std::make_pair(
        DbValue::int64(std::numeric_limits<int64_t>::min()), 4u));
    int64Values.push_back(std::make_pair(DbValue::int64(0), 5u));
    int64Values.push_back(std::make_pair(
        DbValue::int64(std::numeric_limits<int64_t>::max()), 6u));
    IndexRangeBound int64Range;
    int64Range.hasLower = true;
    int64Range.lower = DbValue::int64(0);
    int64Range.lowerInclusive = false;
    int64Range.hasUpper = true;
    int64Range.upper = DbValue::int64(std::numeric_limits<int64_t>::max());
    int64Range.upperInclusive = true;
    checkTypedRange(DbType::Int64, int64Values, int64Range,
                    std::vector<uint32_t>{6u});
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write-fixture") {
        MemoryPageAccess pages;
        GxAllocator allocator = {};
        if (gxHostedAllocatorInit(&allocator, 131072) != GxStatus::Ok) return 2;
        uint64_t root = 0;
        uint32_t height = 0;
        if (!buildMultiLevelTextTree(pages, allocator, root, height) ||
            !writeRangeFixture(argv[2], pages, root, 100)) return 3;
        std::printf("R3AC production-writer fixture: root=%llu height=%u pages=%llu\n",
                    static_cast<unsigned long long>(root), height,
                    static_cast<unsigned long long>(pages.pageCount()));
        return 0;
    }
    if (argc != 1) return 2;
    std::printf("NDB1R4P11R3AC bounded portable index range scan\n");
    testProductionTreeRanges();
    testEmptyAndLeafOnlyTrees();
    testHostedCycleAndCorruptionHandling();
    testPortableRangeVisitorAndFailures();
    testHostedTypedAndTextEdges();
    std::printf("NDB1R4P11R3AC portable bounded range scan: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
