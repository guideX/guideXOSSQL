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
    return 0;
}
