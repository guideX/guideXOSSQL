#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "database_file.h"
#include "database_page.h"
#include "database_relational.h"
#include "database_transaction.h"
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
    testDurableCommitAllocationFailureRecovery();
    std::printf("Checked cache publication: %d passed, %d failed\n",
                gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
