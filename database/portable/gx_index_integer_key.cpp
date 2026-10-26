#include "gx_index_integer_key.h"

namespace {

GxStatus encodeIntegerKey(bool isNull, uint64_t orderedValue,
                          size_t payloadLength, uint8_t* outBytes,
                          size_t outCapacity, size_t* outEncodedLength) {
    if (outEncodedLength) *outEncodedLength = 0;
    if (!outEncodedLength || !outBytes) return GxStatus::InvalidArgument;

    const size_t requiredLength = isNull ? 1 : payloadLength + 1;
    if (outCapacity < requiredLength) return GxStatus::ResourceLimit;

    if (isNull) {
        outBytes[0] = 0x00u;
    } else {
        outBytes[0] = 0x01u;
        for (size_t i = 0; i < payloadLength; ++i) {
            const size_t shift = (payloadLength - i - 1) * 8;
            outBytes[i + 1] =
                static_cast<uint8_t>((orderedValue >> shift) & 0xFFu);
        }
    }
    *outEncodedLength = requiredLength;
    return GxStatus::Ok;
}

GxStatus decodeIntegerKey(const uint8_t* encodedBytes, size_t encodedLength,
                          size_t payloadLength,
                          uint64_t* outOrderedValue, bool* outIsNull) {
    if (outOrderedValue) *outOrderedValue = 0;
    if (outIsNull) *outIsNull = false;
    if (!outOrderedValue || !outIsNull) return GxStatus::InvalidArgument;
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
    if (tag != 0x01u || encodedLength != payloadLength + 1) {
        return GxStatus::CorruptData;
    }

    uint64_t encodedValue = 0;
    for (size_t i = 0; i < payloadLength; ++i) {
        encodedValue = (encodedValue << 8) | encodedBytes[i + 1];
    }
    *outOrderedValue = encodedValue;
    return GxStatus::Ok;
}

int32_t int32FromOrdered(uint32_t orderedValue) {
    const uint32_t signBit = 0x80000000u;
    if (orderedValue >= signBit) {
        return static_cast<int32_t>(orderedValue - signBit);
    }

    const uint32_t magnitude = signBit - orderedValue;
    const int32_t maximum = static_cast<int32_t>(0x7FFFFFFFu);
    if (magnitude == signBit) return -maximum - 1;
    return -static_cast<int32_t>(magnitude);
}

int64_t int64FromOrdered(uint64_t orderedValue) {
    const uint64_t signBit = 0x8000000000000000ull;
    if (orderedValue >= signBit) {
        return static_cast<int64_t>(orderedValue - signBit);
    }

    const uint64_t magnitude = signBit - orderedValue;
    const int64_t maximum = static_cast<int64_t>(0x7FFFFFFFFFFFFFFFull);
    if (magnitude == signBit) return -maximum - 1;
    return -static_cast<int64_t>(magnitude);
}

} // namespace

GxStatus gxEncodeIndexInt32Key(bool isNull, int32_t value,
                               uint8_t* outBytes, size_t outCapacity,
                               size_t* outEncodedLength) {
    const uint32_t ordered =
        static_cast<uint32_t>(value) ^ 0x80000000u;
    return encodeIntegerKey(isNull, ordered, 4, outBytes, outCapacity,
                            outEncodedLength);
}

GxStatus gxDecodeIndexInt32Key(const uint8_t* encodedBytes,
                               size_t encodedLength,
                               int32_t* outValue, bool* outIsNull) {
    if (outValue) *outValue = 0;
    if (outIsNull) *outIsNull = false;
    if (!outValue || !outIsNull) return GxStatus::InvalidArgument;

    uint64_t ordered = 0;
    const GxStatus status = decodeIntegerKey(
        encodedBytes, encodedLength, 4, &ordered, outIsNull);
    if (status != GxStatus::Ok) return status;
    if (!*outIsNull) *outValue = int32FromOrdered(static_cast<uint32_t>(ordered));
    return GxStatus::Ok;
}

GxStatus gxEncodeIndexInt64Key(bool isNull, int64_t value,
                               uint8_t* outBytes, size_t outCapacity,
                               size_t* outEncodedLength) {
    const uint64_t ordered =
        static_cast<uint64_t>(value) ^ 0x8000000000000000ull;
    return encodeIntegerKey(isNull, ordered, 8, outBytes, outCapacity,
                            outEncodedLength);
}

GxStatus gxDecodeIndexInt64Key(const uint8_t* encodedBytes,
                               size_t encodedLength,
                               int64_t* outValue, bool* outIsNull) {
    if (outValue) *outValue = 0;
    if (outIsNull) *outIsNull = false;
    if (!outValue || !outIsNull) return GxStatus::InvalidArgument;

    uint64_t ordered = 0;
    const GxStatus status = decodeIntegerKey(
        encodedBytes, encodedLength, 8, &ordered, outIsNull);
    if (status != GxStatus::Ok) return status;
    if (!*outIsNull) *outValue = int64FromOrdered(ordered);
    return GxStatus::Ok;
}
