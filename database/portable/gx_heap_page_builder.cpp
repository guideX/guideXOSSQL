#include "gx_heap_page_builder.h"

#include "database_endian.h"
#include "database_format.h"

namespace {

const uint32_t kHeapMagicOffset = 0;
const uint32_t kHeapVersionOffset = 4;
const uint32_t kHeapFlagsOffset = 6;
const uint32_t kHeapNextPageOffset = 8;
const uint32_t kHeapSlotCountOffset = 16;
const uint32_t kHeapRowAreaEndOffset = 20;

bool addSize(size_t left, size_t right, size_t* out) {
    if (!out || left > static_cast<size_t>(-1) - right) return false;
    *out = left + right;
    return true;
}

void zeroBytes(uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}

void copyBytes(uint8_t* destination, const uint8_t* source, size_t length) {
    for (size_t i = 0; i < length; ++i) destination[i] = source[i];
}

GxStatus getRow(const GxHeapPageRowSource* rows, size_t index,
                const uint8_t** outBytes, size_t* outLength) {
    if (!rows || !rows->rowAt || !outBytes || !outLength ||
        index >= rows->rowCount) {
        return GxStatus::InvalidArgument;
    }
    *outBytes = 0;
    *outLength = 0;
    return rows->rowAt(rows->context, index, outBytes, outLength);
}

} // namespace

GxStatus gxHeapPagePrepare(GxArray* output, uint32_t pageSize,
                           uint64_t nextPageId,
                           const GxHeapPageRowSource* rows) {
    if (!output || !output->initialized || output->elementSize != 1 ||
        !output->allocator.allocate || !output->allocator.deallocate ||
        !rows || !rows->rowAt) {
        return GxStatus::InvalidArgument;
    }
    if (!gxos::db::isSupportedPageSize(pageSize)) {
        return GxStatus::InvalidArgument;
    }

    const size_t capacity =
        static_cast<size_t>(pageSize - gxos::db::kPageHeaderSize);
    if (output->maximum < capacity) return GxStatus::ResourceLimit;
    if (rows->rowCount != 0 && rows->context == 0) {
        return GxStatus::InvalidArgument;
    }
    if (rows->rowCount >
        (static_cast<size_t>(-1) - gxos::db::kHeapHeaderSize) / 8u) {
        return GxStatus::Overflow;
    }
    if (rows->rowCount > 0xFFFFFFFFu) return GxStatus::ResourceLimit;
    const size_t slotBytes = rows->rowCount * 8u;
    if (slotBytes > capacity - gxos::db::kHeapHeaderSize) {
        return GxStatus::ResourceLimit;
    }

    size_t rowBytes = 0;
    for (size_t i = 0; i < rows->rowCount; ++i) {
        const uint8_t* bytes = 0;
        size_t length = 0;
        const GxStatus sourceStatus = getRow(rows, i, &bytes, &length);
        if (sourceStatus != GxStatus::Ok) return sourceStatus;
        if (!bytes || length < 8 || length > 0xFFFFFFFFu) {
            return GxStatus::InvalidArgument;
        }
        if (gxos::db::loadLe32(bytes) != static_cast<uint32_t>(length)) {
            return GxStatus::CorruptData;
        }
        if (!addSize(rowBytes, length, &rowBytes)) return GxStatus::Overflow;
    }

    size_t requiredBytes = 0;
    if (!addSize(gxos::db::kHeapHeaderSize, rowBytes, &requiredBytes) ||
        !addSize(requiredBytes, slotBytes, &requiredBytes)) {
        return GxStatus::Overflow;
    }
    if (requiredBytes > capacity) return GxStatus::ResourceLimit;

    GxArray prepared = {};
    GxStatus status = gxArrayInit(&prepared, &output->allocator, 1,
                                  output->maximum);
    if (status != GxStatus::Ok) return status;
    status = gxArrayResize(&prepared, capacity);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&prepared);
        return status;
    }

    uint8_t* payload = prepared.data;
    zeroBytes(payload, capacity);
    size_t rowOffset = gxos::db::kHeapHeaderSize;
    for (size_t i = 0; i < rows->rowCount; ++i) {
        const uint8_t* bytes = 0;
        size_t length = 0;
        const GxStatus sourceStatus = getRow(rows, i, &bytes, &length);
        if (sourceStatus != GxStatus::Ok) {
            gxArrayDestroy(&prepared);
            return sourceStatus;
        }
        if (!bytes || length < 8 || length > 0xFFFFFFFFu ||
            gxos::db::loadLe32(bytes) != static_cast<uint32_t>(length)) {
            gxArrayDestroy(&prepared);
            return GxStatus::CorruptData;
        }

        const size_t slotOffset = capacity - 8u * (i + 1u);
        if (rowOffset > slotOffset || length > slotOffset - rowOffset) {
            gxArrayDestroy(&prepared);
            return GxStatus::ResourceLimit;
        }
        copyBytes(payload + rowOffset, bytes, length);
        gxos::db::storeLe32(payload + slotOffset,
                             static_cast<uint32_t>(rowOffset));
        gxos::db::storeLe32(payload + slotOffset + 4u,
                             static_cast<uint32_t>(length));
        rowOffset += length;
    }

    gxos::db::storeLe32(payload + kHeapMagicOffset, gxos::db::kHeapMagic);
    gxos::db::storeLe16(payload + kHeapVersionOffset, gxos::db::kHeapVersion);
    gxos::db::storeLe16(payload + kHeapFlagsOffset, 0);
    gxos::db::storeLe64(payload + kHeapNextPageOffset, nextPageId);
    gxos::db::storeLe32(payload + kHeapSlotCountOffset,
                         static_cast<uint32_t>(rows->rowCount));
    gxos::db::storeLe32(payload + kHeapRowAreaEndOffset,
                         static_cast<uint32_t>(rowOffset));
    status = gxArraySwap(output, &prepared);
    gxArrayDestroy(&prepared);
    return status;
}

GxStatus gxHeapPageAppendPrepare(GxArray* output, uint32_t pageSize,
                                 const uint8_t* existingPayload,
                                 size_t existingLength,
                                 const uint8_t* rowBytes,
                                 size_t rowLength) {
    if (!output || !output->initialized || output->elementSize != 1 ||
        !output->allocator.allocate || !output->allocator.deallocate ||
        !existingPayload || !rowBytes || rowLength < 8 ||
        rowLength > 0xFFFFFFFFu) {
        return GxStatus::InvalidArgument;
    }
    if (!gxos::db::isSupportedPageSize(pageSize)) {
        return GxStatus::InvalidArgument;
    }

    const size_t capacity =
        static_cast<size_t>(pageSize - gxos::db::kPageHeaderSize);
    if (output->maximum < capacity) return GxStatus::ResourceLimit;
    if (existingLength != capacity) return GxStatus::CorruptData;
    if (gxos::db::loadLe32(rowBytes) != static_cast<uint32_t>(rowLength)) {
        return GxStatus::CorruptData;
    }
    if (gxos::db::loadLe32(existingPayload + kHeapMagicOffset) !=
            gxos::db::kHeapMagic ||
        gxos::db::loadLe16(existingPayload + kHeapVersionOffset) !=
            gxos::db::kHeapVersion) {
        return GxStatus::CorruptData;
    }

    const uint32_t slotCount =
        gxos::db::loadLe32(existingPayload + kHeapSlotCountOffset);
    const uint32_t rowAreaEnd =
        gxos::db::loadLe32(existingPayload + kHeapRowAreaEndOffset);
    if (slotCount >
        (capacity - gxos::db::kHeapHeaderSize) / 8u) {
        return GxStatus::CorruptData;
    }
    const size_t currentSlotBytes = static_cast<size_t>(slotCount) * 8u;
    if (rowAreaEnd < gxos::db::kHeapHeaderSize + currentSlotBytes ||
        rowAreaEnd > capacity) {
        return GxStatus::CorruptData;
    }
    for (uint32_t i = 0; i < slotCount; ++i) {
        const size_t oldSlotOffset = capacity - 8u * (i + 1u);
        const uint32_t oldRowOffset =
            gxos::db::loadLe32(existingPayload + oldSlotOffset);
        const uint32_t oldRowLength =
            gxos::db::loadLe32(existingPayload + oldSlotOffset + 4u);
        if (oldRowOffset < gxos::db::kHeapHeaderSize || oldRowLength < 8u ||
            oldRowOffset > rowAreaEnd ||
            oldRowLength > rowAreaEnd - oldRowOffset ||
            gxos::db::loadLe32(existingPayload + oldRowOffset) !=
                oldRowLength) {
            return GxStatus::CorruptData;
        }
    }
    if (slotCount == 0xFFFFFFFFu) return GxStatus::ResourceLimit;
    const size_t newSlotCount = static_cast<size_t>(slotCount) + 1u;
    if (newSlotCount >
        (capacity - gxos::db::kHeapHeaderSize) / 8u) {
        return GxStatus::ResourceLimit;
    }
    const size_t slotOffset = capacity - 8u * newSlotCount;
    if (rowAreaEnd > slotOffset || rowLength > slotOffset - rowAreaEnd) {
        return GxStatus::ResourceLimit;
    }

    GxArray prepared = {};
    GxStatus status = gxArrayInit(&prepared, &output->allocator, 1,
                                  output->maximum);
    if (status != GxStatus::Ok) return status;
    status = gxArrayResize(&prepared, capacity);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&prepared);
        return status;
    }

    copyBytes(prepared.data, existingPayload, capacity);
    copyBytes(prepared.data + rowAreaEnd, rowBytes, rowLength);
    gxos::db::storeLe32(prepared.data + slotOffset,
                         static_cast<uint32_t>(rowAreaEnd));
    gxos::db::storeLe32(prepared.data + slotOffset + 4u,
                         static_cast<uint32_t>(rowLength));
    gxos::db::storeLe32(prepared.data + kHeapSlotCountOffset,
                         slotCount + 1u);
    gxos::db::storeLe32(prepared.data + kHeapRowAreaEndOffset,
                         rowAreaEnd + static_cast<uint32_t>(rowLength));

    status = gxArraySwap(output, &prepared);
    gxArrayDestroy(&prepared);
    return status;
}
