#include "gx_wal_page_image.h"

#include "database_checksum.h"
#include "database_endian.h"
#include "database_format.h"
#include "gx_page_codec.h"
#include "gx_wal_record.h"

namespace {

void fillZero(uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) bytes[i] = 0;
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

uint32_t computeRecordCrc(const uint8_t* record, size_t payloadLength) {
    uint32_t crc = gxos::db::crc32Init();
    crc = gxos::db::crc32Update(crc, record, GX_WAL_RECORD_CRC_OFFSET);
    const uint8_t zero[4] = {0, 0, 0, 0};
    crc = gxos::db::crc32Update(crc, zero, sizeof(zero));
    crc = gxos::db::crc32Update(
        crc, record + GX_WAL_RECORD_PREFIX_SIZE, payloadLength);
    return gxos::db::crc32Final(crc);
}

} // namespace

GxStatus gxWalPageImageRecordEncodeImage(GxArray* output,
                                         uint64_t transactionId,
                                         uint64_t pageId,
                                         const uint8_t* pageImage,
                                         size_t pageImageLength,
                                         uint32_t pageSize,
                                         uint32_t maximumWalPayload) {
    if (output == 0 || !output->initialized || output->elementSize != 1 ||
        !gxos::db::isSupportedPageSize(pageSize) || pageId == 0 ||
        pageId >= gxos::db::kMaxPageCount || pageImage == 0 ||
        pageImageLength != pageSize) {
        return GxStatus::InvalidArgument;
    }
    GxDatabasePageInfo info = {};
    const GxStatus inspect = gxDatabasePageInspect(
        pageImage, pageImageLength, pageSize, pageId, &info);
    if (inspect != GxStatus::Ok) return inspect;

    const size_t payloadLength = GX_WAL_PAGE_IMAGE_HEADER_SIZE +
                                 static_cast<size_t>(pageSize);
    if (payloadLength > maximumWalPayload) return GxStatus::ResourceLimit;
    if (payloadLength > static_cast<size_t>(-1) - GX_WAL_RECORD_PREFIX_SIZE) {
        return GxStatus::Overflow;
    }
    const size_t recordLength = GX_WAL_RECORD_PREFIX_SIZE + payloadLength;
    if (recordLength > output->maximum) return GxStatus::ResourceLimit;

    const GxStatus reserveStatus = gxArrayReserve(output, recordLength);
    if (reserveStatus != GxStatus::Ok) return reserveStatus;
    uint8_t* record = output->data;
    fillZero(record, recordLength);
    uint8_t* walPayload = record + GX_WAL_RECORD_PREFIX_SIZE;
    storeLe16(walPayload, info.pageType);
    storeLe16(walPayload + 2, 0);
    storeLe64(walPayload + 4, pageId);
    for (size_t i = 0; i < pageSize; ++i) {
        walPayload[GX_WAL_PAGE_IMAGE_HEADER_SIZE + i] = pageImage[i];
    }
    record[0] = 2;
    storeLe32(record + 4, static_cast<uint32_t>(payloadLength));
    storeLe64(record + 8, transactionId);
    storeLe32(record + GX_WAL_RECORD_CRC_OFFSET,
              computeRecordCrc(record, payloadLength));
    output->size = recordLength;
    return GxStatus::Ok;
}

GxStatus gxWalPageImageRecordEncode(GxArray* output, uint64_t transactionId,
                                    uint64_t pageId, uint16_t pageType,
                                    uint16_t pageFlags,
                                    uint32_t pageGeneration,
                                    const uint8_t* pagePayload,
                                    size_t pagePayloadLength,
                                    uint32_t pageSize,
                                    uint32_t maximumWalPayload) {
    if (output == 0 || !output->initialized || output->elementSize != 1 ||
        !gxos::db::isSupportedPageSize(pageSize) ||
        pageId == gxos::db::kHeaderPageId ||
        pageId >= gxos::db::kMaxPageCount ||
        (pagePayloadLength != 0 && pagePayload == 0) ||
        pagePayloadLength > pageSize - gxos::db::kPageHeaderSize) {
        return GxStatus::InvalidArgument;
    }

    const size_t pageImageHeaderSize = GX_WAL_PAGE_IMAGE_HEADER_SIZE;
    if (static_cast<size_t>(pageSize) >
        static_cast<size_t>(-1) - pageImageHeaderSize) {
        return GxStatus::Overflow;
    }
    const size_t payloadLength = pageImageHeaderSize + pageSize;
    if (payloadLength > maximumWalPayload) return GxStatus::ResourceLimit;
    if (payloadLength > static_cast<size_t>(-1) - GX_WAL_RECORD_PREFIX_SIZE) {
        return GxStatus::Overflow;
    }
    const size_t recordLength = GX_WAL_RECORD_PREFIX_SIZE + payloadLength;
    if (recordLength > output->maximum) return GxStatus::ResourceLimit;

    const GxStatus reserveStatus = gxArrayReserve(output, recordLength);
    if (reserveStatus != GxStatus::Ok) return reserveStatus;

    uint8_t* record = output->data;
    fillZero(record, recordLength);
    uint8_t* walPayload = record + GX_WAL_RECORD_PREFIX_SIZE;
    storeLe16(walPayload, pageType);
    storeLe16(walPayload + 2, 0);
    storeLe64(walPayload + 4, pageId);
    const GxStatus pageStatus = gxDatabasePageEncode(
        walPayload + pageImageHeaderSize, pageSize, pageId, pageType,
        pageFlags, pageGeneration, pagePayload, pagePayloadLength);
    if (pageStatus != GxStatus::Ok) return pageStatus;

    record[0] = 2; // Existing SQL3 PAGE_IMAGE record type.
    storeLe32(record + 4, static_cast<uint32_t>(payloadLength));
    storeLe64(record + 8, transactionId);
    storeLe32(record + GX_WAL_RECORD_CRC_OFFSET,
              computeRecordCrc(record, payloadLength));
    output->size = recordLength;
    return GxStatus::Ok;
}
