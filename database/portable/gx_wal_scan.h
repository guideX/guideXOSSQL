#ifndef GX_PORTABLE_WAL_SCAN_H
#define GX_PORTABLE_WAL_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"

enum {
    GX_WAL_SCAN_PREFIX_SIZE = 20,
    GX_WAL_SCAN_RECORD_BEGIN = 1,
    GX_WAL_SCAN_RECORD_PAGE_IMAGE = 2,
    GX_WAL_SCAN_RECORD_DB_HEADER = 3,
    GX_WAL_SCAN_RECORD_COMMIT = 4,
    GX_WAL_SCAN_MAX_PAYLOAD = 65536 + 64
};

typedef enum GxWalScanStatus {
    GxWalScanStatus_Ok = 0,
    GxWalScanStatus_InvalidArgument,
    GxWalScanStatus_IoError,
    GxWalScanStatus_Corrupt,
    GxWalScanStatus_Foreign,
    GxWalScanStatus_NoSpace,
    GxWalScanStatus_Overflow,
    GxWalScanStatus_ResourceLimit
} GxWalScanStatus;

typedef bool (*GxWalReadAt)(void* context, uint64_t offset, uint8_t* buffer,
                            size_t length);

typedef GxWalScanStatus (*GxWalRecordVisitor)(
    void* context, uint8_t recordType, uint64_t transactionId,
    const uint8_t* payload, uint32_t payloadLength);

typedef struct GxWalScanResult {
    GxWalScanStatus status;
    uint64_t errorOffset;
    uint64_t recordsVisited;
    bool incompleteTail;
} GxWalScanResult;

// Scans records in [startOffset, fileSize). The caller validates the WAL
// header and supplies the total file size. A payload pointer given to visitor
// is borrowed scratch storage and remains valid only until visitor returns.
// The scan owns at most GX_WAL_SCAN_MAX_PAYLOAD bytes through `allocator`.
GxWalScanStatus gxWalScanRecords(uint64_t startOffset, uint64_t fileSize,
                                 uint64_t maximumFileBytes,
                                 uint32_t maximumPayload,
                                 GxWalReadAt readAt, void* readContext,
                                 const GxAllocator* allocator,
                                 GxWalRecordVisitor visitor,
                                 void* visitorContext,
                                 GxWalScanResult* outResult);

#endif
