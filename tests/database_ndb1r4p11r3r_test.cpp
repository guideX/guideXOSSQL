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

std::vector<uint8_t> referenceTextKey(const uint8_t* textBytes,
                                      size_t textLength) {
    std::vector<uint8_t> encoded;
    encoded.push_back(0x01u);
    if (textLength != 0) {
        encoded.insert(encoded.end(), textBytes, textBytes + textLength);
    }
    return encoded;
}

void testEmptyAndBinaryInputs() {
    uint8_t encoded[8] = {};
    size_t encodedLength = 99;
    CHECK(gxEncodeIndexTextKey(nullptr, 0, encoded, 1, &encodedLength) ==
          GxStatus::Ok);
    CHECK(encodedLength == 1 && encoded[0] == 0x01u);

    const uint8_t binary[] = {0x41u, 0x00u, 0xFFu, 0x42u};
    CHECK(gxEncodeIndexTextKey(binary, sizeof(binary), encoded,
                               sizeof(encoded), &encodedLength) == GxStatus::Ok);
    CHECK(encodedLength == sizeof(binary) + 1);
    CHECK(encoded[0] == 0x01u);
    CHECK(std::memcmp(encoded + 1, binary, sizeof(binary)) == 0);
    const std::vector<uint8_t> reference =
        referenceTextKey(binary, sizeof(binary));
    CHECK(reference.size() == encodedLength);
    CHECK(std::memcmp(encoded, reference.data(), encodedLength) == 0);

    const uint8_t sameBinary[] = {0x41u, 0x00u, 0xFFu, 0x42u};
    uint8_t equivalent[8] = {};
    size_t equivalentLength = 0;
    CHECK(gxEncodeIndexTextKey(sameBinary, sizeof(sameBinary), equivalent,
                               sizeof(equivalent), &equivalentLength) ==
          GxStatus::Ok);
    CHECK(equivalentLength == encodedLength);
    CHECK(std::memcmp(encoded, equivalent, encodedLength) == 0);

    std::vector<uint8_t> hosted;
    const std::string hostedText(reinterpret_cast<const char*>(binary),
                                 sizeof(binary));
    CHECK(encodeIndexKey(DbType::Text, DbValue::text(hostedText), hosted));
    CHECK(hosted == reference);
}

void testOrderingAndHostedCompatibility() {
    uint8_t emptyKey[1] = {};
    uint8_t aKey[2] = {};
    uint8_t abKey[3] = {};
    const uint8_t a[] = {'A'};
    const uint8_t ab[] = {'A', 'B'};
    size_t emptyLength = 0;
    size_t aLength = 0;
    size_t abLength = 0;
    CHECK(gxEncodeIndexTextKey(nullptr, 0, emptyKey, sizeof(emptyKey),
                               &emptyLength) == GxStatus::Ok);
    CHECK(gxEncodeIndexTextKey(a, sizeof(a), aKey, sizeof(aKey), &aLength) ==
          GxStatus::Ok);
    CHECK(gxEncodeIndexTextKey(ab, sizeof(ab), abKey, sizeof(abKey), &abLength) ==
          GxStatus::Ok);
    CHECK(compareEncodedKeys(emptyKey, emptyLength, aKey, aLength) < 0);
    CHECK(compareEncodedKeys(aKey, aLength, abKey, abLength) < 0);
    CHECK(compareEncodedKeys(aKey, aLength, aKey, aLength) == 0);

    const uint8_t low[] = {0x7Fu};
    const uint8_t high[] = {0x80u};
    uint8_t lowKey[2] = {};
    uint8_t highKey[2] = {};
    size_t lowLength = 0;
    size_t highLength = 0;
    CHECK(gxEncodeIndexTextKey(low, sizeof(low), lowKey, sizeof(lowKey),
                               &lowLength) == GxStatus::Ok);
    CHECK(gxEncodeIndexTextKey(high, sizeof(high), highKey, sizeof(highKey),
                               &highLength) == GxStatus::Ok);
    CHECK(compareEncodedKeys(lowKey, lowLength, highKey, highLength) < 0);

    std::vector<uint8_t> hosted;
    const std::vector<uint8_t> emptyReference = referenceTextKey(nullptr, 0);
    CHECK(encodeIndexKey(DbType::Text, DbValue::text(""), hosted));
    CHECK(hosted == emptyReference);
    const std::vector<uint8_t> abReference = referenceTextKey(ab, sizeof(ab));
    CHECK(encodeIndexKey(DbType::Text, DbValue::text("AB"), hosted));
    CHECK(hosted == abReference);
}

void testMaximumSupportedKeyLength() {
    const uint32_t maximumEncodedKeyLength = maxIndexKeyBytes(kMaxPageSize);
    CHECK(maximumEncodedKeyLength == kMaxIndexKeyBytesCap);
    const size_t textLength = maximumEncodedKeyLength - 1;
    std::vector<uint8_t> text(textLength, 0x5Au);
    std::vector<uint8_t> encoded(maximumEncodedKeyLength, 0xCCu);
    size_t encodedLength = 0;
    CHECK(gxEncodeIndexTextKey(text.data(), text.size(), encoded.data(),
                               encoded.size(), &encodedLength) == GxStatus::Ok);
    CHECK(encodedLength == maximumEncodedKeyLength);
    CHECK(encoded.front() == 0x01u && encoded.back() == 0x5Au);

    std::vector<uint8_t> hosted;
    const std::vector<uint8_t> reference =
        referenceTextKey(text.data(), text.size());
    const std::string hostedText(reinterpret_cast<const char*>(text.data()),
                                 text.size());
    CHECK(encodeIndexKey(DbType::Text, DbValue::text(hostedText), hosted));
    CHECK(encoded == reference);
    CHECK(hosted == reference);
}

void testBoundsAndMalformedInputs() {
    uint8_t output[] = {0xAAu, 0xBBu, 0xCCu};
    size_t encodedLength = 77;
    const uint8_t byte = 0x42u;

    CHECK(gxEncodeIndexTextKey(&byte, 1, output, 1, &encodedLength) ==
          GxStatus::ResourceLimit);
    CHECK(encodedLength == 0);
    CHECK(output[0] == 0xAAu && output[1] == 0xBBu && output[2] == 0xCCu);

    CHECK(gxEncodeIndexTextKey(nullptr, 1, output, sizeof(output),
                               &encodedLength) == GxStatus::InvalidArgument);
    CHECK(encodedLength == 0 && output[0] == 0xAAu);
    CHECK(gxEncodeIndexTextKey(&byte, 1, output, sizeof(output), nullptr) ==
          GxStatus::InvalidArgument);
    CHECK(gxEncodeIndexTextKey(&byte, 1, nullptr, sizeof(output),
                               &encodedLength) == GxStatus::InvalidArgument);
    CHECK(encodedLength == 0);

    size_t measured = 77;
    CHECK(gxIndexTextKeyEncodedSize(std::numeric_limits<size_t>::max(),
                                    &measured) == GxStatus::Overflow);
    CHECK(measured == 0);
    CHECK(gxEncodeIndexTextKey(&byte, std::numeric_limits<size_t>::max(),
                               output, sizeof(output), &encodedLength) ==
          GxStatus::Overflow);
    CHECK(encodedLength == 0 && output[0] == 0xAAu);
}

void testOverlappingBuffers() {
    uint8_t same[5] = {'A', 0, 'B', 0xEEu, 0xEEu};
    size_t encodedLength = 0;
    CHECK(gxEncodeIndexTextKey(same, 3, same, sizeof(same), &encodedLength) ==
          GxStatus::Ok);
    const uint8_t expected[] = {0x01u, 'A', 0, 'B'};
    CHECK(encodedLength == sizeof(expected));
    CHECK(std::memcmp(same, expected, sizeof(expected)) == 0);

    uint8_t shifted[8] = {0x11u, 0x22u, 'X', 0, 'Y', 0xEEu, 0xEEu, 0xEEu};
    CHECK(gxEncodeIndexTextKey(shifted + 2, 3, shifted, sizeof(shifted),
                               &encodedLength) == GxStatus::Ok);
    const uint8_t shiftedExpected[] = {0x01u, 'X', 0, 'Y'};
    CHECK(encodedLength == sizeof(expected));
    CHECK(std::memcmp(shifted, shiftedExpected, sizeof(shiftedExpected)) == 0);
}

} // namespace

int main() {
    std::printf("NDB1R4P11R3R portable index Text key tests\n");
    testEmptyAndBinaryInputs();
    testOrderingAndHostedCompatibility();
    testMaximumSupportedKeyLength();
    testBoundsAndMalformedInputs();
    testOverlappingBuffers();
    std::printf("NDB1R4P11R3R portable index Text key: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
