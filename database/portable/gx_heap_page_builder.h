#ifndef GX_PORTABLE_HEAP_PAGE_BUILDER_H
#define GX_PORTABLE_HEAP_PAGE_BUILDER_H

#include <stddef.h>
#include <stdint.h>

#include "gx_array.h"

// Provides borrowed, encoded row bytes in stable order. Row bytes and lengths
// must remain stable for one preparation call. The provider may be queried
// twice per row and must not allocate.
typedef GxStatus (*GxHeapPageRowAt)(const void* context, size_t index,
                                    const uint8_t** outBytes,
                                    size_t* outLength);

struct GxHeapPageRowSource {
    const void* context;
    size_t rowCount;
    GxHeapPageRowAt rowAt;
};

// Prepares the existing SQL heap payload format in caller-owned GxArray
// storage. The array must be initialized for one-byte elements and bounded to
// at least the payload capacity for pageSize. On validation or allocation
// failure, its prior logical contents remain unchanged. Successful output is
// exactly pageSize - the existing database-page header bytes.
GxStatus gxHeapPagePrepare(GxArray* output, uint32_t pageSize,
                           uint64_t nextPageId,
                           const GxHeapPageRowSource* rows);

#endif
