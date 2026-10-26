#ifndef GX_PORTABLE_INDEX_INTEGER_KEY_H
#define GX_PORTABLE_INDEX_INTEGER_KEY_H

#include <stddef.h>
#include <stdint.h>

#include "gx_status.h"

// Encodes the existing Int32 logical-key format: {0x00} for SQL NULL, or
// 0x01 followed by the four-byte big-endian value with its sign bit flipped.
// The caller owns the output buffer. No allocation is performed. The required
// capacity is one byte for NULL and five bytes otherwise. A missing output
// length is InvalidArgument; insufficient capacity is ResourceLimit. The
// output length is cleared on failure and output bytes are unchanged. The
// output-length slot must not overlap the output buffer.
GxStatus gxEncodeIndexInt32Key(bool isNull, int32_t value,
                               uint8_t* outBytes, size_t outCapacity,
                               size_t* outEncodedLength);

// Decodes exactly the existing Int32 logical-key format. NULL is distinct
// from the valid integer value zero. Empty, truncated, extra-byte, unknown-tag,
// and malformed NULL encodings return CorruptData. A null input with a
// nonzero length or missing output is InvalidArgument. Available outputs are
// reset on failure. On success, a NULL result has value zero and outIsNull
// true. Output slots must not overlap each other or the encoded input span.
GxStatus gxDecodeIndexInt32Key(const uint8_t* encodedBytes,
                               size_t encodedLength,
                               int32_t* outValue, bool* outIsNull);

// Encodes the existing Int64 logical-key format: {0x00} for SQL NULL, or
// 0x01 followed by the eight-byte big-endian value with its sign bit flipped.
// The caller owns the output buffer. No allocation is performed. The required
// capacity is one byte for NULL and nine bytes otherwise. A missing output
// length is InvalidArgument; insufficient capacity is ResourceLimit. The
// output length is cleared on failure and output bytes are unchanged. The
// output-length slot must not overlap the output buffer.
GxStatus gxEncodeIndexInt64Key(bool isNull, int64_t value,
                               uint8_t* outBytes, size_t outCapacity,
                               size_t* outEncodedLength);

// Decodes exactly the existing Int64 logical-key format. NULL is distinct
// from the valid integer value zero. Empty, truncated, extra-byte, unknown-tag,
// and malformed NULL encodings return CorruptData. A null input with a
// nonzero length or missing output is InvalidArgument. Available outputs are
// reset on failure. On success, a NULL result has value zero and outIsNull
// true. Output slots must not overlap each other or the encoded input span.
GxStatus gxDecodeIndexInt64Key(const uint8_t* encodedBytes,
                               size_t encodedLength,
                               int64_t* outValue, bool* outIsNull);

#endif
