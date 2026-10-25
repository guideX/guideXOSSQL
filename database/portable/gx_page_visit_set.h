#ifndef GX_PORTABLE_PAGE_VISIT_SET_H
#define GX_PORTABLE_PAGE_VISIT_SET_H

#include <stddef.h>
#include <stdint.h>

#include "gx_array.h"

// Temporary membership for non-header database page identifiers. Capacity is
// bounded by the caller's validated logical page/work limit and grows only as
// actual page visits are recorded.
class GxPageVisitSet {
public:
    GxPageVisitSet();
    ~GxPageVisitSet();

    GxPageVisitSet(const GxPageVisitSet&) = delete;
    GxPageVisitSet& operator=(const GxPageVisitSet&) = delete;

    // pageCount is the authoritative logical page count. maximumVisits must
    // not exceed pageCount - 1. Initialization does not allocate storage.
    GxStatus init(const GxAllocator* allocator, uint64_t pageCount,
                  uint64_t maximumVisits);

    // Records pageId. On success, outRevisited distinguishes first insertion
    // from an already recorded page. Invalid page IDs return CorruptData;
    // allocation and checked-capacity errors remain explicit GxStatus values.
    GxStatus visit(uint64_t pageId, bool& outRevisited);

    // Clears membership while retaining allocated capacity.
    GxStatus reset();
    void destroy();

    uint64_t size() const { return _count; }
    uint64_t maximumVisits() const { return _maximumVisits; }
    size_t capacity() const { return gxArrayCapacity(&_slots); }
    size_t maximumCapacity() const { return _maximumCapacity; }
    bool initialized() const { return _initialized; }

private:
    GxStatus grow();

    GxArray _slots;
    uint64_t _pageCount;
    uint64_t _maximumVisits;
    uint64_t _count;
    size_t _maximumCapacity;
    bool _initialized;
};

#endif
