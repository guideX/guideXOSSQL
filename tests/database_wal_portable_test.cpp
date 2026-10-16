#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "database_relational.h"
#include "database_transaction.h"
#include "database_wal.h"
#include "gx_array.h"
#include "gx_allocator_hosted.h"
#include "gx_test_allocator.h"
#include "gx_wal_record.h"

using namespace gxos::db;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(value) check((value), #value, __LINE__)

void storeReferenceLe32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void storeReferenceLe64(uint8_t* out, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t referenceCrc(const std::vector<uint8_t>& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const uint8_t byte = (i >= 16 && i < 20) ? 0 : bytes[i];
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> referenceRecord(uint8_t type, uint64_t transactionId,
                                     const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> bytes(20 + payload.size(), 0);
    bytes[0] = type;
    storeReferenceLe32(bytes.data() + 4,
                       static_cast<uint32_t>(payload.size()));
    storeReferenceLe64(bytes.data() + 8, transactionId);
    if (!payload.empty()) {
        std::memcpy(bytes.data() + 20, payload.data(), payload.size());
    }
    storeReferenceLe32(bytes.data() + 16, referenceCrc(bytes));
    return bytes;
}

bool arrayEquals(const GxArray& array, const std::vector<uint8_t>& expected) {
    if (gxArraySize(&array) != expected.size()) return false;
    const uint8_t* actual = static_cast<const uint8_t*>(array.data);
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

void testGoldenAndReferenceEncoding() {
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, 20 + kMaxWalRecordPayload) ==
          GxStatus::Ok);
    GxArray output = {};
    CHECK(gxArrayInit(&output, &allocator, 1, 20 + kMaxWalRecordPayload) ==
          GxStatus::Ok);

    CHECK(gxWalRecordEncode(&output, kWalRecordBegin, 1, nullptr, 0,
                            kMaxWalRecordPayload) == GxStatus::Ok);
    const uint8_t beginGolden[20] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xA4, 0xEC, 0x17, 0xF1
    };
    CHECK(gxArraySize(&output) == sizeof(beginGolden));
    for (size_t i = 0; i < sizeof(beginGolden); ++i) {
        CHECK(output.data[i] == beginGolden[i]);
    }

    const uint8_t payload[] = {0x00, 0x7F, 0x80, 0xFF};
    CHECK(gxWalRecordEncode(&output, kWalRecordPageImage,
                            0x0102030405060708ull, payload, sizeof(payload),
                            kMaxWalRecordPayload) == GxStatus::Ok);
    const uint8_t pageGolden[24] = {
        0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x2D, 0x44, 0xAE, 0xF9, 0x00, 0x7F, 0x80, 0xFF
    };
    CHECK(gxArraySize(&output) == sizeof(pageGolden));
    for (size_t i = 0; i < sizeof(pageGolden); ++i) {
        CHECK(output.data[i] == pageGolden[i]);
    }
    CHECK(arrayEquals(output, referenceRecord(kWalRecordPageImage,
                                               0x0102030405060708ull,
                                               std::vector<uint8_t>(payload,
                                                                    payload + sizeof(payload)))));

    std::vector<uint8_t> maximumPayload(kMaxWalRecordPayload);
    for (size_t i = 0; i < maximumPayload.size(); ++i) {
        maximumPayload[i] = static_cast<uint8_t>(i * 37u + 11u);
    }
    CHECK(gxWalRecordEncode(&output, kWalRecordPageImage, 99,
                            maximumPayload.data(),
                            static_cast<uint32_t>(maximumPayload.size()),
                            kMaxWalRecordPayload) == GxStatus::Ok);
    CHECK(arrayEquals(output, referenceRecord(kWalRecordPageImage, 99,
                                               maximumPayload)));

    const std::vector<uint8_t> previous(
        static_cast<const uint8_t*>(output.data),
        static_cast<const uint8_t*>(output.data) + gxArraySize(&output));
    std::vector<uint8_t> tooLong(kMaxWalRecordPayload + 1, 0xA5);
    CHECK(gxWalRecordEncode(&output, kWalRecordPageImage, 100,
                            tooLong.data(),
                            static_cast<uint32_t>(tooLong.size()),
                            kMaxWalRecordPayload) == GxStatus::ResourceLimit);
    CHECK(arrayEquals(output, previous));
    gxArrayDestroy(&output);
}

void testAllocationFailureAndReuse() {
    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);

    GxArray output = {};
    CHECK(gxArrayInit(&output, &allocator, 1, 64) == GxStatus::Ok);
    gxArenaFailAfter(&arena, 0);
    CHECK(gxWalRecordEncode(&output, kWalRecordBegin, 7, nullptr, 0, 44) ==
          GxStatus::OutOfMemory);
    CHECK(gxArraySize(&output) == 0);
    CHECK(gxArrayCapacity(&output) == 0);
    CHECK(arena.outstanding == 0);

    gxArenaDisableFailure(&arena);
    CHECK(gxWalRecordEncode(&output, kWalRecordBegin, 7, nullptr, 0, 44) ==
          GxStatus::Ok);
    const std::vector<uint8_t> original(
        static_cast<const uint8_t*>(output.data),
        static_cast<const uint8_t*>(output.data) + gxArraySize(&output));
    const size_t originalCapacity = gxArrayCapacity(&output);

    uint8_t growthPayload[20];
    for (size_t i = 0; i < sizeof(growthPayload); ++i) {
        growthPayload[i] = static_cast<uint8_t>(i + 1);
    }
    gxArenaFailAfter(&arena, 0);
    CHECK(gxWalRecordEncode(&output, kWalRecordPageImage, 8, growthPayload,
                            sizeof(growthPayload), 44) == GxStatus::OutOfMemory);
    CHECK(arrayEquals(output, original));
    CHECK(gxArrayCapacity(&output) == originalCapacity);
    CHECK(arena.outstanding == 1);

    gxArenaDisableFailure(&arena);
    CHECK(gxWalRecordEncode(&output, kWalRecordPageImage, 8, growthPayload,
                            sizeof(growthPayload), 44) == GxStatus::Ok);
    CHECK(arrayEquals(output, referenceRecord(
                                  kWalRecordPageImage, 8,
                                  std::vector<uint8_t>(growthPayload,
                                                       growthPayload + sizeof(growthPayload)))));

    for (uint64_t i = 0; i < 100; ++i) {
        CHECK(gxWalRecordEncode(&output, kWalRecordCommit, i, nullptr, 0, 44) ==
              GxStatus::Ok);
        CHECK(arrayEquals(output, referenceRecord(kWalRecordCommit, i,
                                                   std::vector<uint8_t>())));
    }
    gxArrayDestroy(&output);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
}

std::string databaseTestPath() {
    const char* temp = std::getenv("TEMP");
    if (!temp || !*temp) temp = std::getenv("TMP");
    if (!temp || !*temp) temp = ".";
    return std::string(temp) + "/guideXOS_NDB1R4P2_wal_portable.gxdb";
}

void removeDatabaseFiles(const std::string& path) {
    std::remove(path.c_str());
    const std::string wal = walPathFor(path);
    std::remove(wal.c_str());
}

void testProductionTransactionPath() {
    const std::string path = databaseTestPath();
    removeDatabaseFiles(path);

    HostFileSystem fileSystem;
    DatabaseCreateOptions createOptions;
    createOptions.overwriteExisting = true;
    createOptions.generateDatabaseId = false;
    createOptions.creationTimeUnixNanos = 123456789;
    for (size_t i = 0; i < sizeof(createOptions.databaseId); ++i) {
        createOptions.databaseId[i] = static_cast<uint8_t>(i + 1);
    }

    std::unique_ptr<Database> database;
    CHECK(Database::create(path, createOptions, database, &fileSystem).isOk());
    CHECK(database && database->close().isOk());
    database.reset();

    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);
    DatabaseOpenOptions openOptions;
    openOptions.walRecordAllocator = &allocator;
    CHECK(Database::open(path, openOptions, database, &fileSystem).isOk());
    if (!database) {
        removeDatabaseFiles(path);
        return;
    }

    const uint64_t startingPageCount = database->pageCount();
    uint64_t walSize = 0;
    std::unique_ptr<Transaction> transaction;

    CHECK(database->beginTransaction(transaction).isOk());
    uint64_t failedPageId = 0;
    CHECK(transaction->allocatePage(PageType::Data, failedPageId).isOk());
    gxArenaFailAfter(&arena, 0);
    DbResult result = transaction->commit();
    CHECK(result.status() == DbStatus::CommitFailed);
    CHECK(!transaction->isActive());
    CHECK(database->pageCount() == startingPageCount);
    CHECK(database->wal().getSize(walSize).isOk());
    CHECK(walSize == kWalHeaderSize);

    gxArenaFailAfter(&arena, 1);
    CHECK(database->beginTransaction(transaction).isOk());
    CHECK(transaction->allocatePage(PageType::Data, failedPageId).isOk());
    result = transaction->commit();
    CHECK(result.status() == DbStatus::CommitFailed);
    CHECK(database->pageCount() == startingPageCount);
    CHECK(database->wal().getSize(walSize).isOk());
    CHECK(walSize == kWalHeaderSize + kWalRecordPrefixSize);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);

    gxArenaDisableFailure(&arena);
    CHECK(database->close().isOk());
    database.reset();
    CHECK(Database::open(path, openOptions, database, &fileSystem).isOk());
    CHECK(database && database->pageCount() == startingPageCount);

    CHECK(database->beginTransaction(transaction).isOk());
    uint64_t committedPageId = 0;
    CHECK(transaction->allocatePage(PageType::Data, committedPageId).isOk());
    CHECK(transaction->commit().isOk());
    CHECK(database->pageCount() == startingPageCount + 1);
    CHECK(arena.successfulAllocations >= 4);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);

    CHECK(database->close().isOk());
    database.reset();
    CHECK(Database::open(path, openOptions, database, &fileSystem).isOk());
    CHECK(database && database->pageCount() == startingPageCount + 1);
    if (database) {
        DatabasePage page;
        CHECK(database->readPage(committedPageId, page).isOk());
        CHECK(page.type == PageType::Data);
        CHECK(page.payload.empty());
        CHECK(database->close().isOk());
    }
    removeDatabaseFiles(path);
}

} // namespace

int main() {
    testGoldenAndReferenceEncoding();
    testAllocationFailureAndReuse();
    testProductionTransactionPath();
    std::printf("database_wal_portable_test: %d passed, %d failed\n",
                g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
