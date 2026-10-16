#ifndef GX_PORTABLE_WAL_RECORD_H
#define GX_PORTABLE_WAL_RECORD_H

#include <stdint.h>

#include "gx_array.h"

// Encodes one SQL3 WAL record into an initialized byte-record array. The
// prefix is the existing 20-byte format. The output is unchanged if validation
// or reserve fails; after reserve, encoding cannot fail. For compatibility,
// a null payload with nonzero length is encoded as zero bytes, matching the
// prior zero-initialized hosted record buffer.
GxStatus gxWalRecordEncode(GxArray* output, uint8_t recordType,
                           uint64_t transactionId, const uint8_t* payload,
                           uint32_t payloadLength, uint32_t maximumPayload);

#endif
