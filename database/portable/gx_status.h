#ifndef GX_PORTABLE_STATUS_H
#define GX_PORTABLE_STATUS_H

enum class GxStatus {
    Ok = 0,
    InvalidArgument,
    OutOfMemory,
    ResourceLimit,
    Overflow,
    UnsupportedAlignment,
    CorruptData
};

#endif
