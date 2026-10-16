#ifndef GX_PORTABLE_ARRAY_H
#define GX_PORTABLE_ARRAY_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"

// GxArray stores fixed-width byte records only. It does not start, move, or
// destroy C++ object lifetimes, so owning or nontrivial C++ values are not
// supported. Decode records into actual objects in caller-owned storage.
struct GxArray {
    GxAllocator allocator;
    uint8_t* data;
    size_t size;
    size_t capacity;
    size_t maximum;
    size_t elementSize;
    bool initialized;
};

// The struct must be zero-initialized before its first init. Init allocates no
// storage; destroy is idempotent and permits a later init.
GxStatus gxArrayInit(GxArray* array, const GxAllocator* allocator,
                     size_t elementSize, size_t maximumElements);
GxStatus gxArrayReserve(GxArray* array, size_t elements);
GxStatus gxArrayAppend(GxArray* array, const void* records, size_t count);
GxStatus gxArrayInsert(GxArray* array, size_t index,
                       const void* records, size_t count);
GxStatus gxArrayErase(GxArray* array, size_t index, size_t count);
GxStatus gxArrayClear(GxArray* array);
void* gxArrayAt(GxArray* array, size_t index);
const void* gxArrayAtConst(const GxArray* array, size_t index);
size_t gxArraySize(const GxArray* array);
size_t gxArrayCapacity(const GxArray* array);
void gxArrayDestroy(GxArray* array);

#endif
