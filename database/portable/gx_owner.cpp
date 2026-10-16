#include "gx_owner.h"

GxStatus gxOwnerInit(GxOwner* owner) {
    if (!owner || owner->initialized) return GxStatus::InvalidArgument;
    owner->pointer = 0;
    owner->context = 0;
    owner->release = 0;
    owner->initialized = true;
    return GxStatus::Ok;
}

GxStatus gxOwnerAdopt(GxOwner* owner, void* pointer, void* context,
                      GxOwnerReleaseCallback release) {
    if (!owner || !owner->initialized || (pointer && !release)) {
        return GxStatus::InvalidArgument;
    }
    if (pointer && pointer == owner->pointer) return GxStatus::InvalidArgument;
    gxOwnerReset(owner);
    owner->pointer = pointer;
    owner->context = pointer ? context : 0;
    owner->release = pointer ? release : 0;
    return GxStatus::Ok;
}

GxStatus gxOwnerMove(GxOwner* destination, GxOwner* source) {
    if (!destination || !source || !destination->initialized || !source->initialized) {
        return GxStatus::InvalidArgument;
    }
    if (destination == source) return GxStatus::Ok;
    gxOwnerReset(destination);
    destination->pointer = source->pointer;
    destination->context = source->context;
    destination->release = source->release;
    source->pointer = 0;
    source->context = 0;
    source->release = 0;
    return GxStatus::Ok;
}

void* gxOwnerGet(const GxOwner* owner) {
    return owner && owner->initialized ? owner->pointer : 0;
}

void gxOwnerReset(GxOwner* owner) {
    if (!owner || !owner->initialized) return;
    void* pointer = owner->pointer;
    GxOwnerReleaseCallback release = owner->release;
    void* context = owner->context;
    owner->pointer = 0;
    owner->context = 0;
    owner->release = 0;
    if (pointer && release) release(context, pointer);
}

void gxOwnerDestroy(GxOwner* owner) {
    if (!owner || !owner->initialized) return;
    gxOwnerReset(owner);
    owner->initialized = false;
}
