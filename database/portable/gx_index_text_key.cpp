#include "gx_index_text_key.h"

static void gxMoveIndexTextBytes(uint8_t* destination, const uint8_t* source,
                                 size_t count) {
    if (count == 0 || destination == source) return;

    const uintptr_t destinationAddress =
        reinterpret_cast<uintptr_t>(destination);
    const uintptr_t sourceAddress = reinterpret_cast<uintptr_t>(source);
    if (destinationAddress > sourceAddress &&
        destinationAddress - sourceAddress < count) {
        for (size_t i = count; i != 0; --i) {
            destination[i - 1] = source[i - 1];
        }
    } else {
        for (size_t i = 0; i < count; ++i) destination[i] = source[i];
    }
}

GxStatus gxIndexTextKeyEncodedSize(size_t textLength,
                                   size_t* outEncodedLength) {
    if (!outEncodedLength) return GxStatus::InvalidArgument;
    *outEncodedLength = 0;
    if (textLength == static_cast<size_t>(-1)) return GxStatus::Overflow;
    *outEncodedLength = textLength + 1;
    return GxStatus::Ok;
}

GxStatus gxEncodeIndexTextKey(const uint8_t* textBytes, size_t textLength,
                              uint8_t* outBytes, size_t outCapacity,
                              size_t* outEncodedLength) {
    if (!outEncodedLength) return GxStatus::InvalidArgument;
    *outEncodedLength = 0;

    size_t required = 0;
    const GxStatus sizeStatus =
        gxIndexTextKeyEncodedSize(textLength, &required);
    if (sizeStatus != GxStatus::Ok) return sizeStatus;
    if (!outBytes || (textLength != 0 && !textBytes)) {
        return GxStatus::InvalidArgument;
    }
    if (outCapacity < required) return GxStatus::ResourceLimit;

    // Move first so an aliased input beginning at outBytes remains intact until
    // its payload has been copied. The final tag write cannot damage the copy.
    gxMoveIndexTextBytes(outBytes + 1, textBytes, textLength);
    outBytes[0] = 0x01u;
    *outEncodedLength = required;
    return GxStatus::Ok;
}
