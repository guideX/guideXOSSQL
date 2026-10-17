#include "database_wal.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "database_index.h"
#include "database_relational.h"
#include "database_sql.h"
#include "database_transaction.h"
#include "database_endian.h"
#include "gx_test_allocator.h"
#include "gx_allocator_hosted.h"

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
    0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xA8,
    0xB9, 0xCA, 0xDB, 0xEC, 0xFD, 0x0E, 0x1F, 0x20
};

struct P5FaultAllocator {
    size_t successfulAllocations;
    size_t freeCalls;
    size_t invalidFrees;
    size_t outstanding;
    size_t failAtSuccessCount;
    bool failureEnabled;
};

bool p5FaultAllocate(void* context, size_t size, size_t alignment,
                     void** outPointer) {
    P5FaultAllocator* state = static_cast<P5FaultAllocator*>(context);
    *outPointer = nullptr;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        alignment > alignof(std::max_align_t) ||
        (state->failureEnabled &&
         state->successfulAllocations >= state->failAtSuccessCount)) {
        return false;
    }
    void* pointer = std::malloc(size);
    if (!pointer) return false;
    ++state->successfulAllocations;
    ++state->outstanding;
    *outPointer = pointer;
    return true;
}

void p5FaultFree(void* context, void* pointer) {
    if (!pointer) return;
    P5FaultAllocator* state = static_cast<P5FaultAllocator*>(context);
    if (state->outstanding == 0) {
        ++state->invalidFrees;
        return;
    }
    --state->outstanding;
    ++state->freeCalls;
    std::free(pointer);
}

void p5FaultAllocatorMake(P5FaultAllocator* state, GxAllocator* allocator) {
    *allocator = {state, p5FaultAllocate, p5FaultFree,
                  8u * 1024u * 1024u, alignof(std::max_align_t)};
}

void p5FaultAllocatorFailAfter(P5FaultAllocator* state, size_t count) {
    state->failAtSuccessCount = state->successfulAllocations + count;
    state->failureEnabled = true;
}

std::string databasePath();
void removeDatabaseFiles(const std::string& path);

class MemoryFile : public IDatabaseFile {
public:
    explicit MemoryFile(const std::vector<uint8_t>& initial = std::vector<uint8_t>())
        : bytes(initial), _open(false), _readOnly(false), _path("p5-memory") {}

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override {
        _path = path;
        _readOnly = mode == FileOpenMode::ReadOnly;
        _open = true;
        if (truncateExisting) bytes.clear();
        return DbResult::ok();
    }
    DbResult close() override { _open = false; return DbResult::ok(); }
    bool isOpen() const override { return _open; }
    bool isReadOnly() const override { return _readOnly; }
    DbResult getSize(uint64_t& outSize) override {
        if (!_open) return DbResult::error(DbStatus::NotOpen, "closed");
        outSize = bytes.size();
        return DbResult::ok();
    }
    bool readAt(uint64_t offset, void* buffer, size_t length) override {
        if (!_open || offset > bytes.size() || length > bytes.size() - offset ||
            (length != 0 && !buffer)) return false;
        if (length != 0) {
            std::memcpy(buffer, bytes.data() + static_cast<size_t>(offset), length);
        }
        return true;
    }
    bool writeAt(uint64_t offset, const void* buffer, size_t length) override {
        if (!_open || _readOnly || offset > static_cast<uint64_t>(-1) - length ||
            offset + length > static_cast<uint64_t>(static_cast<size_t>(-1)) ||
            (length != 0 && !buffer)) return false;
        const size_t end = static_cast<size_t>(offset + length);
        if (end > bytes.size()) bytes.resize(end, 0);
        if (length != 0) {
            std::memcpy(bytes.data() + static_cast<size_t>(offset), buffer, length);
        }
        return true;
    }
    bool truncateTo(uint64_t size) override {
        if (!_open || _readOnly ||
            size > static_cast<uint64_t>(static_cast<size_t>(-1))) return false;
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

DatabasePage makePage(uint64_t pageId, uint8_t seed) {
    DatabasePage page;
    page.pageId = pageId;
    page.type = PageType::Data;
    page.flags = static_cast<uint16_t>(seed * 5u);
    page.payloadSize = 37;
    page.generation = static_cast<uint32_t>(seed) + 9;
    page.payload.resize(page.payloadSize);
    for (size_t i = 0; i < page.payload.size(); ++i) {
        page.payload[i] = static_cast<uint8_t>(seed + i * 11u);
    }
    return page;
}

std::vector<uint8_t> pageImage(const DatabasePage& page) {
    std::vector<uint8_t> image;
    CHECK(page.serialize(image, kPageSize).isOk());
    return image;
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

void testCollectionLifetimeAndFailureSemantics() {
    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);

    const std::vector<uint8_t> a = pageImage(makePage(2, 0x15));
    const std::vector<uint8_t> b = pageImage(makePage(3, 0x26));
    const std::vector<uint8_t> a2 = pageImage(makePage(2, 0x37));
    GxRecoveredPageCollection pages = {};
    CHECK(gxRecoveredPagesInit(&pages, &allocator, kPageSize, 4,
                               4 * kPageSize) == GxStatus::Ok);
    CHECK(gxRecoveredPagesCount(&pages) == 0);
    CHECK(gxRecoveredPagesBytes(&pages) == 0);

    gxArenaFailAfter(&arena, 0);
    CHECK(gxRecoveredPagesRetain(&pages, 2, a.data(), a.size()) ==
          GxStatus::OutOfMemory);
    CHECK(gxRecoveredPagesCount(&pages) == 0);
    CHECK(arena.outstanding == 0);
    gxArenaDisableFailure(&arena);

    CHECK(gxRecoveredPagesRetain(&pages, 2, a.data(), a.size()) == GxStatus::Ok);
    CHECK(gxRecoveredPagesRetain(&pages, 3, b.data(), b.size()) == GxStatus::Ok);
    CHECK(gxRecoveredPagesCount(&pages) == 2);
    CHECK(gxRecoveredPagesBytes(&pages) == 2 * kPageSize);
    const GxRecoveredPage* prior = pageAt(pages, 0);
    CHECK(prior && prior->pageId == 2);
    CHECK(prior && std::memcmp(gxRecoveredPageImage(prior), a.data(), a.size()) == 0);

    // A failed duplicate replacement leaves the old image and list intact.
    gxArenaFailAfter(&arena, 0);
    CHECK(gxRecoveredPagesRetain(&pages, 2, a2.data(), a2.size()) ==
          GxStatus::OutOfMemory);
    CHECK(gxRecoveredPagesCount(&pages) == 2);
    CHECK(pageAt(pages, 0) == prior);
    CHECK(prior && std::memcmp(gxRecoveredPageImage(prior), a.data(), a.size()) == 0);

    gxArenaDisableFailure(&arena);
    CHECK(gxRecoveredPagesRetain(&pages, 2, a2.data(), a2.size()) == GxStatus::Ok);
    CHECK(gxRecoveredPagesCount(&pages) == 2);
    CHECK(pageAt(pages, 0) && pageAt(pages, 0)->pageId == 3);
    CHECK(pageAt(pages, 1) && pageAt(pages, 1)->pageId == 2);
    CHECK(pageAt(pages, 1) &&
          std::memcmp(gxRecoveredPageImage(pageAt(pages, 1)), a2.data(), a2.size()) == 0);

    const size_t attemptsBeforeInvalid = arena.allocationAttempts;
    CHECK(gxRecoveredPagesRetain(&pages, 0, a.data(), a.size()) ==
          GxStatus::InvalidArgument);
    CHECK(gxRecoveredPagesRetain(&pages, kMaxPageCount, a.data(), a.size()) ==
          GxStatus::InvalidArgument);
    std::vector<uint8_t> corrupt = a;
    corrupt[40] ^= 0x01;
    CHECK(gxRecoveredPagesRetain(&pages, 2, corrupt.data(), corrupt.size()) ==
          GxStatus::CorruptData);
    CHECK(arena.allocationAttempts == attemptsBeforeInvalid);
    CHECK(gxRecoveredPagesCount(&pages) == 2);

    const std::vector<uint8_t> c = pageImage(makePage(4, 0x48));
    CHECK(gxRecoveredPagesRetain(&pages, 4, c.data(), c.size()) == GxStatus::Ok);
    const std::vector<uint8_t> d = pageImage(makePage(5, 0x59));
    const std::vector<uint8_t> e = pageImage(makePage(6, 0x6A));
    gxArenaFailAfter(&arena, 0);
    CHECK(gxRecoveredPagesRetain(&pages, 5, d.data(), d.size()) ==
          GxStatus::OutOfMemory);
    CHECK(gxRecoveredPagesCount(&pages) == 3);
    CHECK(pageAt(pages, 2) && pageAt(pages, 2)->pageId == 4);
    gxArenaDisableFailure(&arena);

    CHECK(gxRecoveredPagesRetain(&pages, 5, d.data(), d.size()) == GxStatus::Ok);
    CHECK(gxRecoveredPagesCount(&pages) == 4);
    CHECK(gxRecoveredPagesRetain(&pages, 6, e.data(), e.size()) ==
          GxStatus::ResourceLimit);

    gxRecoveredPagesDestroy(&pages);
    CHECK(gxRecoveredPagesCount(&pages) == 0);
    CHECK(!pages.initialized);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
    CHECK(gxRecoveredPagesInit(&pages, &allocator, kPageSize, 1,
                               kPageSize - 1) == GxStatus::Ok);
    const std::vector<uint8_t> pageSeven = pageImage(makePage(7, 0x71));
    CHECK(gxRecoveredPagesRetain(&pages, 7, pageSeven.data(), pageSeven.size()) ==
          GxStatus::ResourceLimit);
    CHECK(gxRecoveredPagesCount(&pages) == 0);
    gxRecoveredPagesDestroy(&pages);
    CHECK(arena.outstanding == 0);
}

void testCollectionMaximumTransactionSize() {
    GxAllocator allocator = {};
    CHECK(gxHostedAllocatorInit(&allocator, kPageSize + 64) == GxStatus::Ok);
    GxRecoveredPageCollection pages = {};
    CHECK(gxRecoveredPagesInit(&pages, &allocator, kPageSize,
                               kMaxTransactionPages,
                               static_cast<size_t>(kMaxTransactionPages) *
                                   kPageSize) == GxStatus::Ok);
    bool retainedAll = true;
    const std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < kMaxTransactionPages; ++i) {
        DatabasePage page = makePage(i + 2, static_cast<uint8_t>(i));
        std::vector<uint8_t> image;
        if (!page.serialize(image, kPageSize).isOk() ||
            gxRecoveredPagesRetain(&pages, page.pageId, image.data(), image.size()) !=
                GxStatus::Ok) {
            retainedAll = false;
            break;
        }
    }
    const std::chrono::steady_clock::duration elapsed =
        std::chrono::steady_clock::now() - started;
    const double milliseconds =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            elapsed).count();
    CHECK(retainedAll);
    CHECK(gxRecoveredPagesCount(&pages) == kMaxTransactionPages);
    CHECK(gxRecoveredPagesBytes(&pages) ==
          static_cast<size_t>(kMaxTransactionPages) * kPageSize);
    const GxRecoveredPage* last = pageAt(pages, kMaxTransactionPages - 1);
    CHECK(last && last->pageId == kMaxTransactionPages + 1);
    std::fprintf(stderr, "P5 max-size collection: %llu pages in %.2f ms\n",
                 static_cast<unsigned long long>(gxRecoveredPagesCount(&pages)),
                 milliseconds);
    gxRecoveredPagesDestroy(&pages);
}

std::vector<uint8_t> buildWal(size_t transactionCount, size_t pagesPerTransaction) {
    MemoryFile* raw = new MemoryFile();
    CHECK(raw->open("memory.gxwal", FileOpenMode::Create, true).isOk());
    std::unique_ptr<IDatabaseFile> file(raw);
    WriteAheadLog writer;
    CHECK(writer.open(std::move(file), kDatabaseId, kPageSize, false).isOk());
    uint64_t pageId = 2;
    for (size_t txIndex = 0; txIndex < transactionCount; ++txIndex) {
        const uint64_t txId = 100 + txIndex;
        CHECK(writer.appendBegin(txId).isOk());
        for (size_t pageIndex = 0; pageIndex < pagesPerTransaction; ++pageIndex) {
            DatabasePage page = makePage(pageId++,
                static_cast<uint8_t>(17 + txIndex * 7 + pageIndex));
            CHECK(writer.appendPageImage(txId, page).isOk());
        }
        if (txIndex == 0 && pagesPerTransaction != 0) {
            DatabasePage duplicate = makePage(2, 0xE3);
            CHECK(writer.appendPageImage(txId, duplicate).isOk());
        }
        CHECK(writer.appendCommit(txId).isOk());
    }
    const std::vector<uint8_t> bytes = raw->bytes;
    CHECK(writer.close().isOk());
    return bytes;
}

std::unique_ptr<WriteAheadLog> openReader(const std::vector<uint8_t>& bytes,
                                          const GxAllocator* allocator = nullptr) {
    std::unique_ptr<IDatabaseFile> file(new MemoryFile(bytes));
    CHECK(file->open("read.gxwal", FileOpenMode::ReadOnly, false).isOk());
    std::unique_ptr<WriteAheadLog> reader;
    if (allocator) reader.reset(new WriteAheadLog(*allocator));
    else reader.reset(new WriteAheadLog());
    CHECK(reader->open(std::move(file), kDatabaseId, kPageSize, true).isOk());
    return reader;
}

size_t lastPageImageOffset(const std::vector<uint8_t>& bytes) {
    size_t result = bytes.size();
    size_t offset = kWalHeaderSize;
    while (offset + kWalRecordPrefixSize <= bytes.size()) {
        const uint32_t payloadSize = loadLe32(bytes.data() + offset + 4);
        if (bytes[offset] == kWalRecordPageImage) result = offset;
        if (payloadSize > bytes.size() - offset - kWalRecordPrefixSize) break;
        offset += kWalRecordPrefixSize + payloadSize;
    }
    return offset == bytes.size() ? result : bytes.size();
}

void testScannerScratchReuseCommitSelectionAndRetry() {
    const std::vector<uint8_t> bytes = buildWal(2, 2);
    GxArena arena;
    GxAllocator allocator = {};
    gxArenaInit(&arena, sizeof(arena.bytes));
    gxArenaMakeAllocator(&arena, &allocator);
    std::vector<WalTransaction> transactions;
    WalState state = WalState::Clean;
    std::string message;
    std::unique_ptr<WriteAheadLog> reader = openReader(bytes, &allocator);

    gxArenaFailAfter(&arena, 0);
    CHECK(reader->scan(state, transactions, message).status() == DbStatus::NoSpace);
    CHECK(transactions.empty());
    CHECK(arena.outstanding == 0);

    // Scanner scratch, then three page nodes, then the fourth page node fails.
    gxArenaFailAfter(&arena, 4);
    CHECK(reader->scan(state, transactions, message).status() == DbStatus::NoSpace);
    CHECK(transactions.empty());
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);

    gxArenaDisableFailure(&arena);
    CHECK(reader->scan(state, transactions, message).isOk());
    CHECK(state == WalState::Committed);
    CHECK(transactions.size() == 2);
    CHECK(transactions.size() == 2 &&
          gxRecoveredPagesCount(&transactions[0].pages) == 2);
    CHECK(transactions.size() == 2 &&
          gxRecoveredPagesCount(&transactions[1].pages) == 2);
    const std::vector<uint8_t> expectedFirst = pageImage(makePage(3, 18));
    const GxRecoveredPage* firstPage = transactions.empty()
        ? nullptr : gxRecoveredPagesFirst(&transactions[0].pages);
    CHECK(firstPage && firstPage->pageId == 3);
    CHECK(firstPage && std::memcmp(gxRecoveredPageImage(firstPage),
                                   expectedFirst.data(), expectedFirst.size()) == 0);
    const std::vector<uint8_t> expectedReplacement = pageImage(makePage(2, 0xE3));
    const GxRecoveredPage* replacement = gxRecoveredPageNext(firstPage);
    CHECK(replacement && replacement->pageId == 2);
    CHECK(replacement && std::memcmp(gxRecoveredPageImage(replacement),
                                     expectedReplacement.data(),
                                     expectedReplacement.size()) == 0);

    // Closing the WAL object destroys its scanner/file state. Retained images
    // still have their own allocator-owned bytes.
    CHECK(reader->close().isOk());
    reader.reset();
    CHECK(firstPage && std::memcmp(gxRecoveredPageImage(firstPage),
                                   expectedFirst.data(), expectedFirst.size()) == 0);

    std::vector<uint8_t> incomplete = bytes;
    incomplete.resize(incomplete.size() - kWalRecordPrefixSize);
    std::unique_ptr<WriteAheadLog> incompleteReader = openReader(incomplete, &allocator);
    CHECK(incompleteReader->scan(state, transactions, message).isOk());
    CHECK(state == WalState::Committed); // tx 100 remains committed; tx 101 is incomplete.
    CHECK(transactions.size() == 1);
    incompleteReader.reset();

    std::vector<uint8_t> onlyIncomplete = buildWal(1, 1);
    onlyIncomplete.resize(onlyIncomplete.size() - kWalRecordPrefixSize);
    std::unique_ptr<WriteAheadLog> uncommittedReader = openReader(onlyIncomplete, &allocator);
    CHECK(uncommittedReader->scan(state, transactions, message).isOk());
    CHECK(state == WalState::Incomplete);
    CHECK(transactions.empty());
    uncommittedReader.reset();

    std::vector<uint8_t> corrupt = bytes;
    const size_t pageRecord = lastPageImageOffset(corrupt);
    CHECK(pageRecord < corrupt.size());
    if (pageRecord < corrupt.size()) {
        corrupt[pageRecord + kWalRecordPrefixSize + wal_page_offset::PageBytes + 50] ^= 0x40;
    }
    std::unique_ptr<WriteAheadLog> corruptReader = openReader(corrupt, &allocator);
    CHECK(corruptReader->scan(state, transactions, message).status() == DbStatus::WalCorrupt);
    CHECK(state == WalState::Corrupt);
    CHECK(transactions.empty());
    corruptReader.reset();

    transactions.clear();
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
}

void testTransactionPageImageLimit() {
    const std::vector<uint8_t> tooMany =
        buildWal(1, static_cast<size_t>(kMaxTransactionPages) + 1);
    std::unique_ptr<WriteAheadLog> reader = openReader(tooMany);
    WalState state = WalState::Clean;
    std::vector<WalTransaction> transactions;
    std::string message;
    const DbResult result = reader->scan(state, transactions, message);
    CHECK(result.status() == DbStatus::TransactionTooLarge);
    CHECK(state != WalState::Corrupt);
    CHECK(transactions.empty());
}

void testTransactionIdUpperBound() {
    const std::string path = databasePath();
    removeDatabaseFiles(path);
    HostFileSystem fileSystem;
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = kPageSize;
    createOptions.generateDatabaseId = false;
    std::memcpy(createOptions.databaseId, kDatabaseId, sizeof(kDatabaseId));
    std::unique_ptr<Database> database;
    CHECK(Database::create(path, createOptions, database, &fileSystem).isOk());
    if (!database) return;
    CHECK(database->close().isOk());
    database.reset();

    std::unique_ptr<IDatabaseFile> file = fileSystem.createFile();
    CHECK(file != nullptr);
    if (!file) return;
    CHECK(file->open(walPathFor(path), FileOpenMode::Create, true).isOk());
    WriteAheadLog writer;
    CHECK(writer.open(std::move(file), kDatabaseId, kPageSize, false).isOk());
    CHECK(writer.appendBegin(UINT64_MAX).isOk());
    CHECK(writer.appendCommit(UINT64_MAX).isOk());
    CHECK(writer.sync().isOk());
    CHECK(writer.close().isOk());

    CHECK(Database::open(path, DatabaseOpenOptions(), database, &fileSystem).isOk());
    if (database) {
        std::unique_ptr<Transaction> transaction;
        CHECK(database->beginTransaction(transaction).status() ==
              DbStatus::TransactionTooLarge);
        CHECK(!transaction);
        CHECK(database->close().isOk());
        database.reset();
    }
    removeDatabaseFiles(path);
}

std::string testDirectory() {
    const char* base = std::getenv("TEMP");
    if (!base) base = std::getenv("TMP");
    if (!base) base = ".";
    const std::string directory = std::string(base) + "/gxos_sql_ndb1r4p5_tests";
#if defined(_WIN32)
    _mkdir(directory.c_str());
#else
    mkdir(directory.c_str(), 0700);
#endif
    return directory;
}

std::string databasePath() {
    static unsigned id = 0;
    return testDirectory() + "/recovery_" + std::to_string(id++) + ".gxdb";
}

void removeDatabaseFiles(const std::string& path) {
    std::remove(walPathFor(path).c_str());
    std::remove(path.c_str());
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    return std::vector<uint8_t>(chars.begin(), chars.end());
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    return output.good();
}

bool run(SqlEngine& engine, const std::string& sql) {
    const SqlExecutionResult result = engine.execute(sql);
    if (!result.ok) {
        std::fprintf(stderr, "SQL failed: %s\n", result.error.describe().c_str());
    }
    return result.ok;
}

uint64_t countPageImageRecords(const std::vector<uint8_t>& bytes) {
    uint64_t count = 0;
    size_t offset = kWalHeaderSize;
    while (offset + kWalRecordPrefixSize <= bytes.size()) {
        const uint32_t payloadLength = loadLe32(bytes.data() + offset + 4);
        if (bytes[offset] == kWalRecordPageImage) ++count;
        if (payloadLength > bytes.size() - offset - kWalRecordPrefixSize) return 0;
        offset += kWalRecordPrefixSize + payloadLength;
    }
    return offset == bytes.size() ? count : 0;
}

bool removePageImageAtOrAbove(std::vector<uint8_t>& bytes, uint64_t minimumPageId) {
    size_t offset = kWalHeaderSize;
    while (offset + kWalRecordPrefixSize <= bytes.size()) {
        const uint32_t payloadLength = loadLe32(bytes.data() + offset + 4);
        if (payloadLength > bytes.size() - offset - kWalRecordPrefixSize) {
            return false;
        }
        const size_t recordSize = kWalRecordPrefixSize + payloadLength;
        if (bytes[offset] == kWalRecordPageImage &&
            payloadLength >= wal_page_offset::PageBytes) {
            const uint64_t pageId = loadLe64(
                bytes.data() + offset + kWalRecordPrefixSize +
                wal_page_offset::PageId);
            if (pageId >= minimumPageId) {
                bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + recordSize));
                return true;
            }
        }
        offset += recordSize;
    }
    return false;
}

bool checkRelationalState(Database& db, uint64_t expectedInsertedRows,
                          const std::string& payload) {
    std::string integrityMessage;
    CHECK(db.validateIntegrity(integrityMessage).isOk());
    std::vector<TableInfo> tables;
    CHECK(db.listTables(tables).isOk());
    bool childHasMultiplePages = false;
    for (size_t i = 0; i < tables.size(); ++i) {
        if (tables[i].name == "Child" && tables[i].heapPageCount >= 2) {
            childHasMultiplePages = true;
        }
    }
    CHECK(childHasMultiplePages);

    std::vector<IndexInfo> indexes;
    CHECK(db.listIndexes(indexes).isOk());
    CHECK(indexes.size() >= 4);
    uint32_t childPrimaryKeyIndex = 0;
    bool foundSecondary = false;
    for (size_t i = 0; i < indexes.size(); ++i) {
        IndexValidation validation;
        CHECK(db.validateIndex(indexes[i].indexId, validation).isOk());
        CHECK(validation.ok);
        if (indexes[i].tableName == "Child" && indexes[i].kind == IndexKind::PrimaryKey) {
            childPrimaryKeyIndex = indexes[i].indexId;
        }
        if (indexes[i].name == "IX_Child_State") foundSecondary = true;
    }
    CHECK(foundSecondary);
    CHECK(childPrimaryKeyIndex != 0);

    std::vector<ForeignKeyInfo> foreignKeys;
    CHECK(db.listForeignKeys(foreignKeys).isOk());
    CHECK(foreignKeys.size() == 1);

    SqlEngine engine(db);
    SqlExecutionResult selected = engine.execute(
        "SELECT Id, ParentId, State, Payload FROM Child WHERE Id >= 1000 ORDER BY Id;");
    CHECK(selected.ok);
    CHECK(selected.statements.size() == 1);
    if (selected.ok && selected.statements.size() == 1) {
        const SqlResultSet& rows = selected.statements[0].resultSet;
        CHECK(rows.rowCount() == expectedInsertedRows);
        for (size_t i = 0; i < rows.rowCount(); ++i) {
            CHECK(rows.value(i, 0).int64Value() == 1000 + static_cast<int64_t>(i));
            CHECK(rows.value(i, 1).int64Value() ==
                  static_cast<int64_t>((i % 4) + 1));
            CHECK(rows.value(i, 2).textValue() == "recovered");
            CHECK(rows.value(i, 3).textValue() == payload);
        }
    }

    std::vector<uint8_t> primaryKey;
    CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(1000), primaryKey));
    std::vector<RowLocator> locators;
    CHECK(childPrimaryKeyIndex != 0 &&
          db.indexLookup(childPrimaryKeyIndex, primaryKey, locators).isOk());
    CHECK(locators.size() == 1);
    std::unique_ptr<Table> child;
    CHECK(db.openTable("Child", child).isOk());
    if (child && locators.size() == 1) {
        std::vector<DbValue> row;
        CHECK(child->fetchRow(locators[0], row).isOk());
        CHECK(row.size() == 4);
        if (row.size() == 4) {
            CHECK(row[0].int64Value() == 1000);
            CHECK(row[1].int64Value() == 1);
            CHECK(row[2].textValue() == "recovered");
            CHECK(row[3].textValue() == payload);
        }
    }
    return childHasMultiplePages && indexes.size() >= 4 &&
           childPrimaryKeyIndex != 0 && foreignKeys.size() == 1;
}

void testSql9CommittedRecoveryRetryAndReadOnly() {
    const std::string path = databasePath();
    removeDatabaseFiles(path);
    const std::string payload(86, 'q');
    const uint64_t insertedRows = 24;
    HostFileSystem fileSystem;
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = kPageSize;
    std::unique_ptr<Database> database;
    CHECK(Database::create(path, createOptions, database, &fileSystem).isOk());
    if (!database) return;

    {
        SqlEngine engine(*database);
        CHECK(run(engine,
                  "CREATE TABLE Parent (Id INT64 PRIMARY KEY, Code TEXT NOT NULL UNIQUE);"));
        CHECK(run(engine, "INSERT INTO Parent VALUES (1, 'p1');"));
        CHECK(run(engine, "INSERT INTO Parent VALUES (2, 'p2');"));
        CHECK(run(engine, "INSERT INTO Parent VALUES (3, 'p3');"));
        CHECK(run(engine, "INSERT INTO Parent VALUES (4, 'p4');"));
        CHECK(run(engine,
                  "CREATE TABLE Child (Id INT64 PRIMARY KEY, ParentId INT64 NOT NULL, "
                  "State TEXT NOT NULL, Payload TEXT NOT NULL, "
                  "FOREIGN KEY (ParentId) REFERENCES Parent (Id));"));
        CHECK(run(engine, "CREATE INDEX IX_Child_State ON Child (State);"));
        CHECK(run(engine, "INSERT INTO Child VALUES (1, 1, 'seed', 'base');"));
    }
    CHECK(database->wal().checkpoint().isOk());
    CHECK(database->close().isOk());
    database.reset();
    const std::vector<uint8_t> baseBytes = readBytes(path);
    CHECK(!baseBytes.empty() && baseBytes.size() % kPageSize == 0);

    CHECK(Database::open(path, DatabaseOpenOptions(), database, &fileSystem).isOk());
    if (!database) return;
    CHECK(database->wal().checkpoint().isOk());
    {
        SqlEngine engine(*database);
        CHECK(run(engine, "BEGIN;"));
        std::string insertScript;
        for (uint64_t i = 0; i < insertedRows; ++i) {
            const uint64_t id = 1000 + i;
            const uint64_t parentId = (i % 4) + 1;
            insertScript += "INSERT INTO Child VALUES (" + std::to_string(id) + ", " +
                            std::to_string(parentId) + ", 'recovered', '" + payload + "');";
        }
        CHECK(run(engine, insertScript));
        CHECK(run(engine, "COMMIT;"));
    }
    const std::vector<uint8_t> committedWal = readBytes(walPathFor(path));
    const uint64_t pageImageRecords = countPageImageRecords(committedWal);
    CHECK(pageImageRecords >= 4);
    CHECK(database->close().isOk());
    database.reset();

    // Keep a partial final page so only committed WAL images can authorize
    // reconstruction of the appended database extent.
    std::vector<uint8_t> partialDatabase = baseBytes;
    for (uint8_t i = 0; i < 19; ++i) partialDatabase.push_back(0xA0 + i);
    CHECK(writeBytes(path, partialDatabase));

    // Incomplete transaction records are discarded and do not expose rows.
    std::vector<uint8_t> incompleteWal = committedWal;
    if (incompleteWal.size() >= kWalRecordPrefixSize) {
        incompleteWal.resize(incompleteWal.size() - kWalRecordPrefixSize);
    }
    CHECK(writeBytes(walPathFor(path), incompleteWal));
    CHECK(writeBytes(path, baseBytes));
    CHECK(Database::open(path, DatabaseOpenOptions(), database, &fileSystem).isOk());
    if (database) {
        SqlEngine engine(*database);
        const SqlExecutionResult count = engine.execute("SELECT COUNT(*) FROM Child;");
        CHECK(count.ok && count.statements.size() == 1);
        if (count.ok && count.statements.size() == 1) {
            CHECK(count.statements[0].resultSet.value(0, 0).int64Value() == 1);
        }
        CHECK(database->close().isOk());
        database.reset();
    }

    // A corrupt later PAGE_IMAGE does not cause earlier committed data to be
    // replayed or the WAL/base bytes to be rewritten.
    std::vector<uint8_t> corruptWal = committedWal;
    const size_t lastImage = lastPageImageOffset(corruptWal);
    CHECK(lastImage < corruptWal.size());
    if (lastImage < corruptWal.size()) {
        corruptWal[lastImage + kWalRecordPrefixSize + wal_page_offset::PageBytes + 60] ^= 0x20;
    }
    CHECK(writeBytes(path, baseBytes));
    CHECK(writeBytes(walPathFor(path), corruptWal));
    const DbResult corruptOpen = Database::open(path, DatabaseOpenOptions(),
                                                database, &fileSystem);
    CHECK(corruptOpen.status() == DbStatus::WalCorrupt);
    CHECK(!database);
    CHECK(readBytes(path) == baseBytes);
    CHECK(readBytes(walPathFor(path)) == corruptWal);

    // Read-only recovery must preserve both files byte for byte.
    CHECK(writeBytes(path, partialDatabase));
    CHECK(writeBytes(walPathFor(path), committedWal));
    const std::vector<uint8_t> readOnlyDatabase = readBytes(path);
    const std::vector<uint8_t> readOnlyWal = readBytes(walPathFor(path));
    DatabaseOpenOptions readOnlyOptions;
    readOnlyOptions.readOnly = true;
    const DbResult readOnlyResult = Database::open(path, readOnlyOptions,
                                                   database, &fileSystem);
    CHECK(readOnlyResult.status() == DbStatus::RecoveryRequired);
    CHECK(!database);
    CHECK(readBytes(path) == readOnlyDatabase);
    CHECK(readBytes(walPathFor(path)) == readOnlyWal);

    // Read-only opens preserve incomplete and corrupt recovery evidence too.
    CHECK(writeBytes(path, partialDatabase));
    CHECK(writeBytes(walPathFor(path), incompleteWal));
    const std::vector<uint8_t> readOnlyIncompleteDatabase = readBytes(path);
    const std::vector<uint8_t> readOnlyIncompleteWal = readBytes(walPathFor(path));
    const DbResult readOnlyIncomplete = Database::open(
        path, readOnlyOptions, database, &fileSystem);
    CHECK(readOnlyIncomplete.status() == DbStatus::RecoveryRequired);
    CHECK(!database);
    CHECK(readBytes(path) == readOnlyIncompleteDatabase);
    CHECK(readBytes(walPathFor(path)) == readOnlyIncompleteWal);

    CHECK(writeBytes(path, partialDatabase));
    CHECK(writeBytes(walPathFor(path), corruptWal));
    const std::vector<uint8_t> readOnlyCorruptDatabase = readBytes(path);
    const std::vector<uint8_t> readOnlyCorruptWal = readBytes(walPathFor(path));
    const DbResult readOnlyCorrupt = Database::open(
        path, readOnlyOptions, database, &fileSystem);
    CHECK(readOnlyCorrupt.status() == DbStatus::WalCorrupt);
    CHECK(!database);
    CHECK(readBytes(path) == readOnlyCorruptDatabase);
    CHECK(readBytes(walPathFor(path)) == readOnlyCorruptWal);

    // A valid committed transaction with one missing page image cannot
    // authorize reconstruction of the partial database extent.
    std::vector<uint8_t> missingImageWal = committedWal;
    CHECK(removePageImageAtOrAbove(
        missingImageWal, static_cast<uint64_t>(baseBytes.size() / kPageSize)));
    CHECK(countPageImageRecords(missingImageWal) + 1 == pageImageRecords);
    CHECK(writeBytes(path, partialDatabase));
    CHECK(writeBytes(walPathFor(path), missingImageWal));
    const DbResult missingImage = Database::open(
        path, DatabaseOpenOptions(), database, &fileSystem);
    CHECK(missingImage.status() == DbStatus::RecoveryRequired);
    CHECK(!database);
    CHECK(readBytes(path) == partialDatabase);
    CHECK(readBytes(walPathFor(path)) == missingImageWal);

    // Read-only allocation failure while inspecting committed WAL requirements
    // must preserve both files and report NoSpace distinctly.
    CHECK(writeBytes(path, partialDatabase));
    CHECK(writeBytes(walPathFor(path), committedWal));
    P5FaultAllocator readOnlyArena = {};
    GxAllocator readOnlyAllocator = {};
    p5FaultAllocatorMake(&readOnlyArena, &readOnlyAllocator);
    p5FaultAllocatorFailAfter(&readOnlyArena, 4);
    DatabaseOpenOptions readOnlyFaultOptions = readOnlyOptions;
    readOnlyFaultOptions.walRecordAllocator = &readOnlyAllocator;
    const DbResult readOnlyAllocationFailure = Database::open(
        path, readOnlyFaultOptions, database, &fileSystem);
    CHECK(readOnlyAllocationFailure.status() == DbStatus::NoSpace);
    CHECK(!database);
    CHECK(readOnlyArena.outstanding == 0);
    CHECK(readOnlyArena.invalidFrees == 0);
    CHECK(readBytes(path) == partialDatabase);
    CHECK(readBytes(walPathFor(path)) == committedWal);

    // Determine the exact allocator request count through successful recovery,
    // then fail its final preparation request (the tail-coverage bitmap).
    CHECK(writeBytes(path, partialDatabase));
    CHECK(writeBytes(walPathFor(path), committedWal));
    P5FaultAllocator countArena = {};
    GxAllocator countAllocator = {};
    p5FaultAllocatorMake(&countArena, &countAllocator);
    DatabaseOpenOptions countOptions;
    countOptions.walRecordAllocator = &countAllocator;
    const DbResult countedRecovery = Database::open(
        path, countOptions, database, &fileSystem);
    CHECK(countedRecovery.isOk());
    const size_t recoveryAllocationCount = countArena.successfulAllocations;
    CHECK(recoveryAllocationCount > 0);
    CHECK(countArena.outstanding == 0);
    if (database) {
        CHECK(database->close().isOk());
        database.reset();
    }
    CHECK(writeBytes(path, partialDatabase));
    CHECK(writeBytes(walPathFor(path), committedWal));
    P5FaultAllocator coverageArena = {};
    GxAllocator coverageAllocator = {};
    p5FaultAllocatorMake(&coverageArena, &coverageAllocator);
    DatabaseOpenOptions coverageOptions;
    coverageOptions.walRecordAllocator = &coverageAllocator;
    if (recoveryAllocationCount > 0) {
        p5FaultAllocatorFailAfter(&coverageArena, recoveryAllocationCount - 1);
    }
    const DbResult coverageAllocationFailure = Database::open(
        path, coverageOptions, database, &fileSystem);
    CHECK(coverageAllocationFailure.status() == DbStatus::NoSpace);
    CHECK(!database);
    CHECK(coverageArena.successfulAllocations + 1 == recoveryAllocationCount);
    CHECK(coverageArena.outstanding == 0);
    CHECK(coverageArena.invalidFrees == 0);
    CHECK(readBytes(path) == partialDatabase);
    CHECK(readBytes(walPathFor(path)) == committedWal);

    // Fail after several page images have been retained. The failed open must
    // release its collection and leave durable recovery evidence untouched.
    P5FaultAllocator arena = {};
    GxAllocator allocator = {};
    p5FaultAllocatorMake(&arena, &allocator);
    DatabaseOpenOptions faultOptions;
    faultOptions.walRecordAllocator = &allocator;
    p5FaultAllocatorFailAfter(&arena, 4);
    const DbResult failedRecovery = Database::open(path, faultOptions,
                                                   database, &fileSystem);
    CHECK(failedRecovery.status() == DbStatus::NoSpace);
    CHECK(!database);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(readBytes(path) == partialDatabase);
    CHECK(readBytes(walPathFor(path)) == committedWal);

    arena.failureEnabled = false;
    const DbResult retryResult = Database::open(path, faultOptions, database,
                                                &fileSystem);
    if (!retryResult.isOk()) {
        std::fprintf(stderr, "P5 retry failed: %s\n", retryResult.describe().c_str());
    }
    CHECK(retryResult.isOk());
    CHECK(database != nullptr);
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    if (database) {
        CHECK(database->diagnostics().pagesRedone >= 4);
        CHECK(checkRelationalState(*database, insertedRows, payload));
        CHECK(database->close().isOk());
        database.reset();
    }
    CHECK(readBytes(path).size() % kPageSize == 0);

    // A second open sees the checkpointed state and produces the same rows,
    // index contents, row locators, and foreign-key/catalog state.
    CHECK(Database::open(path, DatabaseOpenOptions(), database, &fileSystem).isOk());
    if (database) {
        CHECK(checkRelationalState(*database, insertedRows, payload));
        CHECK(database->close().isOk());
        database.reset();
    }
    CHECK(arena.outstanding == 0);
    CHECK(arena.invalidFrees == 0);
    CHECK(arena.successfulAllocations == arena.freeCalls);
    removeDatabaseFiles(path);
}

} // namespace

int main() {
    testCollectionLifetimeAndFailureSemantics();
    testCollectionMaximumTransactionSize();
    testScannerScratchReuseCommitSelectionAndRetry();
    testTransactionPageImageLimit();
    testTransactionIdUpperBound();
    testSql9CommittedRecoveryRetryAndReadOnly();
    std::printf("database_ndb1r4p5_test: %d checks, %d failures\n",
                gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
