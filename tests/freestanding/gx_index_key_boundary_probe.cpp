#include "gx_index_key.h"
#include "gx_index_leaf_entry.h"
#include "gx_index_physical_key.h"

namespace {

void storeProbeLe32(uint8_t* bytes, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

void storeProbeLe64(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

} // namespace

// Freestanding compile/link probe for the production portable index-key API.
// Keeping the encoded bytes on this stack frame makes the lifetime underlying
// the borrowed decode view explicit for the entire use of that view.
extern "C" int gxIndexKeyBoundaryProbe() {
    const uint8_t textBytes[] = {'a', 0x00u, 'b'};
    uint8_t encodedBytes[sizeof(textBytes) + 1] = {};
    size_t encodedLength = 0;
    GxTextByteView text = {nullptr, 0};
    bool isNull = true;
    int comparison = 0;

    if (gxEncodeIndexTextKey(textBytes, sizeof(textBytes), encodedBytes,
                             sizeof(encodedBytes), &encodedLength) !=
        GxStatus::Ok) {
        return 1;
    }
    if (gxDecodeIndexTextKey(encodedBytes, encodedLength, &text, &isNull) !=
            GxStatus::Ok ||
        isNull || text.data != encodedBytes + 1 ||
        text.length != sizeof(textBytes)) {
        return 2;
    }
    if (gxIndexKeyIsNull(encodedBytes, encodedLength, &isNull) !=
            GxStatus::Ok ||
        isNull) {
        return 3;
    }
    if (gxCompareIndexKeys(text.data, text.length, textBytes,
                           sizeof(textBytes), &comparison) != GxStatus::Ok ||
        comparison != 0) {
        return 4;
    }

    encodedBytes[1] = 'z';
    if (text.data[0] != 'z' || text.data[1] != 0x00u ||
        text.data[2] != 'b') {
        return 5;
    }

    const uint8_t int32Minimum[] = {0x01u, 0x00u, 0x00u, 0x00u, 0x00u};
    int32_t decodedInt32 = 0;
    if (gxDecodeIndexInt32Key(int32Minimum, sizeof(int32Minimum),
                              &decodedInt32, &isNull) != GxStatus::Ok ||
        isNull || decodedInt32 != INT32_MIN) {
        return 6;
    }

    uint8_t encodedInt64[9] = {};
    size_t encodedInt64Length = 0;
    int64_t decodedInt64 = 0;
    if (gxEncodeIndexInt64Key(false, -1, encodedInt64,
                              sizeof(encodedInt64), &encodedInt64Length) !=
            GxStatus::Ok ||
        encodedInt64Length != sizeof(encodedInt64) ||
        gxDecodeIndexInt64Key(encodedInt64, encodedInt64Length,
                              &decodedInt64, &isNull) != GxStatus::Ok ||
        isNull || decodedInt64 != -1) {
        return 7;
    }

    uint8_t encodedInt32[5] = {};
    size_t encodedInt32Length = 0;
    if (gxEncodeIndexInt32Key(false, INT32_MAX, encodedInt32,
                              sizeof(encodedInt32), &encodedInt32Length) !=
            GxStatus::Ok ||
        encodedInt32Length != sizeof(encodedInt32) ||
        encodedInt32[0] != 0x01u || encodedInt32[1] != 0xFFu ||
        encodedInt32[2] != 0xFFu || encodedInt32[3] != 0xFFu ||
        encodedInt32[4] != 0xFFu) {
        return 8;
    }

    uint8_t encodedBoolean[2] = {};
    size_t encodedBooleanLength = 0;
    bool decodedBoolean = true;
    if (gxEncodeIndexBooleanKey(false, false, encodedBoolean,
                                sizeof(encodedBoolean),
                                &encodedBooleanLength) != GxStatus::Ok ||
        encodedBooleanLength != 2 || encodedBoolean[0] != 0x01u ||
        encodedBoolean[1] != 0x00u ||
        gxDecodeIndexBooleanKey(encodedBoolean, encodedBooleanLength,
                                &decodedBoolean, &isNull) != GxStatus::Ok ||
        isNull || decodedBoolean) {
        return 9;
    }
    if (gxEncodeIndexBooleanKey(false, true, encodedBoolean,
                                sizeof(encodedBoolean),
                                &encodedBooleanLength) != GxStatus::Ok ||
        encodedBooleanLength != 2 || encodedBoolean[0] != 0x01u ||
        encodedBoolean[1] != 0x01u ||
        gxDecodeIndexBooleanKey(encodedBoolean, encodedBooleanLength,
                                &decodedBoolean, &isNull) != GxStatus::Ok ||
        isNull || !decodedBoolean) {
        return 10;
    }
    if (gxEncodeIndexBooleanKey(true, false, encodedBoolean,
                                sizeof(encodedBoolean),
                                &encodedBooleanLength) != GxStatus::Ok ||
        encodedBooleanLength != 1 || encodedBoolean[0] != 0x00u ||
        gxDecodeIndexBooleanKey(encodedBoolean, encodedBooleanLength,
                                &decodedBoolean, &isNull) != GxStatus::Ok ||
        !isNull || decodedBoolean) {
        return 11;
    }

    const uint8_t nullKey[] = {0x00u};
    const uint8_t falseKey[] = {0x01u, 0x00u};
    if (gxCompareIndexPhysicalKeyViews(
            nullKey, sizeof(nullKey), true, 4, 2,
            falseKey, sizeof(falseKey), true, 4, 2,
            &comparison) != GxStatus::Ok || comparison != -1) {
        return 12;
    }

    // The stored full bound and a borrowed key-plus-locator view must compare
    // equal without a temporary combined-key buffer.
    const uint8_t storedPhysical[] = {
        0x01u, 'x', 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 4
    };
    if (gxCompareIndexPhysicalKeyViews(
            storedPhysical, sizeof(storedPhysical), false, 0, 0,
            storedPhysical, 2, true, 3, 4,
            &comparison) != GxStatus::Ok || comparison != 0) {
        return 13;
    }

    // Execute the production leaf cursor and validator against a caller-owned
    // payload with the existing little-endian header, entry and reverse slot.
    uint8_t leafBytes[480] = {};
    storeProbeLe32(leafBytes, 0x4C495847u);
    leafBytes[4] = 1;
    storeProbeLe32(leafBytes + 16, 1);
    storeProbeLe32(leafBytes + 20, 5);
    storeProbeLe32(leafBytes + 24, 2);
    leafBytes[28] = 0x01u;
    leafBytes[29] = 0x20u;
    storeProbeLe64(leafBytes + 30, 7);
    storeProbeLe32(leafBytes + 38, 4);
    storeProbeLe32(leafBytes + 472, 24);
    storeProbeLe32(leafBytes + 476, 18);

    GxIndexLeafCursor leafCursor = {};
    GxIndexLeafEntryView leafEntry = {};
    GxIndexLeafError leafError = GxIndexLeafError::None;
    bool hasLeafEntry = false;
    if (gxIndexLeafCursorInit(leafBytes, sizeof(leafBytes), &leafCursor,
                              &leafError) != GxStatus::Ok ||
        leafCursor.page.entryCount != 1 ||
        leafCursor.page.ownerIndexId != 5) {
        return 14;
    }
    if (gxIndexLeafCursorReadNext(&leafCursor, &leafEntry, &hasLeafEntry,
                                  &leafError) != GxStatus::Ok ||
        !hasLeafEntry || leafEntry.keyLength != 2 ||
        leafEntry.keyBytes[0] != 0x01u || leafEntry.keyBytes[1] != 0x20u ||
        leafEntry.locatorPageId != 7 || leafEntry.locatorSlot != 4) {
        return 15;
    }
    if (gxIndexLeafCursorReadNext(&leafCursor, &leafEntry, &hasLeafEntry,
                                  &leafError) != GxStatus::Ok || hasLeafEntry ||
        gxIndexLeafValidate(leafBytes, sizeof(leafBytes), &leafError) !=
            GxStatus::Ok) {
        return 16;
    }
    storeProbeLe32(leafBytes + 476, 0xFFFFFFFFu);
    if (gxIndexLeafCursorInit(leafBytes, sizeof(leafBytes), &leafCursor,
                              &leafError) != GxStatus::Ok ||
        gxIndexLeafCursorReadNext(&leafCursor, &leafEntry, &hasLeafEntry,
                                  &leafError) != GxStatus::CorruptData ||
        hasLeafEntry || leafError != GxIndexLeafError::EntryOutOfRange) {
        return 17;
    }

    // The production leaf lookup returns a physical-key match and a
    // continuation positioned after it, without allocating a combined key.
    storeProbeLe32(leafBytes + 472, 24);
    storeProbeLe32(leafBytes + 476, 18);
    const uint8_t exactPhysicalKey[] = {
        0x01u, 0x20u, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 4
    };
    GxIndexLeafLookupResult lookup = {};
    if (gxIndexLeafLookup(leafBytes, sizeof(leafBytes), exactPhysicalKey,
                          sizeof(exactPhysicalKey), &lookup, &leafError) !=
            GxStatus::Ok ||
        !lookup.hasEntry || !lookup.exactMatch || lookup.entryPosition != 0 ||
        lookup.entry.locatorPageId != 7 || lookup.entry.locatorSlot != 4) {
        return 18;
    }
    if (gxIndexLeafCursorReadNext(&lookup.continuation, &leafEntry,
                                  &hasLeafEntry, &leafError) != GxStatus::Ok ||
        hasLeafEntry) {
        return 19;
    }

    const uint8_t missingPhysicalKey[] = {
        0x01u, 0x30u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    if (gxIndexLeafLookup(leafBytes, sizeof(leafBytes), missingPhysicalKey,
                          sizeof(missingPhysicalKey), &lookup, &leafError) !=
            GxStatus::Ok ||
        lookup.hasEntry || lookup.exactMatch || lookup.entryPosition != 1 ||
        lookup.continuation.position != 1) {
        return 20;
    }

    storeProbeLe32(leafBytes + 476, 0xFFFFFFFFu);
    lookup.entryPosition = 99;
    lookup.hasEntry = true;
    if (gxIndexLeafLookup(leafBytes, sizeof(leafBytes), exactPhysicalKey,
                          sizeof(exactPhysicalKey), &lookup, &leafError) !=
            GxStatus::CorruptData ||
        leafError != GxIndexLeafError::EntryOutOfRange || lookup.hasEntry ||
        lookup.entryPosition != 0 || lookup.continuation.pageBytes != 0) {
        return 21;
    }
    return 0;
}
