#ifndef GX_PORTABLE_INDEX_KEY_H
#define GX_PORTABLE_INDEX_KEY_H

#include "gx_index_text_key.h"

// Tests whether an encoded logical key is exactly the one-byte SQL NULL key.
// The predicate does not otherwise validate or interpret the key. A null
// pointer is valid only for a zero-length span. On failure, outIsNull is
// cleared when available. The output slot must not overlap keyBytes.
GxStatus gxIndexKeyIsNull(const uint8_t* keyBytes, size_t keyLength,
                          bool* outIsNull);

// Unsigned lexicographic comparison of encoded logical-key byte spans. A
// shorter equal prefix sorts first. The output is normalized to -1, 0, or 1
// and is cleared on failure. Null pointers are valid only for zero-length
// spans. The operation borrows both spans and allocates no storage. The
// output slot must not overlap either input span.
GxStatus gxCompareIndexKeys(const uint8_t* leftBytes, size_t leftLength,
                            const uint8_t* rightBytes, size_t rightLength,
                            int* outComparison);

#endif
