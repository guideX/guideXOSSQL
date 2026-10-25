#ifndef GX_PORTABLE_INDEX_TEXT_KEY_H
#define GX_PORTABLE_INDEX_TEXT_KEY_H

#include <stddef.h>
#include <stdint.h>

#include "gx_status.h"

// Borrowed bytes for one decoded, non-NULL Text key. The view owns no storage;
// the encoded key buffer must remain alive and unchanged while the view is
// used. A zero-length view is valid with either a null or non-null data
// pointer. A nonzero length requires a non-null pointer. No NUL terminator is
// required or implied, and embedded zero bytes are ordinary payload.
struct GxTextByteView {
    const uint8_t* data;
    size_t length;
};

static inline bool gxTextByteViewIsValid(GxTextByteView view) {
    return view.data != 0 || view.length == 0;
}

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

// Decodes the existing Text logical-key format without allocation. Exact
// {0x00} represents SQL NULL; 0x01 followed by any byte sequence represents
// Text (including an empty payload). Text keys have no payload length prefix,
// so all bytes after 0x01 are payload. On success a non-NULL Text view borrows
// bytes from encodedBytes; the caller must keep that buffer alive and
// unchanged. The output slots must not overlap the encoded input buffer.
//
// Both output pointers are required. On failure, available outputs are reset
// to {nullptr, 0} and false. An empty encoding or invalid tag/NULL shape is
// CorruptData; a null input with nonzero length or missing output is
// InvalidArgument. A null input with zero length is a malformed empty key.
GxStatus gxDecodeIndexTextKey(const uint8_t* encodedBytes,
                              size_t encodedLength,
                              GxTextByteView* outText,
                              bool* outIsNull);

#endif
