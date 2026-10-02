#include "database_header.h"

#include <chrono>
#include <cstring>
#include <random>

#include "database_checksum.h"
#include "database_endian.h"

namespace gxos {
namespace db {

void generateDatabaseId(uint8_t out[16]) {
    std::random_device rd;
    uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    seed ^= currentUnixTimeNanos();
    std::mt19937_64 engine(seed);
    for (int i = 0; i < 16; i += 8) {
        const uint64_t chunk = engine();
        for (int b = 0; b < 8; ++b) {
            out[i + b] = static_cast<uint8_t>((chunk >> (8 * b)) & 0xFFu);
        }
    }
    // RFC 4122 version 4 and variant bits.
    out[6] = static_cast<uint8_t>((out[6] & 0x0Fu) | 0x40u);
    out[8] = static_cast<uint8_t>((out[8] & 0x3Fu) | 0x80u);
}

std::string formatDatabaseId(const uint8_t id[16]) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(hex[(id[i] >> 4) & 0x0Fu]);
        out.push_back(hex[id[i] & 0x0Fu]);
    }
    return out;
}

uint64_t currentUnixTimeNanos() {
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const std::chrono::nanoseconds ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
    if (ns.count() < 0) {
        return 0;
    }
    return static_cast<uint64_t>(ns.count());
}

DatabaseHeader::DatabaseHeader()
    : formatMajor(kFormatMajor),
      formatMinor(kFormatMinor),
      pageSize(kDefaultPageSize),
      creationTimeUnixNanos(0),
      pageCount(2),
      rootPageId(kBootstrapPageId),
      flags(header_flags::None),
      headerSize(kHeaderSize),
      storedCrc32(0) {
    std::memset(databaseId, 0, sizeof(databaseId));
}

DatabaseHeader DatabaseHeader::makeDefault(uint32_t pageSizeIn,
                                           const uint8_t id[16],
                                           uint64_t creationTimeUnixNanosIn) {
    DatabaseHeader header;
    header.formatMajor = kFormatMajor;
    header.formatMinor = kFormatMinor;
    header.pageSize = pageSizeIn;
    std::memcpy(header.databaseId, id, 16);
    header.creationTimeUnixNanos = creationTimeUnixNanosIn;
    header.pageCount = 2; // header page + bootstrap page
    header.rootPageId = kBootstrapPageId;
    header.flags = header_flags::None;
    header.headerSize = kHeaderSize;
    header.storedCrc32 = 0;
    return header;
}

uint32_t DatabaseHeader::computeCrc() const {
    uint8_t scratch[kHeaderSize];
    std::memset(scratch, 0, sizeof(scratch));
    std::memcpy(scratch + header_offset::Magic, kFileMagic, 8);
    storeLe16(scratch + header_offset::FormatMajor, formatMajor);
    storeLe16(scratch + header_offset::FormatMinor, formatMinor);
    storeLe32(scratch + header_offset::PageSize, pageSize);
    std::memcpy(scratch + header_offset::DatabaseId, databaseId, 16);
    storeLe64(scratch + header_offset::CreationTime, creationTimeUnixNanos);
    storeLe64(scratch + header_offset::PageCount, pageCount);
    storeLe64(scratch + header_offset::RootPageId, rootPageId);
    storeLe32(scratch + header_offset::Flags, flags);
    storeLe32(scratch + header_offset::HeaderSize, headerSize);
    // HeaderCrc32 remains zero for the duration of the computation.
    return crc32(scratch, kHeaderSize);
}

void DatabaseHeader::serialize(uint8_t* out) const {
    if (out == nullptr) {
        return;
    }
    std::memset(out, 0, kHeaderSize);
    std::memcpy(out + header_offset::Magic, kFileMagic, 8);
    storeLe16(out + header_offset::FormatMajor, formatMajor);
    storeLe16(out + header_offset::FormatMinor, formatMinor);
    storeLe32(out + header_offset::PageSize, pageSize);
    std::memcpy(out + header_offset::DatabaseId, databaseId, 16);
    storeLe64(out + header_offset::CreationTime, creationTimeUnixNanos);
    storeLe64(out + header_offset::PageCount, pageCount);
    storeLe64(out + header_offset::RootPageId, rootPageId);
    storeLe32(out + header_offset::Flags, flags);
    storeLe32(out + header_offset::HeaderSize, headerSize);
    storeLe32(out + header_offset::HeaderCrc32, computeCrc());
}

DbResult DatabaseHeader::parse(const uint8_t* bytes, size_t length,
                               DatabaseHeader& out) {
    if (bytes == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "null header buffer");
    }
    if (length < kHeaderSize) {
        return DbResult::error(DbStatus::Truncated, "header shorter than kHeaderSize");
    }

    if (std::memcmp(bytes + header_offset::Magic, kFileMagic, 8) != 0) {
        return DbResult::error(DbStatus::NotDatabase, "file signature does not match GXDB");
    }

    const uint16_t major = loadLe16(bytes + header_offset::FormatMajor);
    const uint16_t minor = loadLe16(bytes + header_offset::FormatMinor);
    if (major != kFormatMajor) {
        return DbResult::error(DbStatus::UnsupportedVersion,
                               "unsupported format major version " +
                                   std::to_string(static_cast<unsigned>(major)));
    }

    const uint32_t pageSize = loadLe32(bytes + header_offset::PageSize);
    if (!isSupportedPageSize(pageSize)) {
        return DbResult::error(DbStatus::CorruptHeader,
                               "invalid page size " + std::to_string(pageSize));
    }

    const uint32_t headerSize = loadLe32(bytes + header_offset::HeaderSize);
    if (headerSize != kHeaderSize) {
        return DbResult::error(DbStatus::CorruptHeader,
                               "unexpected header size " + std::to_string(headerSize));
    }

    DatabaseHeader candidate;
    candidate.formatMajor = major;
    candidate.formatMinor = minor;
    candidate.pageSize = pageSize;
    std::memcpy(candidate.databaseId, bytes + header_offset::DatabaseId, 16);
    candidate.creationTimeUnixNanos = loadLe64(bytes + header_offset::CreationTime);
    candidate.pageCount = loadLe64(bytes + header_offset::PageCount);
    candidate.rootPageId = loadLe64(bytes + header_offset::RootPageId);
    candidate.flags = loadLe32(bytes + header_offset::Flags);
    candidate.headerSize = headerSize;
    candidate.storedCrc32 = loadLe32(bytes + header_offset::HeaderCrc32);

    const uint32_t expectedCrc = candidate.computeCrc();
    if (expectedCrc != candidate.storedCrc32) {
        return DbResult::error(DbStatus::CorruptHeader, "header checksum mismatch");
    }

    if (candidate.pageCount < 2) {
        return DbResult::error(DbStatus::CorruptHeader, "page count below minimum of 2");
    }
    if (candidate.pageCount > kMaxPageCount) {
        return DbResult::error(DbStatus::CorruptHeader, "page count exceeds policy maximum");
    }
    if (candidate.rootPageId < 1 || candidate.rootPageId >= candidate.pageCount) {
        return DbResult::error(DbStatus::CorruptHeader, "root page id outside allocated range");
    }

    out = candidate;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
