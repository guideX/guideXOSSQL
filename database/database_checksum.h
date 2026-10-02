#pragma once
// guideXOS SQL -- Phase SQL1
// CRC32 (IEEE 802.3, reflected polynomial 0xEDB88320) used for corruption
// detection of the database header and pages.
//
// This matches the CRC32 already used elsewhere in guideXOS (the web content
// decoder). It is a lightweight integrity check, NOT a cryptographic
// authentication tag. It reliably detects accidental corruption (bit flips,
// truncation that changes covered bytes, stray writes). It does NOT protect
// against deliberate tampering: an attacker who rewrites a page can recompute
// the checksum.

#include <cstddef>
#include <cstdint>

namespace gxos {
namespace db {

uint32_t crc32Init();
uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t length);
uint32_t crc32Final(uint32_t state);

// One-shot CRC32 over a buffer. Returns 0 for a null buffer with length 0.
uint32_t crc32(const uint8_t* data, size_t length);

} // namespace db
} // namespace gxos
