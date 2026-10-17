#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "database_checksum.h"
#include "database_file.h"
#include "database_heap.h"
#include "database_index.h"
#include "database_relational.h"
#include "database_schema.h"
#include "database_transaction.h"
#include "database_types.h"
#include "database_wal.h"
#include "gx_array.h"
#include "gx_page_codec.h"
#include "gx_test_allocator.h"
#include "gx_wal_page_image.h"
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
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t independentCrc(const uint8_t* bytes, size_t length,
                        size_t zeroOffset, size_t zeroLength) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = i >= zeroOffset && i - zeroOffset < zeroLength
                                 ? 0
                                 : bytes[i];
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> referencePage(uint64_t pageId, uint16_t pageType,
                                   uint16_t flags, uint32_t generation,
                                   const std::vector<uint8_t>& payload,
                                   uint32_t pageSize) {
    std::vector<uint8_t> bytes(pageSize, 0);
    storeLe64(bytes.data(), pageId);
    storeLe16(bytes.data() + 8, pageType);
    storeLe16(bytes.data() + 10, flags);
    storeLe32(bytes.data() + 12, static_cast<uint32_t>(payload.size()));
    storeLe32(bytes.data() + 16, generation);
    if (!payload.empty()) {
        std::memcpy(bytes.data() + kPageHeaderSize, payload.data(), payload.size());
    }
    storeLe32(bytes.data() + 24,
              independentCrc(bytes.data(), bytes.size(), 24, 4));
    return bytes;
}

std::vector<uint8_t> referenceWalRecord(uint8_t type, uint64_t transactionId,
                                        const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> bytes(GX_WAL_RECORD_PREFIX_SIZE + payload.size(), 0);
    bytes[0] = type;
    storeLe32(bytes.data() + 4, static_cast<uint32_t>(payload.size()));
    storeLe64(bytes.data() + 8, transactionId);
    if (!payload.empty()) {
        std::memcpy(bytes.data() + GX_WAL_RECORD_PREFIX_SIZE,
                    payload.data(), payload.size());
    }
    storeLe32(bytes.data() + GX_WAL_RECORD_CRC_OFFSET,
              independentCrc(bytes.data(), bytes.size(),
                             GX_WAL_RECORD_CRC_OFFSET, 4));
    return bytes;
}

std::vector<uint8_t> referencePageRecord(uint64_t transactionId,
                                         uint64_t pageId, uint16_t pageType,
                                         uint16_t flags, uint32_t generation,
                                         const std::vector<uint8_t>& pagePayload,
                                         uint32_t pageSize) {
    const std::vector<uint8_t> page = referencePage(
        pageId, pageType, flags, generation, pagePayload, pageSize);
    std::vector<uint8_t> payload(GX_WAL_PAGE_IMAGE_HEADER_SIZE + pageSize, 0);
    storeLe16(payload.data(), pageType);
    storeLe64(payload.data() + 4, pageId);
    std::memcpy(payload.data() + GX_WAL_PAGE_IMAGE_HEADER_SIZE,
                page.data(), page.size());
    return referenceWalRecord(kWalRecordPageImage, transactionId, payload);
}

bool arrayEquals(const GxArray& array, const std::vector<uint8_t>& expected) {
    if (gxArraySize(&array) != expected.size()) return false;
    const uint8_t* actual = static_cast<const uint8_t*>(array.data);
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

void testCrcGoldenVectors() {
    CHECK(crc32(nullptr, 0) == 0x00000000u);
    const uint8_t single[] = {0xA5};
    CHECK(crc32(single, sizeof(single)) == 0x74BEB8EAu);
    const uint8_t repeated[32] = {
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC
    };
    CHECK(crc32(repeated, sizeof(repeated)) == 0xCBD832D5u);
    const uint8_t known[] = "123456789";
    CHECK(crc32(known, sizeof(known) - 1) == 0xCBF43926u);

    const uint8_t walPrefix[GX_WAL_RECORD_PREFIX_SIZE] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xA4, 0xEC, 0x17, 0xF1
    };
    CHECK(crc32(walPrefix, sizeof(walPrefix)) == 0x5CDF0D2Eu);

    // Serialized empty Data page (page id 1) with its independently recorded
    // page checksum already installed at bytes 24..27.
    std::vector<uint8_t> page(512, 0);
    page[0] = 1;
    page[8] = static_cast<uint8_t>(PageType::Data);
    page[24] = 0x6A;
    page[25] = 0x44;
    page[26] = 0xB9;
    page[27] = 0xBA;
    CHECK(crc32(page.data(), page.size()) == 0x8956487Au);
    page[100] ^= 0x01;
    CHECK(crc32(page.data(), page.size()) == 0x49396B65u);

    std::vector<uint8_t> large(kMaxPageSize);
    for (size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<uint8_t>(i * 73u + 19u);
    }
    CHECK(crc32(large.data(), large.size()) == 0x23E19BE8u);
}

void testPortablePageImageEncodingAndFailure() {
    const uint32_t pageSize = 512;
    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);

    GxArray record = {};
    CHECK(gxArrayInit(&record, &allocator, 1,
                      GX_WAL_RECORD_PREFIX_SIZE + kMaxWalRecordPayload) ==
          GxStatus::Ok);

    const std::vector<uint8_t> emptyPayload;
    gxArenaFailAfter(&arena, 0);
    CHECK(gxWalPageImageRecordEncode(
              &record, 7, 1, static_cast<uint16_t>(PageType::Data), 0, 0,
              nullptr, 0, pageSize, kMaxWalRecordPayload) == GxStatus::OutOfMemory);
    CHECK(gxArraySize(&record) == 0);
    CHECK(arena.outstanding == 0);

    gxArenaDisableFailure(&arena);
    CHECK(gxWalPageImageRecordEncode(
              &record, 7, 1, static_cast<uint16_t>(PageType::Data), 0, 0,
              nullptr, 0, pageSize, kMaxWalRecordPayload) == GxStatus::Ok);
    const std::vector<uint8_t> expectedZero = referencePageRecord(
        7, 1, static_cast<uint16_t>(PageType::Data), 0, 0, emptyPayload,
        pageSize);
    CHECK(arrayEquals(record, expectedZero));

    std::vector<uint8_t> distinctPayload(117);
    for (size_t i = 0; i < distinctPayload.size(); ++i) {
        distinctPayload[i] = static_cast<uint8_t>(i * 29u + 3u);
    }
    const uint64_t lastSupportedPage = kMaxPageCount - 1;
    CHECK(gxWalPageImageRecordEncode(
              &record, 0x0102030405060708ull, lastSupportedPage,
              static_cast<uint16_t>(PageType::IndexLeaf), 0x1234, 0xA1B2C3D4,
              distinctPayload.data(), distinctPayload.size(), pageSize,
              kMaxWalRecordPayload) == GxStatus::Ok);
    CHECK(arrayEquals(record, referencePageRecord(
                                  0x0102030405060708ull, lastSupportedPage,
                                  static_cast<uint16_t>(PageType::IndexLeaf),
                                  0x1234, 0xA1B2C3D4, distinctPayload,
                                  pageSize)));

    const std::vector<uint8_t> prior(
        static_cast<const uint8_t*>(record.data),
        static_cast<const uint8_t*>(record.data) + gxArraySize(&record));
    CHECK(gxWalPageImageRecordEncode(
              &record, 9, 0, static_cast<uint16_t>(PageType::Data), 0, 0,
              nullptr, 0, pageSize, kMaxWalRecordPayload) ==
          GxStatus::InvalidArgument);
    CHECK(arrayEquals(record, prior));
    CHECK(gxWalPageImageRecordEncode(
              &record, 9, kMaxPageCount,
              static_cast<uint16_t>(PageType::Data), 0, 0, nullptr, 0,
              pageSize, kMaxWalRecordPayload) == GxStatus::InvalidArgument);
    CHECK(arrayEquals(record, prior));

    std::vector<uint8_t> tooLarge(pageSize - kPageHeaderSize + 1, 0x5A);
    CHECK(gxWalPageImageRecordEncode(
              &record, 9, 2, static_cast<uint16_t>(PageType::Data), 0, 0,
              tooLarge.data(), tooLarge.size(), pageSize,
              kMaxWalRecordPayload) == GxStatus::InvalidArgument);
    CHECK(arrayEquals(record, prior));
    CHECK(gxWalPageImageRecordEncode(
              &record, 9, 2, static_cast<uint16_t>(PageType::Data), 0, 0,
              nullptr, 1, pageSize, kMaxWalRecordPayload) ==
          GxStatus::InvalidArgument);
    CHECK(gxWalPageImageRecordEncode(
              &record, 9, 2, static_cast<uint16_t>(PageType::Data), 0, 0,
              nullptr, 0, 513, kMaxWalRecordPayload) ==
          GxStatus::InvalidArgument);
    CHECK(gxWalPageImageRecordEncode(
              &record, 9, 2, static_cast<uint16_t>(PageType::Data), 0, 0,
              nullptr, 0, pageSize, pageSize + GX_WAL_PAGE_IMAGE_HEADER_SIZE - 1) ==
          GxStatus::ResourceLimit);
    CHECK(arrayEquals(record, prior));

    // Growth of the encoded record uses the same single reserve boundary.
    gxArenaFailAfter(&arena, 0);
    CHECK(gxWalPageImageRecordEncode(
              &record, 10, 3, static_cast<uint16_t>(PageType::Data), 0, 0,
              nullptr, 0, 4096, kMaxWalRecordPayload) == GxStatus::OutOfMemory);
    CHECK(arrayEquals(record, prior));
    CHECK(arena.outstanding == 1);
    gxArenaDisableFailure(&arena);
    CHECK(gxWalPageImageRecordEncode(
              &record, 10, 3, static_cast<uint16_t>(PageType::Data), 0, 0,
              nullptr, 0, 4096, kMaxWalRecordPayload) == GxStatus::Ok);

    gxArrayDestroy(&record);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
}

class MemoryDatabaseFile : public IDatabaseFile {
public:
    explicit MemoryDatabaseFile(const std::vector<uint8_t>& initial = std::vector<uint8_t>())
        : bytes(initial), _open(false), _readOnly(false), _path("memory.gxwal") {}

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override {
        _path = path;
        _open = true;
        _readOnly = mode == FileOpenMode::ReadOnly;
        if (mode == FileOpenMode::Create && truncateExisting) bytes.clear();
        return DbResult::ok();
    }
    DbResult close() override {
        _open = false;
        return DbResult::ok();
    }
    bool isOpen() const override { return _open; }
    bool isReadOnly() const override { return _readOnly; }
    DbResult getSize(uint64_t& outSize) override {
        if (!_open) return DbResult::error(DbStatus::NotOpen, "closed");
        outSize = bytes.size();
        return DbResult::ok();
    }
    bool readAt(uint64_t offset, void* buffer, size_t length) override {
        if (!_open || offset > bytes.size() || length > bytes.size() - offset ||
            (length != 0 && buffer == nullptr)) {
            return false;
        }
        if (length != 0) std::memcpy(buffer, bytes.data() + offset, length);
        return true;
    }
    bool writeAt(uint64_t offset, const void* buffer, size_t length) override {
        if (!_open || _readOnly || offset > static_cast<uint64_t>(-1) - length ||
            offset + length > static_cast<uint64_t>(-1) ||
            offset + length > static_cast<uint64_t>(static_cast<size_t>(-1)) ||
            (length != 0 && buffer == nullptr)) {
            return false;
        }
        const size_t end = static_cast<size_t>(offset + length);
        if (end > bytes.size()) bytes.resize(end, 0);
        if (length != 0) {
            std::memcpy(bytes.data() + static_cast<size_t>(offset), buffer, length);
        }
        return true;
    }
    bool truncateTo(uint64_t size) override {
        if (!_open || _readOnly || size > static_cast<uint64_t>(static_cast<size_t>(-1))) {
            return false;
        }
        bytes.resize(static_cast<size_t>(size));
        return true;
    }
    bool flush() override { return _open; }
    const std::string& path() const override { return _path; }

    std::vector<uint8_t> bytes;

private:
    bool _open;
    bool _readOnly;
    std::string _path;
};

void testProductionWalBytesAndRecoveryStates() {
    const uint32_t pageSize = 512;
    const uint8_t databaseId[16] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    };
    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);

    MemoryDatabaseFile* capture = new MemoryDatabaseFile();
    capture->open("captured.gxwal", FileOpenMode::Create, true);
    std::unique_ptr<IDatabaseFile> file(capture);
    WriteAheadLog wal(allocator);
    CHECK(wal.open(std::move(file), databaseId, pageSize, false).isOk());
    CHECK(wal.appendBegin(91).isOk());
    const uint64_t sizeAfterBegin = capture->bytes.size();
    DatabasePage invalidPage;
    invalidPage.pageId = 0;
    invalidPage.type = PageType::Data;
    CHECK(wal.appendPageImage(91, invalidPage).status() == DbStatus::InvalidArgument);
    CHECK(capture->bytes.size() == sizeAfterBegin);
    invalidPage.pageId = 1;
    invalidPage.payload.resize(pageSize - kPageHeaderSize + 1, 0xEE);
    CHECK(wal.appendPageImage(91, invalidPage).status() == DbStatus::InvalidArgument);
    CHECK(capture->bytes.size() == sizeAfterBegin);

    DatabasePage zeroPage;
    zeroPage.pageId = 1;
    zeroPage.type = PageType::Data;
    CHECK(wal.appendPageImage(91, zeroPage).isOk());

    DatabasePage patternedPage;
    patternedPage.pageId = 2;
    patternedPage.type = PageType::IndexLeaf;
    patternedPage.flags = 0x4321;
    patternedPage.generation = 27;
    patternedPage.payload.resize(75);
    for (size_t i = 0; i < patternedPage.payload.size(); ++i) {
        patternedPage.payload[i] = static_cast<uint8_t>(i * 17u + 5u);
    }
    CHECK(wal.appendPageImage(91, patternedPage).isOk());

    DatabasePage boundaryPage;
    boundaryPage.pageId = kMaxPageCount - 1;
    boundaryPage.type = PageType::Data;
    boundaryPage.payload.assign(4, 0xA7);
    CHECK(wal.appendPageImage(91, boundaryPage).isOk());
    CHECK(wal.appendCommit(91).isOk());
    CHECK(wal.sync().isOk());

    const std::vector<uint8_t> begin = referenceWalRecord(kWalRecordBegin, 91,
                                                           std::vector<uint8_t>());
    CHECK(capture->bytes.size() == kWalHeaderSize + begin.size() +
          3 * (GX_WAL_RECORD_PREFIX_SIZE + GX_WAL_PAGE_IMAGE_HEADER_SIZE + pageSize) +
          GX_WAL_RECORD_PREFIX_SIZE);
    size_t offset = kWalHeaderSize;
    CHECK(std::memcmp(capture->bytes.data() + offset,
                      begin.data(), begin.size()) == 0);
    offset += begin.size();

    const std::vector<uint8_t> expectedZero = referencePageRecord(
        91, zeroPage.pageId, static_cast<uint16_t>(zeroPage.type), 0, 0,
        std::vector<uint8_t>(), pageSize);
    CHECK(std::memcmp(capture->bytes.data() + offset,
                      expectedZero.data(), expectedZero.size()) == 0);
    offset += expectedZero.size();
    const std::vector<uint8_t> expectedPattern = referencePageRecord(
        91, patternedPage.pageId, static_cast<uint16_t>(patternedPage.type),
        patternedPage.flags, patternedPage.generation,
        patternedPage.payload, pageSize);
    CHECK(std::memcmp(capture->bytes.data() + offset,
                      expectedPattern.data(), expectedPattern.size()) == 0);
    offset += expectedPattern.size();
    const std::vector<uint8_t> expectedBoundary = referencePageRecord(
        91, boundaryPage.pageId, static_cast<uint16_t>(boundaryPage.type), 0, 0,
        boundaryPage.payload, pageSize);
    CHECK(std::memcmp(capture->bytes.data() + offset,
                      expectedBoundary.data(), expectedBoundary.size()) == 0);

    // The actual scanner accepts all three encoded images and their ordering.
    std::vector<uint8_t> validBytes = capture->bytes;
    wal.close();
    MemoryDatabaseFile* validRaw = new MemoryDatabaseFile(validBytes);
    validRaw->open("valid.gxwal", FileOpenMode::ReadOnly, false);
    std::unique_ptr<IDatabaseFile> validFile(validRaw);
    WriteAheadLog validReader;
    CHECK(validReader.open(std::move(validFile), databaseId, pageSize, true).isOk());
    WalState state = WalState::Clean;
    std::vector<WalTransaction> transactions;
    std::string message;
    CHECK(validReader.scan(state, transactions, message).isOk());
    CHECK(state == WalState::Committed);
    CHECK(transactions.size() == 1);
    CHECK(transactions[0].pages.size() == 3);
    CHECK(transactions[0].pages[0].pageId == 1);
    CHECK(transactions[0].pages[1].pageId == 2);
    CHECK(transactions[0].pages[2].pageId == kMaxPageCount - 1);
    validReader.close();

    std::vector<uint8_t> corrupted = validBytes;
    const size_t firstPageData = kWalHeaderSize + begin.size() +
                                 GX_WAL_RECORD_PREFIX_SIZE +
                                 GX_WAL_PAGE_IMAGE_HEADER_SIZE + kPageHeaderSize;
    corrupted[firstPageData] ^= 0x01;
    MemoryDatabaseFile* corruptRaw = new MemoryDatabaseFile(corrupted);
    corruptRaw->open("corrupt.gxwal", FileOpenMode::ReadOnly, false);
    std::unique_ptr<IDatabaseFile> corruptFile(corruptRaw);
    WriteAheadLog corruptReader;
    CHECK(corruptReader.open(std::move(corruptFile), databaseId, pageSize, true).isOk());
    CHECK(corruptReader.scan(state, transactions, message).status() == DbStatus::WalCorrupt);
    CHECK(state == WalState::Corrupt);
    corruptReader.close();

    std::vector<uint8_t> incomplete = validBytes;
    incomplete.resize(incomplete.size() - GX_WAL_RECORD_PREFIX_SIZE / 2);
    MemoryDatabaseFile* incompleteRaw = new MemoryDatabaseFile(incomplete);
    incompleteRaw->open("incomplete.gxwal", FileOpenMode::ReadOnly, false);
    std::unique_ptr<IDatabaseFile> incompleteFile(incompleteRaw);
    WriteAheadLog incompleteReader;
    CHECK(incompleteReader.open(std::move(incompleteFile), databaseId,
                                pageSize, true).isOk());
    CHECK(incompleteReader.scan(state, transactions, message).isOk());
    CHECK(state == WalState::Incomplete);
    CHECK(transactions.empty());
    incompleteReader.close();
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
}

std::string transactionTestPath() {
    const char* temp = std::getenv("TEMP");
    if (!temp || !*temp) temp = std::getenv("TMP");
    if (!temp || !*temp) temp = ".";
    return std::string(temp) + "/guideXOS_NDB1R4P3_page_portable.gxdb";
}

void removeDatabaseFiles(const std::string& path) {
    std::remove(path.c_str());
    std::remove(walPathFor(path).c_str());
}

void testProductionTransactionAllocationRecovery() {
    const std::string path = transactionTestPath();
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

    TableDefinition definition("P3Items");
    definition.columns.push_back(ColumnDefinition("id", DbType::Int64, false));
    uint32_t tableId = 0;
    CHECK(database->createTable(definition, tableId).isOk());
    IndexDefinition index("p3_items_id", tableId, 0, IndexKind::Ordinary);
    uint32_t indexId = 0;
    CHECK(database->createIndex(index, indexId).isOk());
    CHECK(database->wal().checkpoint().isOk());
    const uint64_t initialPageCount = database->pageCount();
    std::unique_ptr<Transaction> transaction;
    CHECK(database->beginTransaction(transaction).isOk());
    std::unique_ptr<Table> table;
    CHECK(transaction->openTable("P3Items", table).isOk());
    CHECK(table->insert(std::vector<DbValue>(1, DbValue::int64(101))).isOk());
    CHECK(transaction->modifiedPageCount() >= 2);

    // BEGIN and the first PAGE_IMAGE allocation succeed; the second page image
    // is rejected before any bytes for that record reach the WAL.
    gxArenaFailAfter(&arena, 2);
    const DbResult failedCommit = transaction->commit();
    CHECK(failedCommit.status() == DbStatus::CommitFailed);
    CHECK(!transaction->isActive());
    CHECK(database->pageCount() == initialPageCount);
    uint64_t walSize = 0;
    CHECK(database->wal().getSize(walSize).isOk());
    const uint64_t expectedIncompleteSize =
        kWalHeaderSize + kWalRecordPrefixSize +
        kWalRecordPrefixSize + GX_WAL_PAGE_IMAGE_HEADER_SIZE + database->pageSize();
    CHECK(walSize == expectedIncompleteSize);
    WalState state = WalState::Clean;
    std::vector<WalTransaction> recoveredTransactions;
    std::string message;
    // The commit fault injection above targets the WAL writer's record buffer.
    // Recovery scanning now uses the same allocator for its separate bounded
    // payload scratch, so disable the writer fault before validating the tail.
    gxArenaDisableFailure(&arena);
    CHECK(database->wal().scan(state, recoveredTransactions, message).isOk());
    CHECK(state == WalState::Incomplete);
    CHECK(recoveredTransactions.empty());
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);

    // Recovery discards the uncommitted begin + first image. A fresh attempt
    // then allocates and commits both pages using the same allocator.
    gxArenaDisableFailure(&arena);
    CHECK(database->close().isOk());
    database.reset();
    CHECK(Database::open(path, openOptions, database, &fileSystem).isOk());
    CHECK(database && database->pageCount() == initialPageCount);
    if (database) {
        CHECK(database->beginTransaction(transaction).isOk());
        CHECK(transaction->openTable("P3Items", table).isOk());
        CHECK(table->insert(std::vector<DbValue>(1, DbValue::int64(101))).isOk());
        CHECK(transaction->commit().isOk());
        CHECK(database->pageCount() >= initialPageCount + 1);
        CHECK(database->close().isOk());
        database.reset();
    }
    CHECK(Database::open(path, openOptions, database, &fileSystem).isOk());
    CHECK(database && database->pageCount() >= initialPageCount + 1);
    if (database) {
        std::unique_ptr<Table> readTable;
        CHECK(database->openTable("P3Items", readTable).isOk());
        CHECK(readTable && readTable->rowCount() == 1);
        IndexValidation validation;
        CHECK(database->validateIndex(indexId, validation).isOk());
        CHECK(database->close().isOk());
    }
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
    removeDatabaseFiles(path);
}

} // namespace

int main() {
    testCrcGoldenVectors();
    testPortablePageImageEncodingAndFailure();
    testProductionWalBytesAndRecoveryStates();
    testProductionTransactionAllocationRecovery();
    std::printf("database_wal_page_portable_test: %d passed, %d failed\n",
                g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
