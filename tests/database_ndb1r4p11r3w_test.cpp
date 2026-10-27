#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "database_relational.h"
#include "database_sql.h"
#include "database_wal.h"
#include "gx_index_physical_key.h"

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

void checkCompare(const uint8_t* leftBytes, size_t leftLength,
                  bool leftHasLocator, uint64_t leftPage, uint32_t leftSlot,
                  const uint8_t* rightBytes, size_t rightLength,
                  bool rightHasLocator, uint64_t rightPage, uint32_t rightSlot,
                  int expected) {
    int comparison = 77;
    const GxStatus status = gxCompareIndexPhysicalKeyViews(
        leftBytes, leftLength, leftHasLocator, leftPage, leftSlot,
        rightBytes, rightLength, rightHasLocator, rightPage, rightSlot,
        &comparison);
    CHECK(status == GxStatus::Ok);
    CHECK(comparison == expected);
}

void testPhysicalKeyViews() {
    const uint8_t nullKey[] = {0x00u};
    const uint8_t falseKey[] = {0x01u, 0x00u};
    const uint8_t trueKey[] = {0x01u, 0x01u};
    const uint8_t int32MinusOne[] = {0x01u, 0x7Fu, 0xFFu, 0xFFu, 0xFFu};
    const uint8_t int32Zero[] = {0x01u, 0x80u, 0x00u, 0x00u, 0x00u};
    const uint8_t int64MinusOne[] = {
        0x01u, 0x7Fu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
    };
    const uint8_t int64Zero[] = {
        0x01u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };
    const uint8_t textWithZeroB[] = {0x01u, 'a', 0x00u, 'b'};
    const uint8_t textWithZeroC[] = {0x01u, 'a', 0x00u, 'c'};

    // Existing logical-key byte order remains the leading physical-key order.
    checkCompare(nullKey, sizeof(nullKey), true, 7, 9,
                 falseKey, sizeof(falseKey), true, 7, 9, -1);
    checkCompare(falseKey, sizeof(falseKey), true, 7, 9,
                 trueKey, sizeof(trueKey), true, 7, 9, -1);
    checkCompare(int32MinusOne, sizeof(int32MinusOne), true, 7, 9,
                 int32Zero, sizeof(int32Zero), true, 7, 9, -1);
    checkCompare(int64MinusOne, sizeof(int64MinusOne), true, 7, 9,
                 int64Zero, sizeof(int64Zero), true, 7, 9, -1);
    checkCompare(textWithZeroB, sizeof(textWithZeroB), true, 7, 9,
                 textWithZeroC, sizeof(textWithZeroC), true, 7, 9, -1);
    checkCompare(textWithZeroB, sizeof(textWithZeroB), true, 7, 9,
                 textWithZeroB, sizeof(textWithZeroB), true, 7, 9, 0);

    // The existing physical suffix is unsigned big-endian page id then slot.
    checkCompare(falseKey, sizeof(falseKey), true, 0xFFu, 0xFFFFFFFFu,
                 falseKey, sizeof(falseKey), true, 0x100u, 0, -1);
    checkCompare(falseKey, sizeof(falseKey), true, 12, 0xFFu,
                 falseKey, sizeof(falseKey), true, 12, 0x100u, -1);
    checkCompare(nullptr, 0, true, 0, 0,
                 nullptr, 0, true, 0, 1, -1);

    // A stored bound is a complete byte span. Compare it with a synthesized
    // logical-key-plus-locator view without allocating a combined key.
    const uint8_t storedPhysical[] = {
        0x01u, 'x', 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 4
    };
    checkCompare(storedPhysical, sizeof(storedPhysical), false, 0, 0,
                 storedPhysical, 2, true, 3, 4, 0);
    checkCompare(storedPhysical, 3, false, 0, 0,
                 storedPhysical, 2, true, 3, 4, -1);
    const uint8_t extendedPhysical[] = {
        0x01u, 'x', 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 4, 0
    };
    checkCompare(extendedPhysical, sizeof(extendedPhysical), false, 0, 0,
                 storedPhysical, 2, true, 3, 4, 1);

    // The operation accepts arbitrary bounded spans. It does not interpret
    // truncated bytes as a typed key or validate index-entry offsets.
    const uint8_t guarded[] = {0xA5u, 0x10u, 0x00u, 0x5Au};
    const uint8_t same[] = {0x10u, 0x00u};
    checkCompare(guarded + 1, 2, false, 0, 0,
                 same, sizeof(same), false, 0, 0, 0);

    int comparison = 77;
    CHECK(gxCompareIndexPhysicalKeyViews(nullptr, 1, false, 0, 0,
                                         same, sizeof(same), false, 0, 0,
                                         &comparison) ==
          GxStatus::InvalidArgument);
    CHECK(comparison == 0);
    CHECK(gxCompareIndexPhysicalKeyViews(same, sizeof(same), false, 0, 0,
                                         same, sizeof(same), false, 0, 0,
                                         nullptr) ==
          GxStatus::InvalidArgument);

    const size_t tooLong = static_cast<size_t>(-1) - 11u;
    CHECK(gxCompareIndexPhysicalKeyViews(same, tooLong, true, 0, 0,
                                         same, sizeof(same), false, 0, 0,
                                         &comparison) == GxStatus::Overflow);
    CHECK(comparison == 0);
}

std::string unusedDatabasePath() {
    const char* temp = std::getenv("TEMP");
    if (temp == nullptr || temp[0] == '\0') temp = std::getenv("TMP");
    if (temp == nullptr || temp[0] == '\0') temp = ".";
    const uint64_t seed = static_cast<uint64_t>(std::time(nullptr)) * 1000000u +
                          static_cast<uint64_t>(std::clock());
    for (uint32_t attempt = 0; attempt < 100; ++attempt) {
        const std::string candidate = std::string(temp) +
            "/gxos_sql_r3w_physical_key_" +
            std::to_string(seed + attempt) + ".gxdb";
        FILE* existing = std::fopen(candidate.c_str(), "rb");
        if (existing == nullptr) return candidate;
        std::fclose(existing);
    }
    return std::string();
}

void testHostedIndexUsesPhysicalOrdering() {
    const std::string path = unusedDatabasePath();
    CHECK(!path.empty());
    if (path.empty()) return;

    DatabaseCreateOptions options;
    options.pageSize = 512;
    std::unique_ptr<Database> db;
    const DbResult createResult = Database::create(path, options, db);
    CHECK(createResult.isOk() && db != nullptr);
    if (!createResult.isOk() || !db) return;

    SqlEngine engine(*db);
    SqlExecutionResult result = engine.execute(
        "CREATE TABLE T (Id INT64 PRIMARY KEY, K TEXT);"
        "CREATE INDEX IX_T_K ON T (K);");
    CHECK(result.ok);

    // Multiple duplicate logical keys force leaf splits. The transaction
    // index path sorts physical key parts and later descends using stored
    // physical bounds, both of which now call the portable operation.
    std::string inserts = "BEGIN;";
    const uint32_t rowCount = 72;
    for (uint32_t i = 0; i < rowCount; ++i) {
        const uint32_t id = (i * 29u) % rowCount;
        char key[16];
        std::snprintf(key, sizeof(key), "k%02u", i % 9u);
        inserts += "INSERT INTO T VALUES (" + std::to_string(id) + ", '" +
                   key + "');";
    }
    inserts += "COMMIT;";
    result = engine.execute(inserts);
    CHECK(result.ok);

    result = engine.execute("SELECT * FROM T WHERE K = 'k03';");
    CHECK(result.ok && result.statements.size() == 1);
    if (result.ok && result.statements.size() == 1) {
        CHECK(result.statements[0].accessPath == "IndexLookup");
        CHECK(result.statements[0].resultSet.rowCount() == rowCount / 9u);
    }

    const DbResult closeResult = db->close();
    CHECK(closeResult.isOk());
    db.reset();
    std::remove(path.c_str());
    const std::string walPath = walPathFor(path);
    std::remove(walPath.c_str());
}

} // namespace

int main() {
    testPhysicalKeyViews();
    testHostedIndexUsesPhysicalOrdering();
    std::printf("NDB1R4P11R3W portable physical index-key operation: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
