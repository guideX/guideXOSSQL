#include "gx_index_physical_key.h"

namespace {

const size_t kIndexPhysicalLocatorBytes = 12;

struct PhysicalKeyView {
    const uint8_t* keyBytes;
    size_t keyLength;
    uint64_t locatorPageId;
    uint32_t locatorSlot;
    size_t totalLength;
};

GxStatus initializeView(const uint8_t* keyBytes, size_t keyLength,
                        bool hasLocator, uint64_t locatorPageId,
                        uint32_t locatorSlot, PhysicalKeyView& out) {
    if (hasLocator && keyLength > static_cast<size_t>(-1) -
                                      kIndexPhysicalLocatorBytes) {
        return GxStatus::Overflow;
    }
    if (keyBytes == nullptr && keyLength != 0) return GxStatus::InvalidArgument;
    out.keyBytes = keyBytes;
    out.keyLength = keyLength;
    out.locatorPageId = locatorPageId;
    out.locatorSlot = locatorSlot;
    out.totalLength = keyLength + (hasLocator ? kIndexPhysicalLocatorBytes : 0);
    return GxStatus::Ok;
}

uint8_t byteAt(const PhysicalKeyView& view, size_t offset) {
    if (offset < view.keyLength) return view.keyBytes[offset];
    offset -= view.keyLength;
    if (offset < 8) {
        return static_cast<uint8_t>(
            view.locatorPageId >> (56u - static_cast<unsigned>(offset) * 8u));
    }
    offset -= 8;
    return static_cast<uint8_t>(
        view.locatorSlot >> (24u - static_cast<unsigned>(offset) * 8u));
}

} // namespace

GxStatus gxCompareIndexPhysicalKeyViews(
    const uint8_t* leftKeyBytes, size_t leftKeyLength,
    bool leftHasLocator, uint64_t leftLocatorPageId,
    uint32_t leftLocatorSlot,
    const uint8_t* rightKeyBytes, size_t rightKeyLength,
    bool rightHasLocator, uint64_t rightLocatorPageId,
    uint32_t rightLocatorSlot,
    int* outComparison) {
    if (outComparison == nullptr) return GxStatus::InvalidArgument;
    *outComparison = 0;

    PhysicalKeyView left = {};
    PhysicalKeyView right = {};
    const GxStatus leftStatus = initializeView(
        leftKeyBytes, leftKeyLength, leftHasLocator,
        leftLocatorPageId, leftLocatorSlot, left);
    if (leftStatus != GxStatus::Ok) return leftStatus;
    const GxStatus rightStatus = initializeView(
        rightKeyBytes, rightKeyLength, rightHasLocator,
        rightLocatorPageId, rightLocatorSlot, right);
    if (rightStatus != GxStatus::Ok) return rightStatus;

    const size_t compareLength = left.totalLength < right.totalLength
        ? left.totalLength : right.totalLength;
    for (size_t i = 0; i < compareLength; ++i) {
        const uint8_t leftByte = byteAt(left, i);
        const uint8_t rightByte = byteAt(right, i);
        if (leftByte != rightByte) {
            *outComparison = leftByte < rightByte ? -1 : 1;
            return GxStatus::Ok;
        }
    }
    if (left.totalLength != right.totalLength) {
        *outComparison = left.totalLength < right.totalLength ? -1 : 1;
    }
    return GxStatus::Ok;
}
