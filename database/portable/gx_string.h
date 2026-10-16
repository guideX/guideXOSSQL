#ifndef GX_PORTABLE_STRING_H
#define GX_PORTABLE_STRING_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"

// Owned length-aware bytes. data[length] is always NUL when data is non-null;
// embedded NUL bytes before length are preserved and are not C strings.
struct GxString {
    GxAllocator allocator;
    uint8_t* data;
    size_t length;
    size_t capacity;
    size_t maximum;
    bool initialized;
};

// Like GxArray, GxString must be zero-initialized before its first init.
GxStatus gxStringInit(GxString* string, const GxAllocator* allocator,
                      size_t maximumLength);
GxStatus gxStringReserve(GxString* string, size_t capacity);
GxStatus gxStringAssign(GxString* string, const void* bytes, size_t length);
GxStatus gxStringCopy(GxString* destination, const GxString* source);
GxStatus gxStringAppend(GxString* string, const void* bytes, size_t length);
GxStatus gxStringCompare(const GxString* left, const GxString* right,
                         int* outComparison);
GxStatus gxStringCString(const GxString* string, const char** outCString);
GxStatus gxStringClear(GxString* string);
size_t gxStringLength(const GxString* string);
size_t gxStringCapacity(const GxString* string);
const uint8_t* gxStringData(const GxString* string);
void gxStringDestroy(GxString* string);

#endif
