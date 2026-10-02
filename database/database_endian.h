#pragma once
// guideXOS SQL -- Phase SQL1
// Explicit little-endian serialization helpers and overflow-checked arithmetic.
//
// The database file format is little-endian regardless of host endianness. All
// multi-byte reads/writes go through these helpers; no raw struct is ever
// reinterpreted. Overflow-checked helpers are used for every file offset,
// size and page-count calculation derived from untrusted input.

#include <cstddef>
#include <cstdint>

#include "database_format.h"

namespace gxos {
namespace db {

inline void storeLe16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

inline void storeLe32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

inline void storeLe64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
    }
}

inline uint16_t loadLe16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8);
}

inline uint32_t loadLe32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

inline uint64_t loadLe64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(in[i]) << (8 * i);
    }
    return value;
}

// Overflow-checked addition. Returns false and leaves `out` untouched on
// overflow.
inline bool checkedAddU64(uint64_t a, uint64_t b, uint64_t& out) {
    if (a > UINT64_MAX - b) {
        return false;
    }
    out = a + b;
    return true;
}

// Overflow-checked multiplication. Returns false on overflow.
inline bool checkedMulU64(uint64_t a, uint64_t b, uint64_t& out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }
    out = a * b;
    return true;
}

// True when `pageSize` is a power of two within the supported range.
inline bool isSupportedPageSize(uint32_t pageSize) {
    if (pageSize < kMinPageSize || pageSize > kMaxPageSize) {
        return false;
    }
    return (pageSize & (pageSize - 1u)) == 0u;
}

} // namespace db
} // namespace gxos
