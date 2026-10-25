#include "gx_page_visit_set.h"

namespace {

static uint64_t mixPageId(uint64_t value) {
    value ^= value >> 30;
    value *= static_cast<uint64_t>(0xbf58476d1ce4e5b9ULL);
    value ^= value >> 27;
    value *= static_cast<uint64_t>(0x94d049bb133111ebULL);
    value ^= value >> 31;
    return value;
}

static uint64_t readSlot(const GxArray& slots, size_t index) {
    const uint8_t* bytes = slots.data + index * sizeof(uint64_t);
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

static void writeSlot(GxArray& slots, size_t index, uint64_t value) {
    uint8_t* bytes = slots.data + index * sizeof(uint64_t);
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

static size_t startSlot(uint64_t pageId, size_t capacity) {
    return static_cast<size_t>(mixPageId(pageId)) & (capacity - 1);
}

// Finds either the matching key or the first empty slot. Since capacity is a
// power of two and the table never exceeds 75% occupancy, probing terminates.
static bool findSlot(const GxArray& slots, size_t capacity, uint64_t pageId,
                     size_t& outSlot, bool& outFound) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) return false;
    size_t index = startSlot(pageId, capacity);
    for (size_t probes = 0; probes < capacity; ++probes) {
        const uint64_t value = readSlot(slots, index);
        if (value == pageId) {
            outSlot = index;
            outFound = true;
            return true;
        }
        if (value == 0) {
            outSlot = index;
            outFound = false;
            return true;
        }
        index = (index + 1) & (capacity - 1);
    }
    return false;
}

static bool insertIntoEmptyTable(GxArray& slots, size_t capacity,
                                 uint64_t pageId) {
    size_t index = 0;
    bool found = false;
    if (!findSlot(slots, capacity, pageId, index, found) || found) return false;
    writeSlot(slots, index, pageId);
    return true;
}

static GxStatus deriveMaximumCapacity(uint64_t maximumVisits,
                                      size_t& outCapacity) {
    outCapacity = 0;
    if (maximumVisits == 0) return GxStatus::Ok;

    // ceil(4 * maximumVisits / 3), computed without multiplying the full
    // value. The next power of two keeps the load threshold inexpensive.
    const uint64_t quotient = maximumVisits / 3;
    const uint64_t remainder = maximumVisits % 3;
    if (quotient > static_cast<uint64_t>(-1) / 4) return GxStatus::Overflow;
    uint64_t required = quotient * 4;
    const uint64_t addition = remainder == 1 ? 2 : (remainder == 2 ? 3 : 0);
    if (required > static_cast<uint64_t>(-1) - addition) {
        return GxStatus::Overflow;
    }
    required += addition;

    const uint64_t maximumSize = static_cast<uint64_t>(static_cast<size_t>(-1));
    if (required > maximumSize) return GxStatus::Overflow;
    size_t capacity = 1;
    const size_t target = static_cast<size_t>(required);
    while (capacity < target) {
        if (capacity > static_cast<size_t>(-1) / 2) return GxStatus::Overflow;
        capacity *= 2;
    }
    if (capacity > static_cast<size_t>(-1) / sizeof(uint64_t)) {
        return GxStatus::Overflow;
    }
    outCapacity = capacity;
    return GxStatus::Ok;
}

} // namespace

GxPageVisitSet::GxPageVisitSet()
    : _slots(), _pageCount(0), _maximumVisits(0), _count(0),
      _maximumCapacity(0), _initialized(false) {}

GxPageVisitSet::~GxPageVisitSet() { destroy(); }

GxStatus GxPageVisitSet::init(const GxAllocator* allocator, uint64_t pageCount,
                              uint64_t maximumVisits) {
    if (_initialized || !allocator || pageCount == 0 ||
        maximumVisits > pageCount - 1) {
        return GxStatus::InvalidArgument;
    }
    size_t maximumCapacity = 0;
    GxStatus status = deriveMaximumCapacity(maximumVisits, maximumCapacity);
    if (status != GxStatus::Ok) return status;
    status = gxArrayInit(&_slots, allocator, sizeof(uint64_t), maximumCapacity);
    if (status != GxStatus::Ok) return status;
    _pageCount = pageCount;
    _maximumVisits = maximumVisits;
    _count = 0;
    _maximumCapacity = maximumCapacity;
    _initialized = true;
    return GxStatus::Ok;
}

GxStatus GxPageVisitSet::visit(uint64_t pageId, bool& outRevisited) {
    outRevisited = false;
    if (!_initialized) return GxStatus::InvalidArgument;
    if (pageId == 0 || pageId >= _pageCount) return GxStatus::CorruptData;

    size_t slot = 0;
    bool found = false;
    const size_t currentCapacity = gxArrayCapacity(&_slots);
    if (currentCapacity != 0) {
        if (!findSlot(_slots, currentCapacity, pageId, slot, found)) {
            return GxStatus::ResourceLimit;
        }
        if (found) {
            outRevisited = true;
            return GxStatus::Ok;
        }
    }
    if (_count >= _maximumVisits) return GxStatus::ResourceLimit;

    const size_t loadLimit = currentCapacity - currentCapacity / 4;
    if (currentCapacity == 0 || _count >= loadLimit) {
        GxStatus status = grow();
        if (status != GxStatus::Ok) return status;
    }

    const size_t capacity = gxArrayCapacity(&_slots);
    if (!findSlot(_slots, capacity, pageId, slot, found) || found) {
        return GxStatus::ResourceLimit;
    }
    writeSlot(_slots, slot, pageId);
    ++_count;
    return GxStatus::Ok;
}

GxStatus GxPageVisitSet::reset() {
    if (!_initialized) return GxStatus::InvalidArgument;
    const size_t capacity = gxArrayCapacity(&_slots);
    for (size_t i = 0; i < capacity * sizeof(uint64_t); ++i) {
        _slots.data[i] = 0;
    }
    _count = 0;
    return GxStatus::Ok;
}

void GxPageVisitSet::destroy() {
    if (!_initialized) return;
    gxArrayDestroy(&_slots);
    _pageCount = 0;
    _maximumVisits = 0;
    _count = 0;
    _maximumCapacity = 0;
    _initialized = false;
}

GxStatus GxPageVisitSet::grow() {
    const size_t oldCapacity = gxArrayCapacity(&_slots);
    size_t newCapacity = 0;
    if (oldCapacity == 0) {
        newCapacity = _maximumCapacity < 16 ? _maximumCapacity : 16;
    } else if (oldCapacity >= _maximumCapacity) {
        return GxStatus::ResourceLimit;
    } else if (oldCapacity > _maximumCapacity / 2) {
        newCapacity = _maximumCapacity;
    } else {
        newCapacity = oldCapacity * 2;
    }
    if (newCapacity <= oldCapacity || newCapacity == 0) {
        return GxStatus::ResourceLimit;
    }

    GxArray replacement = {};
    GxStatus status = gxArrayInit(&replacement, &_slots.allocator,
                                  sizeof(uint64_t), _maximumCapacity);
    if (status != GxStatus::Ok) return status;
    status = gxArrayResize(&replacement, newCapacity);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&replacement);
        return status;
    }
    for (size_t i = 0; i < newCapacity * sizeof(uint64_t); ++i) {
        replacement.data[i] = 0;
    }
    for (size_t i = 0; i < oldCapacity; ++i) {
        const uint64_t value = readSlot(_slots, i);
        if (value != 0 && !insertIntoEmptyTable(replacement, newCapacity, value)) {
            gxArrayDestroy(&replacement);
            return GxStatus::ResourceLimit;
        }
    }
    status = gxArraySwap(&_slots, &replacement);
    gxArrayDestroy(&replacement);
    return status;
}
