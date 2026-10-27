#ifndef GX_PORTABLE_INDEX_PHYSICAL_KEY_H
#define GX_PORTABLE_INDEX_PHYSICAL_KEY_H

#include <stddef.h>
#include <stdint.h>

#include "gx_status.h"

// Compares two physical index-key byte sequences without constructing either
// sequence. Each view is keyBytes[0..keyLength), optionally followed by the
// existing 12-byte big-endian (page id, slot) locator suffix.
//
// When hasLocator is false, keyBytes is already the complete physical-key
// span. This supports stored separator bounds, including their existing
// byte-prefix comparison behavior. The operation compares bytes only: it does
// not validate logical-key encodings, entry records, or page offsets.
//
// Input spans are borrowed for this call and are never modified. A null input
// pointer is valid only when keyLength is zero. outComparison is required,
// receives -1, 0, or 1, and must not overlap either input span. The operation
// does not allocate or retain pointers. Length addition is checked before any
// input bytes are read; failures clear outComparison when it is available.
GxStatus gxCompareIndexPhysicalKeyViews(
    const uint8_t* leftKeyBytes, size_t leftKeyLength,
    bool leftHasLocator, uint64_t leftLocatorPageId,
    uint32_t leftLocatorSlot,
    const uint8_t* rightKeyBytes, size_t rightKeyLength,
    bool rightHasLocator, uint64_t rightLocatorPageId,
    uint32_t rightLocatorSlot,
    int* outComparison);

#endif
