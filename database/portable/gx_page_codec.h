#ifndef GX_PORTABLE_DATABASE_PAGE_CODEC_H
#define GX_PORTABLE_DATABASE_PAGE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "gx_status.h"

// Serializes the existing database page representation into caller-owned
// storage. The buffer must contain pageSize bytes. No allocation is performed.
GxStatus gxDatabasePageEncode(uint8_t* output, uint32_t pageSize,
                              uint64_t pageId, uint16_t pageType,
                              uint16_t flags, uint32_t generation,
                              const uint8_t* payload, size_t payloadLength);

#endif
