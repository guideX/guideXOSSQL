#include "gx_string.h"

static const uint8_t kGxEmptyString[1] = {0};

static bool gxStringReady(const GxString* string) {
    return string && string->initialized;
}

static void gxCopyStringBytes(uint8_t* destination, const uint8_t* source,
                              size_t count) {
    for (size_t i = 0; i < count; ++i) destination[i] = source[i];
}

static void gxMoveStringBytes(uint8_t* destination, const uint8_t* source,
                              size_t count) {
    if (destination == source || count == 0) return;
    if (reinterpret_cast<uintptr_t>(destination) <
        reinterpret_cast<uintptr_t>(source)) {
        for (size_t i = 0; i < count; ++i) destination[i] = source[i];
    } else {
        for (size_t i = count; i != 0; --i) destination[i - 1] = source[i - 1];
    }
}

static bool gxStringAlias(const GxString* string, const void* source,
                          size_t length, size_t* offset) {
    if (!string->data || !source) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(string->data);
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    if (address < base || address - base > string->length) return false;
    *offset = static_cast<size_t>(address - base);
    return length <= string->length - *offset;
}

GxStatus gxStringInit(GxString* string, const GxAllocator* allocator,
                      size_t maximumLength) {
    if (!string || !allocator || !allocator->allocate || !allocator->deallocate ||
        string->initialized) {
        return GxStatus::InvalidArgument;
    }
    string->allocator = *allocator;
    string->data = 0;
    string->length = 0;
    string->capacity = 0;
    string->maximum = maximumLength;
    string->initialized = true;
    return GxStatus::Ok;
}

GxStatus gxStringReserve(GxString* string, size_t capacity) {
    if (!gxStringReady(string)) return GxStatus::InvalidArgument;
    if (capacity <= string->capacity) return GxStatus::Ok;
    if (capacity > string->maximum) return GxStatus::ResourceLimit;
    if (capacity == static_cast<size_t>(-1)) return GxStatus::Overflow;

    size_t newCapacity = string->capacity == 0 ? 1 : string->capacity;
    while (newCapacity < capacity) {
        if (newCapacity > static_cast<size_t>(-1) / 2) {
            return GxStatus::Overflow;
        }
        newCapacity *= 2;
        if (newCapacity > string->maximum) newCapacity = string->maximum;
    }
    if (newCapacity == static_cast<size_t>(-1)) return GxStatus::Overflow;
    void* allocation = 0;
    const GxStatus status = gxAllocatorAllocate(&string->allocator,
                                                 newCapacity + 1, 1,
                                                 &allocation);
    if (status != GxStatus::Ok) return status;
    uint8_t* replacement = static_cast<uint8_t*>(allocation);
    if (string->length) gxCopyStringBytes(replacement, string->data, string->length);
    replacement[string->length] = 0;
    uint8_t* old = string->data;
    string->data = replacement;
    string->capacity = newCapacity;
    gxAllocatorFree(&string->allocator, old);
    return GxStatus::Ok;
}

GxStatus gxStringAssign(GxString* string, const void* bytes, size_t length) {
    if (!gxStringReady(string) || (length != 0 && !bytes)) {
        return GxStatus::InvalidArgument;
    }
    if (length > string->maximum) return GxStatus::ResourceLimit;
    if (length == 0) {
        string->length = 0;
        if (string->data) string->data[0] = 0;
        return GxStatus::Ok;
    }
    size_t sourceOffset = 0;
    const bool aliases = gxStringAlias(string, bytes, length, &sourceOffset);
    if (string->data && reinterpret_cast<uintptr_t>(bytes) >=
            reinterpret_cast<uintptr_t>(string->data) &&
        reinterpret_cast<uintptr_t>(bytes) - reinterpret_cast<uintptr_t>(string->data) <=
            string->length && !aliases) {
        return GxStatus::InvalidArgument;
    }
    const GxStatus status = gxStringReserve(string, length);
    if (status != GxStatus::Ok) return status;
    const uint8_t* source = aliases ? string->data + sourceOffset
                                    : static_cast<const uint8_t*>(bytes);
    gxMoveStringBytes(string->data, source, length);
    string->data[length] = 0;
    string->length = length;
    return GxStatus::Ok;
}

GxStatus gxStringCopy(GxString* destination, const GxString* source) {
    if (!gxStringReady(destination) || !gxStringReady(source)) {
        return GxStatus::InvalidArgument;
    }
    if (destination == source) return GxStatus::Ok;
    return gxStringAssign(destination, source->data, source->length);
}

GxStatus gxStringAppend(GxString* string, const void* bytes, size_t length) {
    if (!gxStringReady(string) || (length != 0 && !bytes)) {
        return GxStatus::InvalidArgument;
    }
    if (length == 0) return GxStatus::Ok;
    if (length > static_cast<size_t>(-1) - string->length) return GxStatus::Overflow;
    const size_t newLength = string->length + length;
    if (newLength > string->maximum) return GxStatus::ResourceLimit;
    size_t sourceOffset = 0;
    const bool aliases = gxStringAlias(string, bytes, length, &sourceOffset);
    if (string->data && reinterpret_cast<uintptr_t>(bytes) >=
            reinterpret_cast<uintptr_t>(string->data) &&
        reinterpret_cast<uintptr_t>(bytes) - reinterpret_cast<uintptr_t>(string->data) <=
            string->length && !aliases) {
        return GxStatus::InvalidArgument;
    }
    const GxStatus status = gxStringReserve(string, newLength);
    if (status != GxStatus::Ok) return status;
    const uint8_t* source = aliases ? string->data + sourceOffset
                                    : static_cast<const uint8_t*>(bytes);
    gxMoveStringBytes(string->data + string->length, source, length);
    string->data[newLength] = 0;
    string->length = newLength;
    return GxStatus::Ok;
}

GxStatus gxStringCompare(const GxString* left, const GxString* right,
                         int* outComparison) {
    if (!gxStringReady(left) || !gxStringReady(right) || !outComparison) {
        return GxStatus::InvalidArgument;
    }
    const size_t common = left->length < right->length ? left->length : right->length;
    for (size_t i = 0; i < common; ++i) {
        const uint8_t a = left->data[i];
        const uint8_t b = right->data[i];
        if (a < b) { *outComparison = -1; return GxStatus::Ok; }
        if (a > b) { *outComparison = 1; return GxStatus::Ok; }
    }
    *outComparison = left->length < right->length ? -1 :
                      left->length > right->length ? 1 : 0;
    return GxStatus::Ok;
}

GxStatus gxStringCString(const GxString* string, const char** outCString) {
    if (!outCString) return GxStatus::InvalidArgument;
    *outCString = 0;
    if (!gxStringReady(string)) return GxStatus::InvalidArgument;
    for (size_t i = 0; i < string->length; ++i) {
        if (string->data[i] == 0) return GxStatus::InvalidArgument;
    }
    *outCString = string->data ? reinterpret_cast<const char*>(string->data)
                               : reinterpret_cast<const char*>(kGxEmptyString);
    return GxStatus::Ok;
}

GxStatus gxStringClear(GxString* string) {
    if (!gxStringReady(string)) return GxStatus::InvalidArgument;
    string->length = 0;
    if (string->data) string->data[0] = 0;
    return GxStatus::Ok;
}

size_t gxStringLength(const GxString* string) {
    return gxStringReady(string) ? string->length : 0;
}

size_t gxStringCapacity(const GxString* string) {
    return gxStringReady(string) ? string->capacity : 0;
}

const uint8_t* gxStringData(const GxString* string) {
    return gxStringReady(string) ? string->data : 0;
}

void gxStringDestroy(GxString* string) {
    if (!string || !string->initialized) return;
    gxAllocatorFree(&string->allocator, string->data);
    string->data = 0;
    string->length = 0;
    string->capacity = 0;
    string->maximum = 0;
    string->initialized = false;
    string->allocator.context = 0;
    string->allocator.allocate = 0;
    string->allocator.deallocate = 0;
    string->allocator.maxAllocation = 0;
    string->allocator.maxAlignment = 0;
}
