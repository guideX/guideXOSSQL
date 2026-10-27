#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "database_index.h"
#include "database_page.h"
#include "database_pagestore.h"
#include "gx_allocator_hosted.h"
#include "gx_index_leaf_entry.h"
#include "gx_index_key.h"

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

struct TestEntry {
    std::vector<uint8_t> key;
    uint64_t locatorPageId;
    uint32_t locatorSlot;
};

std::vector<uint8_t> makeLeaf(size_t pageLength,
                              const std::vector<TestEntry>& entries,
                              uint32_t ownerIndexId = 37,
                              uint64_t nextLeafPageId = 0) {
    std::vector<uint8_t> bytes(pageLength, 0);
    if (pageLength < 24 || entries.size() > (pageLength - 24) / 8) {
        return std::vector<uint8_t>();
    }
    storeLe32(bytes.data(), 0x4C495847u);
    storeLe16(bytes.data() + 4, 1);
    storeLe64(bytes.data() + 8, nextLeafPageId);
    storeLe32(bytes.data() + 16, static_cast<uint32_t>(entries.size()));
    storeLe32(bytes.data() + 20, ownerIndexId);
    size_t offset = 24;
    for (size_t i = 0; i < entries.size(); ++i) {
        const size_t entryLength = 16 + entries[i].key.size();
        if (offset > pageLength || entryLength > pageLength - offset) {
            return std::vector<uint8_t>();
        }
        storeLe32(bytes.data() + offset,
                  static_cast<uint32_t>(entries[i].key.size()));
        if (!entries[i].key.empty()) {
            std::memcpy(bytes.data() + offset + 4, entries[i].key.data(),
                        entries[i].key.size());
        }
        storeLe64(bytes.data() + offset + 4 + entries[i].key.size(),
                  entries[i].locatorPageId);
        storeLe32(bytes.data() + offset + 12 + entries[i].key.size(),
                  entries[i].locatorSlot);
        const size_t slotOffset = pageLength - 8 * (i + 1);
        storeLe32(bytes.data() + slotOffset, static_cast<uint32_t>(offset));
        storeLe32(bytes.data() + slotOffset + 4,
                  static_cast<uint32_t>(entryLength));
        offset += entryLength;
    }
    return bytes;
}

std::vector<uint8_t> makePhysical(const std::vector<uint8_t>& key,
                                  uint64_t pageId, uint32_t slot) {
    std::vector<uint8_t> physical = key;
    for (int shift = 56; shift >= 0; shift -= 8) {
        physical.push_back(static_cast<uint8_t>(pageId >> shift));
    }
    physical.push_back(static_cast<uint8_t>(slot >> 24));
    physical.push_back(static_cast<uint8_t>(slot >> 16));
    physical.push_back(static_cast<uint8_t>(slot >> 8));
    physical.push_back(static_cast<uint8_t>(slot));
    return physical;
}

std::vector<uint8_t> loadFixture() {
    std::ifstream input(GX_R3Y_FIXTURE_PATH, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    std::vector<uint8_t> bytes;
    int high = -1;
    for (size_t i = 0; i < contents.size(); ++i) {
        const unsigned char character =
            static_cast<unsigned char>(contents[i]);
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n') {
            continue;
        }
        int digit = -1;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        }
        if (digit < 0) return std::vector<uint8_t>();
        if (high < 0) {
            high = digit;
        } else {
            bytes.push_back(static_cast<uint8_t>((high << 4) | digit));
            high = -1;
        }
    }
    if (high >= 0 || input.bad()) return std::vector<uint8_t>();
    return bytes;
}

bool lookup(const std::vector<uint8_t>& page,
            const std::vector<uint8_t>& physical,
            GxIndexLeafLookupResult& result,
            GxIndexLeafError& error) {
    return gxIndexLeafLookup(page.data(), page.size(),
                             physical.empty() ? nullptr : physical.data(),
                             physical.size(), &result, &error) == GxStatus::Ok;
}

void testLeafLocalLowerBoundAndExactPhysicalIdentity() {
    std::vector<TestEntry> entries;
    entries.push_back(TestEntry{{0x10u}, 1, 1});
    entries.push_back(TestEntry{{0x20u}, 1, 1});
    entries.push_back(TestEntry{{0x30u}, 1, 1});
    const std::vector<uint8_t> page = makeLeaf(480, entries);
    for (uint32_t i = 0; i < 3; ++i) {
        const std::vector<uint8_t> query = makePhysical(entries[i].key, 1, 1);
        GxIndexLeafLookupResult result = {};
        GxIndexLeafError error = GxIndexLeafError::InvalidArgument;
        CHECK(lookup(page, query, result, error));
        CHECK(error == GxIndexLeafError::None);
        CHECK(result.hasEntry && result.exactMatch);
        CHECK(result.entryPosition == i);
        CHECK(result.entry.keyLength == entries[i].key.size());
        CHECK(result.entry.locatorPageId == 1 && result.entry.locatorSlot == 1);
        CHECK(result.continuation.position == i + 1);
    }

    const std::vector<uint8_t> before = makePhysical({0x05u}, 0, 0);
    const std::vector<uint8_t> between = makePhysical({0x18u}, 0, 0);
    const std::vector<uint8_t> after = makePhysical({0x40u}, 0, 0);
    GxIndexLeafLookupResult result = {};
    GxIndexLeafError error = GxIndexLeafError::None;
    CHECK(lookup(page, before, result, error));
    CHECK(result.hasEntry && !result.exactMatch && result.entryPosition == 0);
    CHECK(result.entry.keyBytes[0] == 0x10u);
    CHECK(lookup(page, between, result, error));
    CHECK(result.hasEntry && !result.exactMatch && result.entryPosition == 1);
    CHECK(result.entry.keyBytes[0] == 0x20u);
    CHECK(lookup(page, after, result, error));
    CHECK(!result.hasEntry && !result.exactMatch && result.entryPosition == 3);
    CHECK(result.continuation.position == 3);

    const std::vector<uint8_t> original = page;
    for (int repeat = 0; repeat < 4; ++repeat) {
        CHECK(lookup(page, between, result, error));
        CHECK(result.entryPosition == 1 && result.entry.keyBytes[0] == 0x20u);
    }
    CHECK(page == original);

    const std::vector<TestEntry> duplicates = {
        TestEntry{{0x01u, 'x'}, 5, 2},
        TestEntry{{0x01u, 'x'}, 5, 9}
    };
    const std::vector<uint8_t> duplicatePage = makeLeaf(480, duplicates);
    const std::vector<uint8_t> firstPhysical =
        makePhysical(duplicates[0].key, 5, 2);
    const std::vector<uint8_t> secondPhysical =
        makePhysical(duplicates[1].key, 5, 9);
    const std::vector<uint8_t> betweenLocators =
        makePhysical(duplicates[0].key, 5, 4);
    CHECK(lookup(duplicatePage, firstPhysical, result, error));
    CHECK(result.hasEntry && result.exactMatch && result.entryPosition == 0);
    CHECK(lookup(duplicatePage, secondPhysical, result, error));
    CHECK(result.hasEntry && result.exactMatch && result.entryPosition == 1);
    CHECK(lookup(duplicatePage, betweenLocators, result, error));
    CHECK(result.hasEntry && !result.exactMatch && result.entryPosition == 1);
    CHECK(result.entry.locatorPageId == 5 && result.entry.locatorSlot == 9);
}

void testEmptyLeafAndKeyFamilies() {
    std::vector<TestEntry> noEntries;
    const std::vector<uint8_t> empty = makeLeaf(480, noEntries);
    GxIndexLeafLookupResult result = {};
    GxIndexLeafError error = GxIndexLeafError::None;
    const std::vector<uint8_t> arbitrary = makePhysical({0x42u}, 1, 1);
    CHECK(lookup(empty, arbitrary, result, error));
    CHECK(!result.hasEntry && !result.exactMatch && result.entryPosition == 0);
    CHECK(result.continuation.position == 0);

    const std::vector<TestEntry> one = {TestEntry{{0x55u}, 3, 7}};
    const std::vector<uint8_t> onePage = makeLeaf(480, one);
    const std::vector<uint8_t> exactOne = makePhysical(one[0].key, 3, 7);
    CHECK(lookup(onePage, exactOne, result, error));
    CHECK(result.hasEntry && result.exactMatch && result.entryPosition == 0);

    // The captured hosted fixture exercises NULL, Boolean, signed integer,
    // binary Text, and duplicate logical-key representations together.
    const std::vector<uint8_t> fixture = loadFixture();
    CHECK(fixture.size() == 480);
    if (fixture.size() == 480) {
        CHECK(gxIndexLeafValidate(fixture.data(), fixture.size(), &error) ==
              GxStatus::Ok);
        const std::vector<TestEntry> fixtureEntries = {
            TestEntry{{0x00u}, 1, 0},
            TestEntry{{0x01u, 0x00u}, 2, 1},
            TestEntry{{0x01u, 0x01u}, 3, 2},
            TestEntry{{0x01u, 'a', 0x00u, 'b'}, 6, 5},
            TestEntry{{0x01u, 'a', 0x00u, 'b'}, 8, 1},
            TestEntry{{0x01u, 0x80u, 0, 0, 0, 0, 0, 0, 0}, 5, 4},
            TestEntry{{0x01u, 0x80u, 0, 0, 0}, 4, 3}
        };
        for (uint32_t i = 0; i < fixtureEntries.size(); ++i) {
            const std::vector<uint8_t> query = makePhysical(
                fixtureEntries[i].key, fixtureEntries[i].locatorPageId,
                fixtureEntries[i].locatorSlot);
            CHECK(lookup(fixture, query, result, error));
            CHECK(result.hasEntry && result.exactMatch &&
                  result.entryPosition == i);
        }
    }

    const std::vector<TestEntry> booleans = {
        TestEntry{{0x01u, 0x00u}, 1, 1},
        TestEntry{{0x01u, 0x01u}, 1, 1}
    };
    const std::vector<uint8_t> boolPage = makeLeaf(480, booleans);
    CHECK(lookup(boolPage, makePhysical(booleans[0].key, 1, 1), result,
                 error));
    CHECK(result.exactMatch && result.entryPosition == 0);
    CHECK(lookup(boolPage, makePhysical(booleans[1].key, 1, 1), result,
                 error));
    CHECK(result.exactMatch && result.entryPosition == 1);

    const std::vector<TestEntry> int32Bounds = {
        TestEntry{{0x01u, 0, 0, 0, 0}, 1, 1},
        TestEntry{{0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu}, 1, 1}
    };
    const std::vector<uint8_t> int32Page = makeLeaf(480, int32Bounds);
    CHECK(lookup(int32Page, makePhysical(int32Bounds[0].key, 1, 1), result,
                 error));
    CHECK(result.exactMatch && result.entryPosition == 0);
    CHECK(lookup(int32Page, makePhysical(int32Bounds[1].key, 1, 1), result,
                 error));
    CHECK(result.exactMatch && result.entryPosition == 1);

    const std::vector<TestEntry> int64Bounds = {
        TestEntry{{0x01u, 0, 0, 0, 0, 0, 0, 0, 0}, 1, 1},
        TestEntry{{0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
                   0xFFu, 0xFFu}, 1, 1}
    };
    const std::vector<uint8_t> int64Page = makeLeaf(480, int64Bounds);
    CHECK(lookup(int64Page, makePhysical(int64Bounds[0].key, 1, 1), result,
                 error));
    CHECK(result.exactMatch && result.entryPosition == 0);
    CHECK(lookup(int64Page, makePhysical(int64Bounds[1].key, 1, 1), result,
                 error));
    CHECK(result.exactMatch && result.entryPosition == 1);

    const std::vector<TestEntry> texts = {
        TestEntry{{0x01u}, 2, 1},
        TestEntry{{0x01u, 'a', 0, 'b'}, 2, 1}
    };
    const std::vector<uint8_t> textPage = makeLeaf(480, texts);
    CHECK(lookup(textPage, makePhysical(texts[0].key, 2, 1), result, error));
    CHECK(result.exactMatch && result.entryPosition == 0);
    CHECK(lookup(textPage, makePhysical(texts[1].key, 2, 1), result, error));
    CHECK(result.exactMatch && result.entryPosition == 1);
}

void expectCorrupt(const std::vector<uint8_t>& page,
                   GxIndexLeafError expectedError) {
    const std::vector<uint8_t> query = makePhysical({0x20u}, 1, 1);
    GxIndexLeafLookupResult result = {};
    result.entryPosition = 91;
    result.exactMatch = true;
    result.hasEntry = true;
    GxIndexLeafError error = GxIndexLeafError::None;
    const GxStatus status = gxIndexLeafLookup(
        page.data(), page.size(), query.data(), query.size(), &result, &error);
    CHECK(status == GxStatus::CorruptData);
    CHECK(error == expectedError);
    CHECK(!result.hasEntry && !result.exactMatch && result.entryPosition == 0);
    CHECK(result.continuation.pageBytes == nullptr);
    CHECK(result.entry.keyBytes == nullptr);
}

void testMalformedBoundsAndFailureStates() {
    const std::vector<TestEntry> one = {TestEntry{{0x20u}, 1, 1}};
    const std::vector<uint8_t> valid = makeLeaf(480, one);
    const std::vector<uint8_t> query = makePhysical(one[0].key, 1, 1);
    GxIndexLeafLookupResult result = {};
    GxIndexLeafError error = GxIndexLeafError::None;

    CHECK(gxIndexLeafLookup(valid.data(), 479, query.data(), query.size(),
                            &result, &error) == GxStatus::CorruptData);
    CHECK(error == GxIndexLeafError::PageSizeOutOfRange);
    CHECK(!result.hasEntry && result.entryPosition == 0);

    std::vector<uint8_t> badOffset = valid;
    storeLe32(badOffset.data() + badOffset.size() - 8, UINT32_MAX);
    expectCorrupt(badOffset, GxIndexLeafError::EntryOutOfRange);

    std::vector<uint8_t> tooLong = valid;
    storeLe32(tooLong.data() + tooLong.size() - 8, 24);
    storeLe32(tooLong.data() + tooLong.size() - 4, UINT32_MAX);
    expectCorrupt(tooLong, GxIndexLeafError::EntryOutOfRange);

    std::vector<uint8_t> noLocator = valid;
    storeLe32(noLocator.data() + noLocator.size() - 4, 16);
    expectCorrupt(noLocator, GxIndexLeafError::KeyLengthInvalid);

    std::vector<uint8_t> truncatedEntry = valid;
    storeLe32(truncatedEntry.data() + truncatedEntry.size() - 4, 15);
    expectCorrupt(truncatedEntry, GxIndexLeafError::EntryTooSmall);

    std::vector<uint8_t> badSequence = makeLeaf(
        480, {TestEntry{{0x30u}, 1, 1}, TestEntry{{0x10u}, 1, 1}});
    expectCorrupt(badSequence, GxIndexLeafError::PhysicalKeysNotIncreasing);

    result.entryPosition = 42;
    result.exactMatch = true;
    result.hasEntry = true;
    CHECK(gxIndexLeafLookup(nullptr, 480, query.data(), query.size(),
                            &result, &error) == GxStatus::InvalidArgument);
    CHECK(error == GxIndexLeafError::InvalidArgument);
    CHECK(!result.hasEntry && !result.exactMatch && result.entryPosition == 0);
    CHECK(gxIndexLeafLookup(valid.data(), valid.size(), nullptr, 1, &result,
                            &error) == GxStatus::InvalidArgument);
    CHECK(error == GxIndexLeafError::InvalidArgument);
    CHECK(!result.hasEntry && result.entryPosition == 0);
    const uint8_t logicalOnlyKey[] = {0x20u};
    CHECK(gxIndexLeafLookup(valid.data(), valid.size(), logicalOnlyKey,
                            sizeof(logicalOnlyKey), &result, &error) ==
          GxStatus::InvalidArgument);
    CHECK(error == GxIndexLeafError::InvalidArgument);
    CHECK(!result.hasEntry && !result.exactMatch && result.entryPosition == 0);
    CHECK(gxIndexLeafLookup(valid.data(), valid.size(), query.data(),
                            query.size(), nullptr, &error) ==
          GxStatus::InvalidArgument);

    std::vector<uint8_t> aliasPage = valid;
    const std::vector<uint8_t> aliasPageOriginal = aliasPage;
    GxIndexLeafLookupResult* aliasedResult =
        reinterpret_cast<GxIndexLeafLookupResult*>(aliasPage.data() + 24);
    CHECK(gxIndexLeafLookup(aliasPage.data(), aliasPage.size(), query.data(),
                            query.size(), aliasedResult, &error) ==
          GxStatus::InvalidArgument);
    CHECK(error == GxIndexLeafError::InvalidArgument);
    CHECK(aliasPage == aliasPageOriginal);

    const std::vector<uint8_t> original = valid;
    CHECK(gxIndexLeafLookup(valid.data(), valid.size(), nullptr,
                            static_cast<size_t>(-1), &result, &error) ==
          GxStatus::InvalidArgument);
    CHECK(valid == original);
}

class MemoryPageAccess : public PageAccess {
public:
    MemoryPageAccess() : _pages(1) {}

    uint32_t pageSize() const override { return 512; }
    uint64_t pageCount() const override { return _pages.size(); }
    bool isReadOnly() const override { return false; }

    DbResult readPage(uint64_t pageId, DatabasePage& out) override {
        if (pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "test page is out of range");
        }
        out = _pages[static_cast<size_t>(pageId)];
        return DbResult::ok();
    }

    DbResult writePage(const DatabasePage& page) override {
        if (page.pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "test page is out of range");
        }
        _pages[static_cast<size_t>(page.pageId)] = page;
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

private:
    std::vector<DatabasePage> _pages;
};

void testHostedLookupMatchesExistingOrderedScan() {
    const std::vector<uint8_t> fixture = loadFixture();
    CHECK(fixture.size() == 480);
    if (fixture.size() != 480) return;

    const std::vector<uint8_t> logicalKey = {0x01u, 'a', 0, 'b'};
    const std::vector<uint8_t> minimumPhysical = makePhysical(logicalKey, 0, 0);
    GxIndexLeafLookupResult lowerBound = {};
    GxIndexLeafError error = GxIndexLeafError::None;
    CHECK(lookup(fixture, minimumPhysical, lowerBound, error));
    CHECK(lowerBound.hasEntry && !lowerBound.exactMatch &&
          lowerBound.entryPosition == 3);

    // This reference loop is the pre-R3Y hosted lookup's ordered scan decision.
    std::vector<IndexEntry> reference;
    GxIndexLeafCursor oldCursor = {};
    CHECK(gxIndexLeafCursorInit(fixture.data(), fixture.size(), &oldCursor,
                                &error) == GxStatus::Ok);
    for (;;) {
        GxIndexLeafEntryView entry = {};
        bool hasEntry = false;
        CHECK(gxIndexLeafCursorReadNext(&oldCursor, &entry, &hasEntry,
                                        &error) == GxStatus::Ok);
        if (!hasEntry) break;
        int comparison = 0;
        CHECK(gxCompareIndexKeys(entry.keyBytes, entry.keyLength,
                                 logicalKey.data(), logicalKey.size(),
                                 &comparison) == GxStatus::Ok);
        if (comparison == 0) {
            IndexEntry found;
            found.key.assign(entry.keyBytes, entry.keyBytes + entry.keyLength);
            found.locator.pageId = entry.locatorPageId;
            found.locator.slot = entry.locatorSlot;
            reference.push_back(found);
        } else if (comparison > 0) {
            break;
        }
    }
    CHECK(reference.size() == 2);
    CHECK(reference.size() == 2 && reference[0].locator.pageId == 6 &&
          reference[0].locator.slot == 5);
    CHECK(reference.size() == 2 && reference[1].locator.pageId == 8 &&
          reference[1].locator.slot == 1);
    CHECK(lowerBound.entry.locatorPageId == reference[0].locator.pageId &&
          lowerBound.entry.locatorSlot == reference[0].locator.slot);

    MemoryPageAccess pages;
    GxAllocator traversalAllocator = {};
    CHECK(gxHostedAllocatorInit(&traversalAllocator, 65536) == GxStatus::Ok);
    IndexTree tree(pages, DbType::Text, 37, nullptr, &traversalAllocator);
    uint64_t root = 0;
    CHECK(tree.createEmpty(root).isOk());
    DatabasePage leaf;
    CHECK(pages.readPage(root, leaf).isOk());
    leaf.payload = fixture;
    leaf.payloadSize = static_cast<uint32_t>(fixture.size());
    CHECK(pages.writePage(leaf).isOk());

    std::vector<IndexEntry> hosted;
    CHECK(tree.lookupAll(logicalKey, root, hosted).isOk());
    CHECK(hosted.size() == reference.size());
    for (size_t i = 0; i < hosted.size() && i < reference.size(); ++i) {
        CHECK(hosted[i].key == reference[i].key);
        CHECK(hosted[i].locator.pageId == reference[i].locator.pageId);
        CHECK(hosted[i].locator.slot == reference[i].locator.slot);
    }

    std::vector<uint8_t> unordered = makeLeaf(
        480, {TestEntry{{0x30u}, 1, 1}, TestEntry{{0x10u}, 1, 1}});
    leaf.payload = unordered;
    CHECK(pages.writePage(leaf).isOk());
    hosted.clear();
    CHECK(tree.lookupAll({0x20u}, root, hosted).status() ==
          DbStatus::CorruptPage);
}

} // namespace

int main() {
    testLeafLocalLowerBoundAndExactPhysicalIdentity();
    testEmptyLeafAndKeyFamilies();
    testMalformedBoundsAndFailureStates();
    testHostedLookupMatchesExistingOrderedScan();
    std::printf("NDB1R4P11R3Y portable index leaf lookup: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
