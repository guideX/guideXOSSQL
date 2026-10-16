#ifndef GX_PORTABLE_OWNER_H
#define GX_PORTABLE_OWNER_H

#include "gx_status.h"

typedef void (*GxOwnerReleaseCallback)(void* context, void* pointer);

// A small explicit owner for one opaque resource. The adopted release callback
// is responsible for any partial-object cleanup and allocator pairing.
struct GxOwner {
    void* pointer;
    void* context;
    GxOwnerReleaseCallback release;
    bool initialized;
};

// The struct must be zero-initialized before its first init; destroy is
// idempotent and permits a later init.
GxStatus gxOwnerInit(GxOwner* owner);
GxStatus gxOwnerAdopt(GxOwner* owner, void* pointer, void* context,
                      GxOwnerReleaseCallback release);
GxStatus gxOwnerMove(GxOwner* destination, GxOwner* source);
void* gxOwnerGet(const GxOwner* owner);
void gxOwnerReset(GxOwner* owner);
void gxOwnerDestroy(GxOwner* owner);

#endif
