#pragma once
// guideXOS SQL -- Phase SQL1
// Fixed-size database page serialization, parsing and integrity checking.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "database_format.h"
#include "database_result.h"

namespace gxos {
namespace db {

struct DatabasePage {
    uint64_t pageId;
    PageType type;
    uint16_t flags;
    uint32_t payloadSize;
    uint32_t generation;
    std::vector<uint8_t> payload;

    DatabasePage();

    // Usable payload bytes in a page of the given size.
    static uint32_t payloadCapacity(uint32_t pageSize);

    // Serializes into `out`, which is resized to exactly `pageSize` bytes and
    // zero-filled first. Computes and stores the page CRC. Returns
    // InvalidArgument when pageSize is unsupported or the payload is too large.
    DbResult serialize(std::vector<uint8_t>& out, uint32_t pageSize) const;

    // Parses and validates one full page of `pageSize` bytes read from index
    // `expectedPageId`. Verifies the page CRC and that the stored page id
    // matches. The payload is copied only after all bounds checks pass.
    static DbResult parse(const uint8_t* bytes, uint32_t pageSize,
                          uint64_t expectedPageId, DatabasePage& out);
};

} // namespace db
} // namespace gxos
