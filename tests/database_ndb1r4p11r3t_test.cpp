#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "database_index.h"
#include "database_types.h"
#include "gx_index_key.h"

using namespace gxos::db;

int r3tPortableHeaderConsumerA();
int r3tPortableHeaderConsumerB();

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

void testPortableKeyComparisonAndNullPredicate() {
    const uint8_t nullKey[] = {0x00u};
    const uint8_t emptyText[] = {0x01u};
    const uint8_t prefix[] = {0x01u, 'a'};
    const uint8_t extended[] = {0x01u, 'a', 'b'};
    const uint8_t low[] = {0x01u, 0x7Fu};
    const uint8_t high[] = {0x01u, 0x80u};
    bool isNull = true;
    int comparison = 42;

    CHECK(gxIndexKeyIsNull(nullKey, sizeof(nullKey), &isNull) == GxStatus::Ok);
    CHECK(isNull);
    CHECK(gxIndexKeyIsNull(emptyText, sizeof(emptyText), &isNull) ==
          GxStatus::Ok);
    CHECK(!isNull);
    CHECK(gxIndexKeyIsNull(nullptr, 0, &isNull) == GxStatus::Ok && !isNull);
    CHECK(gxIndexKeyIsNull(nullptr, 1, &isNull) ==
          GxStatus::InvalidArgument);
    CHECK(!isNull);
    CHECK(gxIndexKeyIsNull(nullKey, sizeof(nullKey), nullptr) ==
          GxStatus::InvalidArgument);

    CHECK(gxCompareIndexKeys(nullptr, 0, nullptr, 0, &comparison) ==
          GxStatus::Ok);
    CHECK(comparison == 0);
    CHECK(gxCompareIndexKeys(nullKey, sizeof(nullKey), emptyText,
                             sizeof(emptyText), &comparison) == GxStatus::Ok);
    CHECK(comparison < 0);
    CHECK(gxCompareIndexKeys(prefix, sizeof(prefix), extended,
                             sizeof(extended), &comparison) == GxStatus::Ok);
    CHECK(comparison == -1);
    CHECK(gxCompareIndexKeys(high, sizeof(high), low, sizeof(low),
                             &comparison) == GxStatus::Ok);
    CHECK(comparison == 1);
    CHECK(gxCompareIndexKeys(prefix, sizeof(prefix), prefix,
                             sizeof(prefix), &comparison) == GxStatus::Ok);
    CHECK(comparison == 0);
    comparison = 42;
    CHECK(gxCompareIndexKeys(nullptr, 1, prefix, sizeof(prefix),
                             &comparison) == GxStatus::InvalidArgument);
    CHECK(comparison == 0);
    CHECK(gxCompareIndexKeys(prefix, sizeof(prefix), extended,
                             sizeof(extended), nullptr) ==
          GxStatus::InvalidArgument);

    CHECK(encodedKeyIsNull(nullKey, sizeof(nullKey)));
    CHECK(!encodedKeyIsNull(emptyText, sizeof(emptyText)));
    CHECK(compareEncodedKeys(prefix, sizeof(prefix), extended,
                             sizeof(extended)) == -1);
    CHECK(compareEncodedKeys(high, sizeof(high), low, sizeof(low)) == 1);
}

void testTextKeyBoundaryAndHostedAdapter() {
    const std::string text("a\0b", 3);
    DbValue value = DbValue::text(text);
    std::vector<uint8_t> hostedEncoding;
    CHECK(encodeIndexKey(DbType::Text, value, hostedEncoding));

    uint8_t portableEncoding[4] = {0xCCu, 0xCCu, 0xCCu, 0xCCu};
    size_t written = 99;
    CHECK(gxEncodeIndexTextKey(
              reinterpret_cast<const uint8_t*>(text.data()), text.size(),
              portableEncoding, sizeof(portableEncoding), &written) ==
          GxStatus::Ok);
    CHECK(written == sizeof(portableEncoding));
    CHECK(hostedEncoding.size() == sizeof(portableEncoding));
    CHECK(std::memcmp(hostedEncoding.data(), portableEncoding,
                      sizeof(portableEncoding)) == 0);

    GxTextByteView borrowed = {nullptr, 0};
    bool isNull = true;
    CHECK(gxDecodeIndexTextKey(portableEncoding, written, &borrowed,
                               &isNull) == GxStatus::Ok);
    CHECK(!isNull && borrowed.data == portableEncoding + 1);
    CHECK(borrowed.length == text.size());
    CHECK(std::memcmp(borrowed.data, text.data(), text.size()) == 0);
    portableEncoding[1] = 'z';
    CHECK(borrowed.data[0] == 'z');
    CHECK(borrowed.data[1] == 0 && borrowed.data[2] == 'b');

    DbValue decoded = DbValue::int64(3);
    CHECK(decodeIndexKey(DbType::Text, hostedEncoding.data(),
                         hostedEncoding.size(), decoded));
    CHECK(!decoded.isNull() && decoded.type() == DbType::Text);
    CHECK(decoded.textValue() == text);

    const uint8_t nullKey[] = {0x00u};
    CHECK(decodeIndexKey(DbType::Text, nullKey, sizeof(nullKey), decoded));
    CHECK(decoded.isNull());

    const uint8_t unknownTag[] = {0x02u, 'x'};
    decoded = DbValue::text("preserved");
    CHECK(!decodeIndexKey(DbType::Text, unknownTag, sizeof(unknownTag),
                          decoded));
    CHECK(decoded.type() == DbType::Text &&
          decoded.textValue() == "preserved");

    GxTextByteView malformedView = {
        reinterpret_cast<const uint8_t*>("old"), 3};
    isNull = true;
    CHECK(gxDecodeIndexTextKey(nullptr, 0, &malformedView, &isNull) ==
          GxStatus::CorruptData);
    CHECK(malformedView.data == nullptr && malformedView.length == 0 &&
          !isNull);
    const uint8_t nullWithTrailingByte[] = {0x00u, 'x'};
    CHECK(gxDecodeIndexTextKey(nullWithTrailingByte,
                               sizeof(nullWithTrailingByte), &malformedView,
                               &isNull) == GxStatus::CorruptData);
    const uint8_t emptyText[] = {0x01u};
    CHECK(gxDecodeIndexTextKey(emptyText, sizeof(emptyText), &borrowed,
                               &isNull) == GxStatus::Ok);
    CHECK(!isNull && borrowed.length == 0 &&
          borrowed.data == emptyText + 1);

    // Text keys have no payload length field. A shorter byte suffix is a
    // valid shorter Text key, so truncation beyond the supplied span is not
    // distinguishable from a different value.
    const uint8_t shorterText[] = {0x01u, 'a'};
    CHECK(gxDecodeIndexTextKey(shorterText, sizeof(shorterText), &borrowed,
                               &isNull) == GxStatus::Ok);
    CHECK(!isNull && borrowed.length == 1 && borrowed.data[0] == 'a');
}

} // namespace

int main() {
    std::printf("NDB1R4P11R3T portable index key boundary tests\n");
    testPortableKeyComparisonAndNullPredicate();
    testTextKeyBoundaryAndHostedAdapter();
    CHECK(r3tPortableHeaderConsumerA() == 1);
    CHECK(r3tPortableHeaderConsumerB() == 1);
    std::printf("NDB1R4P11R3T portable index key boundary: %d passed, %d failed\n",
                gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
