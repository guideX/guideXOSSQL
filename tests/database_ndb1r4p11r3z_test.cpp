#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include "database_index.h"
#include "database_pagestore.h"
#include "gx_allocator_hosted.h"
#include "gx_index_integer_key.h"
#include "gx_index_key.h"
#include "gx_index_leaf_entry.h"
#include "gx_index_leaf_traversal.h"

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

void storeLe64(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

void storeLe32(uint8_t* bytes, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

uint32_t loadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

class MemoryPageAccess : public PageAccess {
public:
    MemoryPageAccess() : _pages(1), _writes(0) {
        _pages[0].pageId = 0;
    }

    uint32_t pageSize() const override { return 512; }
    uint64_t pageCount() const override { return _pages.size(); }
    bool isReadOnly() const override { return false; }

    DbResult readPage(uint64_t pageId, DatabasePage& out) override {
        if (pageId == 0 || pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "R3Z test page is out of range");
        }
        out = _pages[static_cast<size_t>(pageId)];
        return DbResult::ok();
    }

    DbResult writePage(const DatabasePage& page) override {
        if (page.pageId == 0 || page.pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "R3Z test page is out of range");
        }
        _pages[static_cast<size_t>(page.pageId)] = page;
        ++_writes;
        return DbResult::ok();
    }

    DbResult allocatePage(PageType type, uint64_t& outPageId) override {
        DatabasePage page;
        page.pageId = _pages.size();
        page.type = type;
        page.payload.assign(DatabasePage::payloadCapacity(pageSize()), 0);
        page.payloadSize = static_cast<uint32_t>(page.payload.size());
        _pages.push_back(page);
        outPageId = page.pageId;
        return DbResult::ok();
    }

    const DatabasePage* get(uint64_t pageId) const {
        return pageId < _pages.size() ? &_pages[static_cast<size_t>(pageId)]
                                      : nullptr;
    }

    uint64_t writeCount() const { return _writes; }

private:
    std::vector<DatabasePage> _pages;
    uint64_t _writes;
};

std::vector<uint64_t> leafChain(const MemoryPageAccess& pages) {
    std::vector<uint64_t> leafIds;
    std::vector<uint64_t> nextIds;
    for (uint64_t pageId = 1; pageId < pages.pageCount(); ++pageId) {
        const DatabasePage* page = pages.get(pageId);
        if (page == nullptr || page->type != PageType::IndexLeaf) continue;
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(page->payload.data(), page->payload.size(),
                                  &cursor, &error) != GxStatus::Ok) {
            return std::vector<uint64_t>();
        }
        leafIds.push_back(pageId);
        nextIds.push_back(cursor.page.nextLeafPageId);
    }

    uint64_t first = 0;
    for (size_t i = 0; i < leafIds.size(); ++i) {
        bool hasIncoming = false;
        for (size_t j = 0; j < nextIds.size(); ++j) {
            if (nextIds[j] == leafIds[i]) hasIncoming = true;
        }
        if (!hasIncoming) {
            first = leafIds[i];
            break;
        }
    }
    if (first == 0) return std::vector<uint64_t>();

    std::vector<uint64_t> chain;
    uint64_t current = first;
    while (current != 0 && chain.size() <= leafIds.size()) {
        chain.push_back(current);
        size_t position = leafIds.size();
        for (size_t i = 0; i < leafIds.size(); ++i) {
            if (leafIds[i] == current) {
                position = i;
                break;
            }
        }
        if (position == leafIds.size()) return std::vector<uint64_t>();
        current = nextIds[position];
    }
    if (current != 0 || chain.size() != leafIds.size()) {
        return std::vector<uint64_t>();
    }
    return chain;
}

struct Match {
    uint64_t pageId;
    std::vector<uint8_t> key;
    uint64_t locatorPageId;
    uint32_t locatorSlot;
};

struct MatchCollector {
    std::vector<Match> matches;
    class ReusingProvider* provider;
    uint32_t stopAfter;

    MatchCollector() : matches(), provider(nullptr), stopAfter(0) {}
};

class ReusingProvider {
public:
    explicit ReusingProvider(const MemoryPageAccess& source)
        : pages(source), buffer(480, 0), overrides(), acquireCount(0),
          releaseCount(0), failOnAcquire(0), missingPageId(0), shortPageId(0),
          invalidPageId(0), active(false), firstBuffer(nullptr) {}

    const MemoryPageAccess& pages;
    std::vector<uint8_t> buffer;
    std::vector<std::pair<uint64_t, std::vector<uint8_t> > > overrides;
    uint64_t acquireCount;
    uint64_t releaseCount;
    uint64_t failOnAcquire;
    uint64_t missingPageId;
    uint64_t shortPageId;
    uint64_t invalidPageId;
    bool active;
    const uint8_t* firstBuffer;

    const std::vector<uint8_t>* overrideFor(uint64_t pageId) const {
        for (size_t i = 0; i < overrides.size(); ++i) {
            if (overrides[i].first == pageId) return &overrides[i].second;
        }
        return nullptr;
    }

    void setOverride(uint64_t pageId, const std::vector<uint8_t>& payload) {
        for (size_t i = 0; i < overrides.size(); ++i) {
            if (overrides[i].first == pageId) {
                overrides[i].second = payload;
                return;
            }
        }
        overrides.push_back(std::make_pair(pageId, payload));
    }

    static GxIndexPageProviderStatus acquire(
        void* opaque, uint64_t pageId, GxIndexPageView* outView) {
        ReusingProvider* self = static_cast<ReusingProvider*>(opaque);
        if (self == nullptr || outView == nullptr || self->active) {
            return GxIndexPageProviderStatus::Error;
        }
        ++self->acquireCount;
        outView->bytes = nullptr;
        outView->length = 0;
        outView->token = nullptr;
        if (self->failOnAcquire != 0 &&
            self->acquireCount == self->failOnAcquire) {
            return GxIndexPageProviderStatus::Error;
        }
        if (pageId == self->invalidPageId) {
            return GxIndexPageProviderStatus::InvalidPageId;
        }
        if (pageId == self->missingPageId) {
            return GxIndexPageProviderStatus::MissingPage;
        }
        const DatabasePage* source = self->pages.get(pageId);
        if (source == nullptr) return GxIndexPageProviderStatus::InvalidPageId;
        if (source->type != PageType::IndexLeaf) {
            return GxIndexPageProviderStatus::Error;
        }
        const std::vector<uint8_t>* payload = self->overrideFor(pageId);
        if (payload == nullptr) payload = &source->payload;
        if (payload->size() > self->buffer.size()) {
            return GxIndexPageProviderStatus::Error;
        }
        for (size_t i = 0; i < payload->size(); ++i) {
            self->buffer[i] = (*payload)[i];
        }
        self->active = true;
        if (self->firstBuffer == nullptr) self->firstBuffer = self->buffer.data();
        else CHECK(self->firstBuffer == self->buffer.data());
        outView->bytes = self->buffer.data();
        outView->length = payload->size();
        if (pageId == self->shortPageId && outView->length != 0) {
            --outView->length;
        }
        outView->token = &self->buffer;
        return GxIndexPageProviderStatus::Ok;
    }

    static void release(void* opaque, uint64_t, const GxIndexPageView* view) {
        ReusingProvider* self = static_cast<ReusingProvider*>(opaque);
        if (self == nullptr) return;
        CHECK(self->active);
        CHECK(view != nullptr);
        if (view != nullptr) {
            CHECK(view->token == &self->buffer);
            CHECK(view->bytes == self->buffer.data());
        }
        self->active = false;
        ++self->releaseCount;
        std::fill(self->buffer.begin(), self->buffer.end(), 0xDDu);
    }

    GxIndexPageProvider api() {
        GxIndexPageProvider provider = {this, acquire, release};
        return provider;
    }
};

GxIndexLeafVisitControl collectMatch(void* opaque, uint64_t pageId,
                                    const GxIndexLeafEntryView* entry) {
    MatchCollector* collector = static_cast<MatchCollector*>(opaque);
    if (collector == nullptr || collector->provider == nullptr ||
        entry == nullptr || !collector->provider->active ||
        entry->keyBytes < collector->provider->buffer.data() ||
        entry->keyBytes + entry->keyLength >
            collector->provider->buffer.data() +
                collector->provider->buffer.size()) {
        return GxIndexLeafVisitControl::Error;
    }
    Match match;
    match.pageId = pageId;
    match.key.assign(entry->keyBytes, entry->keyBytes + entry->keyLength);
    match.locatorPageId = entry->locatorPageId;
    match.locatorSlot = entry->locatorSlot;
    collector->matches.push_back(match);
    if (collector->stopAfter != 0 &&
        collector->matches.size() >= collector->stopAfter) {
        return GxIndexLeafVisitControl::Stop;
    }
    return GxIndexLeafVisitControl::Continue;
}

std::vector<uint8_t> physicalLowerBound(const std::vector<uint8_t>& key) {
    std::vector<uint8_t> result = key;
    result.insert(result.end(), 12, 0);
    return result;
}

GxIndexLeafTraversalStatus traverse(
    ReusingProvider& provider,
    uint64_t firstPageId,
    uint64_t pageCount,
    const std::vector<uint8_t>& key,
    uint64_t maxPages,
    MatchCollector& collector,
    GxIndexLeafTraversalResult& result) {
    const std::vector<uint8_t> probe = physicalLowerBound(key);
    uint8_t boundaryScratch[480] = {};
    collector.provider = &provider;
    GxIndexPageProvider api = provider.api();
    return gxIndexLeafLookupChain(
        &api, firstPageId, pageCount, 37, maxPages,
        probe.data(), probe.size(), boundaryScratch, sizeof(boundaryScratch),
        collectMatch, &collector, &result);
}

struct Seed {
    std::vector<uint8_t> key;
    uint32_t slot;
};

bool byteKeyLess(const std::vector<uint8_t>& left,
                 const std::vector<uint8_t>& right) {
    const size_t shared = left.size() < right.size() ? left.size() : right.size();
    for (size_t i = 0; i < shared; ++i) {
        if (left[i] != right[i]) return left[i] < right[i];
    }
    return left.size() < right.size();
}

bool createProductionLeafChain(MemoryPageAccess& pages, uint64_t& root,
                               std::vector<uint8_t>& duplicateKey,
                               std::vector<std::vector<uint8_t> >& edgeKeys) {
    GxAllocator traversalAllocator = {};
    CHECK(gxHostedAllocatorInit(&traversalAllocator, 65536) == GxStatus::Ok);
    IndexTree tree(pages, DbType::Text, 37, nullptr, &traversalAllocator);
    if (!tree.createEmpty(root).isOk()) return false;

    const std::vector<uint8_t> nullKey(1, 0x00u);
    const std::vector<uint8_t> boolFalse = {0x01u, 0x00u};
    const std::vector<uint8_t> boolTrue = {0x01u, 0x01u};
    const std::vector<uint8_t> embeddedZeroText = {0x01u, 'a', 0, 'b'};
    duplicateKey = std::vector<uint8_t>{0x01u, 'm', 0, 'n'};
    const std::vector<uint8_t> textAfter = {0x01u, 'z'};

    uint8_t int32MinimumBytes[5] = {};
    uint8_t int32MaximumBytes[5] = {};
    uint8_t int64MinimumBytes[9] = {};
    uint8_t int64MaximumBytes[9] = {};
    size_t int32MinimumLength = 0;
    size_t int32MaximumLength = 0;
    size_t int64MinimumLength = 0;
    size_t int64MaximumLength = 0;
    if (gxEncodeIndexInt32Key(false, (std::numeric_limits<int32_t>::min)(),
                              int32MinimumBytes, sizeof(int32MinimumBytes),
                              &int32MinimumLength) != GxStatus::Ok ||
        gxEncodeIndexInt32Key(false, (std::numeric_limits<int32_t>::max)(),
                              int32MaximumBytes, sizeof(int32MaximumBytes),
                              &int32MaximumLength) != GxStatus::Ok ||
        gxEncodeIndexInt64Key(false, (std::numeric_limits<int64_t>::min)(),
                              int64MinimumBytes, sizeof(int64MinimumBytes),
                              &int64MinimumLength) != GxStatus::Ok ||
        gxEncodeIndexInt64Key(false, (std::numeric_limits<int64_t>::max)(),
                              int64MaximumBytes, sizeof(int64MaximumBytes),
                              &int64MaximumLength) != GxStatus::Ok) {
        return false;
    }

    edgeKeys.push_back(nullKey);
    edgeKeys.push_back(boolFalse);
    edgeKeys.push_back(boolTrue);
    edgeKeys.push_back(std::vector<uint8_t>(int32MinimumBytes,
                                             int32MinimumBytes + int32MinimumLength));
    edgeKeys.push_back(std::vector<uint8_t>(int32MaximumBytes,
                                             int32MaximumBytes + int32MaximumLength));
    edgeKeys.push_back(std::vector<uint8_t>(int64MinimumBytes,
                                             int64MinimumBytes + int64MinimumLength));
    edgeKeys.push_back(std::vector<uint8_t>(int64MaximumBytes,
                                             int64MaximumBytes + int64MaximumLength));
    edgeKeys.push_back(embeddedZeroText);
    edgeKeys.push_back(duplicateKey);
    edgeKeys.push_back(textAfter);
    edgeKeys.push_back(std::vector<uint8_t>(1, 0xFEu));
    std::sort(edgeKeys.begin(), edgeKeys.end(), byteKeyLess);

    std::vector<Seed> seeds;
    uint32_t nextSlot = 1;
    for (size_t i = 0; i < edgeKeys.size(); ++i) {
        if (edgeKeys[i] == duplicateKey) {
            for (uint32_t duplicate = 0; duplicate < 64; ++duplicate) {
                Seed seed;
                seed.key = duplicateKey;
                seed.slot = 100u + duplicate;
                seeds.push_back(seed);
            }
        } else {
            Seed seed;
            seed.key = edgeKeys[i];
            seed.slot = nextSlot++;
            seeds.push_back(seed);
        }
    }
    std::sort(seeds.begin(), seeds.end(), [](const Seed& left, const Seed& right) {
        if (byteKeyLess(left.key, right.key)) return true;
        if (byteKeyLess(right.key, left.key)) return false;
        return left.slot < right.slot;
    });

    for (size_t i = 0; i < seeds.size(); ++i) {
        uint64_t newRoot = 0;
        bool inserted = false;
        const DbResult result = tree.insert(
            seeds[i].key, RowLocator(1, seeds[i].slot), root, false,
            newRoot, inserted);
        if (!result.isOk() || !inserted) return false;
        root = newRoot;
    }
    return true;
}

std::vector<Match> oldHostedOrderedScan(const MemoryPageAccess& pages,
                                        const std::vector<uint64_t>& chain,
                                        const std::vector<uint8_t>& key) {
    std::vector<Match> result;
    for (size_t pageIndex = 0; pageIndex < chain.size(); ++pageIndex) {
        const DatabasePage* page = pages.get(chain[pageIndex]);
        if (page == nullptr) break;
        GxIndexLeafCursor cursor = {};
        GxIndexLeafError error = GxIndexLeafError::None;
        if (gxIndexLeafCursorInit(page->payload.data(), page->payload.size(),
                                  &cursor, &error) != GxStatus::Ok) {
            break;
        }
        for (;;) {
            GxIndexLeafEntryView entry = {};
            bool hasEntry = false;
            if (gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry,
                                          &error) != GxStatus::Ok || !hasEntry) {
                break;
            }
            int comparison = 0;
            if (gxCompareIndexKeys(entry.keyBytes, entry.keyLength,
                                   key.data(), key.size(),
                                   &comparison) != GxStatus::Ok) {
                break;
            }
            if (comparison > 0) return result;
            if (comparison == 0) {
                Match match;
                match.pageId = chain[pageIndex];
                match.key.assign(entry.keyBytes,
                                 entry.keyBytes + entry.keyLength);
                match.locatorPageId = entry.locatorPageId;
                match.locatorSlot = entry.locatorSlot;
                result.push_back(match);
            }
        }
    }
    return result;
}

bool matchesEqual(const std::vector<Match>& left,
                  const std::vector<Match>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].key != right[i].key ||
            left[i].locatorPageId != right[i].locatorPageId ||
            left[i].locatorSlot != right[i].locatorSlot) {
            return false;
        }
    }
    return true;
}

bool writeFreestandingFixture(const char* path,
                              const MemoryPageAccess& pages,
                              const std::vector<uint64_t>& chain) {
    if (path == nullptr || chain.size() < 3) return false;
    std::ofstream output(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << pages.pageCount() << ' ' << 37 << ' ' << chain[0] << ' '
           << chain.size() << '\n';
    const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < chain.size(); ++i) {
        const DatabasePage* page = pages.get(chain[i]);
        if (page == nullptr || page->payload.size() != 480) return false;
        output << chain[i] << ' ';
        for (size_t j = 0; j < page->payload.size(); ++j) {
            const uint8_t value = page->payload[j];
            output.put(hex[value >> 4]);
            output.put(hex[value & 0x0Fu]);
        }
        output << '\n';
    }
    return output.good();
}

void setNext(MemoryPageAccess& pages, uint64_t pageId, uint64_t nextPageId) {
    DatabasePage* page = const_cast<DatabasePage*>(pages.get(pageId));
    if (page == nullptr || page->payload.size() < 16) return;
    storeLe64(page->payload.data() + 8, nextPageId);
}

void setNext(ReusingProvider& provider, uint64_t pageId,
             uint64_t nextPageId) {
    const DatabasePage* page = provider.pages.get(pageId);
    if (page == nullptr) return;
    std::vector<uint8_t> payload = page->payload;
    storeLe64(payload.data() + 8, nextPageId);
    provider.setOverride(pageId, payload);
}

void testSinglePageCases() {
    MemoryPageAccess pages;
    IndexTree tree(pages, DbType::Text, 37);
    uint64_t root = 0;
    CHECK(tree.createEmpty(root).isOk());
    const std::vector<uint8_t> key = {0x01u, 'x'};
    ReusingProvider emptyProvider(pages);
    MatchCollector emptyMatches;
    GxIndexLeafTraversalResult result = {};
    CHECK(traverse(emptyProvider, root, pages.pageCount(), key, 1,
                   emptyMatches, result) ==
          GxIndexLeafTraversalStatus::EndOfChain);
    CHECK(result.pagesVisited == 1 && result.matchesVisited == 0);
    CHECK(emptyProvider.acquireCount == 1 && emptyProvider.releaseCount == 1);

    uint64_t newRoot = 0;
    bool inserted = false;
    CHECK(tree.insert(key, RowLocator(1, 1), root, false,
                      newRoot, inserted).isOk());
    CHECK(inserted);
    root = newRoot;
    ReusingProvider oneProvider(pages);
    MatchCollector oneMatches;
    CHECK(traverse(oneProvider, root, pages.pageCount(), key, 1,
                   oneMatches, result) ==
          GxIndexLeafTraversalStatus::EndOfChain);
    CHECK(oneMatches.matches.size() == 1);
    CHECK(oneMatches.matches.size() == 1 &&
          oneMatches.matches[0].locatorSlot == 1);

    const std::vector<uint8_t> lower = {0x01u, 'a'};
    const std::vector<uint8_t> higher = {0x01u, 'z'};
    CHECK(tree.insert(key, RowLocator(1, 2), root, false,
                      newRoot, inserted).isOk());
    root = newRoot;
    CHECK(tree.insert(lower, RowLocator(1, 2), root, false,
                      newRoot, inserted).isOk());
    root = newRoot;
    CHECK(tree.insert(higher, RowLocator(1, 3), root, false,
                      newRoot, inserted).isOk());
    root = newRoot;
    ReusingProvider multipleProvider(pages);
    MatchCollector multipleMatches;
    const GxIndexLeafTraversalStatus multipleStatus = traverse(
        multipleProvider, root, pages.pageCount(), key, 1,
        multipleMatches, result);
    CHECK(multipleStatus == GxIndexLeafTraversalStatus::EndOfChain ||
          multipleStatus == GxIndexLeafTraversalStatus::KeyBoundary);
    CHECK(multipleMatches.matches.size() == 2);
}

void testProductionMultiPageLookup() {
    MemoryPageAccess pages;
    uint64_t root = 0;
    std::vector<uint8_t> duplicateKey;
    std::vector<std::vector<uint8_t> > edgeKeys;
    CHECK(createProductionLeafChain(pages, root, duplicateKey, edgeKeys));
    const std::vector<uint64_t> chain = leafChain(pages);
    CHECK(chain.size() >= 3);
    if (chain.size() < 3) return;

    std::vector<std::vector<uint8_t> > originalPayloads;
    for (size_t i = 0; i < chain.size(); ++i) {
        originalPayloads.push_back(pages.get(chain[i])->payload);
    }

    const uint64_t writesBeforeRead = pages.writeCount();
    const std::vector<Match> reference =
        oldHostedOrderedScan(pages, chain, duplicateKey);
    CHECK(reference.size() == 64);
    ReusingProvider provider(pages);
    MatchCollector matches;
    GxIndexLeafTraversalResult result = {};
    const GxIndexLeafTraversalStatus status = traverse(
        provider, chain[0], pages.pageCount(), duplicateKey,
        pages.pageCount(), matches, result);
    CHECK(status == GxIndexLeafTraversalStatus::EndOfChain ||
          status == GxIndexLeafTraversalStatus::KeyBoundary);
    CHECK(matchesEqual(matches.matches, reference));
    CHECK(result.matchesVisited == 64);
    CHECK(result.pagesVisited >= 3);
    CHECK(provider.acquireCount == provider.releaseCount);
    CHECK(!provider.active);
    CHECK(provider.firstBuffer == provider.buffer.data());

    // Every source leaf is an existing page image produced by IndexTree.
    // Traversal borrows only one provider buffer and does not mutate sources.
    for (size_t i = 0; i < chain.size(); ++i) {
        CHECK(pages.get(chain[i])->payload == originalPayloads[i]);
    }

    GxAllocator hostedTraversalAllocator = {};
    CHECK(gxHostedAllocatorInit(&hostedTraversalAllocator, 65536) == GxStatus::Ok);
    IndexTree tree(pages, DbType::Text, 37, nullptr,
                   &hostedTraversalAllocator);
    std::vector<IndexEntry> hosted;
    CHECK(tree.lookupAll(duplicateKey, root, hosted).isOk());
    CHECK(hosted.size() == reference.size());
    for (size_t i = 0; i < hosted.size() && i < reference.size(); ++i) {
        CHECK(hosted[i].key == reference[i].key);
        CHECK(hosted[i].locator.pageId == reference[i].locatorPageId);
        CHECK(hosted[i].locator.slot == reference[i].locatorSlot);
    }
    CHECK(pages.writeCount() == writesBeforeRead);

    bool sawMiddlePageMatch = false;
    for (size_t i = 0; i < matches.matches.size(); ++i) {
        for (size_t j = 1; j + 1 < chain.size(); ++j) {
            if (matches.matches[i].pageId == chain[j]) sawMiddlePageMatch = true;
        }
    }
    CHECK(sawMiddlePageMatch);

    // Exact keys placed at the beginning, through an intermediate duplicate
    // run, and at the end of the writer-produced chain.
    const std::vector<uint8_t> nullKey = edgeKeys.front();
    const std::vector<uint8_t> finalKey(1, 0xFEu);
    std::vector<std::vector<uint8_t> > exactKeys = edgeKeys;
    exactKeys.push_back(nullKey);
    exactKeys.push_back(duplicateKey);
    exactKeys.push_back(finalKey);
    for (size_t i = 0; i < exactKeys.size(); ++i) {
        ReusingProvider repeatedProvider(pages);
        MatchCollector repeatedMatches;
        const GxIndexLeafTraversalStatus exactStatus = traverse(
            repeatedProvider, chain[0], pages.pageCount(), exactKeys[i],
            pages.pageCount(), repeatedMatches, result);
        CHECK(exactStatus == GxIndexLeafTraversalStatus::EndOfChain ||
              exactStatus == GxIndexLeafTraversalStatus::KeyBoundary);
        const std::vector<Match> expected =
            oldHostedOrderedScan(pages, chain, exactKeys[i]);
        CHECK(matchesEqual(repeatedMatches.matches, expected));
        if (exactKeys[i] == nullKey) {
            CHECK(!expected.empty() && expected[0].pageId == chain[0]);
        }
        if (exactKeys[i] == finalKey) {
            CHECK(!expected.empty() &&
                  expected[expected.size() - 1].pageId == chain.back());
        }
    }

    // Missing keys before, between, and after valid serialized entries.
    const std::vector<std::vector<uint8_t> > missingKeys = {
        std::vector<uint8_t>{0x00u, 0x01u},
        std::vector<uint8_t>{0x01u, 0x00u, 0x01u},
        std::vector<uint8_t>{0xFFu}
    };
    for (size_t i = 0; i < missingKeys.size(); ++i) {
        ReusingProvider missingProvider(pages);
        MatchCollector missingMatches;
        const GxIndexLeafTraversalStatus missingStatus = traverse(
            missingProvider, chain[0], pages.pageCount(), missingKeys[i],
            pages.pageCount(), missingMatches, result);
        CHECK(missingMatches.matches.empty());
        CHECK(missingStatus == GxIndexLeafTraversalStatus::KeyBoundary ||
              missingStatus == GxIndexLeafTraversalStatus::EndOfChain);
    }

    // Repeated read-only traversals use and release the same reusable buffer.
    for (int repeat = 0; repeat < 3; ++repeat) {
        ReusingProvider repeatedProvider(pages);
        MatchCollector repeatedMatches;
        const GxIndexLeafTraversalStatus repeatedStatus = traverse(
            repeatedProvider, chain[0], pages.pageCount(), duplicateKey,
            pages.pageCount(), repeatedMatches, result);
        CHECK(repeatedStatus == GxIndexLeafTraversalStatus::EndOfChain ||
              repeatedStatus == GxIndexLeafTraversalStatus::KeyBoundary);
        CHECK(matchesEqual(repeatedMatches.matches, reference));
        CHECK(repeatedProvider.acquireCount == repeatedProvider.releaseCount);
        CHECK(!repeatedProvider.active);
    }

    // Cut the writer-produced chain after its second leaf to exercise a true
    // two-page traversal without creating a test-only page encoding.
    ReusingProvider twoPageProvider(pages);
    const DatabasePage* second = pages.get(chain[1]);
    std::vector<uint8_t> secondPayload = second->payload;
    storeLe64(secondPayload.data() + 8, 0);
    twoPageProvider.setOverride(chain[1], secondPayload);
    MatchCollector twoPageMatches;
    CHECK(traverse(twoPageProvider, chain[0], pages.pageCount(),
                   std::vector<uint8_t>{0xFFu}, pages.pageCount(),
                   twoPageMatches, result) ==
          GxIndexLeafTraversalStatus::EndOfChain);
    CHECK(result.pagesVisited == 2);
}

void testProviderErrorsAndCorruption() {
    MemoryPageAccess pages;
    uint64_t root = 0;
    std::vector<uint8_t> duplicateKey;
    std::vector<std::vector<uint8_t> > edgeKeys;
    CHECK(createProductionLeafChain(pages, root, duplicateKey, edgeKeys));
    const std::vector<uint64_t> chain = leafChain(pages);
    if (chain.size() < 3) return;
    const std::vector<uint8_t> beyond(1, 0xFFu);
    GxIndexLeafTraversalResult result = {};

    ReusingProvider invalidStart(pages);
    MatchCollector invalidStartMatches;
    CHECK(traverse(invalidStart, pages.pageCount(), pages.pageCount(),
                   beyond, pages.pageCount(), invalidStartMatches, result) ==
          GxIndexLeafTraversalStatus::InvalidPageId);
    CHECK(invalidStart.acquireCount == 0);

    ReusingProvider invalidLink(pages);
    setNext(invalidLink, chain[0], pages.pageCount());
    MatchCollector invalidLinkMatches;
    CHECK(traverse(invalidLink, chain[0], pages.pageCount(), beyond,
                   pages.pageCount(), invalidLinkMatches, result) ==
          GxIndexLeafTraversalStatus::InvalidPageId);
    CHECK(invalidLink.releaseCount == 1);

    ReusingProvider missing(pages);
    missing.missingPageId = chain[1];
    MatchCollector missingMatches;
    CHECK(traverse(missing, chain[0], pages.pageCount(), beyond,
                   pages.pageCount(), missingMatches, result) ==
          GxIndexLeafTraversalStatus::MissingPage);
    CHECK(missing.releaseCount == 1 && !missing.active);

    ReusingProvider failedProvider(pages);
    failedProvider.failOnAcquire = 2;
    MatchCollector failedMatches;
    CHECK(traverse(failedProvider, chain[0], pages.pageCount(), beyond,
                   pages.pageCount(), failedMatches, result) ==
          GxIndexLeafTraversalStatus::ProviderFailure);
    CHECK(failedProvider.acquireCount == 2);
    CHECK(failedProvider.releaseCount == 1 && !failedProvider.active);

    ReusingProvider shortProvider(pages);
    shortProvider.shortPageId = chain[0];
    MatchCollector shortMatches;
    CHECK(traverse(shortProvider, chain[0], pages.pageCount(), beyond,
                   pages.pageCount(), shortMatches, result) ==
          GxIndexLeafTraversalStatus::CorruptPage);
    CHECK(result.leafError == GxIndexLeafError::PageSizeOutOfRange);
    CHECK(shortProvider.releaseCount == 1);

    ReusingProvider malformed(pages);
    std::vector<uint8_t> malformedPayload = pages.get(chain[0])->payload;
    const uint32_t count = loadLe32(malformedPayload.data() + 16);
    if (count != 0) {
        const size_t slot = malformedPayload.size() - 8u;
        storeLe32(malformedPayload.data() + slot + 4u,
                  (std::numeric_limits<uint32_t>::max)());
    }
    malformed.setOverride(chain[0], malformedPayload);
    MatchCollector malformedMatches;
    CHECK(traverse(malformed, chain[0], pages.pageCount(), beyond,
                   pages.pageCount(), malformedMatches, result) ==
          GxIndexLeafTraversalStatus::CorruptPage);
    CHECK(malformed.releaseCount == 1);

    ReusingProvider wrongOwner(pages);
    std::vector<uint8_t> wrongOwnerPayload = pages.get(chain[0])->payload;
    storeLe32(wrongOwnerPayload.data() + 20, 99);
    wrongOwner.setOverride(chain[0], wrongOwnerPayload);
    MatchCollector wrongOwnerMatches;
    CHECK(traverse(wrongOwner, chain[0], pages.pageCount(), beyond,
                   pages.pageCount(), wrongOwnerMatches, result) ==
          GxIndexLeafTraversalStatus::CorruptPage);
    CHECK(wrongOwner.releaseCount == 1);

    ReusingProvider limited(pages);
    MatchCollector limitedMatches;
    CHECK(traverse(limited, chain[0], pages.pageCount(), beyond, 1,
                   limitedMatches, result) ==
          GxIndexLeafTraversalStatus::TraversalLimit);
    CHECK(limited.releaseCount == 1);

    ReusingProvider visitorStop(pages);
    MatchCollector stoppedMatches;
    stoppedMatches.stopAfter = 1;
    CHECK(traverse(visitorStop, chain[0], pages.pageCount(), duplicateKey,
                   pages.pageCount(), stoppedMatches, result) ==
          GxIndexLeafTraversalStatus::VisitorStopped);
    CHECK(visitorStop.releaseCount == result.pagesVisited &&
          visitorStop.releaseCount != 0 && !visitorStop.active);

    // The result limit is a hop bound, not a cycle diagnosis. Empty leaves
    // created by the production writer isolate self, two-page, and longer
    // cycles from cross-page key-order checks.
    MemoryPageAccess emptyPages;
    IndexTree emptyTree(emptyPages, DbType::Text, 37);
    uint64_t firstEmpty = 0;
    uint64_t secondEmpty = 0;
    uint64_t thirdEmpty = 0;
    CHECK(emptyTree.createEmpty(firstEmpty).isOk());
    CHECK(emptyTree.createEmpty(secondEmpty).isOk());
    CHECK(emptyTree.createEmpty(thirdEmpty).isOk());
    const std::vector<uint8_t> cycleKey(1, 0xFFu);
    const uint64_t emptyCount = emptyPages.pageCount();

    setNext(emptyPages, firstEmpty, firstEmpty);
    ReusingProvider selfCycle(emptyPages);
    MatchCollector selfMatches;
    CHECK(traverse(selfCycle, firstEmpty, emptyCount, cycleKey, 2,
                   selfMatches, result) ==
          GxIndexLeafTraversalStatus::TraversalLimit);
    CHECK(selfCycle.releaseCount == 2);

    setNext(emptyPages, firstEmpty, secondEmpty);
    setNext(emptyPages, secondEmpty, firstEmpty);
    ReusingProvider twoCycle(emptyPages);
    MatchCollector twoCycleMatches;
    CHECK(traverse(twoCycle, firstEmpty, emptyCount, cycleKey, 5,
                   twoCycleMatches, result) ==
          GxIndexLeafTraversalStatus::TraversalLimit);
    CHECK(twoCycle.releaseCount == emptyCount - 1);

    setNext(emptyPages, secondEmpty, thirdEmpty);
    setNext(emptyPages, thirdEmpty, firstEmpty);
    ReusingProvider longCycle(emptyPages);
    MatchCollector longCycleMatches;
    CHECK(traverse(longCycle, firstEmpty, emptyCount, cycleKey, 8,
                   longCycleMatches, result) ==
          GxIndexLeafTraversalStatus::TraversalLimit);
    CHECK(longCycle.releaseCount == emptyCount - 1);

    ReusingProvider badOrder(pages);
    setNext(badOrder, chain[0], chain.back());
    setNext(badOrder, chain.back(), chain[1]);
    MatchCollector badOrderMatches;
    CHECK(traverse(badOrder, chain[0], pages.pageCount(), beyond,
                   pages.pageCount(), badOrderMatches, result) ==
          GxIndexLeafTraversalStatus::CorruptPage);

    const std::vector<uint8_t> shortProbe(11, 0);
    uint8_t scratch[480] = {};
    GxIndexPageProvider api = invalidStart.api();
    GxIndexLeafTraversalResult unchanged = {};
    unchanged.pagesVisited = 91;
    CHECK(gxIndexLeafLookupChain(
              &api, chain[0], pages.pageCount(), 37, pages.pageCount(),
              shortProbe.data(), shortProbe.size(), scratch, sizeof(scratch),
              collectMatch, &invalidStartMatches, &unchanged) ==
          GxIndexLeafTraversalStatus::InvalidArgument);
    CHECK(invalidStart.acquireCount == 0);
    CHECK(unchanged.pagesVisited == 91);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--write-freestanding-fixture") == 0) {
        MemoryPageAccess pages;
        uint64_t root = 0;
        std::vector<uint8_t> duplicateKey;
        std::vector<std::vector<uint8_t> > edgeKeys;
        if (!createProductionLeafChain(pages, root, duplicateKey, edgeKeys)) {
            std::printf("failed to produce R3Z fixture through IndexTree\n");
            return 2;
        }
        const std::vector<uint64_t> chain = leafChain(pages);
        if (!writeFreestandingFixture(argv[2], pages, chain)) {
            std::printf("failed to write R3Z production fixture\n");
            return 2;
        }
        return 0;
    }
    std::printf("NDB1R4P11R3Z portable index cross-page traversal tests\n");
    testSinglePageCases();
    testProductionMultiPageLookup();
    testProviderErrorsAndCorruption();
    std::printf("NDB1R4P11R3Z portable index cross-page traversal: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
