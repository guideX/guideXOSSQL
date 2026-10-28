#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "database_index.h"
#include "database_types.h"
#include "portable/gx_allocator_hosted.h"
#include "portable/gx_index_boolean_key.h"
#include "portable/gx_index_integer_key.h"
#include "portable/gx_index_lookup.h"
#include "portable/gx_index_text_key.h"

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

class MemoryPageAccess : public PageAccess {
public:
    explicit MemoryPageAccess(uint32_t pageBytes = 512)
        : _pageSize(pageBytes), _pages(1), _present(1, true), _writes(0) {
        _pages[0].pageId = 0;
    }

    uint32_t pageSize() const override { return _pageSize; }
    uint64_t pageCount() const override { return _pages.size(); }
    bool isReadOnly() const override { return false; }

    DbResult readPage(uint64_t pageId, DatabasePage& out) override {
        if (pageId == 0 || pageId >= _pages.size() ||
            !_present[static_cast<size_t>(pageId)]) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "R3AB page is missing");
        }
        out = _pages[static_cast<size_t>(pageId)];
        return DbResult::ok();
    }

    DbResult writePage(const DatabasePage& page) override {
        if (page.pageId == 0 || page.pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "R3AB page id is out of range");
        }
        _pages[static_cast<size_t>(page.pageId)] = page;
        _present[static_cast<size_t>(page.pageId)] = true;
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
        _present.push_back(true);
        outPageId = page.pageId;
        return DbResult::ok();
    }

    void initializeSparse(uint64_t pageCount) {
        _pages.assign(static_cast<size_t>(pageCount), DatabasePage());
        _present.assign(static_cast<size_t>(pageCount), false);
        _pages[0].pageId = 0;
        _present[0] = true;
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

    uint64_t writeCount() const { return _writes; }

private:
    uint32_t _pageSize;
    std::vector<DatabasePage> _pages;
    std::vector<bool> _present;
    uint64_t _writes;
};

struct ReusingProvider {
    const MemoryPageAccess& pages;
    std::vector<uint8_t> buffer;
    uint64_t acquireCount;
    uint64_t successfulAcquireCount;
    uint64_t releaseCount;
    uint64_t failOnAcquire;
    uint64_t missingPageId;
    uint64_t overridePageIdFor;
    uint64_t overridePageIdValue;
    uint64_t overrideTypeFor;
    uint16_t overrideTypeValue;
    uint64_t shortPageId;
    size_t shortLength;
    uint64_t aliasPageId;
    const uint8_t* aliasBytes;
    bool hasPageIdOverride;
    bool hasTypeOverride;
    bool hasShortLength;
    bool hasAlias;
    bool active;
    bool releaseMismatch;
    uint64_t activeRequestedPageId;
    const uint8_t* activeBytes;

    explicit ReusingProvider(const MemoryPageAccess& source)
        : pages(source), buffer(DatabasePage::payloadCapacity(source.pageSize()), 0),
          acquireCount(0), successfulAcquireCount(0), releaseCount(0),
          failOnAcquire(0), missingPageId(0), overridePageIdFor(0),
          overridePageIdValue(0), overrideTypeFor(0), overrideTypeValue(0),
          shortPageId(0), shortLength(0), aliasPageId(0), aliasBytes(nullptr),
          hasPageIdOverride(false), hasTypeOverride(false),
          hasShortLength(false), hasAlias(false), active(false),
          releaseMismatch(false), activeRequestedPageId(0), activeBytes(nullptr) {}
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
    std::copy(page->payload.begin(), page->payload.end(),
              provider->buffer.begin());
    size_t length = page->payload.size();
    if (provider->hasShortLength && provider->shortPageId == pageId &&
        provider->shortLength < length) {
        length = provider->shortLength;
    }
    const uint8_t* viewBytes = provider->buffer.data();
    if (provider->hasAlias && provider->aliasPageId == pageId) {
        viewBytes = provider->aliasBytes;
    }
    outView->bytes = viewBytes;
    outView->length = length;
    outView->pageId = provider->hasPageIdOverride &&
        provider->overridePageIdFor == pageId
        ? provider->overridePageIdValue : page->pageId;
    outView->pageType = provider->hasTypeOverride &&
        provider->overrideTypeFor == pageId
        ? provider->overrideTypeValue : static_cast<uint16_t>(page->type);
    outView->token = provider;
    provider->active = true;
    provider->activeRequestedPageId = pageId;
    provider->activeBytes = viewBytes;
    ++provider->successfulAcquireCount;
    return GxIndexPageProviderStatus::Ok;
}

void releasePage(void* opaque, uint64_t pageId,
                 const GxIndexPageView* view) {
    ReusingProvider* provider = static_cast<ReusingProvider*>(opaque);
    if (provider == nullptr) return;
    if (!provider->active || view == nullptr || view->token != provider ||
        pageId != provider->activeRequestedPageId ||
        view->bytes != provider->activeBytes) {
        provider->releaseMismatch = true;
    }
    provider->active = false;
    provider->activeBytes = nullptr;
    ++provider->releaseCount;
    std::fill(provider->buffer.begin(), provider->buffer.end(), 0xDDu);
}

GxIndexPageProvider providerApi(ReusingProvider& provider) {
    GxIndexPageProvider result = {&provider, acquirePage, releasePage};
    return result;
}

struct Collector {
    std::vector<RowLocator> rows;
    size_t stopAfter;
    size_t failAfter;
    size_t callbacks;

    Collector() : rows(), stopAfter(0), failAfter(0), callbacks(0) {}
};

GxIndexLookupVisitControl collect(
    void* opaque, uint64_t locatorPageId, uint32_t locatorSlot) {
    Collector* collector = static_cast<Collector*>(opaque);
    if (collector == nullptr) return GxIndexLookupVisitControl::Error;
    collector->rows.push_back(RowLocator(locatorPageId, locatorSlot));
    ++collector->callbacks;
    if (collector->failAfter != 0 &&
        collector->callbacks == collector->failAfter) {
        return GxIndexLookupVisitControl::Error;
    }
    if (collector->stopAfter != 0 &&
        collector->callbacks == collector->stopAfter) {
        return GxIndexLookupVisitControl::Stop;
    }
    return GxIndexLookupVisitControl::Continue;
}

GxIndexLookupStatus runLookup(
    const MemoryPageAccess& pages, ReusingProvider& pageSource,
    uint64_t rootPageId, const std::vector<uint8_t>& logicalKey,
    Collector& collector, GxIndexLookupResult& outResult,
    uint32_t maxDepth = gxos::db::kMaxIndexDepth,
    uint64_t maxLeafPages = 0) {
    std::vector<uint8_t> lowerBound(logicalKey.size() + 12u, 0);
    std::vector<uint8_t> previousPhysicalKey(
        DatabasePage::payloadCapacity(pages.pageSize()), 0);
    uint64_t visitedPath[gxos::db::kMaxIndexDepth + 1] = {};
    if (maxLeafPages == 0 && pages.pageCount() > 1) {
        maxLeafPages = pages.pageCount() - 1;
    }
    const GxIndexPageProvider provider = providerApi(pageSource);
    return gxIndexLookupAll(
        &provider, rootPageId, pages.pageCount(), 37,
        logicalKey.empty() ? nullptr : logicalKey.data(), logicalKey.size(),
        maxDepth, visitedPath,
        sizeof(visitedPath) / sizeof(visitedPath[0]), maxLeafPages,
        lowerBound.data(), lowerBound.size(), previousPhysicalKey.data(),
        previousPhysicalKey.size(), collect, &collector, &outResult);
}

std::vector<uint8_t> encodedText(const std::string& value) {
    std::vector<uint8_t> encoded(value.size() + 1u, 0);
    size_t length = 0;
    const uint8_t* bytes = value.empty()
        ? nullptr : reinterpret_cast<const uint8_t*>(value.data());
    if (gxEncodeIndexTextKey(bytes, value.size(), encoded.data(),
                             encoded.size(), &length) != GxStatus::Ok) {
        return std::vector<uint8_t>();
    }
    encoded.resize(length);
    return encoded;
}

std::vector<uint8_t> encodedBoolean(bool isNull, bool value) {
    uint8_t bytes[2] = {};
    size_t length = 0;
    if (gxEncodeIndexBooleanKey(isNull, value, bytes, sizeof(bytes),
                                &length) != GxStatus::Ok) {
        return std::vector<uint8_t>();
    }
    return std::vector<uint8_t>(bytes, bytes + length);
}

std::vector<uint8_t> encodedInt32(int32_t value) {
    uint8_t bytes[5] = {};
    size_t length = 0;
    if (gxEncodeIndexInt32Key(false, value, bytes, sizeof(bytes),
                              &length) != GxStatus::Ok) {
        return std::vector<uint8_t>();
    }
    return std::vector<uint8_t>(bytes, bytes + length);
}

std::vector<uint8_t> encodedInt64(int64_t value) {
    uint8_t bytes[9] = {};
    size_t length = 0;
    if (gxEncodeIndexInt64Key(false, value, bytes, sizeof(bytes),
                              &length) != GxStatus::Ok) {
        return std::vector<uint8_t>();
    }
    return std::vector<uint8_t>(bytes, bytes + length);
}

std::vector<uint8_t> encodedFloat64(double value) {
    std::vector<uint8_t> bytes;
    if (!encodeIndexKey(DbType::Float64, DbValue::float64(value), bytes)) {
        return std::vector<uint8_t>();
    }
    return bytes;
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

bool loadProductionFixture(MemoryPageAccess& pages, uint64_t& rootPageId,
                           uint32_t& ownerIndexId,
                           uint32_t& duplicateCount) {
    std::ifstream input(GX_R3AB_FIXTURE_PATH);
    uint32_t pageSize = 0;
    uint64_t pageCount = 0;
    size_t pageTotal = 0;
    if (!input || !(input >> pageSize >> pageCount >> ownerIndexId >>
                    rootPageId >> duplicateCount >> pageTotal) ||
        pageSize != 512 || pageCount < 2 || pageTotal < 3 ||
        duplicateCount < 2 || rootPageId == 0 || rootPageId >= pageCount) {
        return false;
    }
    pages.initializeSparse(pageCount);
    for (size_t i = 0; i < pageTotal; ++i) {
        DatabasePage page;
        uint16_t pageType = 0;
        std::string encoded;
        if (!(input >> page.pageId >> pageType >> encoded) ||
            page.pageId == 0 || page.pageId >= pageCount ||
            !parseHex(encoded, page.payload) || page.payload.size() != 480) {
            return false;
        }
        page.type = static_cast<PageType>(pageType);
        page.payloadSize = static_cast<uint32_t>(page.payload.size());
        pages.put(page);
    }
    std::string trailing;
    return !(input >> trailing);
}

struct Seed {
    std::vector<uint8_t> key;
    uint32_t slot;
};

bool makeMultiLevelTextTree(MemoryPageAccess& pages, uint64_t& rootPageId,
                            std::vector<uint8_t>& duplicateKey,
                            uint32_t& treeHeight) {
    uint64_t rowPageId = 0;
    if (!pages.allocatePage(PageType::Data, rowPageId).isOk()) return false;
    GxAllocator allocator = {};
    if (gxHostedAllocatorInit(&allocator, 131072) != GxStatus::Ok) return false;
    IndexTree tree(pages, DbType::Text, 37, nullptr, &allocator);
    if (!tree.createEmpty(rootPageId).isOk()) return false;
    duplicateKey = encodedText("m-dup");
    std::vector<Seed> seeds;
    for (uint32_t i = 0; i < 160; ++i) {
        char value[16];
        std::snprintf(value, sizeof(value), "a%05u", i);
        seeds.push_back(Seed{encodedText(value), 1u + i});
    }
    for (uint32_t i = 0; i < 100; ++i) {
        seeds.push_back(Seed{duplicateKey, 1000u + i});
    }
    for (uint32_t i = 0; i < 160; ++i) {
        char value[16];
        std::snprintf(value, sizeof(value), "z%05u", i);
        seeds.push_back(Seed{encodedText(value), 2000u + i});
    }
    std::stable_sort(seeds.begin(), seeds.end(),
        [](const Seed& left, const Seed& right) {
            if (left.key != right.key) return left.key < right.key;
            return left.slot < right.slot;
        });
    for (size_t i = 0; i < seeds.size(); ++i) {
        uint64_t updatedRoot = 0;
        bool inserted = false;
        if (!tree.insert(seeds[i].key, RowLocator(rowPageId, seeds[i].slot),
                         rootPageId, false, updatedRoot, inserted).isOk() ||
            !inserted) {
            return false;
        }
        rootPageId = updatedRoot;
    }
    IndexValidation validation;
    if (!tree.validate(rootPageId, validation).isOk() || !validation.ok) {
        return false;
    }
    treeHeight = validation.height;
    return true;
}

bool sameRows(const std::vector<RowLocator>& actual,
              const std::vector<RowLocator>& expected) {
    if (actual.size() != expected.size()) return false;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].pageId != expected[i].pageId ||
            actual[i].slot != expected[i].slot) return false;
    }
    return true;
}

std::vector<std::vector<uint8_t> > pageSnapshot(
    const MemoryPageAccess& pages) {
    std::vector<std::vector<uint8_t> > output;
    for (uint64_t pageId = 1; pageId < pages.pageCount(); ++pageId) {
        const DatabasePage* page = pages.get(pageId);
        output.push_back(page == nullptr ? std::vector<uint8_t>()
                                         : page->payload);
    }
    return output;
}

struct MatchingPagePosition {
    uint64_t pageId;
    size_t chainIndex;
};

std::vector<MatchingPagePosition> findMatchingLeafPages(
    const MemoryPageAccess& pages, uint64_t firstPageId,
    const std::vector<uint8_t>& logicalKey) {
    std::vector<MatchingPagePosition> output;
    uint64_t pageId = firstPageId;
    for (size_t chainIndex = 0; pageId != 0 && chainIndex < pages.pageCount();
         ++chainIndex) {
        const DatabasePage* page = pages.get(pageId);
        if (page == nullptr || page->type != PageType::IndexLeaf) break;
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(page->payload.data(), page->payload.size(),
                                  &cursor, &error) != GxStatus::Ok) break;
        bool hasMatch = false;
        bool pastKey = false;
        for (;;) {
            GxIndexLeafEntryView entry = {};
            bool hasEntry = false;
            if (gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                          &error) != GxStatus::Ok || !hasEntry) {
                break;
            }
            const size_t common = entry.keyLength < logicalKey.size()
                ? entry.keyLength : logicalKey.size();
            const int comparison = common == 0
                ? (entry.keyLength < logicalKey.size() ? -1 :
                   (entry.keyLength > logicalKey.size() ? 1 : 0))
                : std::memcmp(entry.keyBytes, logicalKey.data(), common) != 0
                    ? (std::memcmp(entry.keyBytes, logicalKey.data(), common) < 0
                        ? -1 : 1)
                    : (entry.keyLength < logicalKey.size() ? -1 :
                       (entry.keyLength > logicalKey.size() ? 1 : 0));
            if (comparison == 0) hasMatch = true;
            if (comparison > 0) {
                pastKey = true;
                break;
            }
        }
        if (hasMatch) {
            output.push_back(MatchingPagePosition{pageId, chainIndex});
        }
        if (pastKey) break;
        pageId = cursor.page.nextLeafPageId;
    }
    return output;
}

void testProductionWrittenFixtureAndHostedAdapter() {
    std::printf("R3AB production-written fixture and hosted lookup adapter\n");
    MemoryPageAccess pages;
    uint64_t rootPageId = 0;
    uint32_t owner = 0;
    uint32_t duplicateCount = 0;
    CHECK(loadProductionFixture(pages, rootPageId, owner, duplicateCount));
    if (rootPageId == 0 || duplicateCount == 0) return;
    CHECK(owner == 37 && duplicateCount == 100);

    const std::vector<uint8_t> duplicateKey = encodedText("m-dup");
    const std::vector<std::vector<uint8_t> > before = pageSnapshot(pages);
    ReusingProvider source(pages);
    Collector collector;
    GxIndexLookupResult result = {};
    CHECK(runLookup(pages, source, rootPageId, duplicateKey, collector,
                    result) == GxIndexLookupStatus::Complete);
    CHECK(result.depth >= 2 && result.descentPagesVisited >= 3);
    CHECK(result.matchesVisited == duplicateCount &&
          collector.callbacks == duplicateCount);
    CHECK(collector.rows.size() == duplicateCount);
    for (uint32_t i = 0; i < duplicateCount && i < collector.rows.size(); ++i) {
        CHECK(collector.rows[i].pageId == 1);
        CHECK(collector.rows[i].slot == 1000u + i);
    }
    CHECK(result.leafPagesVisited > 1);
    CHECK(source.releaseCount == source.successfulAcquireCount &&
          !source.active && !source.releaseMismatch);

    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 131072) == GxStatus::Ok);
    IndexTree hostedTree(pages, DbType::Text, owner, nullptr, &allocator);
    std::vector<IndexEntry> hosted;
    CHECK(hostedTree.lookupAll(duplicateKey, rootPageId, hosted).isOk());
    CHECK(hosted.size() == duplicateCount);
    for (uint32_t i = 0; i < duplicateCount && i < hosted.size(); ++i) {
        CHECK(hosted[i].key == duplicateKey);
        CHECK(hosted[i].locator.pageId == collector.rows[i].pageId);
        CHECK(hosted[i].locator.slot == collector.rows[i].slot);
    }

    ReusingProvider repeatSource(pages);
    Collector repeatCollector;
    GxIndexLookupResult repeatResult = {};
    CHECK(runLookup(pages, repeatSource, rootPageId, duplicateKey,
                    repeatCollector, repeatResult) ==
          GxIndexLookupStatus::Complete);
    CHECK(sameRows(repeatCollector.rows, collector.rows));
    CHECK(pageSnapshot(pages) == before);
}

struct Query {
    std::vector<uint8_t> key;
    std::vector<RowLocator> expected;
};

void runTypedCase(DbType type, const std::vector<Seed>& entries,
                  const std::vector<Query>& queries,
                  const char* label) {
    std::printf("R3AB %s portable and hosted behavior\n", label);
    MemoryPageAccess pages;
    uint64_t rowPageId = 0;
    CHECK(pages.allocatePage(PageType::Data, rowPageId).isOk());
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 131072) == GxStatus::Ok);
    IndexTree tree(pages, type, 37, nullptr, &allocator);
    uint64_t root = 0;
    CHECK(tree.createEmpty(root).isOk());
    for (size_t i = 0; i < entries.size(); ++i) {
        uint64_t updatedRoot = 0;
        bool inserted = false;
        const DbResult insertResult = tree.insert(
            entries[i].key, RowLocator(entries[i].slot, entries[i].slot),
            root, false, updatedRoot, inserted);
        CHECK(insertResult.isOk() && inserted);
        if (insertResult.isOk()) root = updatedRoot;
    }

    for (size_t i = 0; i < queries.size(); ++i) {
        for (size_t repeat = 0; repeat < 2; ++repeat) {
            ReusingProvider source(pages);
            Collector collector;
            GxIndexLookupResult lookup = {};
            CHECK(runLookup(pages, source, root, queries[i].key,
                            collector, lookup) == GxIndexLookupStatus::Complete);
            CHECK(lookup.matchesVisited == queries[i].expected.size());
            CHECK(sameRows(collector.rows, queries[i].expected));
            CHECK(source.releaseCount == source.successfulAcquireCount &&
                  !source.active && !source.releaseMismatch);

            std::vector<IndexEntry> hosted;
            CHECK(tree.lookupAll(queries[i].key, root, hosted).isOk());
            CHECK(hosted.size() == queries[i].expected.size());
            for (size_t j = 0; j < hosted.size() &&
                               j < queries[i].expected.size(); ++j) {
                CHECK(hosted[j].key == queries[i].key);
                CHECK(hosted[j].locator.pageId == queries[i].expected[j].pageId);
                CHECK(hosted[j].locator.slot == queries[i].expected[j].slot);
            }
        }
    }
}

void testTypedKeysAndSingleLevelTree() {
    const std::vector<uint8_t> nullKey(1, 0);
    const std::vector<uint8_t> emptyText = encodedText(std::string());
    const std::string embeddedBytes("a\0b", 3);
    const std::vector<uint8_t> embeddedText = encodedText(embeddedBytes);
    const std::vector<uint8_t> boolFalse = encodedBoolean(false, false);
    const std::vector<uint8_t> boolTrue = encodedBoolean(false, true);
    const std::vector<uint8_t> i32Min =
        encodedInt32(std::numeric_limits<int32_t>::min());
    const std::vector<uint8_t> i32Zero = encodedInt32(0);
    const std::vector<uint8_t> i32Max =
        encodedInt32(std::numeric_limits<int32_t>::max());
    const std::vector<uint8_t> i64Min =
        encodedInt64(std::numeric_limits<int64_t>::min());
    const std::vector<uint8_t> i64Zero = encodedInt64(0);
    const std::vector<uint8_t> i64Max =
        encodedInt64(std::numeric_limits<int64_t>::max());

    runTypedCase(DbType::Text,
        std::vector<Seed>{
            Seed{nullKey, 1}, Seed{emptyText, 2},
            Seed{embeddedText, 70}, Seed{embeddedText, 30},
            Seed{embeddedText, 50}, Seed{encodedText("tail"), 4}},
        std::vector<Query>{
            Query{nullKey, std::vector<RowLocator>{RowLocator(1, 1)}},
            Query{emptyText, std::vector<RowLocator>{RowLocator(2, 2)}},
            Query{embeddedText, std::vector<RowLocator>{
                RowLocator(30, 30), RowLocator(50, 50), RowLocator(70, 70)}},
            Query{encodedText("missing"), std::vector<RowLocator>()}},
        "NULL, empty Text, embedded zero, and leaf-local duplicates");

    runTypedCase(DbType::Boolean,
        std::vector<Seed>{
            Seed{nullKey, 1}, Seed{boolFalse, 90}, Seed{boolFalse, 10},
            Seed{boolTrue, 5}, Seed{boolTrue, 3}},
        std::vector<Query>{
            Query{nullKey, std::vector<RowLocator>{RowLocator(1, 1)}},
            Query{boolFalse, std::vector<RowLocator>{
                RowLocator(10, 10), RowLocator(90, 90)}},
            Query{boolTrue, std::vector<RowLocator>{
                RowLocator(3, 3), RowLocator(5, 5)}}},
        "Boolean NULL, false, and true");

    runTypedCase(DbType::Int32,
        std::vector<Seed>{
            Seed{nullKey, 1}, Seed{i32Min, 2}, Seed{i32Zero, 7},
            Seed{i32Zero, 3}, Seed{i32Max, 4}},
        std::vector<Query>{
            Query{nullKey, std::vector<RowLocator>{RowLocator(1, 1)}},
            Query{i32Min, std::vector<RowLocator>{RowLocator(2, 2)}},
            Query{i32Zero, std::vector<RowLocator>{
                RowLocator(3, 3), RowLocator(7, 7)}},
            Query{i32Max, std::vector<RowLocator>{RowLocator(4, 4)}}},
        "Int32 NULL and boundaries");

    runTypedCase(DbType::Int64,
        std::vector<Seed>{
            Seed{nullKey, 1}, Seed{i64Min, 2}, Seed{i64Zero, 7},
            Seed{i64Zero, 3}, Seed{i64Max, 4}},
        std::vector<Query>{
            Query{nullKey, std::vector<RowLocator>{RowLocator(1, 1)}},
            Query{i64Min, std::vector<RowLocator>{RowLocator(2, 2)}},
            Query{i64Zero, std::vector<RowLocator>{
                RowLocator(3, 3), RowLocator(7, 7)}},
            Query{i64Max, std::vector<RowLocator>{RowLocator(4, 4)}}},
        "Int64 NULL and boundaries");

    const std::vector<uint8_t> f64Null(1, 0);
    const std::vector<uint8_t> f64Negative = encodedFloat64(-1.5);
    const std::vector<uint8_t> f64Zero = encodedFloat64(0.0);
    const std::vector<uint8_t> f64Positive = encodedFloat64(1.5);
    runTypedCase(DbType::Float64,
        std::vector<Seed>{
            Seed{f64Null, 1}, Seed{f64Negative, 2},
            Seed{f64Zero, 3}, Seed{f64Positive, 4}},
        std::vector<Query>{
            Query{f64Null, std::vector<RowLocator>{RowLocator(1, 1)}},
            Query{f64Negative, std::vector<RowLocator>{RowLocator(2, 2)}},
            Query{f64Zero, std::vector<RowLocator>{RowLocator(3, 3)}},
            Query{f64Positive, std::vector<RowLocator>{RowLocator(4, 4)}}},
        "host-encoded Float64 compatibility route");

    std::printf("R3AB empty root and production single-level internal root\n");
    MemoryPageAccess emptyPages;
    GxAllocator emptyAllocator = {};
    CHECK(gxHostedAllocatorInit(&emptyAllocator, 131072) == GxStatus::Ok);
    IndexTree emptyTree(emptyPages, DbType::Text, 37, nullptr, &emptyAllocator);
    uint64_t emptyRoot = 0;
    CHECK(emptyTree.createEmpty(emptyRoot).isOk());
    ReusingProvider emptySource(emptyPages);
    Collector emptyCollector;
    GxIndexLookupResult emptyResult = {};
    CHECK(runLookup(emptyPages, emptySource, emptyRoot, emptyText,
                    emptyCollector, emptyResult) == GxIndexLookupStatus::Complete);
    CHECK(emptyResult.matchesVisited == 0 && emptyCollector.callbacks == 0);

    MemoryPageAccess internalPages;
    uint64_t rowPage = 0;
    CHECK(internalPages.allocatePage(PageType::Data, rowPage).isOk());
    GxAllocator internalAllocator = {};
    CHECK(gxHostedAllocatorInit(&internalAllocator, 131072) == GxStatus::Ok);
    IndexTree internalTree(internalPages, DbType::Int32, 37, nullptr,
                           &internalAllocator);
    uint64_t root = 0;
    CHECK(internalTree.createEmpty(root).isOk());
    for (int32_t i = 0; i < 17; ++i) {
        uint64_t updatedRoot = 0;
        bool inserted = false;
        CHECK(internalTree.insert(encodedInt32(i), RowLocator(1, i + 1), root,
                                  false, updatedRoot, inserted).isOk());
        root = updatedRoot;
    }
    IndexValidation validation;
    const DbResult internalValidation = internalTree.validate(root, validation);
    if (!internalValidation.isOk() || !validation.ok || validation.height != 2) {
        std::printf("  single-level writer height=%u entries=%llu message=%s\n",
                    validation.height,
                    static_cast<unsigned long long>(validation.entryCount),
                    validation.message.c_str());
    }
    CHECK(internalValidation.isOk() && validation.ok && validation.height == 2);
    ReusingProvider internalSource(internalPages);
    Collector middleCollector;
    GxIndexLookupResult middleResult = {};
    CHECK(runLookup(internalPages, internalSource, root, encodedInt32(8),
                    middleCollector, middleResult) ==
          GxIndexLookupStatus::Complete);
    CHECK(middleResult.depth == 1 && middleCollector.rows.size() == 1 &&
          middleCollector.rows[0].slot == 9);
}

void testMultiLevelFunctionalRangeAndVisitors() {
    std::printf("R3AB production-writer multi-level functional lookup\n");
    MemoryPageAccess pages;
    uint64_t root = 0;
    uint32_t height = 0;
    std::vector<uint8_t> duplicateKey;
    CHECK(makeMultiLevelTextTree(pages, root, duplicateKey, height));
    CHECK(height >= 3);
    if (root == 0) return;

    struct ExpectedQuery {
        std::vector<uint8_t> key;
        std::vector<RowLocator> rows;
    };
    std::vector<RowLocator> duplicates;
    for (uint32_t i = 0; i < 100; ++i) {
        duplicates.push_back(RowLocator(1, 1000u + i));
    }
    const std::vector<ExpectedQuery> queries = {
        {encodedText("a00000"), std::vector<RowLocator>{RowLocator(1, 1)}},
        {encodedText("a00083"), std::vector<RowLocator>{RowLocator(1, 84)}},
        {duplicateKey, duplicates},
        {encodedText("z00159"), std::vector<RowLocator>{RowLocator(1, 2159)}},
        {encodedText("00000"), std::vector<RowLocator>()},
        {encodedText("m-miss"), std::vector<RowLocator>()},
        {encodedText("zzzzzz"), std::vector<RowLocator>()}
    };

    GxAllocator hostedAllocator = {};
    CHECK(gxHostedAllocatorInit(&hostedAllocator, 131072) == GxStatus::Ok);
    IndexTree hostedTree(pages, DbType::Text, 37, nullptr, &hostedAllocator);
    for (size_t i = 0; i < queries.size(); ++i) {
        ReusingProvider source(pages);
        Collector collector;
        GxIndexLookupResult result = {};
        CHECK(runLookup(pages, source, root, queries[i].key, collector,
                        result) == GxIndexLookupStatus::Complete);
        CHECK(sameRows(collector.rows, queries[i].rows));
        std::vector<IndexEntry> hosted;
        CHECK(hostedTree.lookupAll(queries[i].key, root, hosted).isOk());
        CHECK(hosted.size() == queries[i].rows.size());
        for (size_t j = 0; j < hosted.size() && j < queries[i].rows.size(); ++j) {
            CHECK(hosted[j].key == queries[i].key);
            CHECK(hosted[j].locator.pageId == queries[i].rows[j].pageId);
            CHECK(hosted[j].locator.slot == queries[i].rows[j].slot);
        }
    }

    ReusingProvider stoppedSource(pages);
    Collector firstOnly;
    firstOnly.stopAfter = 1;
    GxIndexLookupResult stoppedResult = {};
    CHECK(runLookup(pages, stoppedSource, root, duplicateKey, firstOnly,
                    stoppedResult) == GxIndexLookupStatus::VisitorStopped);
    CHECK(firstOnly.callbacks == 1 && stoppedResult.matchesVisited == 1);
    CHECK(stoppedSource.releaseCount == stoppedSource.successfulAcquireCount &&
          !stoppedSource.active);

    ReusingProvider severalSource(pages);
    Collector stopSeveral;
    stopSeveral.stopAfter = 5;
    GxIndexLookupResult severalResult = {};
    CHECK(runLookup(pages, severalSource, root, duplicateKey, stopSeveral,
                    severalResult) == GxIndexLookupStatus::VisitorStopped);
    CHECK(stopSeveral.callbacks == 5 && severalResult.matchesVisited == 5);

    ReusingProvider failedVisitorSource(pages);
    Collector failedVisitor;
    failedVisitor.failAfter = 3;
    GxIndexLookupResult failedVisitorResult = {};
    CHECK(runLookup(pages, failedVisitorSource, root, duplicateKey,
                    failedVisitor, failedVisitorResult) ==
          GxIndexLookupStatus::VisitorFailure);
    CHECK(failedVisitor.callbacks == 3 &&
          failedVisitorResult.matchesVisited == 3);
    CHECK(failedVisitorSource.releaseCount ==
          failedVisitorSource.successfulAcquireCount &&
          !failedVisitorSource.active);

    ReusingProvider failedSource(pages);
    Collector partialCollector;
    GxIndexLookupResult baselineResult = {};
    ReusingProvider baselineSource(pages);
    Collector baselineCollector;
    CHECK(runLookup(pages, baselineSource, root, duplicateKey,
                    baselineCollector, baselineResult) ==
          GxIndexLookupStatus::Complete);
    const std::vector<MatchingPagePosition> matchingPages =
        findMatchingLeafPages(pages, baselineResult.leafPageId, duplicateKey);
    CHECK(matchingPages.size() > 1);
    if (matchingPages.size() <= 1) return;
    const size_t secondMatchChainIndex = matchingPages[1].chainIndex;
    const uint64_t boundedLeafPageLimit = secondMatchChainIndex;
    ReusingProvider partialSource(pages);
    Collector boundedPartialCollector;
    GxIndexLookupResult boundedPartialResult = {};
    CHECK(runLookup(pages, partialSource, root, duplicateKey,
                    boundedPartialCollector, boundedPartialResult,
                    gxos::db::kMaxIndexDepth,
                    boundedLeafPageLimit) == GxIndexLookupStatus::TraversalLimit);
    CHECK(boundedPartialCollector.callbacks > 0 &&
          boundedPartialResult.matchesVisited > 0);

    failedSource.failOnAcquire = baselineResult.descentPagesVisited +
        secondMatchChainIndex + 1u;
    GxIndexLookupResult failedResult = {};
    CHECK(runLookup(pages, failedSource, root, duplicateKey,
                    partialCollector, failedResult) ==
          GxIndexLookupStatus::ProviderFailure);
    CHECK(partialCollector.callbacks > 0 && failedResult.matchesVisited > 0);
    CHECK(failedSource.releaseCount == failedSource.successfulAcquireCount &&
          !failedSource.active);
}

void testCorruptionAndProviderFailures() {
    std::printf("R3AB corruption, bounded termination, and provider ownership\n");
    MemoryPageAccess original;
    uint64_t root = 0;
    uint32_t height = 0;
    std::vector<uint8_t> duplicateKey;
    CHECK(makeMultiLevelTextTree(original, root, duplicateKey, height));
    if (root == 0) return;

    ReusingProvider invalidRootSource(original);
    Collector invalidRootCollector;
    GxIndexLookupResult invalidRootResult = {};
    CHECK(runLookup(original, invalidRootSource, 0, duplicateKey,
                    invalidRootCollector, invalidRootResult) ==
          GxIndexLookupStatus::InvalidPageId);
    CHECK(invalidRootSource.acquireCount == 0 &&
          invalidRootResult.descentStatus ==
              GxIndexTreeDescentStatus::InvalidPageId);

    MemoryPageAccess badChild(original);
    DatabasePage* badRoot = badChild.getMutable(root);
    CHECK(badRoot != nullptr && badRoot->type == PageType::IndexInternal);
    if (badRoot != nullptr) {
        const size_t firstSlot = badRoot->payload.size() - 8u;
        const uint32_t firstOffset = loadLe32(badRoot->payload.data() + firstSlot);
        storeLe64(badRoot->payload.data() + firstOffset, badChild.pageCount());
        const std::vector<std::vector<uint8_t> > snapshot = pageSnapshot(badChild);
        ReusingProvider source(badChild);
        Collector collector;
        GxIndexLookupResult result = {};
        CHECK(runLookup(badChild, source, root, encodedText("0"), collector,
                        result) == GxIndexLookupStatus::CorruptPage);
        CHECK(source.releaseCount == source.successfulAcquireCount &&
              !source.active);
        CHECK(pageSnapshot(badChild) == snapshot);
    }

    MemoryPageAccess internalCycle(original);
    DatabasePage* cycleRoot = internalCycle.getMutable(root);
    if (cycleRoot != nullptr) {
        const size_t firstSlot = cycleRoot->payload.size() - 8u;
        const uint32_t firstOffset = loadLe32(cycleRoot->payload.data() + firstSlot);
        storeLe64(cycleRoot->payload.data() + firstOffset, root);
        ReusingProvider source(internalCycle);
        Collector collector;
        GxIndexLookupResult result = {};
        CHECK(runLookup(internalCycle, source, root, encodedText("0"),
                        collector, result) == GxIndexLookupStatus::Cycle);
        CHECK(source.acquireCount == 1 && source.releaseCount == 1 &&
              !source.active);
    }

    ReusingProvider depthSource(original);
    Collector depthCollector;
    GxIndexLookupResult depthResult = {};
    CHECK(runLookup(original, depthSource, root, duplicateKey,
                    depthCollector, depthResult, 0) ==
          GxIndexLookupStatus::TraversalLimit);
    CHECK(depthSource.acquireCount == 1 && depthSource.releaseCount == 1);

    ReusingProvider truncatedInternalSource(original);
    truncatedInternalSource.hasShortLength = true;
    truncatedInternalSource.shortPageId = root;
    truncatedInternalSource.shortLength = 23;
    Collector truncatedInternalCollector;
    GxIndexLookupResult truncatedInternalResult = {};
    CHECK(runLookup(original, truncatedInternalSource, root, duplicateKey,
                    truncatedInternalCollector, truncatedInternalResult) ==
          GxIndexLookupStatus::CorruptPage);
    CHECK(truncatedInternalSource.releaseCount == 1 &&
          !truncatedInternalSource.active);

    ReusingProvider badIdentitySource(original);
    badIdentitySource.hasPageIdOverride = true;
    badIdentitySource.overridePageIdFor = root;
    badIdentitySource.overridePageIdValue = root + 1;
    Collector badIdentityCollector;
    GxIndexLookupResult badIdentityResult = {};
    CHECK(runLookup(original, badIdentitySource, root, duplicateKey,
                    badIdentityCollector, badIdentityResult) ==
          GxIndexLookupStatus::CorruptPage);
    CHECK(badIdentitySource.releaseCount == 1 &&
          !badIdentitySource.active);

    ReusingProvider badTypeSource(original);
    badTypeSource.hasTypeOverride = true;
    badTypeSource.overrideTypeFor = root;
    badTypeSource.overrideTypeValue = 99;
    Collector badTypeCollector;
    GxIndexLookupResult badTypeResult = {};
    CHECK(runLookup(original, badTypeSource, root, duplicateKey,
                    badTypeCollector, badTypeResult) ==
          GxIndexLookupStatus::CorruptPage);
    CHECK(badTypeSource.releaseCount == 1 && !badTypeSource.active);

    ReusingProvider baselineSource(original);
    Collector baselineCollector;
    GxIndexLookupResult baseline = {};
    CHECK(runLookup(original, baselineSource, root, duplicateKey,
                    baselineCollector, baseline) ==
          GxIndexLookupStatus::Complete);
    const std::vector<MatchingPagePosition> matchingPages =
        findMatchingLeafPages(original, baseline.leafPageId, duplicateKey);
    CHECK(matchingPages.size() > 1);
    if (matchingPages.size() <= 1) return;
    const uint64_t secondMatchingPage = matchingPages[1].pageId;

    ReusingProvider missingSource(original);
    missingSource.missingPageId = secondMatchingPage;
    Collector missingCollector;
    GxIndexLookupResult missingResult = {};
    CHECK(runLookup(original, missingSource, root, duplicateKey,
                    missingCollector, missingResult) ==
          GxIndexLookupStatus::MissingPage);
    CHECK(missingCollector.callbacks > 0 && missingResult.matchesVisited > 0);
    CHECK(missingSource.releaseCount == missingSource.successfulAcquireCount &&
          !missingSource.active);

    MemoryPageAccess hostedMissingPages(original);
    hostedMissingPages.markMissing(secondMatchingPage);
    GxAllocator hostedFailureAllocator = {};
    CHECK(gxHostedAllocatorInit(&hostedFailureAllocator, 131072) ==
          GxStatus::Ok);
    IndexTree hostedFailureTree(hostedMissingPages, DbType::Text, 37, nullptr,
                                &hostedFailureAllocator);
    std::vector<IndexEntry> hostedPartial(1);
    hostedPartial[0].key = encodedText("sentinel");
    hostedPartial[0].locator = RowLocator(99, 99);
    CHECK(hostedFailureTree.lookupAll(duplicateKey, root,
                                      hostedPartial).status() ==
          DbStatus::OutOfBounds);
    CHECK(hostedPartial.empty());

    MemoryPageAccess badSuccessor(original);
    DatabasePage* firstLeaf = badSuccessor.getMutable(baseline.leafPageId);
    if (firstLeaf != nullptr) {
        storeLe64(firstLeaf->payload.data() + 8, badSuccessor.pageCount());
        ReusingProvider source(badSuccessor);
        Collector collector;
        GxIndexLookupResult result = {};
        CHECK(runLookup(badSuccessor, source, root, duplicateKey,
                        collector, result) == GxIndexLookupStatus::InvalidPageId);
        CHECK(source.releaseCount == source.successfulAcquireCount &&
              !source.active);
    }

    MemoryPageAccess leafCycle(original);
    DatabasePage* cycleLeaf = leafCycle.getMutable(baseline.leafPageId);
    if (cycleLeaf != nullptr) {
        storeLe64(cycleLeaf->payload.data() + 8, baseline.leafPageId);
        ReusingProvider source(leafCycle);
        Collector collector;
        GxIndexLookupResult result = {};
        const GxIndexLookupStatus status = runLookup(
            leafCycle, source, root, duplicateKey, collector, result);
        CHECK(status == GxIndexLookupStatus::CorruptPage ||
              status == GxIndexLookupStatus::TraversalLimit);
        CHECK(source.acquireCount < 10 && source.releaseCount ==
              source.successfulAcquireCount && !source.active);
    }

    MemoryPageAccess shortLeafPages;
    GxAllocator shortAllocator = {};
    CHECK(gxHostedAllocatorInit(&shortAllocator, 131072) == GxStatus::Ok);
    IndexTree shortTree(shortLeafPages, DbType::Text, 37, nullptr,
                        &shortAllocator);
    uint64_t shortRoot = 0;
    CHECK(shortTree.createEmpty(shortRoot).isOk());
    ReusingProvider shortLeafSource(shortLeafPages);
    shortLeafSource.hasShortLength = true;
    shortLeafSource.shortPageId = shortRoot;
    shortLeafSource.shortLength = 12;
    Collector shortLeafCollector;
    GxIndexLookupResult shortLeafResult = {};
    CHECK(runLookup(shortLeafPages, shortLeafSource, shortRoot, duplicateKey,
                    shortLeafCollector, shortLeafResult) ==
          GxIndexLookupStatus::CorruptPage);
    CHECK(shortLeafSource.releaseCount == 1 && !shortLeafSource.active);

    MemoryPageAccess orderedPages;
    uint64_t rowPage = 0;
    CHECK(orderedPages.allocatePage(PageType::Data, rowPage).isOk());
    GxAllocator orderedAllocator = {};
    CHECK(gxHostedAllocatorInit(&orderedAllocator, 131072) == GxStatus::Ok);
    IndexTree orderedTree(orderedPages, DbType::Text, 37, nullptr,
                          &orderedAllocator);
    uint64_t orderedRoot = 0;
    CHECK(orderedTree.createEmpty(orderedRoot).isOk());
    uint64_t updatedRoot = 0;
    bool inserted = false;
    CHECK(orderedTree.insert(encodedText("a"), RowLocator(1, 1), orderedRoot,
                              false, updatedRoot, inserted).isOk());
    orderedRoot = updatedRoot;
    CHECK(orderedTree.insert(encodedText("b"), RowLocator(1, 2), orderedRoot,
                              false, updatedRoot, inserted).isOk());
    orderedRoot = updatedRoot;
    DatabasePage* orderedLeaf = orderedPages.getMutable(orderedRoot);
    if (orderedLeaf != nullptr) {
        const size_t firstSlot = orderedLeaf->payload.size() - 8u;
        const size_t secondSlot = orderedLeaf->payload.size() - 16u;
        const uint32_t firstOffset = loadLe32(orderedLeaf->payload.data() + firstSlot);
        const uint32_t secondOffset = loadLe32(orderedLeaf->payload.data() + secondSlot);
        CHECK(loadLe32(orderedLeaf->payload.data() + firstOffset) == 2);
        CHECK(loadLe32(orderedLeaf->payload.data() + secondOffset) == 2);
        std::swap(orderedLeaf->payload[firstOffset + 5u],
                  orderedLeaf->payload[secondOffset + 5u]);
        ReusingProvider source(orderedPages);
        Collector collector;
        GxIndexLookupResult result = {};
        CHECK(runLookup(orderedPages, source, orderedRoot, encodedText("a"),
                        collector, result) == GxIndexLookupStatus::CorruptPage);
        CHECK(source.releaseCount == source.successfulAcquireCount &&
              !source.active);
    }

    ReusingProvider aliasSource(original);
    std::vector<uint8_t> aliasLower(duplicateKey.size() + 12u, 0);
    std::vector<uint8_t> aliasPrevious(480, 0);
    uint64_t aliasPath[kMaxIndexDepth + 1] = {};
    Collector aliasCollector;
    GxIndexLookupResult aliasResult = {};
    aliasSource.hasAlias = true;
    aliasSource.aliasPageId = root;
    aliasSource.aliasBytes = aliasLower.data();
    const GxIndexPageProvider aliasProvider = providerApi(aliasSource);
    CHECK(gxIndexLookupAll(
        &aliasProvider, root, original.pageCount(), 37, duplicateKey.data(),
        duplicateKey.size(), kMaxIndexDepth, aliasPath,
        sizeof(aliasPath) / sizeof(aliasPath[0]), original.pageCount() - 1,
        aliasLower.data(), aliasLower.size(), aliasPrevious.data(),
        aliasPrevious.size(), collect, &aliasCollector, &aliasResult) ==
        GxIndexLookupStatus::CorruptPage);
    CHECK(aliasSource.releaseCount == 1 && !aliasSource.active &&
          !aliasSource.releaseMismatch);

    uint8_t oneByteKey = 0x01;
    uint8_t shortScratch[12] = {};
    uint8_t previousScratch[480] = {};
    uint64_t invalidPath[kMaxIndexDepth + 1] = {};
    GxIndexLookupResult unchanged = {};
    unchanged.leafPageId = 99;
    unchanged.matchesVisited = 88;
    GxIndexLookupResult before = unchanged;
    ReusingProvider invalidArgSource(original);
    const GxIndexPageProvider invalidArgProvider = providerApi(invalidArgSource);
    CHECK(gxIndexLookupAll(
        &invalidArgProvider, root, original.pageCount(), 37, &oneByteKey,
        static_cast<size_t>(-1), kMaxIndexDepth, invalidPath,
        sizeof(invalidPath) / sizeof(invalidPath[0]), original.pageCount() - 1,
        shortScratch, sizeof(shortScratch), previousScratch,
        sizeof(previousScratch), collect, &aliasCollector, &unchanged) ==
        GxIndexLookupStatus::InvalidArgument);
    CHECK(unchanged.leafPageId == before.leafPageId &&
          unchanged.matchesVisited == before.matchesVisited);
}

} // namespace

int main() {
    testProductionWrittenFixtureAndHostedAdapter();
    testTypedKeysAndSingleLevelTree();
    testMultiLevelFunctionalRangeAndVisitors();
    testCorruptionAndProviderFailures();
    std::printf("NDB1R4P11R3AB complete portable index lookup: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
