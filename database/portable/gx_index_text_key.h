#ifndef GX_PORTABLE_INDEX_TEXT_KEY_H
#define GX_PORTABLE_INDEX_TEXT_KEY_H

#include <stddef.h>
#include <stdint.h>

#include "gx_status.h"

// Returns the encoded byte count for a Text logical key, including the 0x01
// non-NULL tag. The payload length is explicit; no terminator is included.
GxStatus gxIndexTextKeyEncodedSize(size_t textLength, size_t* outEncodedLength);

// Encodes one Text logical key as 0x01 followed by exactly textLength bytes.
// The caller owns both buffers. No allocation is performed. Capacity and
// pointer failures leave output bytes unchanged and set *outEncodedLength to
// zero. Input and output may overlap.
GxStatus gxEncodeIndexTextKey(const uint8_t* textBytes, size_t textLength,
                              uint8_t* outBytes, size_t outCapacity,
                              size_t* outEncodedLength);

#endif
