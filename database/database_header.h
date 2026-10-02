#pragma once
// guideXOS SQL -- Phase SQL1
// Database header (page 0) serialization, parsing and validation.

#include <cstddef>
#include <cstdint>
#include <string>

#include "database_format.h"
#include "database_result.h"

namespace gxos {
namespace db {

// Fills `out` (16 bytes) with a random RFC 4122 version-4 database identity.
void generateDatabaseId(uint8_t out[16]);

// Canonical lowercase 8-4-4-4-12 representation of a 16-byte identity.
std::string formatDatabaseId(const uint8_t id[16]);

// Nanoseconds since the Unix epoch, or 0 if the platform clock is unavailable.
uint64_t currentUnixTimeNanos();

struct DatabaseHeader {
    uint16_t formatMajor;
    uint16_t formatMinor;
    uint32_t pageSize;
    uint8_t databaseId[16];
    uint64_t creationTimeUnixNanos;
    uint64_t pageCount;
    uint64_t rootPageId;
    uint32_t flags;
    uint32_t headerSize;
    uint32_t storedCrc32;

    DatabaseHeader();

    static DatabaseHeader makeDefault(uint32_t pageSize, const uint8_t id[16],
                                      uint64_t creationTimeUnixNanos);

    // Writes exactly kHeaderSize bytes into `out`. The reserved tail of the
    // header is zeroed. Does not touch bytes beyond kHeaderSize.
    void serialize(uint8_t* out) const;

    // Computes the CRC32 over the first kHeaderSize bytes with the CRC field
    // treated as zero.
    uint32_t computeCrc() const;

    // Parses and validates exactly kHeaderSize bytes. `length` must be at least
    // kHeaderSize. Structural cross-checks that depend on the file size (page
    // alignment, file/page-count agreement) are performed by DatabaseFile.
    static DbResult parse(const uint8_t* bytes, size_t length, DatabaseHeader& out);
};

} // namespace db
} // namespace gxos
