#include <cstdio>
#include <cstring>
#include <limits>
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

struct Int32Golden {
    int32_t value;
    uint8_t bytes[5];
};

struct Int64Golden {
    int64_t value;
    uint8_t bytes[9];
};

void testInt32GoldenBytesAndHostedAdapter() {
    const Int32Golden cases[] = {
        {std::numeric_limits<int32_t>::min(), {0x01u, 0x00u, 0x00u, 0x00u, 0x00u}},
        {-1,                                  {0x01u, 0x7Fu, 0xFFu, 0xFFu, 0xFFu}},
        {0,                                   {0x01u, 0x80u, 0x00u, 0x00u, 0x00u}},
        {1,                                   {0x01u, 0x80u, 0x00u, 0x00u, 0x01u}},
        {std::numeric_limits<int32_t>::max(), {0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu}}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t encoded[5] = {0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu};
        size_t encodedLength = 99;
        CHECK(gxEncodeIndexInt32Key(false, cases[i].value, encoded,
                                    sizeof(encoded), &encodedLength) ==
              GxStatus::Ok);
        CHECK(encodedLength == sizeof(cases[i].bytes));
        CHECK(std::memcmp(encoded, cases[i].bytes, sizeof(encoded)) == 0);

        int32_t decoded = 123;
        bool isNull = true;
        CHECK(gxDecodeIndexInt32Key(encoded, encodedLength, &decoded,
                                    &isNull) == GxStatus::Ok);
        CHECK(!isNull && decoded == cases[i].value);

        std::vector<uint8_t> hosted;
        CHECK(encodeIndexKey(DbType::Int32, DbValue::int32(cases[i].value),
                             hosted));
        CHECK(hosted.size() == sizeof(cases[i].bytes));
        CHECK(hosted.size() == sizeof(cases[i].bytes) &&
              std::memcmp(hosted.data(), cases[i].bytes,
                          sizeof(cases[i].bytes)) == 0);
        DbValue hostedDecoded = DbValue::text("unchanged");
        CHECK(decodeIndexKey(DbType::Int32, cases[i].bytes,
                             sizeof(cases[i].bytes), hostedDecoded));
        CHECK(!hostedDecoded.isNull() &&
              hostedDecoded.type() == DbType::Int32 &&
              hostedDecoded.int32Value() == cases[i].value);
    }
}

void testInt64GoldenBytesAndHostedAdapter() {
    const Int64Golden cases[] = {
        {std::numeric_limits<int64_t>::min(),
         {0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}},
        {-1,
         {0x01u, 0x7Fu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu}},
        {0,
         {0x01u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}},
        {1,
         {0x01u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u}},
        {std::numeric_limits<int64_t>::max(),
         {0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu}}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t encoded[9] = {};
        size_t encodedLength = 0;
        CHECK(gxEncodeIndexInt64Key(false, cases[i].value, encoded,
                                    sizeof(encoded), &encodedLength) ==
              GxStatus::Ok);
        CHECK(encodedLength == sizeof(cases[i].bytes));
        CHECK(std::memcmp(encoded, cases[i].bytes, sizeof(encoded)) == 0);

        int64_t decoded = 123;
        bool isNull = true;
        CHECK(gxDecodeIndexInt64Key(encoded, encodedLength, &decoded,
                                    &isNull) == GxStatus::Ok);
        CHECK(!isNull && decoded == cases[i].value);

        std::vector<uint8_t> hosted;
        CHECK(encodeIndexKey(DbType::Int64, DbValue::int64(cases[i].value),
                             hosted));
        CHECK(hosted.size() == sizeof(cases[i].bytes));
        CHECK(hosted.size() == sizeof(cases[i].bytes) &&
              std::memcmp(hosted.data(), cases[i].bytes,
                          sizeof(cases[i].bytes)) == 0);
        DbValue hostedDecoded = DbValue::text("unchanged");
        CHECK(decodeIndexKey(DbType::Int64, cases[i].bytes,
                             sizeof(cases[i].bytes), hostedDecoded));
        CHECK(!hostedDecoded.isNull() &&
              hostedDecoded.type() == DbType::Int64 &&
              hostedDecoded.int64Value() == cases[i].value);
    }
}

void testNullZeroAndHostedNullCompatibility() {
    const uint8_t nullBytes[] = {0x00u};
    uint8_t encoded32[5] = {};
    size_t length32 = 0;
    CHECK(gxEncodeIndexInt32Key(true, 0, encoded32, sizeof(encoded32),
                                &length32) == GxStatus::Ok);
    CHECK(length32 == 1 && encoded32[0] == 0x00u);
    int32_t value32 = 78;
    bool isNull32 = false;
    CHECK(gxDecodeIndexInt32Key(nullBytes, sizeof(nullBytes), &value32,
                                &isNull32) == GxStatus::Ok);
    CHECK(isNull32 && value32 == 0);

    uint8_t encoded64[9] = {};
    size_t length64 = 0;
    CHECK(gxEncodeIndexInt64Key(true, 0, encoded64, sizeof(encoded64),
                                &length64) == GxStatus::Ok);
    CHECK(length64 == 1 && encoded64[0] == 0x00u);
    int64_t value64 = 91;
    bool isNull64 = false;
    CHECK(gxDecodeIndexInt64Key(nullBytes, sizeof(nullBytes), &value64,
                                &isNull64) == GxStatus::Ok);
    CHECK(isNull64 && value64 == 0);

    uint8_t zero32[5] = {};
    uint8_t zero64[9] = {};
    CHECK(gxEncodeIndexInt32Key(false, 0, zero32, sizeof(zero32),
                                &length32) == GxStatus::Ok);
    CHECK(gxEncodeIndexInt64Key(false, 0, zero64, sizeof(zero64),
                                &length64) == GxStatus::Ok);
    CHECK(length32 == 5 && zero32[0] == 0x01u && zero32[1] == 0x80u);
    CHECK(length64 == 9 && zero64[0] == 0x01u && zero64[1] == 0x80u);
    int nullToZeroComparison = 0;
    CHECK(gxCompareIndexKeys(nullBytes, sizeof(nullBytes), zero32,
                             length32, &nullToZeroComparison) == GxStatus::Ok);
    CHECK(nullToZeroComparison < 0);

    std::vector<uint8_t> hosted;
    CHECK(encodeIndexKey(DbType::Int32, DbValue::null(), hosted));
    CHECK(hosted.size() == 1 && hosted[0] == nullBytes[0]);
    DbValue decoded = DbValue::int32(44);
    CHECK(decodeIndexKey(DbType::Int32, nullBytes, sizeof(nullBytes), decoded));
    CHECK(decoded.isNull());
    CHECK(encodeIndexKey(DbType::Int64, DbValue::null(), hosted));
    CHECK(hosted.size() == 1 && hosted[0] == nullBytes[0]);
    decoded = DbValue::int64(44);
    CHECK(decodeIndexKey(DbType::Int64, nullBytes, sizeof(nullBytes), decoded));
    CHECK(decoded.isNull());
    CHECK(encodedKeyIsNull(nullBytes, sizeof(nullBytes)));
    CHECK(!encodedKeyIsNull(zero32, sizeof(zero32)));
}

void testOrderingAcrossSignTransitions() {
    const int32_t values32[] = {
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min() + 1,
        -2, -1, 0, 1,
        std::numeric_limits<int32_t>::max() - 1,
        std::numeric_limits<int32_t>::max()
    };
    for (size_t i = 0; i < sizeof(values32) / sizeof(values32[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(values32) / sizeof(values32[0]); ++j) {
            uint8_t left[5] = {};
            uint8_t right[5] = {};
            size_t leftLength = 0;
            size_t rightLength = 0;
            CHECK(gxEncodeIndexInt32Key(false, values32[i], left,
                                        sizeof(left), &leftLength) ==
                  GxStatus::Ok);
            CHECK(gxEncodeIndexInt32Key(false, values32[j], right,
                                        sizeof(right), &rightLength) ==
                  GxStatus::Ok);
            int comparison = 0;
            CHECK(gxCompareIndexKeys(left, leftLength, right, rightLength,
                                     &comparison) == GxStatus::Ok);
            CHECK(comparison < 0 && values32[i] < values32[j]);
            CHECK(compareEncodedKeys(left, leftLength, right, rightLength) < 0);
        }
    }

    const int64_t values64[] = {
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::min() + 1,
        -2, -1, 0, 1,
        std::numeric_limits<int64_t>::max() - 1,
        std::numeric_limits<int64_t>::max()
    };
    for (size_t i = 0; i < sizeof(values64) / sizeof(values64[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(values64) / sizeof(values64[0]); ++j) {
            uint8_t left[9] = {};
            uint8_t right[9] = {};
            size_t leftLength = 0;
            size_t rightLength = 0;
            CHECK(gxEncodeIndexInt64Key(false, values64[i], left,
                                        sizeof(left), &leftLength) ==
                  GxStatus::Ok);
            CHECK(gxEncodeIndexInt64Key(false, values64[j], right,
                                        sizeof(right), &rightLength) ==
                  GxStatus::Ok);
            int comparison = 0;
            CHECK(gxCompareIndexKeys(left, leftLength, right, rightLength,
                                     &comparison) == GxStatus::Ok);
            CHECK(comparison < 0 && values64[i] < values64[j]);
            CHECK(compareEncodedKeys(left, leftLength, right, rightLength) < 0);
        }
    }
}

void testCapacityAndArgumentFailures() {
    uint8_t bytes32[5];
    std::memset(bytes32, 0xCC, sizeof(bytes32));
    size_t length = 91;
    CHECK(gxEncodeIndexInt32Key(false, 5, bytes32, 4, &length) ==
          GxStatus::ResourceLimit);
    CHECK(length == 0 && bytes32[0] == 0xCCu && bytes32[4] == 0xCCu);
    length = 91;
    CHECK(gxEncodeIndexInt32Key(true, 5, bytes32, 0, &length) ==
          GxStatus::ResourceLimit);
    CHECK(length == 0 && bytes32[0] == 0xCCu);
    length = 91;
    CHECK(gxEncodeIndexInt32Key(false, 5, nullptr, sizeof(bytes32), &length) ==
          GxStatus::InvalidArgument);
    CHECK(length == 0);
    bytes32[0] = 0xCCu;
    CHECK(gxEncodeIndexInt32Key(false, 5, bytes32, sizeof(bytes32), nullptr) ==
          GxStatus::InvalidArgument);
    CHECK(bytes32[0] == 0xCCu);

    uint8_t bytes64[9];
    std::memset(bytes64, 0xDD, sizeof(bytes64));
    length = 91;
    CHECK(gxEncodeIndexInt64Key(false, 5, bytes64, 8, &length) ==
          GxStatus::ResourceLimit);
    CHECK(length == 0 && bytes64[0] == 0xDDu && bytes64[8] == 0xDDu);
    length = 91;
    CHECK(gxEncodeIndexInt64Key(true, 5, bytes64, 0, &length) ==
          GxStatus::ResourceLimit);
    CHECK(length == 0 && bytes64[0] == 0xDDu);
    CHECK(gxEncodeIndexInt64Key(false, 5, nullptr, sizeof(bytes64), &length) ==
          GxStatus::InvalidArgument);
    CHECK(length == 0);

    int32_t decoded32 = 10;
    bool null32 = true;
    CHECK(gxDecodeIndexInt32Key(nullptr, 0, &decoded32, &null32) ==
          GxStatus::CorruptData);
    CHECK(decoded32 == 0 && !null32);
    decoded32 = 10;
    null32 = true;
    CHECK(gxDecodeIndexInt32Key(nullptr, 1, &decoded32, &null32) ==
          GxStatus::InvalidArgument);
    CHECK(decoded32 == 0 && !null32);
    CHECK(gxDecodeIndexInt32Key(bytes32, 5, nullptr, &null32) ==
          GxStatus::InvalidArgument);
    CHECK(!null32);

    int64_t decoded64 = 10;
    bool null64 = true;
    CHECK(gxDecodeIndexInt64Key(nullptr, 0, &decoded64, &null64) ==
          GxStatus::CorruptData);
    CHECK(decoded64 == 0 && !null64);
    decoded64 = 10;
    null64 = true;
    CHECK(gxDecodeIndexInt64Key(nullptr, 1, &decoded64, &null64) ==
          GxStatus::InvalidArgument);
    CHECK(decoded64 == 0 && !null64);
    CHECK(gxDecodeIndexInt64Key(bytes64, 9, nullptr, &null64) ==
          GxStatus::InvalidArgument);
    CHECK(!null64);
}

void testMalformedAndTypeMismatchedPayloads() {
    const uint8_t int32Good[] = {0x01u, 0x80u, 0x00u, 0x00u, 0x01u};
    const uint8_t int64Good[] = {
        0x01u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u};
    const uint8_t nullExtra[] = {0x00u, 0x00u};
    const uint8_t unknownTag[] = {0x02u, 0x80u, 0x00u, 0x00u, 0x01u};
    int32_t value32 = 44;
    int64_t value64 = 55;
    bool isNull = true;
    CHECK(gxDecodeIndexInt32Key(int32Good, 0, &value32, &isNull) ==
          GxStatus::CorruptData);
    CHECK(value32 == 0 && !isNull);
    CHECK(gxDecodeIndexInt32Key(int32Good, 1, &value32, &isNull) ==
          GxStatus::CorruptData);
    CHECK(gxDecodeIndexInt32Key(int32Good, 4, &value32, &isNull) ==
          GxStatus::CorruptData);
    CHECK(gxDecodeIndexInt32Key(int32Good, 6, &value32, &isNull) ==
          GxStatus::CorruptData);
    CHECK(gxDecodeIndexInt32Key(nullExtra, sizeof(nullExtra), &value32,
                                &isNull) == GxStatus::CorruptData);
    CHECK(gxDecodeIndexInt32Key(unknownTag, sizeof(unknownTag), &value32,
                                &isNull) == GxStatus::CorruptData);
    CHECK(gxDecodeIndexInt64Key(int32Good, sizeof(int32Good),
                                &value64,
                                &isNull) == GxStatus::CorruptData);
    CHECK(value64 == 0 && !isNull);
    CHECK(gxDecodeIndexInt64Key(int64Good, 1, &value64,
                                &isNull) == GxStatus::CorruptData);
    CHECK(gxDecodeIndexInt64Key(int64Good, 8, &value64,
                                &isNull) == GxStatus::CorruptData);
    CHECK(gxDecodeIndexInt64Key(int64Good, 10, &value64,
                                &isNull) == GxStatus::CorruptData);
    CHECK(gxDecodeIndexInt32Key(int64Good, sizeof(int64Good), &value32,
                                &isNull) == GxStatus::CorruptData);

    DbValue preserved = DbValue::int32(777);
    CHECK(!decodeIndexKey(DbType::Int32, unknownTag, sizeof(unknownTag),
                          preserved));
    CHECK(!preserved.isNull() && preserved.type() == DbType::Int32 &&
          preserved.int32Value() == 777);
    CHECK(!decodeIndexKey(DbType::Int32, int64Good, sizeof(int64Good),
                          preserved));
    CHECK(!preserved.isNull() && preserved.int32Value() == 777);
    std::vector<uint8_t> mismatch;
    CHECK(!encodeIndexKey(DbType::Int32, DbValue::int64(9), mismatch));
    CHECK(mismatch.size() == 1 && mismatch[0] == 0x01u);
}

} // namespace

int main() {
    std::printf("NDB1R4P11R3U portable index scalar key codec tests\n");
    testInt32GoldenBytesAndHostedAdapter();
    testInt64GoldenBytesAndHostedAdapter();
    testNullZeroAndHostedNullCompatibility();
    testOrderingAcrossSignTransitions();
    testCapacityAndArgumentFailures();
    testMalformedAndTypeMismatchedPayloads();
    std::printf("NDB1R4P11R3U portable index scalar key codec: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
