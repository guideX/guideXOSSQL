#include "database_wal.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "database_endian.h"
#include "gx_test_allocator.h"
#include "gx_wal_scan.h"

using namespace gxos::db;

namespace {

int gChecks = 0;
int gFailures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++gChecks;                                                              \
        if (!(condition)) {                                                    \
            ++gFailures;                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #condition);                                          \
        }                                                                       \
    } while (0)

const uint32_t kPageSize = 512;
const uint8_t kDatabaseId[16] = {
    0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F
};

class MemoryFile : public IDatabaseFile {
public:
    explicit MemoryFile(const std::vector<uint8_t>& source)
        : bytes(source), failGetSize(false), failReadEnabled(false),
          failReadOffset(0), maximumReadLength(0), readCalls(0), _open(false),
          _readOnly(false), _path("wal-memory") {}

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override {
        _path = path;
        _open = true;
        _readOnly = mode == FileOpenMode::ReadOnly;
        if (truncateExisting) bytes.clear();
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
        if (failGetSize) return DbResult::error(DbStatus::IoError, "injected size failure");
        outSize = bytes.size();
        return DbResult::ok();
    }
    bool readAt(uint64_t offset, void* buffer, size_t length) override {
        ++readCalls;
        if (length > maximumReadLength) maximumReadLength = length;
        if (!_open || (failReadEnabled && offset == failReadOffset) ||
            offset > bytes.size() || length > bytes.size() - offset ||
            (length != 0 && buffer == nullptr)) {
            return false;
        }
        if (length != 0) {
            std::memcpy(buffer, bytes.data() + static_cast<size_t>(offset), length);
        }
        return true;
    }
    bool writeAt(uint64_t offset, const void* buffer, size_t length) override {
        if (!_open || _readOnly || offset > static_cast<uint64_t>(-1) - length ||
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
    bool failGetSize;
    bool failReadEnabled;
    uint64_t failReadOffset;
    size_t maximumReadLength;
    size_t readCalls;

private:
    bool _open;
    bool _readOnly;
    std::string _path;
};

DatabasePage makePage(uint64_t pageId, uint8_t seed) {
    DatabasePage page;
    page.pageId = pageId;
    page.type = PageType::Data;
    page.flags = static_cast<uint16_t>(seed * 3u);
    page.generation = seed + 1u;
    page.payload.resize(13);
    for (size_t i = 0; i < page.payload.size(); ++i) {
        page.payload[i] = static_cast<uint8_t>(seed + i * 7u);
    }
    return page;
}

DatabaseHeader makeHeader(uint64_t pageCount) {
    DatabaseHeader header = DatabaseHeader::makeDefault(kPageSize, kDatabaseId,
                                                         987654321);
    header.pageCount = pageCount;
    header.rootPageId = 1;
    return header;
}

std::vector<uint8_t> buildWal(bool includeTwoTransactions, bool includeGrowth) {
    MemoryFile* raw = new MemoryFile(std::vector<uint8_t>());
    CHECK(raw->open("writer.gxwal", FileOpenMode::Create, true).isOk());
    std::unique_ptr<IDatabaseFile> file(raw);
    WriteAheadLog writer;
    CHECK(writer.open(std::move(file), kDatabaseId, kPageSize, false).isOk());

    if (includeGrowth) {
        CHECK(writer.appendBegin(69).isOk());
        DatabaseHeader header = makeHeader(2);
        CHECK(writer.appendHeaderImage(69, header).isOk());
        CHECK(writer.appendCommit(69).isOk());
        CHECK(writer.appendBegin(70).isOk());
        DatabasePage page = makePage(1, 0x31);
        CHECK(writer.appendPageImage(70, page).isOk());
        CHECK(writer.appendCommit(70).isOk());
    } else {
        CHECK(writer.appendBegin(41).isOk());
        DatabasePage first = makePage(2, 0x24);
        CHECK(writer.appendPageImage(41, first).isOk());
        DatabaseHeader firstHeader = makeHeader(3);
        CHECK(writer.appendHeaderImage(41, firstHeader).isOk());
        CHECK(writer.appendCommit(41).isOk());

        if (includeTwoTransactions) {
            CHECK(writer.appendBegin(42).isOk());
            DatabasePage second = makePage(3, 0x57);
            CHECK(writer.appendPageImage(42, second).isOk());
            DatabaseHeader secondHeader = makeHeader(4);
            CHECK(writer.appendHeaderImage(42, secondHeader).isOk());
            CHECK(writer.appendCommit(42).isOk());
        }
    }
    return raw->bytes;
}

struct CapturedRecord {
    uint8_t type;
    uint64_t transactionId;
    uint32_t payloadLength;
    std::vector<uint8_t> payload;
};

struct CaptureContext {
    std::vector<CapturedRecord> records;
};

GxWalScanStatus captureRecord(void* opaque, uint8_t type, uint64_t transactionId,
                              const uint8_t* payload, uint32_t payloadLength) {
    CaptureContext& context = *static_cast<CaptureContext*>(opaque);
    CapturedRecord record;
    record.type = type;
    record.transactionId = transactionId;
    record.payloadLength = payloadLength;
    if (payloadLength != 0) {
        record.payload.assign(payload, payload + payloadLength);
    }
    context.records.push_back(record);
    return GxWalScanStatus_Ok;
}

bool readMemory(void* opaque, uint64_t offset, uint8_t* buffer, size_t length) {
    MemoryFile* file = static_cast<MemoryFile*>(opaque);
    return file->readAt(offset, buffer, length);
}

bool sameBytes(const std::vector<uint8_t>& left, const uint8_t* right,
               size_t length) {
    return left.size() == length &&
           (length == 0 || std::memcmp(left.data(), right, length) == 0);
}

const GxRecoveredPage* pageAt(const GxRecoveredPageCollection& pages,
                              size_t index) {
    const GxRecoveredPage* page = gxRecoveredPagesFirst(&pages);
    while (page && index != 0) {
        page = gxRecoveredPageNext(page);
        --index;
    }
    return page;
}

void testGoldenBeginAndExactDecodedRecords() {
    // Golden SQL3 BEGIN(1), independently fixed to the P2 CRC vector.
    const uint8_t golden[] = {
        1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
        0xA4, 0xEC, 0x17, 0xF1
    };
    MemoryFile goldenFile(std::vector<uint8_t>(golden, golden + sizeof(golden)));
    CHECK(goldenFile.open("golden", FileOpenMode::ReadOnly, false).isOk());
    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);
    CaptureContext goldenCapture;
    GxWalScanResult scanResult = {};
    CHECK(gxWalScanRecords(0, sizeof(golden), sizeof(golden),
                           GX_WAL_SCAN_MAX_PAYLOAD, readMemory, &goldenFile,
                           &allocator, captureRecord, &goldenCapture,
                           &scanResult) == GxWalScanStatus_Ok);
    CHECK(scanResult.recordsVisited == 1);
    CHECK(!scanResult.incompleteTail);
    CHECK(goldenCapture.records.size() == 1);
    if (goldenCapture.records.size() == 1) {
        CHECK(goldenCapture.records[0].type == GX_WAL_SCAN_RECORD_BEGIN);
        CHECK(goldenCapture.records[0].transactionId == 1);
        CHECK(goldenCapture.records[0].payloadLength == 0);
    }

    const std::vector<uint8_t> bytes = buildWal(true, false);
    MemoryFile walFile(bytes);
    CHECK(walFile.open("capture", FileOpenMode::ReadOnly, false).isOk());
    CaptureContext capture;
    scanResult = GxWalScanResult();
    CHECK(gxWalScanRecords(kWalHeaderSize, bytes.size(), kMaxWalFileBytes,
                           kMaxWalRecordPayload, readMemory, &walFile,
                           &allocator, captureRecord, &capture,
                           &scanResult) == GxWalScanStatus_Ok);
    CHECK(scanResult.recordsVisited == 8);
    CHECK(capture.records.size() == 8);
    const uint8_t expectedTypes[8] = {1, 2, 3, 4, 1, 2, 3, 4};
    const uint64_t expectedIds[8] = {41, 41, 41, 41, 42, 42, 42, 42};
    for (size_t i = 0; i < 8 && i < capture.records.size(); ++i) {
        CHECK(capture.records[i].type == expectedTypes[i]);
        CHECK(capture.records[i].transactionId == expectedIds[i]);
    }
    if (capture.records.size() >= 3) {
        CHECK(capture.records[1].payloadLength == wal_page_offset::PageBytes + kPageSize);
        CHECK(capture.records[2].payloadLength == kHeaderSize);
    }

    size_t offset = kWalHeaderSize;
    size_t recordIndex = 0;
    while (offset < bytes.size() && recordIndex < capture.records.size()) {
        const uint32_t payloadLength = loadLe32(bytes.data() + offset + 4);
        CHECK(capture.records[recordIndex].payloadLength == payloadLength);
        CHECK(sameBytes(capture.records[recordIndex].payload,
                        bytes.data() + offset + kWalRecordPrefixSize,
                        payloadLength));
        offset += kWalRecordPrefixSize + payloadLength;
        ++recordIndex;
    }
    CHECK(offset == bytes.size());
    CHECK(recordIndex == capture.records.size());
    CHECK(walFile.maximumReadLength <= GX_WAL_SCAN_MAX_PAYLOAD);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
}

std::unique_ptr<WriteAheadLog> openReader(const std::vector<uint8_t>& bytes,
                                          MemoryFile*& raw,
                                          const uint8_t* databaseId = kDatabaseId,
                                          const GxAllocator* allocator = nullptr) {
    raw = new MemoryFile(bytes);
    CHECK(raw->open("reader.gxwal", FileOpenMode::ReadOnly, false).isOk());
    std::unique_ptr<IDatabaseFile> file(raw);
    std::unique_ptr<WriteAheadLog> reader;
    if (allocator) reader.reset(new WriteAheadLog(*allocator));
    else reader.reset(new WriteAheadLog());
    CHECK(reader->open(std::move(file), databaseId, kPageSize, true).isOk());
    return reader;
}

DbResult scanBytes(const std::vector<uint8_t>& bytes, WalState& state,
                   std::vector<WalTransaction>& transactions,
                   std::string& message) {
    MemoryFile* raw = nullptr;
    std::unique_ptr<WriteAheadLog> reader = openReader(bytes, raw);
    DbResult result = reader->scan(state, transactions, message);
    return result;
}

void testProductionRecoveryEquivalence() {
    const std::vector<uint8_t> bytes = buildWal(true, false);
    MemoryFile* raw = nullptr;
    std::unique_ptr<WriteAheadLog> reader = openReader(bytes, raw);
    WalState state = WalState::Clean;
    std::vector<WalTransaction> transactions;
    std::string message;
    CHECK(reader->scan(state, transactions, message).isOk());
    CHECK(state == WalState::Committed);
    CHECK(transactions.size() == 2);
    if (transactions.size() == 2) {
        CHECK(transactions[0].transactionId == 41);
        CHECK(transactions[0].hasHeaderImage);
        CHECK(transactions[0].headerImage.pageCount == 3);
        CHECK(gxRecoveredPagesCount(&transactions[0].pages) == 1);
        const GxRecoveredPage* firstPage = pageAt(transactions[0].pages, 0);
        DatabasePage decodedFirst;
        CHECK(firstPage && DatabasePage::parse(gxRecoveredPageImage(firstPage),
                                               kPageSize, kPageSize, 2,
                                               decodedFirst).isOk());
        CHECK(firstPage && firstPage->pageId == 2);
        CHECK(decodedFirst.payload == makePage(2, 0x24).payload);
        CHECK(transactions[1].transactionId == 42);
        CHECK(transactions[1].hasHeaderImage);
        CHECK(transactions[1].headerImage.pageCount == 4);
        CHECK(gxRecoveredPagesCount(&transactions[1].pages) == 1);
        const GxRecoveredPage* secondPage = pageAt(transactions[1].pages, 0);
        DatabasePage decodedSecond;
        CHECK(secondPage && DatabasePage::parse(gxRecoveredPageImage(secondPage),
                                                kPageSize, kPageSize, 3,
                                                decodedSecond).isOk());
        CHECK(secondPage && secondPage->pageId == 3);
        CHECK(decodedSecond.payload == makePage(3, 0x57).payload);
    }
    CHECK(reader->lastTransactionId() == 42);
}

void testMalformedAndTruncatedRecords() {
    const std::vector<uint8_t> valid = buildWal(true, false);
    WalState state = WalState::Clean;
    std::vector<WalTransaction> transactions;
    std::string message;

    std::vector<uint8_t> unknown = valid;
    unknown[kWalHeaderSize] = 99;
    CHECK(scanBytes(unknown, state, transactions, message).status() == DbStatus::WalCorrupt);
    CHECK(state == WalState::Corrupt);

    std::vector<uint8_t> reserved = valid;
    reserved[kWalHeaderSize + 1] = 1;
    CHECK(scanBytes(reserved, state, transactions, message).status() == DbStatus::WalCorrupt);
    CHECK(state == WalState::Corrupt);

    std::vector<uint8_t> badCrc = valid;
    badCrc[kWalHeaderSize + kWalRecordPrefixSize +
           wal_page_offset::PageBytes + kPageSize - 1] ^= 0x80;
    CHECK(scanBytes(badCrc, state, transactions, message).status() == DbStatus::WalCorrupt);
    CHECK(state == WalState::Corrupt);

    std::vector<uint8_t> oversized = valid;
    storeLe32(oversized.data() + kWalHeaderSize + kWalRecordPrefixSize + 4,
              kMaxWalRecordPayload + 1);
    CHECK(scanBytes(oversized, state, transactions, message).status() == DbStatus::WalCorrupt);
    CHECK(state == WalState::Corrupt);

    std::vector<uint8_t> overflowing = valid;
    storeLe32(overflowing.data() + kWalHeaderSize + kWalRecordPrefixSize + 4,
              0xFFFFFFFFu);
    CHECK(scanBytes(overflowing, state, transactions, message).status() == DbStatus::WalCorrupt);
    CHECK(state == WalState::Corrupt);

    std::vector<uint8_t> shortPrefix(valid.begin(),
                                     valid.begin() + kWalHeaderSize + 7);
    CHECK(scanBytes(shortPrefix, state, transactions, message).isOk());
    CHECK(state == WalState::Incomplete);
    CHECK(transactions.empty());

    const size_t firstPagePayloadOffset = kWalHeaderSize + kWalRecordPrefixSize * 2;
    std::vector<uint8_t> shortPayload(valid.begin(),
        valid.begin() + firstPagePayloadOffset + 23);
    CHECK(scanBytes(shortPayload, state, transactions, message).isOk());
    CHECK(state == WalState::Incomplete);
    CHECK(transactions.empty());

    std::vector<uint8_t> committedThenShort(valid.begin(),
        valid.begin() + kWalHeaderSize +
        2 * (kWalRecordPrefixSize + 0) +
        (kWalRecordPrefixSize + wal_page_offset::PageBytes + kPageSize) +
        (kWalRecordPrefixSize + kHeaderSize) + 2);
    CHECK(scanBytes(committedThenShort, state, transactions, message).isOk());
    CHECK(state == WalState::Committed);
    CHECK(transactions.size() == 1);
}

void testIoAndIdentityFailures() {
    const std::vector<uint8_t> bytes = buildWal(false, false);
    WalState state = WalState::Clean;
    std::vector<WalTransaction> transactions;
    std::string message;
    MemoryFile* raw = nullptr;
    std::unique_ptr<WriteAheadLog> reader = openReader(bytes, raw);

    raw->failGetSize = true;
    CHECK(reader->scan(state, transactions, message).status() == DbStatus::IoError);
    CHECK(state != WalState::Corrupt);
    raw->failGetSize = false;

    // A read failure at an in-range record prefix is an I/O error, distinct
    // from a file whose reported size ends in a short prefix.
    raw->failReadEnabled = true;
    raw->failReadOffset = kWalHeaderSize;
    CHECK(reader->scan(state, transactions, message).status() == DbStatus::IoError);
    CHECK(state != WalState::Corrupt);
    raw->failReadOffset = kWalHeaderSize + 2 * kWalRecordPrefixSize;
    CHECK(reader->scan(state, transactions, message).status() == DbStatus::IoError);
    CHECK(state != WalState::Corrupt);
    raw->failReadEnabled = false;
    CHECK(reader->scan(state, transactions, message).isOk());
    CHECK(state == WalState::Committed);

    uint8_t otherId[16];
    std::memcpy(otherId, kDatabaseId, sizeof(otherId));
    otherId[0] ^= 1;
    std::unique_ptr<WriteAheadLog> foreign = openReader(bytes, raw, otherId);
    CHECK(foreign->scan(state, transactions, message).status() ==
          DbStatus::WalDatabaseMismatch);
    CHECK(state == WalState::Foreign);
}

void testAllocationFailureAndRetry() {
    const std::vector<uint8_t> bytes = buildWal(false, false);
    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);
    MemoryFile* raw = nullptr;
    std::unique_ptr<WriteAheadLog> reader = openReader(bytes, raw, kDatabaseId,
                                                       &allocator);
    WalState state = WalState::Clean;
    std::vector<WalTransaction> transactions;
    std::string message;

    const size_t walSizeBefore = raw->bytes.size();
    gxArenaFailAfter(&arena, 0);
    CHECK(reader->scan(state, transactions, message).status() == DbStatus::NoSpace);
    CHECK(state != WalState::Corrupt);
    CHECK(transactions.empty());
    CHECK(raw->bytes.size() == walSizeBefore);
    CHECK(raw->bytes == bytes);
    CHECK(arena.outstanding == 0);

    gxArenaDisableFailure(&arena);
    CHECK(reader->scan(state, transactions, message).isOk());
    CHECK(state == WalState::Committed);
    CHECK(transactions.size() == 1);
    CHECK(transactions.size() == 1 &&
          arena.outstanding == gxRecoveredPagesCount(&transactions[0].pages));
    transactions.clear();
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
}

void testPayloadScratchGrowthFailure() {
    const std::vector<uint8_t> bytes = buildWal(false, true);
    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);
    MemoryFile* raw = nullptr;
    std::unique_ptr<WriteAheadLog> reader = openReader(bytes, raw, kDatabaseId,
                                                       &allocator);
    WalState state = WalState::Clean;
    std::vector<WalTransaction> transactions;
    std::string message;

    // DB_HEADER allocates the first 128-byte scratch. PAGE_IMAGE then forces
    // growth to hold its 524-byte payload; fail exactly that second allocation.
    gxArenaFailAfter(&arena, 1);
    CHECK(reader->scan(state, transactions, message).status() == DbStatus::NoSpace);
    CHECK(transactions.empty());
    CHECK(arena.successfulAllocations == 1);
    CHECK(arena.freeCalls == 1);
    CHECK(arena.outstanding == 0);

    gxArenaDisableFailure(&arena);
    CHECK(reader->scan(state, transactions, message).isOk());
    CHECK(state == WalState::Committed);
    CHECK(transactions.size() == 2);
    if (transactions.size() == 2) {
        CHECK(transactions[0].transactionId == 69);
        CHECK(transactions[0].hasHeaderImage);
        CHECK(transactions[1].transactionId == 70);
        CHECK(gxRecoveredPagesCount(&transactions[1].pages) == 1);
    }
    CHECK(arena.outstanding == 1);
    transactions.clear();
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
}

} // namespace

int main() {
    testGoldenBeginAndExactDecodedRecords();
    testProductionRecoveryEquivalence();
    testMalformedAndTruncatedRecords();
    testIoAndIdentityFailures();
    testAllocationFailureAndRetry();
    testPayloadScratchGrowthFailure();
    std::printf("database_wal_recovery_portable_test: %d checks, %d failures\n",
                gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
