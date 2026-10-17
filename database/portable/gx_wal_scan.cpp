#include "gx_wal_scan.h"

#include "gx_array.h"

namespace gxos {
namespace db {

uint32_t crc32Init();
uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t length);
uint32_t crc32Final(uint32_t state);

} // namespace db
} // namespace gxos

namespace {

const size_t kPrefixSize = GX_WAL_SCAN_PREFIX_SIZE;
const size_t kPayloadLengthOffset = 4;
const size_t kTransactionIdOffset = 8;
const size_t kRecordCrcOffset = 16;

uint16_t loadLe16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t loadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t loadLe64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

bool checkedAddU64(uint64_t left, uint64_t right, uint64_t* out) {
    if (!out || left > static_cast<uint64_t>(-1) - right) return false;
    *out = left + right;
    return true;
}

bool knownRecordType(uint8_t type) {
    return type >= GX_WAL_SCAN_RECORD_BEGIN &&
           type <= GX_WAL_SCAN_RECORD_COMMIT;
}

uint32_t computeRecordCrc(const uint8_t* prefix, const uint8_t* payload,
                          uint32_t payloadLength) {
    uint32_t crc = gxos::db::crc32Init();
    crc = gxos::db::crc32Update(crc, prefix, kRecordCrcOffset);
    const uint8_t zero[4] = {0, 0, 0, 0};
    crc = gxos::db::crc32Update(crc, zero, sizeof(zero));
    crc = gxos::db::crc32Update(
        crc, prefix + kRecordCrcOffset + sizeof(zero),
        kPrefixSize - (kRecordCrcOffset + sizeof(zero)));
    if (payloadLength != 0) {
        crc = gxos::db::crc32Update(crc, payload, payloadLength);
    }
    return gxos::db::crc32Final(crc);
}

GxWalScanStatus mapAllocatorStatus(GxStatus status) {
    if (status == GxStatus::InvalidArgument) {
        return GxWalScanStatus_InvalidArgument;
    }
    return GxWalScanStatus_NoSpace;
}

void setResult(GxWalScanResult* result, GxWalScanStatus status,
               uint64_t offset) {
    result->status = status;
    result->errorOffset = offset;
}

} // namespace

GxWalScanStatus gxWalScanRecords(uint64_t startOffset, uint64_t fileSize,
                                 uint64_t maximumFileBytes,
                                 uint32_t maximumPayload,
                                 GxWalReadAt readAt, void* readContext,
                                 const GxAllocator* allocator,
                                 GxWalRecordVisitor visitor,
                                 void* visitorContext,
                                 GxWalScanResult* outResult) {
    if (outResult) {
        outResult->status = GxWalScanStatus_InvalidArgument;
        outResult->errorOffset = startOffset;
        outResult->recordsVisited = 0;
        outResult->incompleteTail = false;
    }
    if (!outResult || !readAt || !allocator || !visitor ||
        maximumPayload > GX_WAL_SCAN_MAX_PAYLOAD ||
        startOffset > fileSize || fileSize > maximumFileBytes) {
        if (outResult && fileSize > maximumFileBytes) {
            setResult(outResult, GxWalScanStatus_Corrupt, startOffset);
        }
        return outResult ? outResult->status : GxWalScanStatus_InvalidArgument;
    }

    GxArray payloadScratch = {};
    GxStatus initStatus = gxArrayInit(&payloadScratch, allocator, 1,
                                      maximumPayload);
    if (initStatus != GxStatus::Ok) {
        setResult(outResult, mapAllocatorStatus(initStatus), startOffset);
        return outResult->status;
    }

    uint64_t offset = startOffset;
    GxWalScanStatus status = GxWalScanStatus_Ok;
    while (offset < fileSize) {
        const uint64_t remaining = fileSize - offset;
        if (remaining < kPrefixSize) {
            outResult->incompleteTail = true;
            break;
        }

        uint8_t prefix[GX_WAL_SCAN_PREFIX_SIZE];
        if (!readAt(readContext, offset, prefix, sizeof(prefix))) {
            status = GxWalScanStatus_IoError;
            setResult(outResult, status, offset);
            break;
        }

        const uint8_t recordType = prefix[0];
        if (!knownRecordType(recordType)) {
            status = GxWalScanStatus_Corrupt;
            setResult(outResult, status, offset);
            break;
        }
        if (prefix[1] != 0 || loadLe16(prefix + 2) != 0) {
            status = GxWalScanStatus_Corrupt;
            setResult(outResult, status, offset);
            break;
        }

        const uint32_t payloadLength = loadLe32(prefix + kPayloadLengthOffset);
        if (payloadLength > maximumPayload ||
            payloadLength > GX_WAL_SCAN_MAX_PAYLOAD) {
            status = GxWalScanStatus_Corrupt;
            setResult(outResult, status, offset);
            break;
        }
        if (static_cast<size_t>(payloadLength) >
            static_cast<size_t>(-1) - kPrefixSize) {
            status = GxWalScanStatus_Corrupt;
            setResult(outResult, status, offset);
            break;
        }
        uint64_t recordSize = 0;
        uint64_t recordEnd = 0;
        if (!checkedAddU64(static_cast<uint64_t>(kPrefixSize), payloadLength,
                           &recordSize) ||
            !checkedAddU64(offset, recordSize, &recordEnd)) {
            status = GxWalScanStatus_Corrupt;
            setResult(outResult, status, offset);
            break;
        }
        if (recordSize > remaining) {
            outResult->incompleteTail = true;
            break;
        }

        const uint8_t* payload = 0;
        if (payloadLength != 0) {
            gxArrayClear(&payloadScratch);
            const GxStatus reserveStatus =
                gxArrayReserve(&payloadScratch, payloadLength);
            if (reserveStatus != GxStatus::Ok) {
                status = mapAllocatorStatus(reserveStatus);
                setResult(outResult, status, offset);
                break;
            }
            uint8_t* payloadBytes = payloadScratch.data;
            payload = payloadBytes;
            uint64_t payloadOffset = 0;
            if (!checkedAddU64(offset, static_cast<uint64_t>(kPrefixSize),
                               &payloadOffset)) {
                status = GxWalScanStatus_Corrupt;
                setResult(outResult, status, offset);
                break;
            }
            if (!readAt(readContext, payloadOffset, payloadBytes, payloadLength)) {
                status = GxWalScanStatus_IoError;
                setResult(outResult, status, offset);
                break;
            }
        }

        const uint32_t storedCrc = loadLe32(prefix + kRecordCrcOffset);
        if (storedCrc != computeRecordCrc(prefix, payload, payloadLength)) {
            status = GxWalScanStatus_Corrupt;
            setResult(outResult, status, offset);
            break;
        }

        const uint64_t transactionId =
            loadLe64(prefix + kTransactionIdOffset);
        status = visitor(visitorContext, recordType, transactionId, payload,
                         payloadLength);
        if (status != GxWalScanStatus_Ok) {
            setResult(outResult, status, offset);
            break;
        }
        ++outResult->recordsVisited;
        offset = recordEnd;
    }

    gxArrayDestroy(&payloadScratch);
    outResult->status = status;
    if (status == GxWalScanStatus_Ok) outResult->errorOffset = offset;
    return status;
}
