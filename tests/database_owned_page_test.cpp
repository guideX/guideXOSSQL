#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

#include "database_format.h"
#include "database_io.h"
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

struct PageReadFaults {
    uint64_t targetOffset;
    uint32_t pageSize;
    bool failSizeQueryOnce;
    bool truncateReportedExtent;
    bool failReadOnce;
    bool shortReadOnce;
    size_t targetReadCalls;

    PageReadFaults()
        : targetOffset(0), pageSize(0), failSizeQueryOnce(false),
          truncateReportedExtent(false), failReadOnce(false),
          shortReadOnce(false), targetReadCalls(0) {}
};

class FaultingPageReadFile : public IDatabaseFile {
public:
    FaultingPageReadFile(std::unique_ptr<IDatabaseFile> inner,
                         PageReadFaults* faults)
        : _inner(std::move(inner)), _faults(faults) {}

    DbResult open(const std::string& path, FileOpenMode mode,
                  bool truncateExisting) override {
        return _inner->open(path, mode, truncateExisting);
    }
    DbResult close() override { return _inner->close(); }
    bool isOpen() const override { return _inner->isOpen(); }
    bool isReadOnly() const override { return _inner->isReadOnly(); }
    DbResult getSize(uint64_t& outSize) override {
        if (_faults->failSizeQueryOnce) {
            _faults->failSizeQueryOnce = false;
            return DbResult::error(DbStatus::IoError, "injected size query failure");
        }
        DbResult result = _inner->getSize(outSize);
        if (result.isOk() && _faults->truncateReportedExtent) {
            const uint64_t pageEnd =
                _faults->targetOffset + _faults->pageSize;
            if (outSize >= pageEnd) outSize = pageEnd - 1;
        }
        return result;
    }
    bool readAt(uint64_t offset, void* buffer, size_t length) override {
        if (offset == _faults->targetOffset) {
            ++_faults->targetReadCalls;
            if (_faults->failReadOnce) {
                _faults->failReadOnce = false;
                return false;
            }
            if (_faults->shortReadOnce) {
                _faults->shortReadOnce = false;
                const size_t partial = length == 0 ? 0 : length - 1;
                if (partial != 0 && !_inner->readAt(offset, buffer, partial)) {
                    return false;
                }
                return false;
            }
        }
        return _inner->readAt(offset, buffer, length);
    }
    bool writeAt(uint64_t offset, const void* buffer, size_t length) override {
        return _inner->writeAt(offset, buffer, length);
    }
    bool truncateTo(uint64_t size) override { return _inner->truncateTo(size); }
    bool flush() override { return _inner->flush(); }
    const std::string& path() const override { return _inner->path(); }

private:
    std::unique_ptr<IDatabaseFile> _inner;
    PageReadFaults* _faults;
};

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

void testDatabaseFileOwnedPageRead() {
    const std::string path = databasePath();
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;

    std::unique_ptr<IDatabaseFile> createFile =
        defaultFileSystem().createFile();
    CHECK(createFile &&
          createFile->open(path, FileOpenMode::Create, true).isOk());
    std::unique_ptr<DatabaseFile> created;
    CHECK(DatabaseFile::create(std::move(createFile), createOptions, created)
              .isOk());
    if (!created) {
        removeDatabase(path);
        return;
    }

    uint64_t pageId = 0;
    CHECK(created->allocatePage(PageType::Data, pageId).isOk());
    DatabasePage expectedPage;
    expectedPage.pageId = pageId;
    expectedPage.type = PageType::Data;
    expectedPage.flags = 0x34;
    expectedPage.generation = 0x01020304u;
    expectedPage.payload = {0x10, 0x20, 0x30, 0x40, 0x50};
    expectedPage.payloadSize =
        static_cast<uint32_t>(expectedPage.payload.size());
    std::vector<uint8_t> expectedImage;
    CHECK(expectedPage.serialize(expectedImage, createOptions.pageSize).isOk());
    const std::vector<uint8_t> fixture = referencePage(
        pageId, static_cast<uint16_t>(PageType::Data), expectedPage.flags,
        expectedPage.generation, expectedPage.payload.data(),
        expectedPage.payload.size(), createOptions.pageSize);
    CHECK(expectedImage == fixture);
    CHECK(created->writePage(expectedPage).isOk());
    CHECK(created->flush().isOk());
    CHECK(created->close().isOk());
    created.reset();

    PageReadFaults faults;
    faults.pageSize = createOptions.pageSize;
    faults.targetOffset = pageId * createOptions.pageSize;
    std::unique_ptr<IDatabaseFile> inner = defaultFileSystem().createFile();
    CHECK(inner && inner->open(path, FileOpenMode::ReadWrite, false).isOk());
    FaultingPageReadFile* faulting =
        new FaultingPageReadFile(std::move(inner), &faults);
    std::unique_ptr<IDatabaseFile> wrapped(faulting);
    DatabaseOpenOptions openOptions;
    std::unique_ptr<DatabaseFile> file;
    CHECK(DatabaseFile::open(std::move(wrapped), openOptions, file).isOk());
    if (!file) {
        removeDatabase(path);
        return;
    }
    // Open-time bootstrap validation already consumed the new owned file-read
    // path, while the selected data page remains available for fault tests.
    CHECK(file->pageSize() == createOptions.pageSize &&
          file->pageCount() == pageId + 1);
    CHECK(faults.targetReadCalls == 0);

    GxArena arenaA;
    GxArena arenaB;
    GxArena arenaInitialFailure;
    gxArenaInit(&arenaA, createOptions.pageSize);
    gxArenaInit(&arenaB, createOptions.pageSize);
    gxArenaInit(&arenaInitialFailure, createOptions.pageSize);
    GxAllocator allocatorA = {};
    GxAllocator allocatorB = {};
    GxAllocator initialFailureAllocator = {};
    gxArenaMakeAllocator(&arenaA, &allocatorA);
    gxArenaMakeAllocator(&arenaB, &allocatorB);
    gxArenaMakeAllocator(&arenaInitialFailure, &initialFailureAllocator);

    GxOwnedPage pageA;
    GxOwnedPage pageB;
    GxOwnedPage initialFailure;
    CHECK(pageA.init(&allocatorA, createOptions.pageSize) == GxStatus::Ok);
    CHECK(pageB.init(&allocatorB, createOptions.pageSize) == GxStatus::Ok);
    CHECK(initialFailure.init(&initialFailureAllocator,
                              createOptions.pageSize) == GxStatus::Ok);

    gxArenaFailAfter(&arenaInitialFailure, 0);
    CHECK(file->readOwnedPage(pageId, initialFailure).status() ==
          DbStatus::NoSpace);
    CHECK(!initialFailure.isValid() && arenaInitialFailure.outstanding == 0);
    gxArenaDisableFailure(&arenaInitialFailure);
    CHECK(file->readOwnedPage(pageId, initialFailure).isOk());
    CHECK(sameBytes(initialFailure, fixture));

    CHECK(file->readOwnedPage(pageId, pageA).isOk());
    CHECK(sameBytes(pageA, fixture) && pageA.byteCount() == 512 &&
          pageA.allocatedCapacity() == 512);
    CHECK(pageA.allocator().context == &arenaA && arenaA.outstanding == 1);
    CHECK(faults.targetReadCalls == 2);

    DatabasePage legacy;
    legacy.pageId = 99;
    legacy.type = PageType::IndexLeaf;
    legacy.payload = {0xCC, 0xDD};
    faults.failReadOnce = true;
    CHECK(file->readPage(pageId, legacy).status() == DbStatus::IoError);
    CHECK(legacy.pageId == 99 && legacy.type == PageType::IndexLeaf &&
          legacy.payload == std::vector<uint8_t>({0xCC, 0xDD}));
    faults.shortReadOnce = true;
    CHECK(file->readOwnedPage(pageId, pageA).status() == DbStatus::IoError);
    CHECK(sameBytes(pageA, fixture) && arenaA.outstanding == 1);

    const uint8_t* oldA = pageA.encodedBytes();
    gxArenaFailAfter(&arenaA, 0);
    const size_t readCallsBeforeAllocationFailure = faults.targetReadCalls;
    CHECK(file->readOwnedPage(pageId, pageA).status() == DbStatus::NoSpace);
    CHECK(file->readOwnedPage(pageId, pageA).status() == DbStatus::NoSpace);
    CHECK(pageA.encodedBytes() == oldA && sameBytes(pageA, fixture) &&
          arenaA.outstanding == 1 &&
          faults.targetReadCalls == readCallsBeforeAllocationFailure);
    gxArenaDisableFailure(&arenaA);
    CHECK(file->readOwnedPage(pageId, pageA).isOk());
    CHECK(sameBytes(pageA, fixture) && arenaA.outstanding == 1);
    oldA = pageA.encodedBytes();

    faults.failSizeQueryOnce = true;
    CHECK(file->readOwnedPage(pageId, pageA).status() == DbStatus::IoError);
    faults.truncateReportedExtent = true;
    const size_t attemptsBeforeShortExtent = arenaA.allocationAttempts;
    CHECK(file->readOwnedPage(pageId, pageA).status() == DbStatus::Truncated);
    CHECK(arenaA.allocationAttempts == attemptsBeforeShortExtent &&
          pageA.encodedBytes() == oldA && sameBytes(pageA, fixture));
    faults.truncateReportedExtent = false;

    GxArena wrongSizeArena;
    gxArenaInit(&wrongSizeArena, 1024);
    GxAllocator wrongSizeAllocator = {};
    gxArenaMakeAllocator(&wrongSizeArena, &wrongSizeAllocator);
    GxOwnedPage wrongPageSize;
    CHECK(wrongPageSize.init(&wrongSizeAllocator, 1024) == GxStatus::Ok);
    CHECK(file->readOwnedPage(pageId, wrongPageSize).status() ==
          DbStatus::InvalidArgument);
    GxOwnedPage unconfigured;
    CHECK(file->readOwnedPage(pageId, unconfigured).status() ==
          DbStatus::InvalidArgument);
    CHECK(file->readOwnedPage(0, pageA).status() == DbStatus::InvalidArgument);
    CHECK(file->readOwnedPage(pageId + 1, pageA).status() ==
          DbStatus::OutOfBounds);
    CHECK(oldA != nullptr && sameBytes(pageA, fixture));

    std::vector<uint8_t> invalidImage(createOptions.pageSize, 0);
    CHECK(gxDatabasePageEncode(
              invalidImage.data(), createOptions.pageSize, pageId + 1,
              static_cast<uint16_t>(PageType::Data), expectedPage.flags,
              expectedPage.generation, expectedPage.payload.data(),
              expectedPage.payload.size()) == GxStatus::Ok);
    CHECK(faulting->writeAt(faults.targetOffset, invalidImage.data(),
                            invalidImage.size()));
    CHECK(file->readOwnedPage(pageId, pageA).status() == DbStatus::CorruptPage);
    CHECK(pageA.encodedBytes() == oldA && sameBytes(pageA, fixture));

    CHECK(gxDatabasePageEncode(
              invalidImage.data(), createOptions.pageSize, pageId, 0xFFFFu,
              expectedPage.flags, expectedPage.generation,
              expectedPage.payload.data(), expectedPage.payload.size()) ==
          GxStatus::Ok);
    CHECK(faulting->writeAt(faults.targetOffset, invalidImage.data(),
                            invalidImage.size()));
    CHECK(file->readOwnedPage(pageId, pageA).status() == DbStatus::CorruptPage);
    CHECK(pageA.encodedBytes() == oldA && sameBytes(pageA, fixture));

    invalidImage = fixture;
    invalidImage[page_offset::PageCrc32] ^= 0x80;
    CHECK(faulting->writeAt(faults.targetOffset, invalidImage.data(),
                            invalidImage.size()));
    CHECK(file->readOwnedPage(pageId, pageA).status() == DbStatus::CorruptPage);
    CHECK(pageA.encodedBytes() == oldA && sameBytes(pageA, fixture));
    CHECK(faulting->writeAt(faults.targetOffset, fixture.data(), fixture.size()));

    void* foreignImage = nullptr;
    CHECK(gxAllocatorAllocate(&allocatorB, createOptions.pageSize, 1,
                              &foreignImage) == GxStatus::Ok);
    if (foreignImage) {
        std::memcpy(foreignImage, fixture.data(), fixture.size());
        CHECK(pageA.adoptEncodedBytes(&allocatorB,
                                      static_cast<uint8_t*>(foreignImage),
                                      fixture.size(), pageId) ==
              GxStatus::InvalidArgument);
        CHECK(pageA.encodedBytes() == oldA && sameBytes(pageA, fixture));
        gxAllocatorFree(&allocatorB, foreignImage);
    }

    // The public DatabasePage reader remains a hosted compatibility edge. It
    // decodes the checked owner and moves the complete value into the caller.
    DatabasePage compatible;
    CHECK(file->readPage(pageId, compatible).isOk());
    std::vector<uint8_t> compatibleBytes;
    CHECK(compatible.serialize(compatibleBytes, createOptions.pageSize).isOk());
    CHECK(compatibleBytes == fixture);

    CHECK(file->close().isOk());
    file.reset();
    CHECK(pageA.validate() == GxStatus::Ok && sameBytes(pageA, fixture));
    CHECK(pageA.cloneTo(&allocatorB, pageB) == GxStatus::Ok);
    CHECK(pageA.encodedBytes() != pageB.encodedBytes() &&
          pageA.allocator().context == &arenaA &&
          pageB.allocator().context == &arenaB &&
          sameBytes(pageB, fixture));
    const uint8_t* oldB = pageB.encodedBytes();
    gxArenaFailAfter(&arenaB, 0);
    CHECK(pageA.cloneTo(&allocatorB, pageB) == GxStatus::OutOfMemory);
    CHECK(pageB.encodedBytes() == oldB && sameBytes(pageB, fixture) &&
          sameBytes(pageA, fixture));
    gxArenaDisableFailure(&arenaB);

    pageA.destroy();
    CHECK(arenaA.outstanding == 0 && pageB.validate() == GxStatus::Ok &&
          sameBytes(pageB, fixture));
    pageB.destroy();
    initialFailure.destroy();
    wrongPageSize.destroy();
    unconfigured.destroy();
    CHECK(arenaA.outstanding == 0 && arenaB.outstanding == 0 &&
          arenaInitialFailure.outstanding == 0 &&
          wrongSizeArena.outstanding == 0);
    CHECK(arenaA.invalidFrees == 0 && arenaB.invalidFrees == 0 &&
          arenaInitialFailure.invalidFrees == 0 &&
          wrongSizeArena.invalidFrees == 0);
    removeDatabase(path);
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
        const size_t allocationsBeforeIndexedSelect =
            cacheArena.allocationAttempts;
        SqlExecutionResult indexed = engine.execute(
            "SELECT Id FROM PortableRows WHERE Value='before';");
        CHECK(indexed.ok && indexed.statements.size() == 1 &&
              indexed.statements[0].accessPath == "IndexLookup" &&
              indexed.statements[0].resultSet.rowCount() == 1);
        CHECK(cacheArena.allocationAttempts > allocationsBeforeIndexedSelect);

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

    // The database-level compatibility reader now materializes through its
    // checked encoded owner. Fail that actual cache-allocator request while a
    // stable cache pin is held, then verify the old output and pin survive.
    BufferSlot* readerPinned = nullptr;
    CHECK(db->buffer().pinPage(1, readerPinned).isOk() &&
          readerPinned != nullptr);
    if (readerPinned != nullptr) {
        const uint32_t readerPinCount = readerPinned->pinCount;
        const uint32_t readerManagerPins = db->buffer().pinnedCount();
        const uint8_t* const pinnedPayload =
            readerPinned->page.payload.empty()
                ? nullptr
                : readerPinned->page.payload.data();
        const uint64_t priorOutputPageId = hosted.pageId;
        const PageType priorOutputType = hosted.type;
        const uint32_t priorOutputGeneration = hosted.generation;
        const std::vector<uint8_t> priorOutputPayload = hosted.payload;
        const size_t scratchAttemptsBefore = cacheArena.allocationAttempts;
        const size_t scratchOutstandingBefore = cacheArena.outstanding;

        gxArenaFailAfter(&cacheArena, 0);
        const DbResult ownerAllocationFailure = db->readPage(1, hosted);
        gxArenaDisableFailure(&cacheArena);
        CHECK(ownerAllocationFailure.status() == DbStatus::NoSpace);
        CHECK(cacheArena.allocationAttempts == scratchAttemptsBefore + 1 &&
              cacheArena.outstanding == scratchOutstandingBefore);
        CHECK(hosted.pageId == priorOutputPageId &&
              hosted.type == priorOutputType &&
              hosted.generation == priorOutputGeneration &&
              hosted.payload == priorOutputPayload);
        CHECK(readerPinned->pageId == 1 &&
              readerPinned->page.payload.data() == pinnedPayload &&
              readerPinned->pinCount == readerPinCount &&
              db->buffer().pinnedCount() == readerManagerPins);

        CHECK(db->readPage(1, hosted).isOk());
        CHECK(hosted.serialize(hostedEncoding, db->pageSize()).isOk() &&
              hostedEncoding == expected);
        CHECK(readerPinned->page.payload.data() == pinnedPayload &&
              readerPinned->pinCount == readerPinCount &&
              db->buffer().pinnedCount() == readerManagerPins);
        CHECK(db->buffer().unpin(*readerPinned).isOk());
    }

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
    CHECK(hosted.serialize(hostedEncoding,
                           static_cast<uint32_t>(expected.size())).isOk() &&
          hostedEncoding == expected);
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

void testDatabaseReadPageByteCompatibility512() {
    const std::string path = databasePath();
    GxArena cacheArena;
    gxArenaInit(&cacheArena, 8192);
    GxAllocator cacheAllocator = {};
    gxArenaMakeAllocator(&cacheArena, &cacheAllocator);

    DatabaseCreateOptions createOptions;
    createOptions.pageSize = 512;
    createOptions.bufferCapacity = 2;
    createOptions.pageCacheAllocator = &cacheAllocator;
    std::unique_ptr<Database> db;
    CHECK(Database::create(path, createOptions, db).isOk() && db != nullptr);
    if (!db) {
        removeDatabase(path);
        return;
    }

    DatabasePage page;
    CHECK(db->readPage(1, page).isOk());
    std::vector<uint8_t> compatibilityBytes;
    CHECK(page.serialize(compatibilityBytes, db->pageSize()).isOk());

    GxArena callerArena;
    gxArenaInit(&callerArena, 1024);
    GxAllocator callerAllocator = {};
    gxArenaMakeAllocator(&callerArena, &callerAllocator);
    GxOwnedPage encoded;
    CHECK(encoded.init(&callerAllocator, db->pageSize()) == GxStatus::Ok);
    CHECK(db->readOwnedPage(1, encoded).isOk());
    CHECK(encoded.byteCount() == 512 &&
          compatibilityBytes.size() == encoded.byteCount() &&
          std::memcmp(compatibilityBytes.data(), encoded.encodedBytes(),
                      encoded.byteCount()) == 0);

    CHECK(db->close().isOk());
    db.reset();
    CHECK(page.serialize(compatibilityBytes, 512).isOk() &&
          compatibilityBytes.size() == 512);
    CHECK(cacheArena.outstanding == 0 && cacheArena.invalidFrees == 0);
    encoded.destroy();
    CHECK(callerArena.outstanding == 0 && callerArena.invalidFrees == 0);
    removeDatabase(path);
}

} // namespace

int main() {
    testOwnerContracts();
    testDatabaseFileOwnedPageRead();
    testProductionReadLifetimeAndSql9();
    testDatabaseReadPageByteCompatibility512();
    std::printf("Caller-owned page: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
