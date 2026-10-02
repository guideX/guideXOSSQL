#pragma once
// guideXOS SQL -- Phase SQL2
// Strict UTF-8 validation for identifier names.
//
// Policy: table and column names must be valid UTF-8. The validator rejects
// overlong encodings, UTF-16 surrogates, code points above U+10FFFF,
// truncated sequences and stray continuation bytes. No normalization is
// performed; bytes are compared verbatim (case-sensitive).

#include <cstddef>
#include <cstdint>

namespace gxos {
namespace db {

inline bool isValidUtf8(const uint8_t* data, size_t length) {
    size_t i = 0;
    while (i < length) {
        const uint8_t c = data[i];
        if (c < 0x80u) {
            ++i;
            continue;
        }
        int bytes;
        uint32_t cp;
        if ((c & 0xE0u) == 0xC0u) {
            bytes = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0u) == 0xE0u) {
            bytes = 3;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8u) == 0xF0u) {
            bytes = 4;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + static_cast<size_t>(bytes) > length) {
            return false;
        }
        for (int j = 1; j < bytes; ++j) {
            const uint8_t cc = data[i + static_cast<size_t>(j)];
            if ((cc & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (bytes == 2 && cp < 0x80u) return false;
        if (bytes == 3 && cp < 0x800u) return false;
        if (bytes == 4 && cp < 0x10000u) return false;
        if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
        if (cp > 0x10FFFFu) return false;
        i += static_cast<size_t>(bytes);
    }
    return true;
}

} // namespace db
} // namespace gxos
