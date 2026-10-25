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

GxStatus gxDecodeIndexTextKey(const uint8_t* encodedBytes,
                              size_t encodedLength,
                              GxTextByteView* outText,
                              bool* outIsNull) {
    if (outText) {
        outText->data = 0;
        outText->length = 0;
    }
    if (outIsNull) *outIsNull = false;
    if (!outText || !outIsNull) return GxStatus::InvalidArgument;
    if (!encodedBytes) {
        return encodedLength == 0 ? GxStatus::CorruptData
                                  : GxStatus::InvalidArgument;
    }
    if (encodedLength == 0) return GxStatus::CorruptData;

    const uint8_t tag = encodedBytes[0];
    if (tag == 0x00u) {
        if (encodedLength != 1) return GxStatus::CorruptData;
        *outIsNull = true;
        return GxStatus::Ok;
    }
    if (tag != 0x01u) return GxStatus::CorruptData;

    // encodedLength is nonzero, so this subtraction cannot underflow. Text
    // has no stored payload length: the caller-supplied span bounds the view.
    outText->data = encodedBytes + 1;
    outText->length = encodedLength - 1;
    return GxStatus::Ok;
}
