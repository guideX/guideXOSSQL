#include <cstdio>
#include <cstring>
#include <vector>

#include "database_index.h"
#include "database_types.h"
#include "gx_index_key.h"

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

struct BooleanGolden {
    bool isNull;
    bool value;
    const uint8_t* bytes;
    size_t length;
};

void testGoldenBytesAndHostedAdapters() {
    // Captured by calling encodeIndexKey in the pre-migration hosted library
    // at the R3V starting HEAD (50ce94774d48ecaaf5c7b440cbbcf91beded6aff).
    const uint8_t nullBytes[] = {0x00u};
    const uint8_t falseBytes[] = {0x01u, 0x00u};
    const uint8_t trueBytes[] = {0x01u, 0x01u};
    const BooleanGolden cases[] = {
        {true, false, nullBytes, sizeof(nullBytes)},
        {false, false, falseBytes, sizeof(falseBytes)},
        {false, true, trueBytes, sizeof(trueBytes)}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t encoded[4] = {0xCCu, 0xCCu, 0xCCu, 0xCCu};
        size_t encodedLength = 99;
        CHECK(gxEncodeIndexBooleanKey(cases[i].isNull, cases[i].value,
                                     encoded, sizeof(encoded),
                                     &encodedLength) == GxStatus::Ok);
        CHECK(encodedLength == cases[i].length);
        CHECK(encodedLength == cases[i].length &&
              std::memcmp(encoded, cases[i].bytes, cases[i].length) == 0);
        CHECK(encoded[cases[i].length] == 0xCCu);

        std::vector<uint8_t> hosted;
        const DbValue input = cases[i].isNull
            ? DbValue::null() : DbValue::boolean(cases[i].value);
        CHECK(encodeIndexKey(DbType::Boolean, input, hosted));
        CHECK(hosted.size() == cases[i].length);
        CHECK(hosted.size() == cases[i].length &&
              std::memcmp(hosted.data(), cases[i].bytes,
                          cases[i].length) == 0);

        bool decodedValue = true;
        bool decodedNull = !cases[i].isNull;
        CHECK(gxDecodeIndexBooleanKey(encoded, encodedLength,
                                      &decodedValue, &decodedNull) ==
              GxStatus::Ok);
        CHECK(decodedNull == cases[i].isNull);
        CHECK(decodedValue == (!cases[i].isNull && cases[i].value));

        DbValue hostedDecoded = DbValue::int32(77);
        CHECK(decodeIndexKey(DbType::Boolean, cases[i].bytes,
                             cases[i].length, hostedDecoded));
        CHECK(hostedDecoded.isNull() == cases[i].isNull);
        CHECK(cases[i].isNull ||
              (hostedDecoded.type() == DbType::Boolean &&
               hostedDecoded.booleanValue() == cases[i].value));
    }
}

void testNullFalseTrueOrderingAndDeterminism() {
    const uint8_t nullKey[] = {0x00u};
    const uint8_t falseKey[] = {0x01u, 0x00u};
    const uint8_t trueKey[] = {0x01u, 0x01u};
    int comparison = 0;
    CHECK(gxCompareIndexKeys(nullKey, sizeof(nullKey), falseKey,
                             sizeof(falseKey), &comparison) == GxStatus::Ok);
    CHECK(comparison < 0);
    CHECK(gxCompareIndexKeys(falseKey, sizeof(falseKey), trueKey,
                             sizeof(trueKey), &comparison) == GxStatus::Ok);
    CHECK(comparison < 0);
    CHECK(gxCompareIndexKeys(nullKey, sizeof(nullKey), trueKey,
                             sizeof(trueKey), &comparison) == GxStatus::Ok);
    CHECK(comparison < 0);
    CHECK(compareEncodedKeys(nullKey, sizeof(nullKey), falseKey,
                             sizeof(falseKey)) < 0);
    CHECK(compareEncodedKeys(falseKey, sizeof(falseKey), trueKey,
                             sizeof(trueKey)) < 0);
    CHECK(encodedKeyIsNull(nullKey, sizeof(nullKey)));
    CHECK(!encodedKeyIsNull(falseKey, sizeof(falseKey)));
    CHECK(!encodedKeyIsNull(trueKey, sizeof(trueKey)));

    uint8_t first[2] = {};
    uint8_t repeated[2] = {};
    size_t firstLength = 0;
    size_t repeatedLength = 0;
    for (int i = 0; i < 32; ++i) {
        const bool value = (i % 2) != 0;
        CHECK(gxEncodeIndexBooleanKey(false, value, first, sizeof(first),
                                      &firstLength) == GxStatus::Ok);
        CHECK(gxEncodeIndexBooleanKey(false, value, repeated,
                                      sizeof(repeated),
                                      &repeatedLength) == GxStatus::Ok);
        CHECK(firstLength == repeatedLength);
        CHECK(std::memcmp(first, repeated, firstLength) == 0);
    }
}

void testCapacityAndGuardBytes() {
    uint8_t guarded[] = {0xA5u, 0xCCu, 0xCCu, 0x5Au};
    size_t length = 17;
    CHECK(gxEncodeIndexBooleanKey(false, true, guarded + 1, 2, &length) ==
          GxStatus::Ok);
    CHECK(length == 2);
    CHECK(guarded[0] == 0xA5u && guarded[3] == 0x5Au);
    CHECK(guarded[1] == 0x01u && guarded[2] == 0x01u);

    uint8_t tooSmall[] = {0xA5u, 0xCCu, 0xCCu, 0x5Au};
    length = 17;
    CHECK(gxEncodeIndexBooleanKey(false, true, tooSmall + 1, 1, &length) ==
          GxStatus::ResourceLimit);
    CHECK(length == 0);
    CHECK(tooSmall[0] == 0xA5u && tooSmall[1] == 0xCCu &&
          tooSmall[2] == 0xCCu && tooSmall[3] == 0x5Au);
    CHECK(gxEncodeIndexBooleanKey(true, true, tooSmall + 1, 0, &length) ==
          GxStatus::ResourceLimit);
    CHECK(length == 0 && tooSmall[1] == 0xCCu);

    CHECK(gxEncodeIndexBooleanKey(false, false, tooSmall + 1,
                                  sizeof(tooSmall) - 2, nullptr) ==
          GxStatus::InvalidArgument);
    CHECK(gxEncodeIndexBooleanKey(false, false, nullptr, 2, &length) ==
          GxStatus::InvalidArgument);
    CHECK(length == 0);
}

void testMalformedAndFailureState() {
    const uint8_t emptyByte = 0x01u;
    const uint8_t nullWithSuffix[] = {0x00u, 0x00u};
    const uint8_t truncated[] = {0x01u};
    const uint8_t extended[] = {0x01u, 0x00u, 0x00u};
    const uint8_t wrongTag[] = {0x02u, 0x00u};
    const uint8_t payloadTwo[] = {0x01u, 0x02u};
    const uint8_t payloadFF[] = {0x01u, 0xFFu};
    struct InvalidKey { const uint8_t* bytes; size_t length; };
    const InvalidKey invalid[] = {
        {nullptr, 0}, {&emptyByte, 0}, {nullWithSuffix, sizeof(nullWithSuffix)},
        {truncated, sizeof(truncated)}, {extended, sizeof(extended)},
        {wrongTag, sizeof(wrongTag)}, {payloadTwo, sizeof(payloadTwo)},
        {payloadFF, sizeof(payloadFF)}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        bool value = true;
        bool isNull = true;
        CHECK(gxDecodeIndexBooleanKey(invalid[i].bytes, invalid[i].length,
                                      &value, &isNull) == GxStatus::CorruptData);
        CHECK(!value && !isNull);
        DbValue hosted = DbValue::int32(1234);
        CHECK(!decodeIndexKey(DbType::Boolean, invalid[i].bytes,
                              invalid[i].length, hosted));
        CHECK(!hosted.isNull() && hosted.type() == DbType::Int32 &&
              hosted.int32Value() == 1234);
    }

    bool value = true;
    bool isNull = true;
    CHECK(gxDecodeIndexBooleanKey(nullptr, 1, &value, &isNull) ==
          GxStatus::InvalidArgument);
    CHECK(!value && !isNull);
    CHECK(gxDecodeIndexBooleanKey(payloadTwo, sizeof(payloadTwo), nullptr,
                                  &isNull) == GxStatus::InvalidArgument);
    CHECK(!isNull);
    CHECK(gxDecodeIndexBooleanKey(payloadTwo, sizeof(payloadTwo), &value,
                                  nullptr) == GxStatus::InvalidArgument);
    CHECK(!value);
}

void testHostedLegacyTypeMismatchBehavior() {
    std::vector<uint8_t> encoded;
    CHECK(!encodeIndexKey(DbType::Boolean, DbValue::int32(4), encoded));
    CHECK(encoded.size() == 1 && encoded[0] == 0x01u);
    CHECK(encodeIndexKey(DbType::Boolean, DbValue::null(), encoded));
    CHECK(encoded.size() == 1 && encoded[0] == 0x00u);
}

} // namespace

int main() {
    testGoldenBytesAndHostedAdapters();
    testNullFalseTrueOrderingAndDeterminism();
    testCapacityAndGuardBytes();
    testMalformedAndFailureState();
    testHostedLegacyTypeMismatchBehavior();
    std::printf("NDB1R4P11R3V portable Boolean index key codec: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
