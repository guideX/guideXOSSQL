#include "gx_wal_record.h"

namespace gxos {
namespace db {

uint32_t crc32Init();
uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t length);
uint32_t crc32Final(uint32_t state);

} // namespace db
} // namespace gxos

namespace {

const size_t kWalRecordPrefixBytes = GX_WAL_RECORD_PREFIX_SIZE;
const size_t kWalRecordCrcOffset = GX_WAL_RECORD_CRC_OFFSET;

void copyBytes(uint8_t* destination, const uint8_t* source, size_t length) {
    for (size_t i = 0; i < length; ++i) destination[i] = source[i];
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

uint32_t computeRecordCrc(const uint8_t* record, uint32_t payloadLength) {
    uint32_t crc = gxos::db::crc32Init();
    crc = gxos::db::crc32Update(crc, record, kWalRecordCrcOffset);
    const uint8_t zero[4] = {0, 0, 0, 0};
    crc = gxos::db::crc32Update(crc, zero, sizeof(zero));
    crc = gxos::db::crc32Update(
        crc, record + kWalRecordPrefixBytes,
        static_cast<size_t>(payloadLength));
    return gxos::db::crc32Final(crc);
}

bool payloadAliasesOutput(const GxArray* output, const uint8_t* payload) {
    if (!output->data || !payload || output->capacity == 0) return false;
    const size_t capacityBytes = output->capacity * output->elementSize;
    const uintptr_t base = reinterpret_cast<uintptr_t>(output->data);
    const uintptr_t source = reinterpret_cast<uintptr_t>(payload);
    return source >= base && source - base < capacityBytes;
}

} // namespace

GxStatus gxWalRecordEncode(GxArray* output, uint8_t recordType,
                           uint64_t transactionId, const uint8_t* payload,
                           uint32_t payloadLength, uint32_t maximumPayload) {
    if (!output || !output->initialized || output->elementSize != 1) {
        return GxStatus::InvalidArgument;
    }
    if (payloadLength > maximumPayload) return GxStatus::ResourceLimit;
    if (payloadAliasesOutput(output, payload)) return GxStatus::InvalidArgument;
    if (static_cast<size_t>(payloadLength) >
        static_cast<size_t>(-1) - kWalRecordPrefixBytes) {
        return GxStatus::Overflow;
    }
    const size_t totalBytes = kWalRecordPrefixBytes + payloadLength;
    if (totalBytes > output->maximum) return GxStatus::ResourceLimit;

    const GxStatus reserveStatus = gxArrayReserve(output, totalBytes);
    if (reserveStatus != GxStatus::Ok) return reserveStatus;

    uint8_t* bytes = output->data;
    for (size_t i = 0; i < totalBytes; ++i) bytes[i] = 0;
    bytes[0] = recordType;
    storeLe32(bytes + 4, payloadLength);
    storeLe64(bytes + 8, transactionId);
    if (payload && payloadLength != 0) {
        copyBytes(bytes + kWalRecordPrefixBytes, payload, payloadLength);
    }
    storeLe32(bytes + kWalRecordCrcOffset,
              computeRecordCrc(bytes, payloadLength));
    output->size = totalBytes;
    return GxStatus::Ok;
}
