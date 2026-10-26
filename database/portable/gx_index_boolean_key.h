#ifndef GX_PORTABLE_INDEX_BOOLEAN_KEY_H
#define GX_PORTABLE_INDEX_BOOLEAN_KEY_H

#include <stddef.h>
#include <stdint.h>

#include "gx_status.h"

// Encodes the existing Boolean logical-key format. SQL NULL is the single
// byte {0x00}; false and true are {0x01, 0x00} and {0x01, 0x01}.
// The caller owns the output buffer. Capacity and pointer failures leave the
// buffer unchanged and set *outEncodedLength to zero.
GxStatus gxEncodeIndexBooleanKey(bool isNull, bool value, uint8_t* outBytes,
                                 size_t outCapacity,
                                 size_t* outEncodedLength);

// Decodes only the existing canonical Boolean key representations. The two
// distinct output pointers are required and must not overlap encodedBytes.
// On failure, available outputs are reset to false. A null input with zero
// length is a malformed empty key; a null input with nonzero length is
// InvalidArgument. NULL and false remain distinct via outIsNull.
GxStatus gxDecodeIndexBooleanKey(const uint8_t* encodedBytes,
                                 size_t encodedLength, bool* outValue,
                                 bool* outIsNull);

#endif
