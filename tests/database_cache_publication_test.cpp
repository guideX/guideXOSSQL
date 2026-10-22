#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

struct HostedAllocationFault {
    size_t targetSize;
    unsigned triggerCount;
    bool armed;
};

thread_local HostedAllocationFault gHostedAllocationFault = {0, 0, false};

void* allocateHostedTestMemory(size_t size) {
    if (gHostedAllocationFault.armed &&
        size == gHostedAllocationFault.targetSize) {
        gHostedAllocationFault.armed = false;
        ++gHostedAllocationFault.triggerCount;
        throw std::bad_alloc();
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}

class ScopedHostedAllocationFailure {
public:
    explicit ScopedHostedAllocationFailure(size_t targetSize) {
        gHostedAllocationFault.targetSize = targetSize;
        gHostedAllocationFault.triggerCount = 0;
        gHostedAllocationFault.armed = true;
    }

    ~ScopedHostedAllocationFailure() {
        gHostedAllocationFault.armed = false;
    }

    unsigned triggerCount() const {
        return gHostedAllocationFault.triggerCount;
    }

    ScopedHostedAllocationFailure(const ScopedHostedAllocationFailure&) = delete;
    ScopedHostedAllocationFailure& operator=(
        const ScopedHostedAllocationFailure&) = delete;
};

bool hostedAllocationFailureArmed() {
    return gHostedAllocationFault.armed;
}

} // namespace

// This executable-local replacement keeps deterministic hosted allocation
// failure injection out of the production library and other test targets.
void* operator new(std::size_t size) {
    return allocateHostedTestMemory(size);
}

void* operator new[](std::size_t size) {
    return allocateHostedTestMemory(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

#include "database_file.h"
#include "database_page.h"
#include "database_relational.h"
#include "database_sql.h"
#include "database_transaction.h"
#include "database_index.h"
#include "gx_owned_page.h"
#include "gx_test_allocator.h"

using namespace gxos::db;

namespace {

int gPassed = 0;
int gFailed = 0;
int gPathId = 0;

void check(bool value, const char* expression, int line) {
    if (value) {
        ++gPassed;
    } else {
        ++gFailed;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(value) check((value), #value, __LINE__)

std::string databasePath() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    return std::string(base) + "/gxos_cache_publication_" +
           std::to_string(gPathId++) + ".gxdb";
}

void removeDatabase(const std::string& path) {
    const std::string walPath = walPathFor(path);
    std::remove(walPath.c_str());
    std::remove(path.c_str());
}

bool runSql(SqlEngine& engine, const char* sql) {
    const SqlExecutionResult result = engine.execute(sql);
    if (!result.ok) {
        std::printf("SQL failed (%s): %s\n", sql,
                    result.error.describe().c_str());
    }
    return result.ok;
}

long fileSize(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return -1;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return -1;
    }
    const long size = std::ftell(file);
    std::fclose(file);
    return size;
}

bool readDatabaseHeaderForTest(const std::string& path,
                               DatabaseHeader& header) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    uint8_t bytes[kHeaderSize];
    const bool read = std::fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    std::fclose(file);
    return read && DatabaseHeader::parse(bytes, sizeof(bytes), header).isOk();
}

uint16_t loadLe16ForTest(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t loadLe32ForTest(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t loadLe64ForTest(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

bool readWalHeaderForTest(const std::string& path, uint16_t& version,
                          uint32_t& pageSize, uint64_t& generation,
                          uint64_t& lastTransactionId) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    uint8_t bytes[kWalHeaderSize];
    const bool read = std::fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    std::fclose(file);
    if (!read || std::memcmp(bytes + wal_header_offset::Magic, kWalMagic, 8) != 0) {
        return false;
    }
    version = loadLe16ForTest(bytes + wal_header_offset::FormatVersion);
    pageSize = loadLe32ForTest(bytes + wal_header_offset::PageSize);
    generation = loadLe64ForTest(bytes + wal_header_offset::Generation);
    lastTransactionId = loadLe64ForTest(
        bytes + wal_header_offset::LastTransactionId);
    return true;
}

bool scanWalForTest(Database& db, WalState& state,
                    std::vector<WalTransaction>& transactions) {
    std::string message;
    DbResult result = db.wal().scan(state, transactions, message);
    if (!result.isOk()) {
        std::printf("WAL test scan failed: %s (%s)\n",
                    result.describe().c_str(), message.c_str());
    }
    return result.isOk();
}

std::string recoveryValue(uint64_t id, uint32_t pageSize) {
    const size_t width = pageSize == 512
        ? static_cast<size_t>(24 + (id % 29))
        : static_cast<size_t>(160 + (id % 83));
    return "row_" + std::to_string(id) + "_" +
           std::string(width,
                       static_cast<char>('a' + (id % 26)));
}

void verifyRecoveredChildrenRows(
    Database& db, uint32_t primaryIndexId,
    const std::vector<std::string>& expectedValues,
    const std::vector<int64_t>& expectedParentIds) {
    std::unique_ptr<Table> table;
    CHECK(db.openTable("Children", table).isOk() && table != nullptr);
    if (!table) return;

    std::unique_ptr<TableScan> scan;
    CHECK(table->scanStart(scan).isOk() && scan != nullptr);
    if (!scan) return;

    std::vector<uint8_t> seen(expectedValues.size(), 0);
    std::vector<uint64_t> rowPageIds(expectedValues.size(), UINT64_MAX);
    std::vector<uint32_t> rowSlots(expectedValues.size(), UINT32_MAX);
    std::vector<uint8_t> heapPages(static_cast<size_t>(db.pageCount()), 0);
    size_t uniqueHeapPages = 0;
    size_t rowCount = 0;
    std::vector<DbValue> row;
    while (scan->next(row)) {
        CHECK(row.size() == 3);
        if (row.size() != 3) continue;
        const int64_t signedId = row[0].int64Value();
        CHECK(signedId > 0 &&
              static_cast<size_t>(signedId) < expectedValues.size());
        if (signedId <= 0 ||
            static_cast<size_t>(signedId) >= expectedValues.size()) {
            continue;
        }
        const size_t id = static_cast<size_t>(signedId);
        CHECK(!seen[id]);
        CHECK(row[1].int64Value() == expectedParentIds[id]);
        CHECK(row[2].textValue() == expectedValues[id]);
        seen[id] = 1;
        rowPageIds[id] = scan->currentPageId();
        rowSlots[id] = scan->currentSlotIndex();
        ++rowCount;
        if (rowPageIds[id] < heapPages.size() &&
            !heapPages[static_cast<size_t>(rowPageIds[id])]) {
            heapPages[static_cast<size_t>(rowPageIds[id])] = 1;
            ++uniqueHeapPages;
        }
    }
    CHECK(scan->status().isOk());
    CHECK(rowCount == expectedValues.size() - 2);
    CHECK(uniqueHeapPages >= 2);

    for (size_t id = 1; id < expectedValues.size(); ++id) {
        std::vector<uint8_t> key;
        CHECK(encodeIndexKey(DbType::Int64,
                             DbValue::int64(static_cast<int64_t>(id)), key));
        std::vector<RowLocator> locators;
        CHECK(db.indexLookup(primaryIndexId, key, locators).isOk());
        if (id == 4) {
            CHECK(!seen[id] && locators.empty());
        } else {
            CHECK(seen[id] && locators.size() == 1);
            if (locators.size() == 1) {
                CHECK(locators[0].pageId == rowPageIds[id]);
                CHECK(locators[0].slot == rowSlots[id]);
            }
        }
    }
}

std::vector<uint8_t> bytesOf(const GxOwnedPage& page) {
    if (!page.isValid()) return std::vector<uint8_t>();
    return std::vector<uint8_t>(page.encodedBytes(),
                                page.encodedBytes() + page.byteCount());
}

bool equals(const GxOwnedPage& page, const std::vector<uint8_t>& bytes) {
    return page.isValid() && page.byteCount() == bytes.size() &&
           std::memcmp(page.encodedBytes(), bytes.data(), bytes.size()) == 0;
}

void testPinnedImagePublication() {
    const std::string path = databasePath();
    GxArena cacheArena;
    GxArena sourceArena;
    GxArena outputArena;
    gxArenaInit(&cacheArena, 8192);
    gxArenaInit(&sourceArena, 8192);
    gxArenaInit(&outputArena, 8192);
    GxAllocator cacheAllocator = {};
    GxAllocator sourceAllocator = {};
    GxAllocator outputAllocator = {};
    gxArenaMakeAllocator(&cacheArena, &cacheAllocator);
    gxArenaMakeAllocator(&sourceArena, &sourceAllocator);
    gxArenaMakeAllocator(&outputArena, &outputAllocator);

    DatabaseCreateOptions createOptions;
    createOptions.bufferCapacity = 2;
    createOptions.pageCacheAllocator = &cacheAllocator;
    std::unique_ptr<Database> db;
    CHECK(Database::create(path, createOptions, db).isOk() && db != nullptr);
    if (!db) {
        removeDatabase(path);
        return;
    }

    GxOwnedPage original;
    GxOwnedPage source;
    GxOwnedPage observed;
    CHECK(original.init(&sourceAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(source.init(&sourceAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(observed.init(&outputAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(db->readOwnedPage(1, original).isOk());
    const std::vector<uint8_t> originalBytes = bytesOf(original);

    DatabasePage replacement;
    CHECK(DatabasePage::parse(original.encodedBytes(), original.byteCount(),
                              original.pageSize(), 1, replacement).isOk());
    replacement.generation += 17;
    std::vector<uint8_t> replacementBytes;
    CHECK(replacement.serialize(replacementBytes, db->pageSize()).isOk());
    CHECK(source.materialize(replacementBytes.data(), replacementBytes.size(),
                             replacement.pageId) == GxStatus::Ok);
    CHECK(source.allocator().context == &sourceArena &&
          source.allocator().context != &cacheArena);

    BufferSlot* pinned = nullptr;
    CHECK(db->buffer().pinPage(1, pinned).isOk() && pinned != nullptr);
    if (pinned != nullptr) {
        const uint32_t slotPinsBefore = pinned->pinCount;
        const uint32_t managerPinsBefore = db->buffer().pinnedCount();
        const size_t cacheAllocationsBefore = cacheArena.outstanding;
        const uint64_t priorGeneration = pinned->page.generation;

        std::vector<uint8_t> corrupted = replacementBytes;
        corrupted[corrupted.size() - 1] ^= 0x80;
        CHECK(db->buffer().installPage(1, corrupted.data(), corrupted.size())
                  .status() == DbStatus::CorruptPage);
        CHECK(pinned->page.generation == priorGeneration &&
              pinned->pinCount == slotPinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);

        gxArenaFailAfter(&cacheArena, 0);
        CHECK(db->buffer().installPage(1, source.encodedBytes(),
                                       source.byteCount()).status() ==
              DbStatus::NoSpace);
        CHECK(cacheArena.outstanding == cacheAllocationsBefore &&
              pinned->page.generation == priorGeneration &&
              pinned->pinCount == slotPinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);
        gxArenaDisableFailure(&cacheArena);
        CHECK(db->readOwnedPage(1, observed).isOk());
        CHECK(equals(observed, originalBytes));

        CHECK(db->buffer().installPage(1, source.encodedBytes(),
                                       source.byteCount()).isOk());
        CHECK(pinned->page.generation == replacement.generation &&
              pinned->page.pageId == 1 &&
              pinned->pinCount == slotPinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);
        CHECK(cacheArena.outstanding == cacheAllocationsBefore);
        CHECK(db->buffer().installPage(1, source.encodedBytes(),
                                       source.byteCount()).isOk());
        CHECK(pinned->page.generation == replacement.generation &&
              pinned->pinCount == slotPinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore &&
              cacheArena.outstanding == cacheAllocationsBefore);

        source.destroy();
        CHECK(sourceArena.outstanding == 1 && sourceArena.invalidFrees == 0);
        CHECK(db->readOwnedPage(1, observed).isOk());
        CHECK(equals(observed, replacementBytes));
        CHECK(pinned->pinCount == slotPinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);
        CHECK(db->buffer().unpin(*pinned).isOk());
        CHECK(db->buffer().pinnedCount() + 1 == managerPinsBefore);
        CHECK(db->buffer().unpin(*pinned).status() == DbStatus::InvalidArgument);
        CHECK(db->buffer().installPage(1, replacementBytes.data(),
                                       replacementBytes.size()).isOk());
        CHECK(db->buffer().installPage(1, replacementBytes.data(),
                                       replacementBytes.size()).isOk());
        CHECK(db->buffer().pinnedCount() == 0 &&
              cacheArena.outstanding == cacheAllocationsBefore);
    }

    CHECK(db->close().isOk());
    db.reset();
    CHECK(cacheArena.outstanding == 0 && cacheArena.invalidFrees == 0);
    CHECK(outputArena.outstanding == 1);
    original.destroy();
    observed.destroy();
    CHECK(sourceArena.outstanding == 0 && outputArena.outstanding == 0);

    DatabaseOpenOptions openOptions;
    std::unique_ptr<Database> reopened;
    CHECK(Database::open(path, openOptions, reopened).isOk() &&
          reopened != nullptr);
    if (reopened) {
        GxArena reopenedArena;
        gxArenaInit(&reopenedArena, 8192);
        GxAllocator reopenedAllocator = {};
        gxArenaMakeAllocator(&reopenedArena, &reopenedAllocator);
        GxOwnedPage persisted;
        CHECK(persisted.init(&reopenedAllocator, reopened->pageSize()) ==
              GxStatus::Ok);
        CHECK(reopened->readOwnedPage(1, persisted).isOk());
        CHECK(equals(persisted, replacementBytes));
        CHECK(reopened->close().isOk());
        persisted.destroy();
        CHECK(reopenedArena.outstanding == 0 &&
              reopenedArena.invalidFrees == 0);
    }
    reopened.reset();
    removeDatabase(path);
}

void testHostedPinnedViewAllocationFailure() {
    const std::string path = databasePath();
    GxArena cacheArena;
    GxArena pageArena;
    GxArena outputArena;
    gxArenaInit(&cacheArena, 8192);
    gxArenaInit(&pageArena, 8192);
    gxArenaInit(&outputArena, 8192);
    GxAllocator cacheAllocator = {};
    GxAllocator pageAllocator = {};
    GxAllocator outputAllocator = {};
    gxArenaMakeAllocator(&cacheArena, &cacheAllocator);
    gxArenaMakeAllocator(&pageArena, &pageAllocator);
    gxArenaMakeAllocator(&outputArena, &outputAllocator);

    DatabaseCreateOptions createOptions;
    createOptions.bufferCapacity = 1;
    createOptions.pageCacheAllocator = &cacheAllocator;
    std::unique_ptr<Database> db;
    CHECK(Database::create(path, createOptions, db).isOk() && db != nullptr);
    if (!db) {
        removeDatabase(path);
        return;
    }

    uint64_t pageId = 0;
    std::unique_ptr<Transaction> tx;
    CHECK(db->beginTransaction(tx).isOk() && tx != nullptr);
    if (!tx) {
        CHECK(db->close().isOk());
        db.reset();
        removeDatabase(path);
        return;
    }
    CHECK(tx->allocatePage(PageType::Data, pageId).isOk());
    DatabasePage originalPage;
    originalPage.pageId = pageId;
    originalPage.type = PageType::Data;
    originalPage.generation = 41;
    originalPage.payload.assign(127, 0x4D);
    CHECK(tx->writePage(originalPage).isOk());
    CHECK(tx->commit().isOk());
    tx.reset();
    CHECK(pageId != 0);

    GxOwnedPage original;
    GxOwnedPage source;
    GxOwnedPage observed;
    CHECK(original.init(&pageAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(source.init(&pageAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(observed.init(&outputAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(db->readOwnedPage(pageId, original).isOk());
    const std::vector<uint8_t> originalBytes = bytesOf(original);

    DatabasePage compatibility;
    CHECK(db->readPage(pageId, compatibility).isOk());
    std::vector<uint8_t> compatibilityBytes;
    CHECK(compatibility.serialize(compatibilityBytes, db->pageSize()).isOk());
    CHECK(compatibilityBytes == originalBytes);

    BufferSlot* compatibilityPin = nullptr;
    CHECK(db->buffer().pinPage(pageId, compatibilityPin).isOk() &&
          compatibilityPin != nullptr);
    if (compatibilityPin != nullptr) {
        const uint32_t pinsBefore = compatibilityPin->pinCount;
        const uint32_t managerPinsBefore = db->buffer().pinnedCount();
        const uint8_t* const cachePayloadBefore =
            compatibilityPin->page.payload.data();
        const std::vector<uint8_t> priorPayload = compatibility.payload;
        const uint64_t priorPageId = compatibility.pageId;
        const uint32_t priorGeneration = compatibility.generation;
        const size_t scratchAttemptsBefore = cacheArena.allocationAttempts;
        const size_t scratchOutstandingBefore = cacheArena.outstanding;

        gxArenaFailAfter(&cacheArena, 0);
        const DbResult ownerFailure = db->readPage(pageId, compatibility);
        gxArenaDisableFailure(&cacheArena);
        CHECK(ownerFailure.status() == DbStatus::NoSpace);
        CHECK(cacheArena.allocationAttempts == scratchAttemptsBefore + 1 &&
              cacheArena.outstanding == scratchOutstandingBefore);
        CHECK(compatibility.pageId == priorPageId &&
              compatibility.generation == priorGeneration &&
              compatibility.payload == priorPayload);
        CHECK(compatibilityPin->pageId == pageId &&
              compatibilityPin->page.payload.data() == cachePayloadBefore &&
              compatibilityPin->pinCount == pinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);

        DbResult hostedFailure;
        unsigned hostedTriggerCount = 0;
        const size_t parseScratchAttempts = cacheArena.allocationAttempts;
        {
            ScopedHostedAllocationFailure failure(compatibility.payload.size());
            hostedFailure = db->readPage(pageId, compatibility);
            hostedTriggerCount = failure.triggerCount();
        }
        CHECK(hostedTriggerCount == 1);
        CHECK(hostedFailure.status() == DbStatus::NoSpace &&
              !hostedAllocationFailureArmed());
        CHECK(cacheArena.allocationAttempts == parseScratchAttempts + 1 &&
              cacheArena.outstanding == scratchOutstandingBefore);
        CHECK(compatibility.pageId == priorPageId &&
              compatibility.generation == priorGeneration &&
              compatibility.payload == priorPayload);
        CHECK(compatibilityPin->pageId == pageId &&
              compatibilityPin->page.payload.data() == cachePayloadBefore &&
              compatibilityPin->pinCount == pinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);

        CHECK(db->readPage(pageId, compatibility).isOk());
        CHECK(compatibility.serialize(compatibilityBytes, db->pageSize()).isOk() &&
              compatibilityBytes == originalBytes);
        CHECK(compatibilityPin->page.payload.data() == cachePayloadBefore &&
              compatibilityPin->pinCount == pinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);
        CHECK(db->buffer().unpin(*compatibilityPin).isOk());
    }

    DatabasePage replacement;
    CHECK(DatabasePage::parse(original.encodedBytes(), original.byteCount(),
                              original.pageSize(), pageId, replacement).isOk());
    replacement.generation += 9;
    replacement.payload[0] ^= 0xFF;
    std::vector<uint8_t> replacementBytes;
    CHECK(replacement.serialize(replacementBytes, db->pageSize()).isOk());
    CHECK(source.materialize(replacementBytes.data(), replacementBytes.size(),
                             pageId) == GxStatus::Ok);

    BufferSlot* pinned = nullptr;
    CHECK(db->buffer().pinPage(pageId, pinned).isOk() && pinned != nullptr);
    if (pinned != nullptr) {
        BufferSlot* const stableSlot = pinned;
        const uint32_t slotPinsBefore = pinned->pinCount;
        const uint32_t managerPinsBefore = db->buffer().pinnedCount();
        const size_t cacheAllocationsBefore = cacheArena.outstanding;
        const uint64_t priorGeneration = pinned->page.generation;
        const std::vector<uint8_t> priorPayload = pinned->page.payload;

        CHECK(priorPayload.size() == 127);
        for (int attempt = 0; attempt < 2; ++attempt) {
            DbResult failed;
            unsigned triggerCount = 0;
            {
                ScopedHostedAllocationFailure failure(priorPayload.size());
                failed = db->buffer().installPage(
                    pageId, source.encodedBytes(), source.byteCount());
                triggerCount = failure.triggerCount();
            }
            CHECK(triggerCount == 1);
            CHECK(failed.status() == DbStatus::NoSpace);
            CHECK(!hostedAllocationFailureArmed());
            CHECK(cacheArena.outstanding == cacheAllocationsBefore);
            CHECK(pinned == stableSlot && pinned->page.pageId == pageId &&
                  pinned->page.generation == priorGeneration &&
                  pinned->page.payload == priorPayload &&
                  pinned->pinCount == slotPinsBefore &&
                  db->buffer().pinnedCount() == managerPinsBefore);
            CHECK(db->readOwnedPage(pageId, observed).isOk());
            CHECK(equals(observed, originalBytes));
            CHECK(equals(source, replacementBytes));
        }

        CHECK(db->buffer().installPage(pageId, source.encodedBytes(),
                                       source.byteCount()).isOk());
        CHECK(pinned == stableSlot && pinned->page.pageId == pageId &&
              pinned->page.generation == replacement.generation &&
              pinned->page.payload == replacement.payload &&
              pinned->pinCount == slotPinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);
        CHECK(cacheArena.outstanding == cacheAllocationsBefore);
        CHECK(db->readOwnedPage(pageId, observed).isOk());
        CHECK(equals(observed, replacementBytes));
        CHECK(db->buffer().unpin(*pinned).isOk());
        CHECK(db->buffer().pinnedCount() + 1 == managerPinsBefore);
        CHECK(db->buffer().unpin(*pinned).status() == DbStatus::InvalidArgument);

        // The same hosted parse is also used when a cached encoded page is
        // materialized for a read. That failure happens before the pin count
        // advances and leaves the encoded cache image available for retry.
        CHECK(db->buffer().installPage(pageId, replacementBytes.data(),
                                       replacementBytes.size()).isOk());
        BufferSlot* failedPin = nullptr;
        DbResult readFailure;
        unsigned readTriggerCount = 0;
        {
            ScopedHostedAllocationFailure failure(replacement.payload.size());
            readFailure = db->buffer().pinPage(pageId, failedPin);
            readTriggerCount = failure.triggerCount();
        }
        CHECK(readTriggerCount == 1);
        CHECK(readFailure.status() == DbStatus::NoSpace && failedPin == nullptr &&
              db->buffer().pinnedCount() == 0 &&
              cacheArena.outstanding == cacheAllocationsBefore);
        CHECK(!hostedAllocationFailureArmed());
        CHECK(db->readOwnedPage(pageId, observed).isOk());
        CHECK(equals(observed, replacementBytes));
        BufferSlot* retriedPin = nullptr;
        CHECK(db->buffer().pinPage(pageId, retriedPin).isOk() &&
              retriedPin != nullptr && retriedPin->page.payload == replacement.payload);
        if (retriedPin != nullptr) {
            CHECK(db->buffer().unpin(*retriedPin).isOk());
        }

        // Force the repaired page out of the one-slot cache, then load it
        // again from its encoded file image.
        BufferSlot* pressurePin = nullptr;
        CHECK(db->buffer().pinPage(1, pressurePin).isOk() &&
              pressurePin != nullptr);
        if (pressurePin != nullptr) {
            CHECK(db->buffer().unpin(*pressurePin).isOk());
        }
        CHECK(db->readOwnedPage(pageId, observed).isOk());
        CHECK(equals(observed, replacementBytes));
    }

    CHECK(db->close().isOk());
    db.reset();
    CHECK(cacheArena.outstanding == 0 && cacheArena.invalidFrees == 0);
    original.destroy();
    source.destroy();
    observed.destroy();
    CHECK(pageArena.outstanding == 0 && pageArena.invalidFrees == 0);
    CHECK(outputArena.outstanding == 0 && outputArena.invalidFrees == 0);

    DatabaseOpenOptions openOptions;
    std::unique_ptr<Database> reopened;
    CHECK(Database::open(path, openOptions, reopened).isOk() &&
          reopened != nullptr);
    if (reopened) {
        GxArena reopenedArena;
        gxArenaInit(&reopenedArena, 8192);
        GxAllocator reopenedAllocator = {};
        gxArenaMakeAllocator(&reopenedArena, &reopenedAllocator);
        GxOwnedPage persisted;
        CHECK(persisted.init(&reopenedAllocator, reopened->pageSize()) ==
              GxStatus::Ok);
        CHECK(reopened->readOwnedPage(pageId, persisted).isOk());
        CHECK(equals(persisted, replacementBytes));
        CHECK(reopened->close().isOk());
        persisted.destroy();
        CHECK(reopenedArena.outstanding == 0 &&
              reopenedArena.invalidFrees == 0);
    }
    reopened.reset();
    removeDatabase(path);
}

void testSqlCommitHostedViewFailureRecovery(bool checkpointSeed,
                                            uint32_t pageSize) {
    const std::string path = databasePath();
    const std::string walPath = walPathFor(path);
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = pageSize;
    createOptions.bufferCapacity = 256;
    std::unique_ptr<Database> db;
    CHECK(Database::create(path, createOptions, db).isOk() && db != nullptr);
    if (!db) {
        removeDatabase(path);
        return;
    }

    uint64_t rowPageId = 0;
    uint64_t recoveredPageCount = 0;
    uint32_t recoveredPageSize = 0;
    uint32_t primaryIndexId = 0;
    std::vector<std::string> expectedValues(49);
    std::vector<int64_t> expectedParentIds(49, 0);
    std::vector<uint8_t> priorPagePayload;
    BufferSlot* pinned = nullptr;
    {
        SqlEngine engine(*db);
        CHECK(runSql(engine,
                     "CREATE TABLE Parents (Id INT64 PRIMARY KEY, Name TEXT NOT NULL);"));
        CHECK(runSql(engine,
                     "CREATE TABLE Children (Id INT64 PRIMARY KEY, ParentId INT64 NOT NULL, "
                     "Value TEXT NOT NULL, FOREIGN KEY (ParentId) REFERENCES Parents (Id));"));
        CHECK(runSql(engine,
                     "CREATE INDEX IX_Children_Value ON Children (Value);"));
        CHECK(runSql(engine, "INSERT INTO Parents VALUES (1, 'parent-one');"));
        CHECK(runSql(engine, "INSERT INTO Parents VALUES (2, 'parent-two');"));

        for (uint64_t firstId = 1; firstId <= 48; firstId += 16) {
            CHECK(runSql(engine, "BEGIN;"));
            const uint64_t endId = firstId + 15;
            for (uint64_t id = firstId; id <= endId; ++id) {
                const int64_t parentId = id % 2 == 0 ? 2 : 1;
                expectedValues[static_cast<size_t>(id)] =
                    recoveryValue(id, pageSize);
                expectedParentIds[static_cast<size_t>(id)] = parentId;
                const std::string statement =
                    "INSERT INTO Children VALUES (" + std::to_string(id) + ", " +
                    std::to_string(parentId) + ", '" +
                    expectedValues[static_cast<size_t>(id)] + "');";
                CHECK(runSql(engine, statement.c_str()));
            }
            CHECK(runSql(engine, "COMMIT;"));
        }
        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE Children SET Value = 'pre_fault_update' WHERE Id = 3;"));
        CHECK(runSql(engine, "DELETE FROM Children WHERE Id = 4;"));
        CHECK(runSql(engine, "COMMIT;"));
        expectedValues[3] = "pre_fault_update";

        std::vector<IndexInfo> indexes;
        CHECK(db->listIndexes(indexes).isOk());
        for (size_t i = 0; i < indexes.size(); ++i) {
            if (indexes[i].tableName == "Children" &&
                indexes[i].columnName == "Id" &&
                indexes[i].kind == IndexKind::PrimaryKey) {
                primaryIndexId = indexes[i].indexId;
            }
        }
        CHECK(primaryIndexId != 0);
        if (primaryIndexId == 0) {
            CHECK(db->close().isOk());
            db.reset();
            removeDatabase(path);
            return;
        }
        std::vector<uint8_t> key;
        CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(7), key));
        std::vector<RowLocator> locators;
        CHECK(db->indexLookup(primaryIndexId, key, locators).isOk() &&
              locators.size() == 1);
        if (locators.size() != 1) {
            CHECK(db->close().isOk());
            db.reset();
            removeDatabase(path);
            return;
        }
        rowPageId = locators[0].pageId;

        SqlPreparedStatement indexedRead;
        CHECK(engine.prepare(
                  ("SELECT Id FROM Children WHERE Value = '" +
                   expectedValues[7] + "';").c_str(),
                  indexedRead).isOk());
        SqlStatementResult indexedResult = indexedRead.executeQuery();
        CHECK(indexedResult.ok && indexedResult.accessPath == "IndexLookup" &&
              indexedResult.resultSet.rowCount() == 1 &&
              indexedResult.resultSet.value(0, 0).int64Value() == 7);

        // Leave the schema, index, and seed-row commits in WAL. These earlier
        // durable transactions include page-count growth and form the history
        // that recovery must replay before the faulted update.
        std::vector<WalTransaction> seedTransactions;
        WalState seedWalState = WalState::Clean;
        CHECK(scanWalForTest(*db, seedWalState, seedTransactions));
        const size_t seedTransactionCount = seedTransactions.size();
        const uint64_t lastSeedTransactionId = seedTransactionCount == 0
            ? 0 : seedTransactions.back().transactionId;
        uint64_t firstHeaderPageCount = 0;
        uint64_t lastHeaderPageCount = 0;
        size_t headerImageCount = 0;
        std::vector<uint64_t> headerPageCounts;
        for (size_t i = 0; i < seedTransactions.size(); ++i) {
            if (!seedTransactions[i].hasHeaderImage) continue;
            if (headerImageCount == 0) {
                firstHeaderPageCount = seedTransactions[i].headerImage.pageCount;
            }
            lastHeaderPageCount = seedTransactions[i].headerImage.pageCount;
            headerPageCounts.push_back(lastHeaderPageCount);
            ++headerImageCount;
        }
        uint16_t walVersion = 0;
        uint32_t walPageSize = 0;
        uint64_t walGeneration = 0;
        uint64_t walHeaderLastTransactionId = 0;

        if (checkpointSeed) {
            CHECK(db->flush().isOk());
            seedTransactions.clear();
            CHECK(scanWalForTest(*db, seedWalState, seedTransactions));
            CHECK(seedWalState == WalState::Clean && seedTransactions.empty());
            DatabaseHeader diskHeader;
            CHECK(readDatabaseHeaderForTest(path, diskHeader));
            CHECK(diskHeader.pageCount == db->pageCount());
            CHECK(fileSize(path) ==
                  static_cast<long>(db->pageCount() * db->pageSize()));
        } else {
            DatabaseHeader diskHeader;
            CHECK(seedWalState == WalState::Committed);
            CHECK(seedTransactionCount >= 2 && headerImageCount >= 2);
            CHECK(lastHeaderPageCount > firstHeaderPageCount);
            CHECK(readDatabaseHeaderForTest(path, diskHeader));
            CHECK(diskHeader.pageCount == 2);
            CHECK(fileSize(path) == static_cast<long>(2u * db->pageSize()));
            CHECK(diskHeader.pageCount < db->pageCount());
            CHECK(fileSize(path) <
                  static_cast<long>(db->pageCount() * db->pageSize()));
            if (pageSize == kDefaultPageSize) {
                CHECK(seedTransactionCount == 9);
                CHECK(headerPageCounts.size() == 4);
                CHECK(headerPageCounts[0] == 4 && headerPageCounts[1] == 8 &&
                      headerPageCounts[2] == 23 && headerPageCounts[3] == 48);
                CHECK(db->pageCount() == 48);
            }
        }
        CHECK(readWalHeaderForTest(walPath, walVersion, walPageSize,
                                   walGeneration, walHeaderLastTransactionId));
        CHECK(walVersion == kWalFormatVersion && walPageSize == db->pageSize());
        CHECK(walGeneration == (checkpointSeed ? 2u : 1u));
        CHECK(walHeaderLastTransactionId ==
              (checkpointSeed ? lastSeedTransactionId : 0u));
        if (pageSize == kDefaultPageSize) {
            DatabaseHeader beforeHeader;
            readDatabaseHeaderForTest(path, beforeHeader);
            std::printf("fixture checkpoint=%u pageSize=%u pageCount=%llu diskHeader=%llu "
                        "dbBytes=%ld seedTx=%zu headerImages=%zu firstHeader=%llu "
                        "lastHeader=%llu walGeneration=%llu walHeaderTx=%llu\n",
                        checkpointSeed ? 1u : 0u, pageSize,
                        static_cast<unsigned long long>(db->pageCount()),
                        static_cast<unsigned long long>(beforeHeader.pageCount),
                        fileSize(path), seedTransactionCount, headerImageCount,
                        static_cast<unsigned long long>(firstHeaderPageCount),
                        static_cast<unsigned long long>(lastHeaderPageCount),
                        static_cast<unsigned long long>(walGeneration),
                        static_cast<unsigned long long>(walHeaderLastTransactionId));
            for (size_t i = 0; i < seedTransactions.size(); ++i) {
                const WalTransaction& tx = seedTransactions[i];
                if (tx.hasHeaderImage) {
                    std::printf("  seed tx=%llu pageImages=%zu headerPageCount=%llu\n",
                                static_cast<unsigned long long>(tx.transactionId),
                                gxRecoveredPagesCount(&tx.pages),
                                static_cast<unsigned long long>(
                                    tx.headerImage.pageCount));
                } else {
                    std::printf("  seed tx=%llu pageImages=%zu header=none\n",
                                static_cast<unsigned long long>(tx.transactionId),
                                gxRecoveredPagesCount(&tx.pages));
                }
            }
        }
        recoveredPageCount = db->pageCount();
        recoveredPageSize = db->pageSize();
        verifyRecoveredChildrenRows(*db, primaryIndexId,
                                    expectedValues, expectedParentIds);

        CHECK(db->buffer().pinPage(rowPageId, pinned).isOk() && pinned != nullptr);
        if (pinned == nullptr) {
            CHECK(db->close().isOk());
            db.reset();
            removeDatabase(path);
            return;
        }
        priorPagePayload = pinned->page.payload;

        CHECK(runSql(engine, "BEGIN;"));
        CHECK(runSql(engine,
                     "UPDATE Children SET Value = 'newtag' WHERE Id = 7;"));
        SqlPreparedStatement readYourWrites;
        CHECK(engine.prepare(
                  "SELECT Value FROM Children WHERE Id = 7;",
                  readYourWrites).isOk());
        SqlStatementResult staged = readYourWrites.executeQuery();
        CHECK(staged.ok && staged.resultSet.rowCount() == 1 &&
              staged.resultSet.value(0, 0).textValue() == "newtag");

        SqlExecutionResult rolledBackStatement = engine.execute(
            "INSERT INTO Children VALUES (7, 1, 'dupe');");
        CHECK(!rolledBackStatement.ok &&
              rolledBackStatement.transactionActiveAfter &&
              engine.inTransaction());
        staged = readYourWrites.executeQuery();
        CHECK(staged.ok && staged.resultSet.rowCount() == 1 &&
              staged.resultSet.value(0, 0).textValue() == "newtag");
        CHECK(pinned->pinCount == 1 && db->buffer().pinnedCount() == 1 &&
              pinned->page.payload == priorPagePayload);

        SqlExecutionResult commit;
        unsigned commitTriggerCount = 0;
        {
            ScopedHostedAllocationFailure failure(pinned->page.payload.size());
            commit = engine.execute("COMMIT;");
            commitTriggerCount = failure.triggerCount();
        }
        CHECK(commitTriggerCount == 1);
        CHECK(!commit.ok && commit.error.code == SqlErrorCode::TransactionError &&
              commit.error.message == "pinned page-view allocation failed");
        CHECK(!engine.inTransaction() && !commit.transactionActiveAfter &&
              db->diagnostics().recoveryRequired);
        expectedValues[7] = "newtag";
        CHECK(pinned->page.payload == priorPagePayload && pinned->page.pageId == rowPageId &&
              pinned->pinCount == 1 && db->buffer().pinnedCount() == 1);
        DatabasePage blockedRead;
        CHECK(db->readPage(rowPageId, blockedRead).status() ==
              DbStatus::RecoveryRequired);
        CHECK(db->flush().status() == DbStatus::RecoveryRequired);
        CHECK(fileSize(walPath) > 0);
        std::vector<WalTransaction> durableTransactions;
        WalState durableWalState = WalState::Clean;
        CHECK(scanWalForTest(*db, durableWalState, durableTransactions));
        CHECK(durableWalState == WalState::Committed);
        CHECK(durableTransactions.size() ==
              (checkpointSeed ? 1u : seedTransactionCount + 1u));
        uint16_t durableWalVersion = 0;
        uint32_t durableWalPageSize = 0;
        uint64_t durableWalGeneration = 0;
        uint64_t durableWalHeaderLastTransactionId = 0;
        CHECK(readWalHeaderForTest(walPath, durableWalVersion,
                                   durableWalPageSize, durableWalGeneration,
                                   durableWalHeaderLastTransactionId));
        CHECK(durableWalVersion == kWalFormatVersion &&
              durableWalPageSize == recoveredPageSize &&
              durableWalGeneration == walGeneration &&
              durableWalHeaderLastTransactionId == walHeaderLastTransactionId);
        CHECK(!durableTransactions.empty());
        if (!durableTransactions.empty()) {
            const WalTransaction& durableCommit = durableTransactions.back();
            if (pageSize == kDefaultPageSize) {
                std::printf("  fault commit tx=%llu pageImages=%zu header=%s walRecords=%zu\n",
                            static_cast<unsigned long long>(durableCommit.transactionId),
                            gxRecoveredPagesCount(&durableCommit.pages),
                            durableCommit.hasHeaderImage ? "yes" : "no",
                            durableTransactions.size());
            }
            CHECK(durableCommit.transactionId == lastSeedTransactionId + 1);
            CHECK(!durableCommit.hasHeaderImage &&
                  gxRecoveredPagesCount(&durableCommit.pages) > 0);
        }
        CHECK(db->buffer().unpin(*pinned).isOk());
        CHECK(db->buffer().pinnedCount() == 0);
        CHECK(db->buffer().unpin(*pinned).status() == DbStatus::InvalidArgument);
    }

    CHECK(fileSize(walPath) > 0);
    CHECK(db->close().status() == DbStatus::RecoveryRequired);
    db.reset();
    CHECK(fileSize(walPath) > 0);
    DatabaseHeader closedHeader;
    CHECK(readDatabaseHeaderForTest(path, closedHeader));
    CHECK(closedHeader.pageCount == recoveredPageCount);
    if (pageSize == kDefaultPageSize) {
        std::printf("  closed DB header=%llu dbBytes=%ld walBytes=%ld\n",
                    static_cast<unsigned long long>(closedHeader.pageCount),
                    fileSize(path), fileSize(walPath));
    }
    if (checkpointSeed) {
        CHECK(fileSize(path) ==
              static_cast<long>(recoveredPageCount * recoveredPageSize));
    } else {
        CHECK(fileSize(path) <
              static_cast<long>(recoveredPageCount * recoveredPageSize));
    }

    DatabaseOpenOptions openOptions;
    std::unique_ptr<Database> recovered;
    DbResult recoveryOpen = Database::open(path, openOptions, recovered);
    if (!recoveryOpen.isOk()) {
        std::printf("SQL recovery open failed: %s\n",
                    recoveryOpen.describe().c_str());
    }
    CHECK(recoveryOpen.isOk() && recovered != nullptr);
    if (recovered) {
        CHECK(recovered->pageCount() == recoveredPageCount);
        CHECK(recovered->diagnostics().pagesRedone > 0);
        std::string integrityMessage;
        CHECK(recovered->validateIntegrity(integrityMessage).isOk());
        std::vector<IndexInfo> indexes;
        CHECK(recovered->listIndexes(indexes).isOk());
        for (size_t i = 0; i < indexes.size(); ++i) {
            IndexValidation validation;
            CHECK(recovered->validateIndex(indexes[i].indexId, validation).isOk());
            CHECK(validation.ok);
        }
        std::vector<ForeignKeyInfo> foreignKeys;
        CHECK(recovered->listForeignKeys(foreignKeys).isOk());
        CHECK(foreignKeys.size() == 1);
        verifyRecoveredChildrenRows(*recovered, primaryIndexId,
                                    expectedValues, expectedParentIds);
        {
            SqlEngine engine(*recovered);
            SqlPreparedStatement committedValue;
            CHECK(engine.prepare(
                      "SELECT Id FROM Children WHERE Value = 'newtag';",
                      committedValue).isOk());
            SqlStatementResult query = committedValue.executeQuery();
            CHECK(query.ok && query.accessPath == "IndexLookup" &&
                  query.resultSet.rowCount() == 1 &&
                  query.resultSet.value(0, 0).int64Value() == 7);
            SqlPreparedStatement preFaultUpdate;
            CHECK(engine.prepare(
                      "SELECT Id FROM Children WHERE Value = 'pre_fault_update';",
                      preFaultUpdate).isOk());
            SqlStatementResult priorValue = preFaultUpdate.executeQuery();
            CHECK(priorValue.ok && priorValue.accessPath == "IndexLookup" &&
                  priorValue.resultSet.rowCount() == 1 &&
                  priorValue.resultSet.value(0, 0).int64Value() == 3);
            std::string laterIntegrityMessage;
            CHECK(recovered->validateIntegrity(laterIntegrityMessage).isOk());
        }
        CHECK(recovered->close().isOk());
    }
    recovered.reset();

    std::unique_ptr<Database> reopened;
    DbResult secondOpen = Database::open(path, openOptions, reopened);
    if (!secondOpen.isOk()) {
        std::printf("SQL second open failed: %s\n",
                    secondOpen.describe().c_str());
    }
    CHECK(secondOpen.isOk() && reopened != nullptr);
    if (reopened) {
        CHECK(reopened->pageCount() == recoveredPageCount);
        CHECK(reopened->diagnostics().pagesRedone == 0);
        std::string integrityMessage;
        CHECK(reopened->validateIntegrity(integrityMessage).isOk());
        std::vector<ForeignKeyInfo> foreignKeys;
        CHECK(reopened->listForeignKeys(foreignKeys).isOk());
        CHECK(foreignKeys.size() == 1);
        verifyRecoveredChildrenRows(*reopened, primaryIndexId,
                                    expectedValues, expectedParentIds);
        SqlEngine engine(*reopened);
        SqlPreparedStatement latestValue;
        CHECK(engine.prepare(
                  "SELECT Id FROM Children WHERE Value = 'newtag';",
                  latestValue).isOk());
        SqlStatementResult query = latestValue.executeQuery();
        CHECK(query.ok && query.accessPath == "IndexLookup" &&
              query.resultSet.rowCount() == 1 &&
              query.resultSet.value(0, 0).int64Value() == 7);
        CHECK(reopened->close().isOk());
    }
    reopened.reset();
    removeDatabase(path);
}

void testSqlCommitHostedViewFailureRecovery() {
    testSqlCommitHostedViewFailureRecovery(false, kDefaultPageSize);
}

void testSqlCommitHostedViewFailureRecoveryCheckpointedControl() {
    testSqlCommitHostedViewFailureRecovery(true, kDefaultPageSize);
}

void testSqlCommitHostedViewFailureRecoverySmallPages() {
    testSqlCommitHostedViewFailureRecovery(false, 512);
}

void testRejectsRegressingCommittedHeaderImages() {
    const std::string path = databasePath();
    const std::string walPath = walPathFor(path);
    std::unique_ptr<Database> db;
    CHECK(Database::create(path, DatabaseCreateOptions(), db).isOk() &&
          db != nullptr);
    if (!db) {
        removeDatabase(path);
        return;
    }
    const DatabaseHeader databaseHeader = db->file().header();
    CHECK(db->close().isOk());
    db.reset();

    std::unique_ptr<IDatabaseFile> walFile = defaultFileSystem().createFile();
    CHECK(walFile != nullptr);
    if (!walFile) {
        removeDatabase(path);
        return;
    }
    CHECK(walFile->open(walPath, FileOpenMode::Create, true).isOk());
    WriteAheadLog wal;
    CHECK(wal.open(std::move(walFile), databaseHeader.databaseId,
                   databaseHeader.pageSize, false).isOk());

    DatabasePage page2;
    page2.pageId = 2;
    page2.type = PageType::Data;
    page2.payload.push_back(0x22);
    DatabasePage page3;
    page3.pageId = 3;
    page3.type = PageType::Data;
    page3.payload.push_back(0x33);

    DatabaseHeader firstHeader = databaseHeader;
    firstHeader.pageCount = 4;
    CHECK(wal.appendBegin(1).isOk());
    CHECK(wal.appendPageImage(1, page2).isOk());
    CHECK(wal.appendPageImage(1, page3).isOk());
    CHECK(wal.appendHeaderImage(1, firstHeader).isOk());
    CHECK(wal.appendCommit(1).isOk());
    CHECK(wal.sync().isOk());

    DatabaseHeader regressingHeader = databaseHeader;
    regressingHeader.pageCount = 3;
    CHECK(wal.appendBegin(2).isOk());
    CHECK(wal.appendHeaderImage(2, regressingHeader).isOk());
    CHECK(wal.appendCommit(2).isOk());
    CHECK(wal.sync().isOk());
    CHECK(wal.close().isOk());
    CHECK(fileSize(walPath) > static_cast<long>(kWalHeaderSize));

    std::unique_ptr<Database> reopened;
    const DbResult result = Database::open(path, DatabaseOpenOptions(), reopened);
    CHECK(result.status() == DbStatus::WalCorrupt);
    CHECK(reopened == nullptr);
    CHECK(fileSize(walPath) > static_cast<long>(kWalHeaderSize));
    removeDatabase(path);
}

void testDurableCommitAllocationFailureRecovery() {
    const std::string path = databasePath();
    const std::string walPath = walPathFor(path);
    GxArena cacheArena;
    gxArenaInit(&cacheArena, 8192);
    GxAllocator cacheAllocator = {};
    gxArenaMakeAllocator(&cacheArena, &cacheAllocator);

    DatabaseCreateOptions createOptions;
    createOptions.bufferCapacity = 2;
    createOptions.pageCacheAllocator = &cacheAllocator;
    std::unique_ptr<Database> db;
    CHECK(Database::create(path, createOptions, db).isOk() && db != nullptr);
    if (!db) {
        removeDatabase(path);
        return;
    }

    std::unique_ptr<Transaction> tx;
    const DbResult beginResult = db->beginTransaction(tx);
    if (!beginResult.isOk()) {
        std::printf("Transaction begin failed: %s\n",
                    beginResult.describe().c_str());
    }
    CHECK(beginResult.isOk() && tx != nullptr);
    uint32_t committedGeneration = 0;
    uint64_t allocatedPageId = 0;
    if (tx) {
        DatabasePage staged;
        CHECK(tx->readPage(1, staged).isOk());
        staged.generation += 23;
        committedGeneration = staged.generation;
        CHECK(tx->writePage(staged).isOk());
        CHECK(tx->allocatePage(PageType::Data, allocatedPageId).isOk());
        DatabasePage newPage;
        newPage.pageId = allocatedPageId;
        newPage.type = PageType::Data;
        newPage.generation = 37;
        newPage.payload.push_back(0xA7);
        CHECK(tx->writePage(newPage).isOk());

        gxArenaFailAfter(&cacheArena, 0);
        const DbResult commitResult = tx->commit();
        CHECK(commitResult.status() == DbStatus::CommitFailed);
        CHECK(!tx->isActive());
        CHECK(db->diagnostics().recoveryRequired);
        CHECK(db->readPage(1, staged).status() == DbStatus::RecoveryRequired);
        CHECK(db->flush().status() == DbStatus::RecoveryRequired);
        CHECK(fileSize(walPath) > 0);
        gxArenaDisableFailure(&cacheArena);
    }

    const DbResult closeResult = db->close();
    if (closeResult.status() != DbStatus::RecoveryRequired) {
        std::printf("Close after commit failure returned: %s\n",
                    closeResult.describe().c_str());
    }
    CHECK(closeResult.status() == DbStatus::RecoveryRequired);
    tx.reset();
    db.reset();
    CHECK(fileSize(walPath) > 0);

    DatabaseOpenOptions openOptions;
    std::unique_ptr<Database> recovered;
    CHECK(Database::open(path, openOptions, recovered).isOk() &&
          recovered != nullptr);
    if (recovered) {
        DatabasePage page;
        CHECK(recovered->readPage(1, page).isOk());
        CHECK(page.generation == committedGeneration);
        CHECK(recovered->readPage(allocatedPageId, page).isOk());
        CHECK(page.pageId == allocatedPageId && page.type == PageType::Data &&
              page.generation == 37 && page.payload.size() == 1 &&
              page.payload[0] == 0xA7);
        CHECK(fileSize(walPath) > 0);
        CHECK(recovered->close().isOk());
    }
    recovered.reset();
    CHECK(fileSize(walPath) > 0);
    CHECK(cacheArena.outstanding == 0 && cacheArena.invalidFrees == 0);
    removeDatabase(path);
}

} // namespace

int main() {
    testPinnedImagePublication();
    testHostedPinnedViewAllocationFailure();
    testSqlCommitHostedViewFailureRecovery();
    testSqlCommitHostedViewFailureRecoveryCheckpointedControl();
    testSqlCommitHostedViewFailureRecoverySmallPages();
    testRejectsRegressingCommittedHeaderImages();
    testDurableCommitAllocationFailureRecovery();
    std::printf("Checked cache publication: %d passed, %d failed\n",
                gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
