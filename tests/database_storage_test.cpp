// guideXOS SQL -- Phase SQL1
// Hosted test suite for the database storage foundation.
//
// No external test framework is used; this keeps the suite buildable with the
// same toolchain as the engine. Every case writes real .gxdb files under the
// OS temp directory so the on-disk format is exercised end to end.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "database_checksum.h"
#include "database_endian.h"
#include "database_engine.h"
#include "database_file.h"
#include "database_format.h"

using namespace gxos::db;

namespace {

int g_pass = 0;
int g_fail = 0;
int g_caseCounter = 0;

void checkTrue(bool condition, const char* what, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s\n", line, what);
    }
}

void checkStatus(const DbResult& result, DbStatus expected, const char* what, int line) {
    if (result.status() == expected) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- expected %s, got %s (%s)\n", line, what,
                    dbStatusName(expected), dbStatusName(result.status()),
                    result.message().c_str());
    }
}

void checkStatusIn(const DbResult& result, DbStatus a, DbStatus b, const char* what,
                   int line) {
    if (result.status() == a || result.status() == b) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL (line %d): %s -- expected %s or %s, got %s (%s)\n", line,
                    what, dbStatusName(a), dbStatusName(b), dbStatusName(result.status()),
                    result.message().c_str());
    }
}

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)
#define CHECK_STATUS(expr, expected) checkStatus((expr), (expected), #expr, __LINE__)
#define CHECK_STATUS_IN(expr, a, b) checkStatusIn((expr), (a), (b), #expr, __LINE__)

std::string testDir() {
    const char* base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMP");
    if (base == nullptr) base = ".";
    std::string dir = std::string(base) + "/gxos_gxdb_tests";
#if defined(_WIN32)
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0700);
#endif
    return dir;
}

std::string uniquePath(const std::string& name) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "_%d_", g_caseCounter++);
    return testDir() + "/" + name + buffer + ".gxdb";
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return data;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize(static_cast<size_t>(size));
        size_t read = std::fread(data.data(), 1, data.size(), f);
        if (read != data.size()) data.clear();
    }
    std::fclose(f);
    return data;
}

bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    size_t written = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return written == data.size();
}

uint64_t fileSizeOf(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return 0;
    return static_cast<uint64_t>(info.st_size);
}

// Copies base to work, applies a byte mutation, writes work.
void mutateCopy(const std::string& base, const std::string& work, size_t offset,
                uint8_t value) {
    std::vector<uint8_t> bytes = readFileBytes(base);
    if (offset < bytes.size()) bytes[offset] = value;
    writeFileBytes(work, bytes);
}

// Recomputes the header CRC after a header field mutation so structural
// validation (rather than the checksum) is exercised.
void fixHeaderCrc(std::vector<uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize) return;
    for (int i = 0; i < 4; ++i) bytes[header_offset::HeaderCrc32 + i] = 0;
    uint32_t crc = crc32(bytes.data(), kHeaderSize);
    storeLe32(bytes.data() + header_offset::HeaderCrc32, crc);
}

void setHeaderField64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    storeLe64(bytes.data() + offset, value);
}

std::vector<uint8_t> deterministicPayload(size_t size, uint32_t seed) {
    std::vector<uint8_t> payload(size);
    uint32_t x = seed * 2654435761u + 1u;
    for (size_t i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        payload[i] = static_cast<uint8_t>((x >> 16) & 0xFFu);
    }
    return payload;
}

// ---------------------------------------------------------------------------

void testChecksumVector() {
    std::printf("checksum\n");
    const uint8_t text[] = "123456789";
    CHECK(crc32(text, 9) == 0xCBF43926u);
    CHECK(crc32(nullptr, 0) == crc32(reinterpret_cast<const uint8_t*>(""), 0));
}

void testCreationAndReopen() {
    std::printf("creation/reopen\n");
    const std::string path = uniquePath("create");

    DatabaseCreateOptions options;
    options.pageSize = 4096;

    std::unique_ptr<DatabaseFile> db;
    DbResult result = DatabaseEngine::createDatabase(path, options, db);
    CHECK_STATUS(result, DbStatus::Ok);
    CHECK(db != nullptr);
    if (!db) return;

    const std::string id = db->diagnostics().databaseId;
    CHECK(!id.empty());
    CHECK(db->pageCount() == 2);
    CHECK(db->rootPageId() == kBootstrapPageId);
    CHECK(db->pageSize() == 4096);
    CHECK(db->header().formatMajor == kFormatMajor);
    CHECK(db->header().formatMinor == kFormatMinor);
    CHECK(db->header().creationTimeUnixNanos != 0);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    CHECK(fileSizeOf(path) == 2ull * 4096ull);

    std::unique_ptr<DatabaseFile> reopened;
    DatabaseOpenOptions openOptions;
    CHECK_STATUS(DatabaseEngine::openDatabase(path, openOptions, reopened), DbStatus::Ok);
    CHECK(reopened != nullptr);
    if (reopened) {
        CHECK(reopened->diagnostics().databaseId == id);
        CHECK(reopened->pageCount() == 2);
        CHECK(reopened->rootPageId() == kBootstrapPageId);
        CHECK_STATUS(reopened->close(), DbStatus::Ok);
    }
    std::remove(path.c_str());
}

void testCreateRefusesOverwrite() {
    std::printf("creation overwrite guard\n");
    const std::string path = uniquePath("overwrite");

    DatabaseCreateOptions options;
    std::unique_ptr<DatabaseFile> first;
    CHECK_STATUS(DatabaseEngine::createDatabase(path, options, first), DbStatus::Ok);
    if (first) first->close();

    std::unique_ptr<DatabaseFile> second;
    CHECK_STATUS(DatabaseEngine::createDatabase(path, options, second), DbStatus::AlreadyExists);
    CHECK(second == nullptr);

    std::unique_ptr<DatabaseFile> reopened;
    DatabaseOpenOptions openOptions;
    CHECK_STATUS(DatabaseEngine::openDatabase(path, openOptions, reopened), DbStatus::Ok);
    if (reopened) reopened->close();

    options.overwriteExisting = true;
    std::unique_ptr<DatabaseFile> third;
    CHECK_STATUS(DatabaseEngine::createDatabase(path, options, third), DbStatus::Ok);
    if (third) third->close();
    std::remove(path.c_str());
}

void testInvalidCreateArguments() {
    std::printf("creation invalid arguments\n");
    const std::string path = uniquePath("badparams");

    const uint32_t badSizes[] = {0u, 100u, 3000u, 4095u, 131072u};
    for (size_t i = 0; i < sizeof(badSizes) / sizeof(badSizes[0]); ++i) {
        DatabaseCreateOptions options;
        options.pageSize = badSizes[i];
        options.overwriteExisting = true;
        std::unique_ptr<DatabaseFile> db;
        CHECK_STATUS(DatabaseEngine::createDatabase(path, options, db), DbStatus::InvalidArgument);
    }

    DatabaseCreateOptions options;
    std::unique_ptr<DatabaseFile> db;
    CHECK_STATUS(DatabaseEngine::createDatabase("", options, db), DbStatus::InvalidArgument);
    std::remove(path.c_str());
}

void testSupportedPageSizes() {
    std::printf("page size boundaries\n");
    const uint32_t goodSizes[] = {512u, 4096u, 8192u, 65536u};
    for (size_t i = 0; i < sizeof(goodSizes) / sizeof(goodSizes[0]); ++i) {
        const std::string path = uniquePath("pagesize");
        DatabaseCreateOptions options;
        options.pageSize = goodSizes[i];
        std::unique_ptr<DatabaseFile> db;
        CHECK_STATUS(DatabaseEngine::createDatabase(path, options, db), DbStatus::Ok);
        if (db) {
            CHECK(db->pageSize() == goodSizes[i]);
            CHECK_STATUS(db->close(), DbStatus::Ok);
            CHECK(fileSizeOf(path) == 2ull * goodSizes[i]);
        }
        std::remove(path.c_str());
    }
}

void testPageIoPersistence() {
    std::printf("page I/O persistence\n");
    const std::string path = uniquePath("pageio");

    DatabaseCreateOptions createOptions;
    std::unique_ptr<DatabaseFile> db;
    CHECK_STATUS(DatabaseEngine::createDatabase(path, createOptions, db), DbStatus::Ok);
    if (!db) return;

    const size_t capacity = DatabasePage::payloadCapacity(db->pageSize());
    std::vector<uint64_t> allocated;
    std::vector<std::vector<uint8_t>> payloads;
    for (int i = 0; i < 6; ++i) {
        uint64_t pageId = 0;
        CHECK_STATUS(db->allocatePage(PageType::Data, pageId), DbStatus::Ok);
        CHECK(pageId == static_cast<uint64_t>(i + 2)); // pages 2..7 (0 header, 1 root)
        allocated.push_back(pageId);

        DatabasePage page;
        page.pageId = pageId;
        page.type = PageType::Data;
        page.generation = static_cast<uint32_t>(i + 1);
        page.payload = deterministicPayload(capacity / 2 + i, static_cast<uint32_t>(i));
        page.payloadSize = static_cast<uint32_t>(page.payload.size());
        CHECK_STATUS(db->writePage(page), DbStatus::Ok);
        payloads.push_back(page.payload);
    }
    CHECK(db->pageCount() == 2 + allocated.size());
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    std::unique_ptr<DatabaseFile> reopened;
    DatabaseOpenOptions openOptions;
    openOptions.validateAllPages = true;
    CHECK_STATUS(DatabaseEngine::openDatabase(path, openOptions, reopened), DbStatus::Ok);
    if (!reopened) return;
    CHECK(reopened->pageCount() == 2 + allocated.size());

    // Nonsequential read order.
    const int order[] = {4, 0, 5, 2, 1, 3};
    for (int i = 0; i < 6; ++i) {
        const int index = order[i];
        DatabasePage page;
        CHECK_STATUS(reopened->readPage(allocated[index], page), DbStatus::Ok);
        CHECK(page.type == PageType::Data);
        CHECK(page.generation == static_cast<uint32_t>(index + 1));
        CHECK(page.payload == payloads[index]);
    }
    CHECK_STATUS(reopened->close(), DbStatus::Ok);
    std::remove(path.c_str());
}

void testHeaderCorruption() {
    std::printf("header corruption\n");
    const std::string base = uniquePath("hdrbase");
    DatabaseCreateOptions options;
    std::unique_ptr<DatabaseFile> db;
    CHECK_STATUS(DatabaseEngine::createDatabase(base, options, db), DbStatus::Ok);
    if (db) db->close();

    DatabaseOpenOptions openOptions;

    // magic
    {
        const std::string work = uniquePath("hdrmagic");
        mutateCopy(base, work, header_offset::Magic, 0x00);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out), DbStatus::NotDatabase);
        std::remove(work.c_str());
    }
    // format version
    {
        const std::string work = uniquePath("hdrversion");
        std::vector<uint8_t> bytes = readFileBytes(base);
        storeLe16(bytes.data() + header_offset::FormatMajor, 99);
        fixHeaderCrc(bytes);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out),
                     DbStatus::UnsupportedVersion);
        std::remove(work.c_str());
    }
    // header checksum
    {
        const std::string work = uniquePath("hdrcrc");
        mutateCopy(base, work, header_offset::HeaderCrc32, 0xAA);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out),
                     DbStatus::CorruptHeader);
        std::remove(work.c_str());
    }
    // page size (checksum also invalid but page-size check runs first)
    {
        const std::string work = uniquePath("hdrpagesize");
        std::vector<uint8_t> bytes = readFileBytes(base);
        storeLe32(bytes.data() + header_offset::PageSize, 1000);
        fixHeaderCrc(bytes);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out),
                     DbStatus::CorruptHeader);
        std::remove(work.c_str());
    }
    // root page id outside range
    {
        const std::string work = uniquePath("hdrroot");
        std::vector<uint8_t> bytes = readFileBytes(base);
        setHeaderField64(bytes, header_offset::RootPageId, 99);
        fixHeaderCrc(bytes);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out),
                     DbStatus::CorruptHeader);
        std::remove(work.c_str());
    }
    // page count below minimum
    {
        const std::string work = uniquePath("hdrcount1");
        std::vector<uint8_t> bytes = readFileBytes(base);
        setHeaderField64(bytes, header_offset::PageCount, 1);
        fixHeaderCrc(bytes);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out),
                     DbStatus::CorruptHeader);
        std::remove(work.c_str());
    }
    // page count above policy maximum
    {
        const std::string work = uniquePath("hdrcount2");
        std::vector<uint8_t> bytes = readFileBytes(base);
        setHeaderField64(bytes, header_offset::PageCount, 0x200000000ull);
        fixHeaderCrc(bytes);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out),
                     DbStatus::CorruptHeader);
        std::remove(work.c_str());
    }
    // page count larger than file (truncation via allocation metadata)
    {
        const std::string work = uniquePath("hdrcount3");
        std::vector<uint8_t> bytes = readFileBytes(base);
        setHeaderField64(bytes, header_offset::PageCount, 5);
        fixHeaderCrc(bytes);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out), DbStatus::Truncated);
        std::remove(work.c_str());
    }
    std::remove(base.c_str());
}

void testPageCorruption() {
    std::printf("page corruption\n");
    const std::string base = uniquePath("pagecorruptbase");
    DatabaseCreateOptions createOptions;
    std::unique_ptr<DatabaseFile> db;
    CHECK_STATUS(DatabaseEngine::createDatabase(base, createOptions, db), DbStatus::Ok);
    if (!db) return;
    uint64_t pageId = 0;
    CHECK_STATUS(db->allocatePage(PageType::Data, pageId), DbStatus::Ok);
    DatabasePage page;
    page.pageId = pageId;
    page.type = PageType::Data;
    page.payload = deterministicPayload(256, 7);
    page.payloadSize = static_cast<uint32_t>(page.payload.size());
    CHECK_STATUS(db->writePage(page), DbStatus::Ok);
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    DatabaseOpenOptions openOptions;

    // Corrupt root page payload: detected at open time.
    {
        const std::string work = uniquePath("rootcorrupt");
        mutateCopy(base, work, static_cast<size_t>(kBootstrapPageId) * 4096 + kPageHeaderSize + 2, 0xEE);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out), DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    // Corrupt a data page: detected on read and by full validation.
    {
        const std::string work = uniquePath("datacorrupt");
        mutateCopy(base, work, static_cast<size_t>(pageId) * 4096 + kPageHeaderSize + 10, 0x5A);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out), DbStatus::Ok);
        if (out) {
            DatabasePage read;
            CHECK_STATUS(out->readPage(pageId, read), DbStatus::CorruptPage);
            out->close();
        }
        std::unique_ptr<DatabaseFile> validating;
        openOptions.validateAllPages = true;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, validating),
                     DbStatus::CorruptPage);
        std::remove(work.c_str());
    }
    std::remove(base.c_str());
}

void testTruncation() {
    std::printf("truncation\n");
    const std::string base = uniquePath("truncbase");
    DatabaseCreateOptions createOptions;
    std::unique_ptr<DatabaseFile> db;
    CHECK_STATUS(DatabaseEngine::createDatabase(base, createOptions, db), DbStatus::Ok);
    if (!db) return;
    uint64_t pageId = 0;
    CHECK_STATUS(db->allocatePage(PageType::Data, pageId), DbStatus::Ok);
    DatabasePage page;
    page.pageId = pageId;
    page.type = PageType::Data;
    page.payload = deterministicPayload(512, 3);
    page.payloadSize = static_cast<uint32_t>(page.payload.size());
    CHECK_STATUS(db->writePage(page), DbStatus::Ok);
    CHECK_STATUS(db->flush(), DbStatus::Ok);
    CHECK_STATUS(db->close(), DbStatus::Ok);

    DatabaseOpenOptions openOptions;

    // inside header
    {
        const std::string work = uniquePath("trunchdr");
        std::vector<uint8_t> bytes = readFileBytes(base);
        bytes.resize(64);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out), DbStatus::Truncated);
        std::remove(work.c_str());
    }
    // immediately after header
    {
        const std::string work = uniquePath("truncposthdr");
        std::vector<uint8_t> bytes = readFileBytes(base);
        bytes.resize(kHeaderSize);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out), DbStatus::Truncated);
        std::remove(work.c_str());
    }
    // inside a page (not page aligned)
    {
        const std::string work = uniquePath("truncmidpage");
        std::vector<uint8_t> bytes = readFileBytes(base);
        bytes.resize(2ull * 4096ull + 100ull);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS_IN(DatabaseEngine::openDatabase(work, openOptions, out),
                        DbStatus::Truncated, DbStatus::CorruptHeader);
        std::remove(work.c_str());
    }
    // before referenced root page (header claims 3 pages, file has 2)
    {
        const std::string work = uniquePath("truncroot");
        std::vector<uint8_t> bytes = readFileBytes(base);
        setHeaderField64(bytes, header_offset::PageCount, 3);
        fixHeaderCrc(bytes);
        bytes.resize(2ull * 4096ull);
        writeFileBytes(work, bytes);
        std::unique_ptr<DatabaseFile> out;
        CHECK_STATUS(DatabaseEngine::openDatabase(work, openOptions, out), DbStatus::Truncated);
        std::remove(work.c_str());
    }
    std::remove(base.c_str());
}

void testBounds() {
    std::printf("bounds and overflow\n");
    const std::string path = uniquePath("bounds");
    DatabaseCreateOptions createOptions;
    std::unique_ptr<DatabaseFile> db;
    CHECK_STATUS(DatabaseEngine::createDatabase(path, createOptions, db), DbStatus::Ok);
    if (!db) return;

    DatabasePage page;
    CHECK_STATUS(db->readPage(kHeaderPageId, page), DbStatus::InvalidArgument);
    CHECK_STATUS(db->readPage(UINT64_MAX, page), DbStatus::OutOfBounds);
    CHECK_STATUS(db->readPage(static_cast<uint64_t>(1) << 40, page), DbStatus::OutOfBounds);
    CHECK_STATUS(db->readPage(2, page), DbStatus::OutOfBounds); // not yet allocated

    DatabasePage bogus;
    bogus.pageId = UINT64_MAX;
    bogus.type = PageType::Data;
    CHECK_STATUS(db->writePage(bogus), DbStatus::OutOfBounds);

    // Payload larger than capacity is rejected.
    DatabasePage huge;
    huge.pageId = 1;
    huge.type = PageType::Data;
    huge.payload.assign(DatabasePage::payloadCapacity(db->pageSize()) + 1, 0x11);
    huge.payloadSize = static_cast<uint32_t>(huge.payload.size());
    CHECK_STATUS(db->writePage(huge), DbStatus::InvalidArgument);

    // Exact capacity is accepted.
    uint64_t allocated = 0;
    CHECK_STATUS(db->allocatePage(PageType::Data, allocated), DbStatus::Ok);
    DatabasePage full;
    full.pageId = allocated;
    full.type = PageType::Data;
    full.payload.assign(DatabasePage::payloadCapacity(db->pageSize()), 0x22);
    full.payloadSize = static_cast<uint32_t>(full.payload.size());
    CHECK_STATUS(db->writePage(full), DbStatus::Ok);

    // Writing at pageCount (one past the end) is rejected.
    DatabasePage past;
    past.pageId = db->pageCount();
    past.type = PageType::Data;
    CHECK_STATUS(db->writePage(past), DbStatus::OutOfBounds);

    CHECK_STATUS(db->close(), DbStatus::Ok);

    // Operations on a closed database fail cleanly.
    CHECK_STATUS(db->readPage(1, page), DbStatus::NotOpen);
    CHECK_STATUS(db->writePage(full), DbStatus::NotOpen);
    uint64_t ignored = 0;
    CHECK_STATUS(db->allocatePage(PageType::Data, ignored), DbStatus::NotOpen);

    // Read-only enforcement.
    DatabaseOpenOptions readOnly;
    readOnly.readOnly = true;
    std::unique_ptr<DatabaseFile> ro;
    CHECK_STATUS(DatabaseEngine::openDatabase(path, readOnly, ro), DbStatus::Ok);
    if (ro) {
        DatabasePage read;
        CHECK_STATUS(ro->readPage(1, read), DbStatus::Ok);
        CHECK_STATUS(ro->writePage(full), DbStatus::ReadOnly);
        CHECK_STATUS(ro->allocatePage(PageType::Data, ignored), DbStatus::ReadOnly);
        ro->close();
    }
    std::remove(path.c_str());
}

void testDeterministicIdentity() {
    std::printf("deterministic identity\n");
    const std::string path = uniquePath("identity");
    uint8_t id[16];
    for (int i = 0; i < 16; ++i) id[i] = static_cast<uint8_t>(i * 7 + 1);

    DatabaseCreateOptions options;
    options.generateDatabaseId = false;
    std::memcpy(options.databaseId, id, 16);
    options.creationTimeUnixNanos = 123456789ull;

    std::unique_ptr<DatabaseFile> db;
    CHECK_STATUS(DatabaseEngine::createDatabase(path, options, db), DbStatus::Ok);
    const std::string expected = formatDatabaseId(id);
    if (db) {
        CHECK(db->diagnostics().databaseId == expected);
        CHECK(db->header().creationTimeUnixNanos == 123456789ull);
        db->close();
    }
    std::unique_ptr<DatabaseFile> reopened;
    DatabaseOpenOptions openOptions;
    CHECK_STATUS(DatabaseEngine::openDatabase(path, openOptions, reopened), DbStatus::Ok);
    if (reopened) {
        CHECK(reopened->diagnostics().databaseId == expected);
        reopened->close();
    }
    std::remove(path.c_str());
}

void testRepeatability() {
    std::printf("repeatability (40 cycles)\n");
    const std::string path = uniquePath("repeat");
    DatabaseCreateOptions createOptions;
    createOptions.overwriteExisting = true;

    bool allOk = true;
    for (int cycle = 0; cycle < 40 && allOk; ++cycle) {
        std::unique_ptr<DatabaseFile> db;
        DbResult result = DatabaseEngine::createDatabase(path, createOptions, db);
        if (!result.isOk() || !db) {
            allOk = false;
            break;
        }
        for (int p = 0; p < 3; ++p) {
            uint64_t pageId = 0;
            result = db->allocatePage(PageType::Data, pageId);
            if (!result.isOk()) { allOk = false; break; }
            DatabasePage page;
            page.pageId = pageId;
            page.type = PageType::Data;
            page.generation = static_cast<uint32_t>(cycle);
            page.payload = deterministicPayload(128, static_cast<uint32_t>(cycle * 10 + p));
            page.payloadSize = static_cast<uint32_t>(page.payload.size());
            result = db->writePage(page);
            if (!result.isOk()) { allOk = false; break; }
        }
        if (!allOk) break;
        if (!db->flush().isOk()) { allOk = false; break; }
        if (!db->close().isOk()) { allOk = false; break; }

        std::unique_ptr<DatabaseFile> reopened;
        result = DatabaseEngine::openDatabase(path, DatabaseOpenOptions(), reopened);
        if (!result.isOk() || !reopened) { allOk = false; break; }
        if (reopened->pageCount() != 5) { allOk = false; break; }
        DatabasePage page;
        result = reopened->readPage(4, page);
        if (!result.isOk()) { allOk = false; break; }
        if (page.payload != deterministicPayload(128, static_cast<uint32_t>(cycle * 10 + 2))) {
            allOk = false;
            break;
        }
        if (!reopened->close().isOk()) { allOk = false; break; }
    }
    CHECK(allOk);
    std::remove(path.c_str());
}

void testMultipleDatabases() {
    std::printf("multiple databases\n");
    const std::string pathA = uniquePath("multiA");
    const std::string pathB = uniquePath("multiB");

    std::unique_ptr<DatabaseFile> a;
    std::unique_ptr<DatabaseFile> b;
    CHECK_STATUS(DatabaseEngine::createDatabase(pathA, DatabaseCreateOptions(), a), DbStatus::Ok);
    CHECK_STATUS(DatabaseEngine::createDatabase(pathB, DatabaseCreateOptions(), b), DbStatus::Ok);
    if (!a || !b) return;

    CHECK(a->diagnostics().databaseId != b->diagnostics().databaseId);

    uint64_t pageA = 0;
    uint64_t pageB = 0;
    CHECK_STATUS(a->allocatePage(PageType::Data, pageA), DbStatus::Ok);
    CHECK_STATUS(b->allocatePage(PageType::Data, pageB), DbStatus::Ok);

    DatabasePage pa;
    pa.pageId = pageA;
    pa.type = PageType::Data;
    pa.payload = deterministicPayload(64, 111);
    pa.payloadSize = static_cast<uint32_t>(pa.payload.size());
    CHECK_STATUS(a->writePage(pa), DbStatus::Ok);

    DatabasePage pb;
    pb.pageId = pageB;
    pb.type = PageType::Data;
    pb.payload = deterministicPayload(64, 222);
    pb.payloadSize = static_cast<uint32_t>(pb.payload.size());
    CHECK_STATUS(b->writePage(pb), DbStatus::Ok);

    CHECK_STATUS(a->flush(), DbStatus::Ok);
    CHECK_STATUS(b->flush(), DbStatus::Ok);
    a->close();
    b->close();

    std::unique_ptr<DatabaseFile> ra;
    std::unique_ptr<DatabaseFile> rb;
    DatabaseOpenOptions openOptions;
    openOptions.validateAllPages = true;
    CHECK_STATUS(DatabaseEngine::openDatabase(pathA, openOptions, ra), DbStatus::Ok);
    CHECK_STATUS(DatabaseEngine::openDatabase(pathB, openOptions, rb), DbStatus::Ok);
    if (ra && rb) {
        DatabasePage readA;
        DatabasePage readB;
        CHECK_STATUS(ra->readPage(pageA, readA), DbStatus::Ok);
        CHECK_STATUS(rb->readPage(pageB, readB), DbStatus::Ok);
        CHECK(readA.payload == deterministicPayload(64, 111));
        CHECK(readB.payload == deterministicPayload(64, 222));
        ra->close();
        rb->close();
    }
    std::remove(pathA.c_str());
    std::remove(pathB.c_str());
}

void testDiagnostics() {
    std::printf("diagnostics\n");
    const std::string path = uniquePath("diag");
    std::unique_ptr<DatabaseFile> db;
    CHECK_STATUS(DatabaseEngine::createDatabase(path, DatabaseCreateOptions(), db), DbStatus::Ok);
    if (db) db->close();

    DatabaseDiagnostics diag;
    CHECK_STATUS(DatabaseEngine::inspectDatabase(path, diag), DbStatus::Ok);
    CHECK(diag.open);
    CHECK(diag.readOnly);
    CHECK(diag.pageSize == 4096);
    CHECK(diag.pageCount == 2);
    CHECK(diag.rootPageId == kBootstrapPageId);
    CHECK(!diag.databaseId.empty());
    CHECK(diag.formatMajor == kFormatMajor);

    // Corrupt and inspect: failure is reported with a description.
    const std::string work = uniquePath("diagbad");
    mutateCopy(path, work, header_offset::Magic, 0x00);
    DatabaseDiagnostics bad;
    CHECK_STATUS(DatabaseEngine::inspectDatabase(work, bad), DbStatus::NotDatabase);
    CHECK(!bad.lastValidationFailure.empty());
    std::remove(work.c_str());
    std::remove(path.c_str());
}

} // namespace

int main() {
    std::printf("guideXOS SQL1 database storage tests\n");
    std::printf("temp dir: %s\n", testDir().c_str());

    testChecksumVector();
    testCreationAndReopen();
    testCreateRefusesOverwrite();
    testInvalidCreateArguments();
    testSupportedPageSizes();
    testPageIoPersistence();
    testHeaderCorruption();
    testPageCorruption();
    testTruncation();
    testBounds();
    testDeterministicIdentity();
    testRepeatability();
    testMultipleDatabases();
    testDiagnostics();

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
