#include "gx_index_boolean_key.h"

GxStatus gxEncodeIndexBooleanKey(bool isNull, bool value, uint8_t* outBytes,
                                 size_t outCapacity,
                                 size_t* outEncodedLength) {
    if (outEncodedLength) *outEncodedLength = 0;
    if (!outEncodedLength || !outBytes) return GxStatus::InvalidArgument;

    const size_t requiredLength = isNull ? 1u : 2u;
    if (outCapacity < requiredLength) return GxStatus::ResourceLimit;

    if (isNull) {
        outBytes[0] = 0x00u;
    } else {
        outBytes[0] = 0x01u;
        outBytes[1] = value ? 0x01u : 0x00u;
    }
    *outEncodedLength = requiredLength;
    return GxStatus::Ok;
}

GxStatus gxDecodeIndexBooleanKey(const uint8_t* encodedBytes,
                                 size_t encodedLength, bool* outValue,
                                 bool* outIsNull) {
    if (outValue) *outValue = false;
    if (outIsNull) *outIsNull = false;
    if (!outValue || !outIsNull) return GxStatus::InvalidArgument;
    if (!encodedBytes) {
        return encodedLength == 0 ? GxStatus::CorruptData
                                  : GxStatus::InvalidArgument;
    }
    if (encodedLength == 0) return GxStatus::CorruptData;

    if (encodedBytes[0] == 0x00u) {
        if (encodedLength != 1) return GxStatus::CorruptData;
        *outIsNull = true;
        return GxStatus::Ok;
    }
    if (encodedBytes[0] != 0x01u || encodedLength != 2 ||
        encodedBytes[1] > 0x01u) {
        return GxStatus::CorruptData;
    }

    *outValue = encodedBytes[1] == 0x01u;
    return GxStatus::Ok;
}
