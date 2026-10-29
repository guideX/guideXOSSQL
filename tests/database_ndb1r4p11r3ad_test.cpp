#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "database_index.h"
#include "portable/gx_allocator_hosted.h"

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

class SnapshotPageAccess : public PageAccess {
public:
    explicit SnapshotPageAccess(uint32_t pageSize = 512)
        : _pageSize(pageSize), _pages(1), _present(1, true), _readOnly(false),
          _expired(false), _readCount(0), _failOnRead(0), _missingPageId(0),
          _readIds(), _readTypes() {
        _pages[0].pageId = 0;
    }

    uint32_t pageSize() const override { return _pageSize; }
    uint64_t pageCount() const override { return _pages.size(); }
    bool isReadOnly() const override { return _readOnly; }

    DbResult readPage(uint64_t pageId, DatabasePage& out) override {
        ++_readCount;
        _readIds.push_back(pageId);
        if (_expired) {
            return DbResult::error(DbStatus::NoActiveTransaction,
                                   "R3AD snapshot expired");
        }
        if (_failOnRead != 0 && _readCount == _failOnRead) {
            return DbResult::error(DbStatus::IoError,
                                   "R3AD injected snapshot read failure");
        }
        if (pageId == 0 || pageId >= _pages.size() ||
            !_present[static_cast<size_t>(pageId)] || pageId == _missingPageId) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "R3AD page unavailable in snapshot");
        }
        out = _pages[static_cast<size_t>(pageId)];
        _readTypes.push_back(out.type);
        return DbResult::ok();
    }

    DbResult writePage(const DatabasePage& page) override {
        if (_readOnly) {
            return DbResult::error(DbStatus::ReadOnly,
                                   "R3AD snapshot is read-only");
        }
        if (page.pageId == 0 || page.pageId >= _pages.size()) {
            return DbResult::error(DbStatus::OutOfBounds,
                                   "R3AD page write is out of range");
        }
        _pages[static_cast<size_t>(page.pageId)] = page;
        _present[static_cast<size_t>(page.pageId)] = true;
        return DbResult::ok();
    }

    DbResult allocatePage(PageType type, uint64_t& outPageId) override {
        if (_readOnly) {
            return DbResult::error(DbStatus::ReadOnly,
                                   "R3AD snapshot is read-only");
        }
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

    bool loadFixture(const char* path, uint64_t& outRoot,
                     uint32_t& outOwner, uint32_t& outDuplicates) {
        std::ifstream input(path);
        uint32_t fixturePageSize = 0;
        uint64_t fixturePageCount = 0;
        size_t pageTotal = 0;
        if (!input || !(input >> fixturePageSize >> fixturePageCount >> outOwner
                        >> outRoot >> outDuplicates >> pageTotal) ||
            fixturePageSize != 512 || fixturePageCount < 2 || outRoot == 0 ||
            outRoot >= fixturePageCount || pageTotal + 1 != fixturePageCount) {
            return false;
        }
        _pageSize = fixturePageSize;
        _pages.assign(static_cast<size_t>(fixturePageCount), DatabasePage());
        _present.assign(static_cast<size_t>(fixturePageCount), false);
        _pages[0].pageId = 0;
        _present[0] = true;
        for (size_t i = 0; i < pageTotal; ++i) {
            DatabasePage page;
            uint16_t type = 0;
            std::string encoded;
            if (!(input >> page.pageId >> type >> encoded) || page.pageId == 0 ||
                page.pageId >= fixturePageCount || !parseHex(encoded, page.payload) ||
                page.payload.size() != DatabasePage::payloadCapacity(_pageSize)) {
                return false;
            }
            page.type = static_cast<PageType>(type);
            page.payloadSize = static_cast<uint32_t>(page.payload.size());
            _pages[static_cast<size_t>(page.pageId)] = page;
            _present[static_cast<size_t>(page.pageId)] = true;
        }
        std::string extra;
        resetTrace();
        return !(input >> extra);
    }

    void setReadOnly(bool value) { _readOnly = value; }
    void expire() { _expired = true; }
    void failOnRead(uint64_t readNumber) {
        resetTrace();
        _failOnRead = readNumber;
    }
    void makeMissing(uint64_t pageId) { _missingPageId = pageId; }
    void corruptIdentity(uint64_t pageId) {
        if (pageId < _pages.size()) ++_pages[static_cast<size_t>(pageId)].pageId;
    }
    void resetTrace() {
        _readCount = 0;
        _failOnRead = 0;
        _readIds.clear();
        _readTypes.clear();
    }
    uint64_t readCount() const { return _readCount; }
    const std::vector<uint64_t>& readIds() const { return _readIds; }
    const std::vector<PageType>& readTypes() const { return _readTypes; }
    const DatabasePage* page(uint64_t pageId) const {
        return pageId < _pages.size() && _present[static_cast<size_t>(pageId)]
            ? &_pages[static_cast<size_t>(pageId)] : nullptr;
    }
    DatabasePage* mutablePage(uint64_t pageId) {
        return pageId < _pages.size() && _present[static_cast<size_t>(pageId)]
            ? &_pages[static_cast<size_t>(pageId)] : nullptr;
    }

private:
    uint32_t _pageSize;
    std::vector<DatabasePage> _pages;
    std::vector<bool> _present;
    bool _readOnly;
    bool _expired;
    uint64_t _readCount;
    uint64_t _failOnRead;
    uint64_t _missingPageId;
    std::vector<uint64_t> _readIds;
    std::vector<PageType> _readTypes;
};

std::vector<uint8_t> textKey(const std::string& value) {
    std::vector<uint8_t> key(1, 1);
    key.insert(key.end(), value.begin(), value.end());
    return key;
}

bool sameEntries(const std::vector<IndexEntry>& actual,
                 const std::vector<IndexEntry>& expected) {
    if (actual.size() != expected.size()) return false;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].key != expected[i].key ||
            actual[i].locator.pageId != expected[i].locator.pageId ||
            actual[i].locator.slot != expected[i].locator.slot) return false;
    }
    return true;
}

std::vector<IndexEntry> expectedFixtureEntries() {
    std::vector<IndexEntry> entries;
    for (uint32_t i = 0; i < 160; ++i) {
        char text[16];
        std::snprintf(text, sizeof(text), "a%05u", i);
        IndexEntry entry;
        entry.key = textKey(text);
        entry.locator = RowLocator(1, 1u + i);
        entries.push_back(entry);
    }
    for (uint32_t i = 0; i < 100; ++i) {
        IndexEntry entry;
        entry.key = textKey("m-dup");
        entry.locator = RowLocator(1, 1000u + i);
        entries.push_back(entry);
    }
    for (uint32_t i = 0; i < 160; ++i) {
        char text[16];
        std::snprintf(text, sizeof(text), "z%05u", i);
        IndexEntry entry;
        entry.key = textKey(text);
        entry.locator = RowLocator(1, 2000u + i);
        entries.push_back(entry);
    }
    return entries;
}

void testStableSnapshots(GxAllocator& allocator) {
    std::printf("R3AD frozen production page sets through hosted index adapters\n");
    SnapshotPageAccess source;
    uint64_t rootA = 0;
    uint32_t owner = 0;
    uint32_t duplicates = 0;
    CHECK(source.loadFixture(GX_R3AD_FIXTURE_PATH, rootA, owner, duplicates));
    CHECK(owner == 37 && duplicates == 100);

    SnapshotPageAccess snapshotA = source;
    snapshotA.setReadOnly(true);
    SnapshotPageAccess writableB = source;
    writableB.setReadOnly(false);
    IndexTree writerB(writableB, DbType::Text, owner, nullptr, &allocator);
    const std::vector<uint8_t> newKey = textKey("m-snapshot-b");
    uint64_t rootB = rootA;
    bool inserted = false;
    CHECK(writerB.insert(newKey, RowLocator(1, 5000), rootA, false,
                         rootB, inserted).isOk());
    CHECK(inserted);
    IndexValidation validationB;
    CHECK(writerB.validate(rootB, validationB).isOk() && validationB.ok);
    SnapshotPageAccess snapshotB = writableB;
    snapshotB.setReadOnly(true);

    IndexTree treeA(snapshotA, DbType::Text, owner, nullptr, &allocator);
    IndexTree treeB(snapshotB, DbType::Text, owner, nullptr, &allocator);
    const std::vector<IndexEntry> expected = expectedFixtureEntries();
    std::vector<IndexEntry> rows;
    CHECK(treeA.rangeScan(IndexRangeBound(), rootA, rows).isOk());
    CHECK(sameEntries(rows, expected));
    const std::vector<IndexEntry> stableA = rows;

    std::vector<IndexEntry> duplicatesA;
    CHECK(treeA.lookupAll(textKey("m-dup"), rootA, duplicatesA).isOk());
    CHECK(duplicatesA.size() == 100);
    for (uint32_t i = 0; i < duplicatesA.size(); ++i) {
        CHECK(duplicatesA[i].key == textKey("m-dup"));
        CHECK(duplicatesA[i].locator.pageId == 1);
        CHECK(duplicatesA[i].locator.slot == 1000u + i);
    }
    std::vector<IndexEntry> duplicateRange;
    IndexRangeBound duplicateBound;
    duplicateBound.hasLower = true;
    duplicateBound.lowerInclusive = true;
    duplicateBound.lower = DbValue::text("m-dup");
    duplicateBound.hasUpper = true;
    duplicateBound.upperInclusive = true;
    duplicateBound.upper = DbValue::text("m-dup");
    CHECK(treeA.rangeScan(duplicateBound, rootA, duplicateRange).isOk());
    CHECK(duplicateRange.size() == 100);
    CHECK(sameEntries(duplicateRange, duplicatesA));

    IndexRangeBound lowerOnly;
    lowerOnly.hasLower = true;
    lowerOnly.lower = DbValue::text("m-dup");
    CHECK(treeA.rangeScan(lowerOnly, rootA, rows).isOk());
    CHECK(rows.size() == 260 && rows.front().key == textKey("m-dup"));
    IndexRangeBound upperOnly;
    upperOnly.hasUpper = true;
    upperOnly.upper = DbValue::text("m-dup");
    CHECK(treeA.rangeScan(upperOnly, rootA, rows).isOk());
    CHECK(rows.size() == 260 && rows.back().key == textKey("m-dup"));

    std::vector<IndexEntry> keyA;
    std::vector<IndexEntry> keyB;
    CHECK(treeA.lookupAll(newKey, rootA, keyA).isOk() && keyA.empty());
    CHECK(treeB.lookupAll(newKey, rootB, keyB).isOk() && keyB.size() == 1);
    CHECK(keyB[0].locator.pageId == 1 && keyB[0].locator.slot == 5000);
    CHECK(treeA.rangeScan(IndexRangeBound(), rootA, rows).isOk());
    CHECK(sameEntries(rows, stableA));
    DatabasePage rejectedWrite;
    CHECK(snapshotA.writePage(rejectedWrite).status() == DbStatus::ReadOnly);
}

void testSnapshotFailures(GxAllocator& allocator) {
    SnapshotPageAccess source;
    uint64_t root = 0;
    uint32_t owner = 0;
    uint32_t duplicateCount = 0;
    CHECK(source.loadFixture(GX_R3AD_FIXTURE_PATH, root, owner, duplicateCount));
    source.setReadOnly(true);
    IndexTree sourceTree(source, DbType::Text, owner, nullptr, &allocator);
    IndexValidation validation;
    CHECK(sourceTree.validate(root, validation).isOk() && validation.ok);
    source.resetTrace();

    std::vector<IndexEntry> rows;
    IndexRangeBound duplicates;
    duplicates.hasLower = true;
    duplicates.lowerInclusive = true;
    duplicates.lower = DbValue::text("m-dup");
    duplicates.hasUpper = true;
    duplicates.upperInclusive = true;
    duplicates.upper = DbValue::text("m-dup");
    CHECK(sourceTree.rangeScan(duplicates, root, rows).isOk());
    CHECK(rows.size() == duplicateCount);
    const std::vector<uint64_t> traceIds = source.readIds();
    const std::vector<PageType> traceTypes = source.readTypes();
    size_t firstLeaf = traceTypes.size();
    for (size_t i = 0; i < traceTypes.size(); ++i) {
        if (traceTypes[i] == PageType::IndexLeaf) {
            firstLeaf = i;
            break;
        }
    }
    CHECK(firstLeaf < traceTypes.size());
    CHECK(firstLeaf + 1 < traceIds.size());

    SnapshotPageAccess expired = source;
    expired.expire();
    IndexTree expiredTree(expired, DbType::Text, owner, nullptr, &allocator);
    rows.push_back(IndexEntry());
    DbResult expiredResult = expiredTree.lookupAll(textKey("m-dup"), root, rows);
    CHECK(!expiredResult.isOk() &&
          expiredResult.status() == DbStatus::NoActiveTransaction);
    CHECK(rows.empty());

    SnapshotPageAccess invalidated = source;
    invalidated.failOnRead(static_cast<uint64_t>(firstLeaf + 2));
    IndexTree invalidatedTree(invalidated, DbType::Text, owner, nullptr, &allocator);
    rows.push_back(IndexEntry());
    DbResult invalidatedResult = invalidatedTree.rangeScan(duplicates, root, rows);
    CHECK(!invalidatedResult.isOk() &&
          invalidatedResult.status() == DbStatus::IoError);
    CHECK(rows.empty());
    CHECK(invalidated.readCount() == firstLeaf + 2);

    SnapshotPageAccess missingLeaf = source;
    missingLeaf.makeMissing(traceIds[firstLeaf]);
    IndexTree missingTree(missingLeaf, DbType::Text, owner, nullptr, &allocator);
    rows.push_back(IndexEntry());
    DbResult missingResult = missingTree.rangeScan(duplicates, root, rows);
    CHECK(!missingResult.isOk() && rows.empty());

    SnapshotPageAccess badRoot = source;
    badRoot.corruptIdentity(root);
    IndexTree badRootTree(badRoot, DbType::Text, owner, nullptr, &allocator);
    rows.push_back(IndexEntry());
    DbResult rootResult = badRootTree.lookupAll(textKey("m-dup"), root, rows);
    CHECK(!rootResult.isOk() && rootResult.status() == DbStatus::CorruptPage);
    CHECK(rows.empty());
}

} // namespace

int main() {
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 131072) == GxStatus::Ok);
    testStableSnapshots(allocator);
    testSnapshotFailures(allocator);
    std::printf("NDB1R4P11R3AD SQL-side snapshot contract: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
