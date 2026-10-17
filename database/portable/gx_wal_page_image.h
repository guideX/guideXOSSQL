#ifndef GX_PORTABLE_WAL_PAGE_IMAGE_H
#define GX_PORTABLE_WAL_PAGE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "gx_array.h"

// Encodes the SQL3 PAGE_IMAGE record and its nested database page directly
// into one initialized byte array. The array allocator owns the only temporary
// storage; pagePayload remains owned by the caller.
GxStatus gxWalPageImageRecordEncode(GxArray* output, uint64_t transactionId,
                                    uint64_t pageId, uint16_t pageType,
                                    uint16_t pageFlags,
                                    uint32_t pageGeneration,
                                    const uint8_t* pagePayload,
                                    size_t pagePayloadLength,
                                    uint32_t pageSize,
                                    uint32_t maximumWalPayload);

#endif
