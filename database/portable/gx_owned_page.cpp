#include "gx_owned_page.h"

#include "database_endian.h"

namespace {

bool sameAllocationDomain(const GxAllocator& left,
                          const GxAllocator& right) {
    return left.context == right.context && left.allocate == right.allocate &&
           left.deallocate == right.deallocate;
}

void copyPageBytes(uint8_t* destination, const uint8_t* source,
                   size_t byteCount) {
    for (size_t i = 0; i < byteCount; ++i) destination[i] = source[i];
}

} // namespace

GxOwnedPage::GxOwnedPage()
    : _allocator(), _bytes(0), _byteCount(0), _capacity(0), _pageSize(0),
      _info(), _initialized(false), _valid(false) {
    _info.pageId = 0;
    _info.pageType = 0;
    _info.flags = 0;
    _info.payloadSize = 0;
    _info.generation = 0;
}

GxOwnedPage::~GxOwnedPage() { destroy(); }

GxStatus GxOwnedPage::init(const GxAllocator* allocator, uint32_t pageSize) {
    if (_initialized || !allocator || !allocator->allocate ||
        !allocator->deallocate || !gxos::db::isSupportedPageSize(pageSize)) {
        return GxStatus::InvalidArgument;
    }
    if (allocator->maxAllocation < pageSize) return GxStatus::ResourceLimit;
    if (allocator->maxAlignment < 1) return GxStatus::UnsupportedAlignment;

    _allocator = *allocator;
    _bytes = 0;
    _byteCount = 0;
    _capacity = 0;
    _pageSize = pageSize;
    _info.pageId = 0;
    _info.pageType = 0;
    _info.flags = 0;
    _info.payloadSize = 0;
    _info.generation = 0;
    _initialized = true;
    _valid = false;
    return GxStatus::Ok;
}

GxStatus GxOwnedPage::initLike(const GxOwnedPage& source) {
    if (!source._initialized) return GxStatus::InvalidArgument;
    return init(&source._allocator, source._pageSize);
}

GxStatus GxOwnedPage::materialize(const uint8_t* bytes, size_t byteCount,
                                  uint64_t expectedPageId) {
    if (!_initialized) return GxStatus::InvalidArgument;

    GxDatabasePageInfo info = {};
    const GxStatus inspectStatus = gxDatabasePageInspect(
        bytes, byteCount, _pageSize, expectedPageId, &info);
    if (inspectStatus != GxStatus::Ok) return inspectStatus;

    void* allocation = 0;
    const GxStatus allocationStatus = gxAllocatorAllocate(
        &_allocator, _pageSize, 1, &allocation);
    if (allocationStatus != GxStatus::Ok) return allocationStatus;
    copyPageBytes(static_cast<uint8_t*>(allocation), bytes, _pageSize);

    uint8_t* oldBytes = _bytes;
    _bytes = static_cast<uint8_t*>(allocation);
    _byteCount = _pageSize;
    _capacity = _pageSize;
    _info = info;
    _valid = true;
    gxAllocatorFree(&_allocator, oldBytes);
    return GxStatus::Ok;
}

GxStatus GxOwnedPage::cloneTo(const GxAllocator* destinationAllocator,
                             GxOwnedPage& destination) const {
    if (!_initialized || !_valid || !destinationAllocator ||
        !destinationAllocator->allocate || !destinationAllocator->deallocate) {
        return GxStatus::InvalidArgument;
    }

    GxOwnedPage prepared;
    GxStatus status = prepared.init(destinationAllocator, _pageSize);
    if (status != GxStatus::Ok) return status;
    status = prepared.materialize(_bytes, _byteCount, _info.pageId);
    if (status != GxStatus::Ok) return status;

    destination.replaceFrom(prepared);
    return GxStatus::Ok;
}

GxStatus GxOwnedPage::moveTo(GxOwnedPage& destination) {
    if (this == &destination || !_initialized) {
        return GxStatus::InvalidArgument;
    }
    if (destination._initialized &&
        (!sameAllocationDomain(_allocator, destination._allocator) ||
         _pageSize != destination._pageSize)) {
        return GxStatus::InvalidArgument;
    }

    if (!destination._initialized) {
        destination._allocator = _allocator;
        destination._pageSize = _pageSize;
        destination._initialized = true;
    } else {
        destination.releaseStorage();
        destination._allocator = _allocator;
    }
    destination._bytes = _bytes;
    destination._byteCount = _byteCount;
    destination._capacity = _capacity;
    destination._info = _info;
    destination._valid = _valid;

    _bytes = 0;
    _byteCount = 0;
    _capacity = 0;
    _info.pageId = 0;
    _info.pageType = 0;
    _info.flags = 0;
    _info.payloadSize = 0;
    _info.generation = 0;
    _valid = false;
    return GxStatus::Ok;
}

GxStatus GxOwnedPage::clear() {
    if (!_initialized) return GxStatus::InvalidArgument;
    releaseStorage();
    return GxStatus::Ok;
}

void GxOwnedPage::destroy() {
    releaseStorage();
    resetConfiguration();
}

uint32_t GxOwnedPage::payloadCapacity() const {
    if (!_initialized || _pageSize <= gxos::db::kPageHeaderSize) return 0;
    return _pageSize - gxos::db::kPageHeaderSize;
}

GxStatus GxOwnedPage::validate() const {
    if (!_initialized || !_valid || !_bytes ||
        _byteCount != _pageSize || _capacity < _byteCount) {
        return GxStatus::InvalidArgument;
    }
    GxDatabasePageInfo inspected = {};
    const GxStatus status = gxDatabasePageInspect(
        _bytes, _byteCount, _pageSize, _info.pageId, &inspected);
    if (status != GxStatus::Ok) return status;
    if (inspected.pageId != _info.pageId ||
        inspected.pageType != _info.pageType ||
        inspected.flags != _info.flags ||
        inspected.payloadSize != _info.payloadSize ||
        inspected.generation != _info.generation) {
        return GxStatus::CorruptData;
    }
    return GxStatus::Ok;
}

void GxOwnedPage::releaseStorage() {
    gxAllocatorFree(&_allocator, _bytes);
    _bytes = 0;
    _byteCount = 0;
    _capacity = 0;
    _info.pageId = 0;
    _info.pageType = 0;
    _info.flags = 0;
    _info.payloadSize = 0;
    _info.generation = 0;
    _valid = false;
}

void GxOwnedPage::resetConfiguration() {
    _allocator.context = 0;
    _allocator.allocate = 0;
    _allocator.deallocate = 0;
    _allocator.maxAllocation = 0;
    _allocator.maxAlignment = 0;
    _pageSize = 0;
    _initialized = false;
}

void GxOwnedPage::replaceFrom(GxOwnedPage& source) {
    if (this == &source) return;
    releaseStorage();
    resetConfiguration();
    _allocator = source._allocator;
    _bytes = source._bytes;
    _byteCount = source._byteCount;
    _capacity = source._capacity;
    _pageSize = source._pageSize;
    _info = source._info;
    _initialized = source._initialized;
    _valid = source._valid;

    source._bytes = 0;
    source._byteCount = 0;
    source._capacity = 0;
    source._pageSize = 0;
    source._info.pageId = 0;
    source._info.pageType = 0;
    source._info.flags = 0;
    source._info.payloadSize = 0;
    source._info.generation = 0;
    source._allocator.context = 0;
    source._allocator.allocate = 0;
    source._allocator.deallocate = 0;
    source._allocator.maxAllocation = 0;
    source._allocator.maxAlignment = 0;
    source._initialized = false;
    source._valid = false;
}
