#include "gx_page_codec.h"

#include "database_checksum.h"
#include "database_endian.h"
#include "database_format.h"

namespace {

void fillZero(uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}

void copyBytes(uint8_t* destination, const uint8_t* source, size_t length) {
    for (size_t i = 0; i < length; ++i) destination[i] = source[i];
}

void storeLe16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void storeLe32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void storeLe64(uint8_t* out, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t computePageCrc(const uint8_t* bytes, uint32_t pageSize) {
    uint32_t crc = gxos::db::crc32Init();
    crc = gxos::db::crc32Update(crc, bytes, gxos::db::page_offset::PageCrc32);
    const uint8_t zero[4] = {0, 0, 0, 0};
    crc = gxos::db::crc32Update(crc, zero, sizeof(zero));
    const uint32_t tailOffset = gxos::db::page_offset::PageCrc32 + 4;
    crc = gxos::db::crc32Update(crc, bytes + tailOffset,
                                pageSize - tailOffset);
    return gxos::db::crc32Final(crc);
}

} // namespace

GxStatus gxDatabasePageEncode(uint8_t* output, uint32_t pageSize,
                              uint64_t pageId, uint16_t pageType,
                              uint16_t flags, uint32_t generation,
                              const uint8_t* payload, size_t payloadLength) {
    if (output == 0 ||
        !gxos::db::isSupportedPageSize(pageSize) ||
        (payloadLength != 0 && payload == 0) ||
        payloadLength > pageSize - gxos::db::kPageHeaderSize) {
        return GxStatus::InvalidArgument;
    }

    fillZero(output, pageSize);
    storeLe64(output + gxos::db::page_offset::PageId, pageId);
    storeLe16(output + gxos::db::page_offset::PageType, pageType);
    storeLe16(output + gxos::db::page_offset::Flags, flags);
    storeLe32(output + gxos::db::page_offset::PayloadSize,
              static_cast<uint32_t>(payloadLength));
    storeLe32(output + gxos::db::page_offset::Generation, generation);
    if (payloadLength != 0) {
        copyBytes(output + gxos::db::kPageHeaderSize, payload, payloadLength);
    }

    storeLe32(output + gxos::db::page_offset::PageCrc32,
              computePageCrc(output, pageSize));
    return GxStatus::Ok;
}
