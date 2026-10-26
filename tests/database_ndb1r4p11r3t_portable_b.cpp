#include "gx_index_key.h"

int r3tPortableHeaderConsumerB() {
    const uint8_t source[] = {'x', 0x00u, 'y'};
    uint8_t encoded[sizeof(source) + 1] = {};
    size_t encodedLength = 0;
    GxTextByteView view = {0, 0};
    bool isNull = true;
    return gxEncodeIndexTextKey(source, sizeof(source), encoded,
                                sizeof(encoded), &encodedLength) == GxStatus::Ok &&
                   gxDecodeIndexTextKey(encoded, encodedLength, &view,
                                        &isNull) == GxStatus::Ok &&
                   !isNull && view.data == encoded + 1 &&
                   view.length == sizeof(source) && view.data[1] == 0x00u
        ? 1 : 0;
}
