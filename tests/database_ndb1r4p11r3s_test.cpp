#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "database_format.h"
#include "database_index.h"
#include "database_types.h"
#include "gx_index_text_key.h"

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

std::vector<uint8_t> textKey(const uint8_t* bytes, size_t length) {
    std::vector<uint8_t> encoded(length + 1, 0xCCu);
    size_t encodedLength = 0;
    const GxStatus status = gxEncodeIndexTextKey(
        bytes, length, encoded.data(), encoded.size(), &encodedLength);
    CHECK(status == GxStatus::Ok);
    CHECK(encodedLength == encoded.size());
    return encoded;
}

void checkTextDecode(const std::vector<uint8_t>& encoded,
                     const std::vector<uint8_t>& expected) {
    GxTextByteView view = {reinterpret_cast<const uint8_t*>("bad"), 77};
    bool isNull = true;
    CHECK(gxDecodeIndexTextKey(encoded.data(), encoded.size(), &view,
                               &isNull) == GxStatus::Ok);
    CHECK(!isNull);
    CHECK(gxTextByteViewIsValid(view));
    CHECK(view.data == encoded.data() + 1);
    CHECK(view.length == encoded.size() - 1);
    CHECK(view.length == expected.size());
    if (!expected.empty() && view.length == expected.size()) {
        CHECK(std::memcmp(view.data, expected.data(), expected.size()) == 0);
    }

    DbValue hosted = DbValue::int64(91);
    CHECK(decodeIndexKey(DbType::Text, encoded.data(), encoded.size(), hosted));
    CHECK(!hosted.isNull());
    CHECK(hosted.type() == DbType::Text);
    CHECK(hosted.textValue().size() == expected.size());
    if (!expected.empty() && hosted.textValue().size() == expected.size()) {
        CHECK(std::memcmp(hosted.textValue().data(), expected.data(),
                          expected.size()) == 0);
    }
}

void testPayloadRoundTrips() {
    const std::vector<std::vector<uint8_t> > cases = {
        std::vector<uint8_t>(),
        std::vector<uint8_t>(1, static_cast<uint8_t>('x')),
        std::vector<uint8_t>{'g', 'u', 'i', 'd', 'e', '-', 'i', 'n', 'd', 'e', 'x'},
        std::vector<uint8_t>{0xE2u, 0x82u, 0xACu, 0xF0u, 0x9Fu, 0x8Cu, 0x8Du},
        std::vector<uint8_t>{'a', 0x00u, 'b', 0x00u, 0xFFu}
    };

    for (size_t i = 0; i < cases.size(); ++i) {
        const std::vector<uint8_t>& payload = cases[i];
        const std::vector<uint8_t> encoded = textKey(
            payload.empty() ? nullptr : payload.data(), payload.size());
        CHECK(encoded[0] == 0x01u);
        checkTextDecode(encoded, payload);
    }
}

void testMaximumKeyAndBorrowing() {
    const size_t maximumEncoded = maxIndexKeyBytes(kMaxPageSize);
    CHECK(maximumEncoded == kMaxIndexKeyBytesCap);
    std::vector<uint8_t> payload(maximumEncoded - 1, 0xA5u);
    const std::vector<uint8_t> encoded = textKey(payload.data(), payload.size());
    CHECK(encoded.size() == maximumEncoded);
    checkTextDecode(encoded, payload);

    uint8_t mutableKey[] = {0x01u, 'A', 0x00u, 'B'};
    GxTextByteView borrowed = {nullptr, 0};
    bool isNull = false;
    CHECK(gxDecodeIndexTextKey(mutableKey, sizeof(mutableKey), &borrowed,
                               &isNull) == GxStatus::Ok);
    CHECK(!isNull && borrowed.data == mutableKey + 1 && borrowed.length == 3);
    mutableKey[1] = 'Z';
    CHECK(borrowed.data[0] == 'Z');
    CHECK(borrowed.data[1] == 0x00u && borrowed.data[2] == 'B');
}

void testNullAndMalformedEncodings() {
    const uint8_t nullKey[] = {0x00u};
    GxTextByteView view = {reinterpret_cast<const uint8_t*>("old"), 3};
    bool isNull = false;
    CHECK(gxDecodeIndexTextKey(nullKey, sizeof(nullKey), &view, &isNull) ==
          GxStatus::Ok);
    CHECK(isNull);
    CHECK(view.data == nullptr && view.length == 0);
    CHECK(gxTextByteViewIsValid(view));

    DbValue hosted = DbValue::int64(4);
    CHECK(decodeIndexKey(DbType::Text, nullKey, sizeof(nullKey), hosted));
    CHECK(hosted.isNull());

    view.data = reinterpret_cast<const uint8_t*>("old");
    view.length = 3;
    isNull = true;
    CHECK(gxDecodeIndexTextKey(nullptr, 0, &view, &isNull) ==
          GxStatus::CorruptData);
    CHECK(view.data == nullptr && view.length == 0 && !isNull);

    const uint8_t emptyEncoding[] = {0x01u}; // valid empty Text, not truncation
    CHECK(gxDecodeIndexTextKey(emptyEncoding, sizeof(emptyEncoding), &view,
                               &isNull) == GxStatus::Ok);
    CHECK(!isNull && view.data == emptyEncoding + 1 && view.length == 0);

    const uint8_t nullWithTrailingByte[] = {0x00u, 0x41u};
    const uint8_t unknownTag[] = {0x02u, 0x41u};
    const uint8_t highUnknownTag[] = {0xFFu};
    CHECK(gxDecodeIndexTextKey(nullWithTrailingByte,
                               sizeof(nullWithTrailingByte), &view,
                               &isNull) == GxStatus::CorruptData);
    CHECK(view.data == nullptr && view.length == 0 && !isNull);
    CHECK(gxDecodeIndexTextKey(unknownTag, sizeof(unknownTag), &view,
                               &isNull) == GxStatus::CorruptData);
    CHECK(gxDecodeIndexTextKey(highUnknownTag, sizeof(highUnknownTag), &view,
                               &isNull) == GxStatus::CorruptData);
    CHECK(gxDecodeIndexTextKey(nullptr, 1, &view, &isNull) ==
          GxStatus::InvalidArgument);

    hosted = DbValue::text("preserved");
    CHECK(!decodeIndexKey(DbType::Text, unknownTag, sizeof(unknownTag), hosted));
    CHECK(hosted.type() == DbType::Text && hosted.textValue() == "preserved");
}

void testOutputAndLengthBoundaries() {
    const uint8_t tag = 0x01u;
    GxTextByteView view = {reinterpret_cast<const uint8_t*>("old"), 3};
    bool isNull = true;
    CHECK(gxDecodeIndexTextKey(&tag, 0, &view, &isNull) ==
          GxStatus::CorruptData);
    CHECK(view.data == nullptr && view.length == 0 && !isNull);

    CHECK(gxDecodeIndexTextKey(&tag, 1, nullptr, &isNull) ==
          GxStatus::InvalidArgument);
    CHECK(!isNull);
    isNull = true;
    CHECK(gxDecodeIndexTextKey(&tag, 1, &view, nullptr) ==
          GxStatus::InvalidArgument);
    CHECK(view.data == nullptr && view.length == 0);

    CHECK(!gxTextByteViewIsValid(
        GxTextByteView{nullptr, std::numeric_limits<size_t>::max()}));
    size_t encodedSize = 12;
    CHECK(gxIndexTextKeyEncodedSize(std::numeric_limits<size_t>::max(),
                                    &encodedSize) == GxStatus::Overflow);
    CHECK(encodedSize == 0);

    const uint8_t keyA[] = {0x01u, 'a'};
    const uint8_t keyAB[] = {0x01u, 'a', 'b'};
    const uint8_t keyHigh[] = {0x01u, 0x80u};
    const uint8_t keyLow[] = {0x01u, 0x7Fu};
    CHECK(compareEncodedKeys(keyA, sizeof(keyA), keyAB, sizeof(keyAB)) < 0);
    CHECK(compareEncodedKeys(keyLow, sizeof(keyLow), keyHigh,
                             sizeof(keyHigh)) < 0);
}

} // namespace

int main() {
    std::printf("NDB1R4P11R3S portable index Text decoder tests\n");
    testPayloadRoundTrips();
    testMaximumKeyAndBorrowing();
    testNullAndMalformedEncodings();
    testOutputAndLengthBoundaries();
    std::printf("NDB1R4P11R3S portable index Text decoder: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
