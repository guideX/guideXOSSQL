#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

#include "database_format.h"
#include "database_page.h"
#include "database_relational.h"
#include "database_sql.h"
#include "database_transaction.h"
#include "gx_owned_page.h"
#include "gx_test_allocator.h"

using namespace gxos::db;

namespace {

int gPassed = 0;
int gFailed = 0;
int gCase = 0;

void check(bool value, const char* expression, int line) {
    if (value) {
        ++gPassed;
    } else {
        ++gFailed;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(value) check((value), #value, __LINE__)

void storeLe16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void storeLe32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void storeLe64(uint8_t* out, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

uint32_t referenceCrc(const uint8_t* bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = i >= 24 && i < 28 ? 0 : bytes[i];
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> referencePage(uint64_t pageId, uint16_t pageType,
                                   uint16_t flags, uint32_t generation,
                                   const uint8_t* payload, size_t payloadSize,
                                   uint32_t pageSize) {
    std::vector<uint8_t> image(pageSize, 0);
    storeLe64(image.data(), pageId);
    storeLe16(image.data() + 8, pageType);
    storeLe16(image.data() + 10, flags);
    storeLe32(image.data() + 12, static_cast<uint32_t>(payloadSize));
    storeLe32(image.data() + 16, generation);
    if (payloadSize != 0) {
        std::memcpy(image.data() + kPageHeaderSize, payload, payloadSize);
    }
    storeLe32(image.data() + 24, referenceCrc(image.data(), image.size()));
    return image;
}

bool sameBytes(const GxOwnedPage& page, const std::vector<uint8_t>& bytes) {
    if (!page.isValid() || page.byteCount() != bytes.size()) return false;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (page.encodedBytes()[i] != bytes[i]) return false;
    }
    return true;
}

bool sqlOk(SqlEngine& engine, const char* sql) {
    const SqlExecutionResult result = engine.execute(sql);
    if (!result.ok) {
        std::printf("SQL failed: %s\n", result.error.describe().c_str());
    }
    return result.ok;
}

std::string testDirectory() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    const std::string path = std::string(base) + "/gxos_owned_page_tests";
#if defined(_WIN32)
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0700);
#endif
    return path;
}

std::string databasePath() {
    return testDirectory() + "/caller_owned_" + std::to_string(gCase++) + ".gxdb";
}

void removeDatabase(const std::string& path) {
    const std::string walPath = walPathFor(path);
    std::remove(walPath.c_str());
    std::remove(path.c_str());
}

void testOwnerContracts() {
    GxArena arenaA;
    GxArena arenaB;
    gxArenaInit(&arenaA, 4096);
    gxArenaInit(&arenaB, 4096);
    GxAllocator allocatorA = {};
    GxAllocator allocatorB = {};
    gxArenaMakeAllocator(&arenaA, &allocatorA);
    gxArenaMakeAllocator(&arenaB, &allocatorB);

    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<uint8_t> golden = referencePage(
        0x0102030405060708ull, static_cast<uint16_t>(PageType::Data), 0x1234,
        0x0A0B0C0Du, payload, sizeof(payload), 512);
    CHECK(golden[24] == 0x6C && golden[25] == 0x9A &&
          golden[26] == 0xEE && golden[27] == 0xBD);

    GxOwnedPage source;
    CHECK(source.init(&allocatorA, 512) == GxStatus::Ok);
    CHECK(source.isInitialized() && !source.isValid());
    CHECK(source.materialize(golden.data(), golden.size(),
                             0x0102030405060708ull) == GxStatus::Ok);
    CHECK(source.isValid() && source.validate() == GxStatus::Ok);
    CHECK(source.pageId() == 0x0102030405060708ull);
    CHECK(source.pageType() == static_cast<uint16_t>(PageType::Data));
    CHECK(source.flags() == 0x1234 && source.generation() == 0x0A0B0C0Du);
    CHECK(source.payloadSize() == sizeof(payload));
    CHECK(source.payloadCapacity() == 512 - kPageHeaderSize);
    CHECK(source.byteCount() == 512 && source.allocatedCapacity() == 512);
    CHECK(source.allocator().context == &arenaA);
    CHECK(sameBytes(source, golden));

    DatabasePage hosted;
    CHECK(DatabasePage::parse(golden.data(), golden.size(), 512,
                              0x0102030405060708ull, hosted).isOk());
    std::vector<uint8_t> hostedEncoding;
    CHECK(hosted.serialize(hostedEncoding, 512).isOk());
    CHECK(hostedEncoding == golden && sameBytes(source, hostedEncoding));

    GxOwnedPage destination;
    CHECK(destination.init(&allocatorB, 512) == GxStatus::Ok);
    CHECK(source.cloneTo(&allocatorB, destination) == GxStatus::Ok);
    CHECK(source.encodedBytes() != destination.encodedBytes());
    CHECK(sameBytes(source, golden) && sameBytes(destination, golden));
    CHECK(destination.allocator().context == &arenaB);
    CHECK(arenaA.outstanding == 1 && arenaB.outstanding == 1);

    const uint8_t replacementPayload[] = {1, 3, 5};
    const std::vector<uint8_t> replacement = referencePage(
        19, static_cast<uint16_t>(PageType::Catalog), 7, 11,
        replacementPayload, sizeof(replacementPayload), 512);
    const uint8_t* destinationBeforeFailure = destination.encodedBytes();
    gxArenaFailAfter(&arenaB, 0);
    CHECK(source.cloneTo(&allocatorB, destination) == GxStatus::OutOfMemory);
    CHECK(destination.encodedBytes() == destinationBeforeFailure);
    CHECK(sameBytes(destination, golden) && sameBytes(source, golden));
    CHECK(arenaA.outstanding == 1 && arenaB.outstanding == 1);
    gxArenaDisableFailure(&arenaB);
    CHECK(source.materialize(replacement.data(), replacement.size(), 19) ==
          GxStatus::Ok);
    CHECK(sameBytes(source, replacement));
    const uint8_t* oldDestination = destination.encodedBytes();
    CHECK(source.cloneTo(&allocatorB, destination) == GxStatus::Ok);
    CHECK(destination.encodedBytes() != oldDestination);
    CHECK(sameBytes(destination, replacement));
    CHECK(arenaA.outstanding == 1 && arenaB.outstanding == 1);

    const uint8_t* sourceBeforeFailure = source.encodedBytes();
    gxArenaFailAfter(&arenaA, 0);
    CHECK(source.materialize(golden.data(), golden.size(),
                             0x0102030405060708ull) == GxStatus::OutOfMemory);
    CHECK(source.encodedBytes() == sourceBeforeFailure);
    CHECK(sameBytes(source, replacement));
    CHECK(arenaA.outstanding == 1);
    gxArenaDisableFailure(&arenaA);
    CHECK(source.materialize(golden.data(), golden.size(),
                             0x0102030405060708ull) == GxStatus::Ok);
    CHECK(sameBytes(source, golden) && arenaA.outstanding == 1);

    CHECK(source.materialize(golden.data(), golden.size() - 1,
                             0x0102030405060708ull) == GxStatus::CorruptData);
    CHECK(source.encodedBytes() != nullptr && sameBytes(source, golden));
    std::vector<uint8_t> invalidType = golden;
    storeLe16(invalidType.data() + 8, 0xFFFF);
    storeLe32(invalidType.data() + 24,
              referenceCrc(invalidType.data(), invalidType.size()));
    const uint8_t* validBeforeCorrupt = source.encodedBytes();
    CHECK(source.materialize(invalidType.data(), invalidType.size(),
                             0x0102030405060708ull) == GxStatus::CorruptData);
    CHECK(source.encodedBytes() == validBeforeCorrupt && sameBytes(source, golden));
    std::vector<uint8_t> overCapacity = golden;
    storeLe32(overCapacity.data() + 12,
              static_cast<uint32_t>(512 - kPageHeaderSize + 1));
    storeLe32(overCapacity.data() + 24,
              referenceCrc(overCapacity.data(), overCapacity.size()));
    CHECK(source.materialize(overCapacity.data(), overCapacity.size(),
                             0x0102030405060708ull) == GxStatus::CorruptData);
    CHECK(source.encodedBytes() == validBeforeCorrupt && sameBytes(source, golden));
    CHECK(source.materialize(nullptr, golden.size(),
                             0x0102030405060708ull) == GxStatus::InvalidArgument);
    CHECK(source.encodedBytes() == validBeforeCorrupt && sameBytes(source, golden));

    GxAllocator incomplete = allocatorA;
    incomplete.allocate = nullptr;
    GxOwnedPage invalidOwner;
    CHECK(invalidOwner.init(&incomplete, 512) == GxStatus::InvalidArgument);
    CHECK(!invalidOwner.isInitialized() && arenaA.allocationAttempts >= 3);
    GxOwnedPage unsupportedSize;
    CHECK(unsupportedSize.init(&allocatorA, 1000) == GxStatus::InvalidArgument);
    GxAllocator tooSmall = allocatorA;
    tooSmall.maxAllocation = 128;
    GxOwnedPage bounded;
    CHECK(bounded.init(&tooSmall, 512) == GxStatus::ResourceLimit);
    CHECK(!bounded.isInitialized());

    const uint8_t* destinationBeforeInvalidClone = destination.encodedBytes();
    CHECK(source.cloneTo(&incomplete, destination) == GxStatus::InvalidArgument);
    CHECK(destination.encodedBytes() == destinationBeforeInvalidClone &&
          sameBytes(destination, replacement));
    const uint8_t* sourceBeforeWrongMove = source.encodedBytes();
    CHECK(source.moveTo(destination) == GxStatus::InvalidArgument);
    CHECK(source.encodedBytes() == sourceBeforeWrongMove && sameBytes(source, golden));
    CHECK(destination.encodedBytes() == destinationBeforeInvalidClone &&
          sameBytes(destination, replacement));
    CHECK(source.allocator().allocate == destination.allocator().allocate);
    CHECK(source.allocator().context != destination.allocator().context);
    CHECK(arenaA.outstanding == 1 && arenaB.outstanding == 1);

    GxOwnedPage moved;
    const uint8_t* movedBytes = source.encodedBytes();
    CHECK(source.moveTo(moved) == GxStatus::Ok);
    CHECK(!source.isValid() && source.isInitialized());
    CHECK(moved.isValid() && moved.encodedBytes() == movedBytes);
    CHECK(moved.allocator().context == &arenaA);
    CHECK(moved.validate() == GxStatus::Ok);
    CHECK(moved.clear() == GxStatus::Ok && !moved.isValid());
    CHECK(moved.init(&allocatorA, 512) == GxStatus::InvalidArgument);
    moved.destroy();
    CHECK(moved.init(&allocatorA, 512) == GxStatus::Ok);
    CHECK(moved.clear() == GxStatus::Ok);
    moved.destroy();
    source.destroy();
    destination.destroy();
    CHECK(arenaA.outstanding == 0 && arenaB.outstanding == 0);
    CHECK(arenaA.invalidFrees == 0 && arenaB.invalidFrees == 0);
}

void testProductionReadLifetimeAndSql9() {
    const std::string path = databasePath();
    GxArena cacheArena;
    GxArena callerArena;
    GxArena cloneArena;
    gxArenaInit(&cacheArena, 8192);
    gxArenaInit(&callerArena, 8192);
    gxArenaInit(&cloneArena, 8192);
    GxAllocator cacheAllocator = {};
    GxAllocator callerAllocator = {};
    GxAllocator cloneAllocator = {};
    gxArenaMakeAllocator(&cacheArena, &cacheAllocator);
    gxArenaMakeAllocator(&callerArena, &callerAllocator);
    gxArenaMakeAllocator(&cloneArena, &cloneAllocator);

    DatabaseCreateOptions createOptions;
    createOptions.bufferCapacity = 2;
    createOptions.pageCacheAllocator = &cacheAllocator;
    std::unique_ptr<Database> db;
    DbResult result = Database::create(path, createOptions, db);
    CHECK(result.isOk() && db != nullptr);
    if (!db) {
        removeDatabase(path);
        return;
    }

    {
        SqlEngine engine(*db);
        CHECK(sqlOk(engine, "CREATE TABLE PortableRows (Id INT64 PRIMARY KEY, Value TEXT NOT NULL);"));
        CHECK(sqlOk(engine, "CREATE INDEX IX_PortableRows_Value ON PortableRows (Value);"));
        CHECK(sqlOk(engine, "INSERT INTO PortableRows VALUES (1, 'before');"));
        SqlExecutionResult indexed = engine.execute(
            "SELECT Id FROM PortableRows WHERE Value='before';");
        CHECK(indexed.ok && indexed.statements.size() == 1 &&
              indexed.statements[0].accessPath == "IndexLookup" &&
              indexed.statements[0].resultSet.rowCount() == 1);

        CHECK(sqlOk(engine, "BEGIN;"));
        CHECK(sqlOk(engine, "UPDATE PortableRows SET Value='after' WHERE Id=1;"));
        SqlExecutionResult readYourWrites = engine.execute(
            "SELECT Value FROM PortableRows WHERE Id=1;");
        CHECK(readYourWrites.ok && readYourWrites.statements.size() == 1 &&
              readYourWrites.statements[0].resultSet.rowCount() == 1 &&
              readYourWrites.statements[0].resultSet.value(0, 0).textValue() ==
                  "after");
        const SqlExecutionResult statementFailure = engine.execute(
            "INSERT INTO PortableRows VALUES (1, 'duplicate');");
        CHECK(!statementFailure.ok && engine.inTransaction());
        SqlExecutionResult afterStatementFailure = engine.execute(
            "SELECT Value FROM PortableRows WHERE Id=1;");
        CHECK(afterStatementFailure.ok &&
              afterStatementFailure.statements[0].resultSet.value(0, 0)
                      .textValue() == "after");
        CHECK(sqlOk(engine, "COMMIT;"));
        SqlExecutionResult indexedAfterCommit = engine.execute(
            "SELECT Id FROM PortableRows WHERE Value='after';");
        CHECK(indexedAfterCommit.ok && indexedAfterCommit.statements.size() == 1 &&
              indexedAfterCommit.statements[0].accessPath == "IndexLookup" &&
              indexedAfterCommit.statements[0].resultSet.rowCount() == 1);

        CHECK(sqlOk(engine, "BEGIN;"));
        CHECK(sqlOk(engine, "DELETE FROM PortableRows WHERE Id=1;"));
        SqlExecutionResult deletedInTransaction = engine.execute(
            "SELECT Id FROM PortableRows WHERE Id=1;");
        CHECK(deletedInTransaction.ok &&
              deletedInTransaction.statements[0].resultSet.rowCount() == 0);
        CHECK(sqlOk(engine, "ROLLBACK;"));
        SqlExecutionResult restoredAfterRollback = engine.execute(
            "SELECT Value FROM PortableRows WHERE Id=1;");
        CHECK(restoredAfterRollback.ok &&
              restoredAfterRollback.statements[0].resultSet.rowCount() == 1 &&
              restoredAfterRollback.statements[0].resultSet.value(0, 0)
                      .textValue() == "after");
    }

    // The owned API reads only the committed cache. An explicit transaction's
    // staged image remains visible to Transaction::readPage, not this reader.
    GxOwnedPage committedOnly;
    CHECK(committedOnly.init(&callerAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(db->readOwnedPage(1, committedOnly).isOk());
    const uint32_t committedGeneration = committedOnly.generation();
    std::unique_ptr<Transaction> overlayTransaction;
    CHECK(db->beginTransaction(overlayTransaction).isOk() &&
          overlayTransaction != nullptr);
    if (overlayTransaction) {
        DatabasePage staged;
        CHECK(overlayTransaction->readPage(1, staged).isOk());
        staged.generation = committedGeneration + 1;
        CHECK(overlayTransaction->writePage(staged).isOk());
        DatabasePage overlay;
        CHECK(overlayTransaction->readPage(1, overlay).isOk());
        CHECK(overlay.generation == committedGeneration + 1);
        CHECK(db->readOwnedPage(1, committedOnly).isOk());
        CHECK(committedOnly.generation() == committedGeneration);
        CHECK(overlayTransaction->rollback().isOk());
    }

    GxOwnedPage initialFailure;
    CHECK(initialFailure.init(&callerAllocator, db->pageSize()) == GxStatus::Ok);
    gxArenaFailAfter(&callerArena, 0);
    CHECK(db->readOwnedPage(1, initialFailure).status() == DbStatus::NoSpace);
    CHECK(!initialFailure.isValid());
    gxArenaDisableFailure(&callerArena);

    GxOwnedPage unconfiguredCaller;
    CHECK(db->readOwnedPage(1, unconfiguredCaller).status() ==
          DbStatus::InvalidArgument);
    CHECK(!unconfiguredCaller.isInitialized() && !unconfiguredCaller.isValid());

    GxOwnedPage retained;
    CHECK(retained.init(&callerAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(db->readOwnedPage(1, retained).isOk());
    CHECK(retained.isValid() && retained.pageId() == 1 &&
          retained.validate() == GxStatus::Ok);
    CHECK(retained.allocator().context == &callerArena);
    CHECK(retained.allocator().context != &cacheArena);

    DatabasePage hosted;
    CHECK(db->readPage(1, hosted).isOk());
    const std::vector<uint8_t> expected = referencePage(
        hosted.pageId, static_cast<uint16_t>(hosted.type), hosted.flags,
        hosted.generation,
        hosted.payload.empty() ? nullptr : hosted.payload.data(),
        hosted.payload.size(), db->pageSize());
    std::vector<uint8_t> hostedEncoding;
    CHECK(hosted.serialize(hostedEncoding, db->pageSize()).isOk());
    CHECK(hostedEncoding == expected);
    CHECK(sameBytes(retained, expected));

    const uint8_t* retainedBeforeFailure = retained.encodedBytes();
    gxArenaFailAfter(&callerArena, 0);
    CHECK(db->readOwnedPage(1, retained).status() == DbStatus::NoSpace);
    CHECK(retained.encodedBytes() == retainedBeforeFailure &&
          sameBytes(retained, expected));
    gxArenaDisableFailure(&callerArena);
    CHECK(db->readOwnedPage(1, retained).isOk());
    CHECK(sameBytes(retained, expected));

    // Reads from a resident, already-pinned cache entry borrow its stable
    // serialized image without acquiring or releasing the caller's pin.
    BufferSlot* pinned = nullptr;
    CHECK(db->buffer().pinPage(1, pinned).isOk() && pinned != nullptr);
    if (pinned) {
        const uint32_t slotPinsBefore = pinned->pinCount;
        const uint32_t managerPinsBefore = db->buffer().pinnedCount();
        const uint8_t* retainedBeforePinnedFailure = retained.encodedBytes();
        gxArenaFailAfter(&callerArena, 0);
        CHECK(db->readOwnedPage(1, retained).status() == DbStatus::NoSpace);
        CHECK(retained.encodedBytes() == retainedBeforePinnedFailure &&
              sameBytes(retained, expected));
        CHECK(pinned->pinCount == slotPinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);
        gxArenaDisableFailure(&callerArena);
        CHECK(db->readOwnedPage(1, retained).isOk());
        CHECK(sameBytes(retained, expected));
        CHECK(pinned->pinCount == slotPinsBefore &&
              db->buffer().pinnedCount() == managerPinsBefore);
        CHECK(db->buffer().unpin(*pinned).isOk());
        CHECK(db->buffer().pinnedCount() + 1 == managerPinsBefore);
    }

    // Stream through the on-disk pages so page 1 is outside the two-slot
    // cache. Exercise both cache-image allocation failure and caller-owner
    // allocation failure before a miss can replace a resident entry.
    const uint64_t committedPageCount = db->pageCount();
    CHECK(committedPageCount > db->bufferCapacity() + 1);
    for (uint64_t pageId = 1; pageId < committedPageCount; ++pageId) {
        BufferSlot* scanSlot = nullptr;
        const DbResult pinResult = db->buffer().pinPage(pageId, scanSlot);
        CHECK(pinResult.isOk() && scanSlot != nullptr);
        if (pinResult.isOk() && scanSlot) {
            CHECK(db->buffer().unpin(*scanSlot).isOk());
        }
    }

    const uint8_t* retainedBeforeMissFailure = retained.encodedBytes();
    const size_t cacheOutstandingBeforeMissFailure = cacheArena.outstanding;
    gxArenaFailAfter(&cacheArena, 0);
    CHECK(db->readOwnedPage(1, retained).status() == DbStatus::NoSpace);
    CHECK(retained.encodedBytes() == retainedBeforeMissFailure &&
          sameBytes(retained, expected));
    CHECK(cacheArena.outstanding == cacheOutstandingBeforeMissFailure &&
          db->buffer().pinnedCount() == 0);
    gxArenaDisableFailure(&cacheArena);

    const size_t cacheOutstandingBeforeCallerFailure = cacheArena.outstanding;
    gxArenaFailAfter(&callerArena, 0);
    CHECK(db->readOwnedPage(1, retained).status() == DbStatus::NoSpace);
    CHECK(retained.encodedBytes() == retainedBeforeMissFailure &&
          sameBytes(retained, expected));
    CHECK(cacheArena.outstanding == cacheOutstandingBeforeCallerFailure &&
          db->buffer().pinnedCount() == 0);
    gxArenaDisableFailure(&callerArena);
    CHECK(db->readOwnedPage(1, retained).isOk());
    CHECK(sameBytes(retained, expected));
    CHECK(db->buffer().pinnedCount() == 0);

    GxOwnedPage wrongPageSize;
    CHECK(wrongPageSize.init(&callerAllocator, 1024) == GxStatus::Ok);
    const uint8_t* retainedBeforeInvalidOwner = wrongPageSize.encodedBytes();
    CHECK(db->readOwnedPage(1, wrongPageSize).status() ==
          DbStatus::InvalidArgument);
    CHECK(wrongPageSize.encodedBytes() == retainedBeforeInvalidOwner &&
          !wrongPageSize.isValid());

    GxOwnedPage unchanged;
    CHECK(unchanged.init(&callerAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(db->readOwnedPage(1, unchanged).isOk());
    const uint8_t* prior = unchanged.encodedBytes();
    CHECK(db->readOwnedPage(0, unchanged).status() == DbStatus::InvalidArgument);
    CHECK(unchanged.encodedBytes() == prior && sameBytes(unchanged, expected));

    CHECK(db->close().isOk());
    db.reset();
    CHECK(cacheArena.outstanding == 0);
    CHECK(callerArena.outstanding == 3);
    CHECK(retained.validate() == GxStatus::Ok && sameBytes(retained, expected));
    GxOwnedPage clonedAfterClose;
    CHECK(clonedAfterClose.init(&cloneAllocator, retained.pageSize()) == GxStatus::Ok);
    CHECK(retained.cloneTo(&cloneAllocator, clonedAfterClose) == GxStatus::Ok);
    CHECK(clonedAfterClose.allocator().context == &cloneArena);
    CHECK(sameBytes(clonedAfterClose, expected));

    DatabaseOpenOptions openOptions;
    std::unique_ptr<Database> reopened;
    CHECK(Database::open(path, openOptions, reopened).isOk());
    if (reopened) {
        SqlEngine engine(*reopened);
        SqlExecutionResult persisted = engine.execute(
            "SELECT Id FROM PortableRows WHERE Value='after';");
        CHECK(persisted.ok && persisted.statements.size() == 1 &&
              persisted.statements[0].accessPath == "IndexLookup" &&
              persisted.statements[0].resultSet.rowCount() == 1);
        CHECK(reopened->close().isOk());
    }
    reopened.reset();

    clonedAfterClose.destroy();
    committedOnly.destroy();
    unchanged.destroy();
    wrongPageSize.destroy();
    initialFailure.destroy();
    retained.destroy();
    CHECK(callerArena.outstanding == 0 && cloneArena.outstanding == 0);
    CHECK(cacheArena.invalidFrees == 0 && callerArena.invalidFrees == 0 &&
          cloneArena.invalidFrees == 0);
    removeDatabase(path);
}

} // namespace

int main() {
    testOwnerContracts();
    testProductionReadLifetimeAndSql9();
    std::printf("Caller-owned page: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
