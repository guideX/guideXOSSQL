#include <cctype>
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

uint32_t loadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
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

int hexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    character = static_cast<char>(std::tolower(
        static_cast<unsigned char>(character)));
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

std::vector<uint8_t> loadHostedGoldenFixture() {
    std::ifstream input(GX_R3X_FIXTURE_PATH, std::ios::binary);
    std::vector<uint8_t> bytes;
    int high = -1;
    char character = 0;
    while (input.get(character)) {
        if (std::isspace(static_cast<unsigned char>(character))) continue;
        const int nibble = hexValue(character);
        if (nibble < 0) return std::vector<uint8_t>();
        if (high < 0) {
            high = nibble;
        } else {
            bytes.push_back(static_cast<uint8_t>((high << 4) | nibble));
            high = -1;
        }
    }
    if (!input.eof() || high >= 0) return std::vector<uint8_t>();
    return bytes;
}

void testCapturedHostedLeafFixture() {
    // This payload was captured from IndexTree::writeLeaf at the clean R3X
    // starting revision before changing its read path.
    std::vector<uint8_t> bytes = loadHostedGoldenFixture();
    CHECK(bytes.size() == 480);
    if (bytes.size() != 480) return;
    const std::vector<uint8_t> unchanged = bytes;

    GxIndexLeafCursor cursor = {};
    GxIndexLeafError error = GxIndexLeafError::InvalidArgument;
    CHECK(gxIndexLeafCursorInit(bytes.data(), bytes.size(), &cursor, &error) ==
          GxStatus::Ok);
    CHECK(error == GxIndexLeafError::None);
    CHECK(cursor.page.entryCount == 7);
    CHECK(cursor.page.ownerIndexId == 37);
    CHECK(cursor.page.nextLeafPageId == 0);
    CHECK(gxIndexLeafValidate(bytes.data(), bytes.size(), &error) ==
          GxStatus::Ok);

    const uint8_t keys[][9] = {
        {0x00u},
        {0x01u, 0x00u},
        {0x01u, 0x01u},
        {0x01u, 'a', 0x00u, 'b'},
        {0x01u, 'a', 0x00u, 'b'},
        {0x01u, 0x80u, 0, 0, 0, 0, 0, 0, 0},
        {0x01u, 0x80u, 0, 0, 0}
    };
    const size_t keyLengths[] = {1, 2, 2, 4, 4, 9, 5};
    const uint64_t locatorPages[] = {1, 2, 3, 6, 8, 5, 4};
    const uint32_t locatorSlots[] = {0, 1, 2, 5, 1, 4, 3};
    for (size_t i = 0; i < 7; ++i) {
        GxIndexLeafEntryView entry = {};
        bool hasEntry = false;
        CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
              GxStatus::Ok);
        CHECK(hasEntry);
        CHECK(entry.keyLength == keyLengths[i]);
        CHECK(entry.keyLength == keyLengths[i] &&
              std::memcmp(entry.keyBytes, keys[i], keyLengths[i]) == 0);
        CHECK(entry.locatorPageId == locatorPages[i]);
        CHECK(entry.locatorSlot == locatorSlots[i]);
        CHECK(entry.keyBytes >= bytes.data() &&
              entry.keyBytes + entry.keyLength <= bytes.data() + bytes.size());
    }
    GxIndexLeafEntryView endEntry = {};
    bool hasEntry = true;
    CHECK(gxIndexLeafCursorReadNext(&cursor, &endEntry, &hasEntry, &error) ==
          GxStatus::Ok);
    CHECK(!hasEntry);
    CHECK(cursor.position == cursor.page.entryCount);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &endEntry, &hasEntry, &error) ==
          GxStatus::Ok);
    CHECK(!hasEntry);
    CHECK(bytes == unchanged);
}

void testEmptyAndEntryBoundaries() {
    std::vector<uint8_t> empty = makeLeaf(480, std::vector<TestEntry>());
    GxIndexLeafCursor cursor = {};
    GxIndexLeafEntryView entry = {};
    GxIndexLeafError error = GxIndexLeafError::None;
    bool hasEntry = true;
    CHECK(gxIndexLeafCursorInit(empty.data(), empty.size(), &cursor, &error) ==
          GxStatus::Ok);
    CHECK(cursor.page.entryCount == 0);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::Ok);
    CHECK(!hasEntry);
    CHECK(gxIndexLeafValidate(empty.data(), empty.size(), &error) ==
          GxStatus::Ok);

    TestEntry emptyKey = {std::vector<uint8_t>(), 0x0102030405060708ULL,
                          0xA1B2C3D4u};
    std::vector<TestEntry> oneEntry(1, emptyKey);
    std::vector<uint8_t> minimum = makeLeaf(480, oneEntry);
    CHECK(gxIndexLeafCursorInit(minimum.data(), minimum.size(), &cursor,
                                &error) == GxStatus::Ok);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::Ok);
    CHECK(hasEntry && entry.keyLength == 0);
    CHECK(entry.locatorPageId == emptyKey.locatorPageId);
    CHECK(entry.locatorSlot == emptyKey.locatorSlot);
    CHECK(entry.entryLength == 16);
    CHECK(gxIndexLeafValidate(minimum.data(), minimum.size(), &error) ==
          GxStatus::Ok);

    TestEntry maximumEntry;
    maximumEntry.key.assign(90, 0x42u);
    maximumEntry.locatorPageId = 9;
    maximumEntry.locatorSlot = 10;
    std::vector<uint8_t> maximum = makeLeaf(
        480, std::vector<TestEntry>(1, maximumEntry));
    CHECK(gxIndexLeafCursorInit(maximum.data(), maximum.size(), &cursor,
                                &error) == GxStatus::Ok);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::Ok);
    CHECK(hasEntry && entry.keyLength == 90);
    CHECK(entry.entryLength == 106);
    CHECK(gxIndexLeafValidate(maximum.data(), maximum.size(), &error) ==
          GxStatus::Ok);

    TestEntry absoluteMaximum;
    absoluteMaximum.key.assign(1024, 0x24u);
    absoluteMaximum.locatorPageId = 11;
    absoluteMaximum.locatorSlot = 12;
    std::vector<uint8_t> maximumPageEntry = makeLeaf(
        65504, std::vector<TestEntry>(1, absoluteMaximum));
    CHECK(gxIndexLeafCursorInit(maximumPageEntry.data(),
                                maximumPageEntry.size(), &cursor, &error) ==
          GxStatus::Ok);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::Ok);
    CHECK(hasEntry && entry.keyLength == 1024);
    CHECK(entry.entryLength == 1040);
    CHECK(gxIndexLeafValidate(maximumPageEntry.data(),
                              maximumPageEntry.size(), &error) ==
          GxStatus::Ok);

    std::vector<uint8_t> minimumPage = makeLeaf(480, std::vector<TestEntry>());
    std::vector<uint8_t> maximumPage = makeLeaf(65504, std::vector<TestEntry>());
    CHECK(gxIndexLeafCursorInit(minimumPage.data(), minimumPage.size(),
                                &cursor, &error) == GxStatus::Ok);
    CHECK(gxIndexLeafCursorInit(maximumPage.data(), maximumPage.size(),
                                &cursor, &error) == GxStatus::Ok);
    CHECK(gxIndexLeafValidate(maximumPage.data(), maximumPage.size(), &error) ==
          GxStatus::Ok);
}

void expectOpenError(const uint8_t* bytes, size_t length,
                     GxIndexLeafError expected) {
    GxIndexLeafCursor cursor = {};
    GxIndexLeafError error = GxIndexLeafError::None;
    CHECK(gxIndexLeafCursorInit(bytes, length, &cursor, &error) ==
          GxStatus::CorruptData);
    CHECK(error == expected);
}

void expectFirstEntryError(std::vector<uint8_t> bytes,
                           GxIndexLeafError expected) {
    GxIndexLeafCursor cursor = {};
    GxIndexLeafError error = GxIndexLeafError::None;
    CHECK(gxIndexLeafCursorInit(bytes.data(), bytes.size(), &cursor, &error) ==
          GxStatus::Ok);
    const std::vector<uint8_t> unchanged = bytes;
    GxIndexLeafEntryView entry = {};
    bool hasEntry = true;
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::CorruptData);
    CHECK(!hasEntry);
    CHECK(error == expected);
    CHECK(cursor.position == 0);
    CHECK(bytes == unchanged);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::CorruptData);
    CHECK(!hasEntry);
}

void testMalformedLeafData() {
    std::vector<uint8_t> shortHeader(23, 0);
    expectOpenError(shortHeader.data(), shortHeader.size(),
                    GxIndexLeafError::HeaderTruncated);
    std::vector<uint8_t> tooSmall(479, 0);
    expectOpenError(tooSmall.data(), tooSmall.size(),
                    GxIndexLeafError::PageSizeOutOfRange);
    std::vector<uint8_t> tooLarge(65505, 0);
    expectOpenError(tooLarge.data(), tooLarge.size(),
                    GxIndexLeafError::PageSizeOutOfRange);

    std::vector<uint8_t> badSignature = makeLeaf(480, std::vector<TestEntry>());
    badSignature[0] ^= 1;
    expectOpenError(badSignature.data(), badSignature.size(),
                    GxIndexLeafError::SignatureMismatch);
    std::vector<uint8_t> badVersion = makeLeaf(480, std::vector<TestEntry>());
    storeLe16(badVersion.data() + 4, 9);
    expectOpenError(badVersion.data(), badVersion.size(),
                    GxIndexLeafError::UnsupportedVersion);
    std::vector<uint8_t> badCount = makeLeaf(480, std::vector<TestEntry>());
    storeLe32(badCount.data() + 16, 0xFFFFFFFFu);
    expectOpenError(badCount.data(), badCount.size(),
                    GxIndexLeafError::ImpossibleEntryCount);

    std::vector<TestEntry> one(1);
    one[0].key.push_back(0x01u);
    one[0].locatorPageId = 1;
    one[0].locatorSlot = 1;

    std::vector<uint8_t> shortEntry = makeLeaf(480, one);
    uint32_t slot = 480 - 8;
    uint32_t offset = loadLe32(shortEntry.data() + slot);
    storeLe32(shortEntry.data() + slot + 4, 15);
    expectFirstEntryError(shortEntry, GxIndexLeafError::EntryTooSmall);

    std::vector<uint8_t> offsetPastEnd = makeLeaf(480, one);
    storeLe32(offsetPastEnd.data() + slot, 481);
    expectFirstEntryError(offsetPastEnd, GxIndexLeafError::EntryOutOfRange);

    std::vector<uint8_t> lengthPastEnd = makeLeaf(480, one);
    storeLe32(lengthPastEnd.data() + slot, 470);
    storeLe32(lengthPastEnd.data() + slot + 4, 0xFFFFFFFFu);
    expectFirstEntryError(lengthPastEnd, GxIndexLeafError::EntryOutOfRange);

    std::vector<uint8_t> arithmeticOverflow = makeLeaf(480, one);
    storeLe32(arithmeticOverflow.data() + slot, 0xFFFFFFFFu);
    storeLe32(arithmeticOverflow.data() + slot + 4, 0xFFFFFFFFu);
    expectFirstEntryError(arithmeticOverflow, GxIndexLeafError::EntryOutOfRange);

    std::vector<uint8_t> badKeyLength = makeLeaf(480, one);
    offset = loadLe32(badKeyLength.data() + slot);
    storeLe32(badKeyLength.data() + offset, 0xFFFFFFFFu);
    expectFirstEntryError(badKeyLength, GxIndexLeafError::KeyLengthInvalid);

    std::vector<uint8_t> missingLocator = makeLeaf(480, one);
    storeLe32(missingLocator.data() + slot + 4, 16);
    expectFirstEntryError(missingLocator, GxIndexLeafError::KeyLengthInvalid);

    std::vector<uint8_t> entryOverlapsDirectory = makeLeaf(
        480, std::vector<TestEntry>(1, TestEntry()));
    storeLe32(entryOverlapsDirectory.data() + slot, 460);
    storeLe32(entryOverlapsDirectory.data() + slot + 4, 16);
    expectFirstEntryError(entryOverlapsDirectory,
                          GxIndexLeafError::EntryOutOfRange);

    std::vector<TestEntry> duplicate(2, one[0]);
    std::vector<uint8_t> overlapping = makeLeaf(480, duplicate);
    const uint32_t firstOffset = loadLe32(overlapping.data() + 480 - 8);
    storeLe32(overlapping.data() + 480 - 16, firstOffset);
    GxIndexLeafError error = GxIndexLeafError::None;
    GxIndexLeafCursor cursor = {};
    GxIndexLeafEntryView entry = {};
    bool hasEntry = false;
    CHECK(gxIndexLeafCursorInit(overlapping.data(), overlapping.size(),
                                &cursor, &error) == GxStatus::Ok);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::Ok && hasEntry);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::Ok && hasEntry);
    CHECK(cursor.position == 2);
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, &hasEntry, &error) ==
          GxStatus::Ok && !hasEntry);
    CHECK(gxIndexLeafValidate(overlapping.data(), overlapping.size(), &error) ==
          GxStatus::CorruptData);
    CHECK(error == GxIndexLeafError::EntryRegionsOverlap);

    TestEntry low = {{0x01u, 0x10u}, 1, 2};
    TestEntry high = {{0x01u, 0x20u}, 1, 1};
    std::vector<TestEntry> unordered;
    unordered.push_back(high);
    unordered.push_back(low);
    std::vector<uint8_t> badOrder = makeLeaf(480, unordered);
    CHECK(gxIndexLeafValidate(badOrder.data(), badOrder.size(), &error) ==
          GxStatus::CorruptData);
    CHECK(error == GxIndexLeafError::PhysicalKeysNotIncreasing);

    TestEntry sameA = {{0x01u, 0x20u}, 3, 7};
    TestEntry sameB = sameA;
    std::vector<TestEntry> exactDuplicates;
    exactDuplicates.push_back(sameA);
    exactDuplicates.push_back(sameB);
    std::vector<uint8_t> duplicatePhysical = makeLeaf(480, exactDuplicates);
    CHECK(gxIndexLeafValidate(duplicatePhysical.data(),
                             duplicatePhysical.size(), &error) ==
          GxStatus::CorruptData);
    CHECK(error == GxIndexLeafError::PhysicalKeysNotIncreasing);
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

void testHostedIndexReaderAndValidatorReuse() {
    MemoryPageAccess pages;
    GxAllocator traversalAllocator = {};
    CHECK(gxHostedAllocatorInit(&traversalAllocator, 65536) == GxStatus::Ok);
    IndexTree tree(pages, DbType::Text, 37, nullptr, &traversalAllocator);
    uint64_t root = 0;
    CHECK(tree.createEmpty(root).isOk());
    const std::vector<uint8_t> key = {0x01u, 0x20u};
    RowLocator later(1, 9);
    RowLocator earlier(1, 2);
    uint64_t newRoot = 0;
    bool inserted = false;
    CHECK(tree.insert(key, later, root, false, newRoot, inserted).isOk());
    CHECK(inserted);
    root = newRoot;
    CHECK(tree.insert(key, earlier, root, false, newRoot, inserted).isOk());
    CHECK(inserted);
    root = newRoot;

    std::vector<IndexEntry> found;
    const DbResult lookupResult = tree.lookupAll(key, root, found);
    if (!lookupResult.isOk()) {
        std::printf("  hosted lookup error: %s\n", lookupResult.message().c_str());
    }
    CHECK(lookupResult.isOk());
    CHECK(found.size() == 2);
    CHECK(found.size() == 2 && found[0].locator.slot == 2);
    CHECK(found.size() == 2 && found[1].locator.slot == 9);

    IndexValidation validation;
    const DbResult validTreeResult = tree.validate(root, validation);
    if (!validTreeResult.isOk()) {
        std::printf("  hosted validation error: %s\n",
                    validTreeResult.message().c_str());
    }
    CHECK(validTreeResult.isOk());
    CHECK(validation.ok);
    CHECK(validation.entryCount == 2);

    DatabasePage page;
    CHECK(pages.readPage(root, page).isOk());
    DatabasePage savedPage = page;
    page.type = PageType::Data;
    CHECK(pages.writePage(page).isOk());
    found.clear();
    CHECK(tree.lookupAll(key, root, found).status() == DbStatus::CorruptPage);
    page = savedPage;
    storeLe32(page.payload.data() + 20, 38);
    CHECK(pages.writePage(page).isOk());
    found.clear();
    CHECK(tree.lookupAll(key, root, found).status() == DbStatus::CorruptPage);
    page = savedPage;

    const uint32_t count = loadLe32(page.payload.data() + 16);
    CHECK(count == 2);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t slotOffset = page.payload.size() - 8 * (i + 1u);
        const uint32_t entryOffset = loadLe32(page.payload.data() + slotOffset);
        const uint32_t keyLength = loadLe32(page.payload.data() + entryOffset);
        storeLe32(page.payload.data() + entryOffset + 4 + keyLength + 8,
                  i == 0 ? 9u : 2u);
    }
    CHECK(pages.writePage(page).isOk());
    validation = IndexValidation();
    CHECK(tree.validate(root, validation).status() == DbStatus::CorruptPage);
    CHECK(!validation.ok);
    CHECK(validation.message == "index leaf entries not strictly ordered");
}

void testInvalidArguments() {
    GxIndexLeafCursor cursor = {};
    GxIndexLeafError error = GxIndexLeafError::None;
    CHECK(gxIndexLeafCursorInit(nullptr, 480, &cursor, &error) ==
          GxStatus::InvalidArgument);
    CHECK(error == GxIndexLeafError::InvalidArgument);
    CHECK(gxIndexLeafCursorInit(nullptr, 0, nullptr, &error) ==
          GxStatus::InvalidArgument);
    CHECK(gxIndexLeafValidate(nullptr, 480, &error) ==
          GxStatus::InvalidArgument);

    std::vector<uint8_t> empty = makeLeaf(480, std::vector<TestEntry>());
    CHECK(gxIndexLeafCursorInit(empty.data(), empty.size(), &cursor, &error) ==
          GxStatus::Ok);
    bool hasEntry = true;
    GxIndexLeafEntryView entry = {};
    CHECK(gxIndexLeafCursorReadNext(&cursor, &entry, nullptr, &error) ==
          GxStatus::InvalidArgument);
    CHECK(gxIndexLeafCursorReadNext(nullptr, &entry, &hasEntry, &error) ==
          GxStatus::InvalidArgument);
}

} // namespace

int main() {
    testCapturedHostedLeafFixture();
    testEmptyAndEntryBoundaries();
    testMalformedLeafData();
    testHostedIndexReaderAndValidatorReuse();
    testInvalidArguments();
    std::printf("NDB1R4P11R3X portable index leaf traversal: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
