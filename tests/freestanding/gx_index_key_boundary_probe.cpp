#include "gx_index_key.h"

// Freestanding compile/link probe for the production portable index-key API.
// Keeping the encoded bytes on this stack frame makes the lifetime underlying
// the borrowed decode view explicit for the entire use of that view.
extern "C" int gxIndexKeyBoundaryProbe() {
    const uint8_t textBytes[] = {'a', 0x00u, 'b'};
    uint8_t encodedBytes[sizeof(textBytes) + 1] = {};
    size_t encodedLength = 0;
    GxTextByteView text = {nullptr, 0};
    bool isNull = true;
    int comparison = 0;

    if (gxEncodeIndexTextKey(textBytes, sizeof(textBytes), encodedBytes,
                             sizeof(encodedBytes), &encodedLength) !=
        GxStatus::Ok) {
        return 1;
    }
    if (gxDecodeIndexTextKey(encodedBytes, encodedLength, &text, &isNull) !=
            GxStatus::Ok ||
        isNull || text.data != encodedBytes + 1 ||
        text.length != sizeof(textBytes)) {
        return 2;
    }
    if (gxIndexKeyIsNull(encodedBytes, encodedLength, &isNull) !=
            GxStatus::Ok ||
        isNull) {
        return 3;
    }
    if (gxCompareIndexKeys(text.data, text.length, textBytes,
                           sizeof(textBytes), &comparison) != GxStatus::Ok ||
        comparison != 0) {
        return 4;
    }

    encodedBytes[1] = 'z';
    if (text.data[0] != 'z' || text.data[1] != 0x00u ||
        text.data[2] != 'b') {
        return 5;
    }

    const uint8_t int32Minimum[] = {0x01u, 0x00u, 0x00u, 0x00u, 0x00u};
    int32_t decodedInt32 = 0;
    if (gxDecodeIndexInt32Key(int32Minimum, sizeof(int32Minimum),
                              &decodedInt32, &isNull) != GxStatus::Ok ||
        isNull || decodedInt32 != INT32_MIN) {
        return 6;
    }

    uint8_t encodedInt64[9] = {};
    size_t encodedInt64Length = 0;
    int64_t decodedInt64 = 0;
    if (gxEncodeIndexInt64Key(false, -1, encodedInt64,
                              sizeof(encodedInt64), &encodedInt64Length) !=
            GxStatus::Ok ||
        encodedInt64Length != sizeof(encodedInt64) ||
        gxDecodeIndexInt64Key(encodedInt64, encodedInt64Length,
                              &decodedInt64, &isNull) != GxStatus::Ok ||
        isNull || decodedInt64 != -1) {
        return 7;
    }

    uint8_t encodedInt32[5] = {};
    size_t encodedInt32Length = 0;
    if (gxEncodeIndexInt32Key(false, INT32_MAX, encodedInt32,
                              sizeof(encodedInt32), &encodedInt32Length) !=
            GxStatus::Ok ||
        encodedInt32Length != sizeof(encodedInt32) ||
        encodedInt32[0] != 0x01u || encodedInt32[1] != 0xFFu ||
        encodedInt32[2] != 0xFFu || encodedInt32[3] != 0xFFu ||
        encodedInt32[4] != 0xFFu) {
        return 8;
    }
    return 0;
}
