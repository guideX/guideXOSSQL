#ifndef GX_PORTABLE_OWNED_PAGE_H
#define GX_PORTABLE_OWNED_PAGE_H

#include <stddef.h>
#include <stdint.h>

#include "gx_allocator.h"
#include "gx_page_codec.h"

// Caller-owned, fixed-capacity storage for one existing serialized SQL page.
// The allocator callbacks and context are copied into the owner. The context
// is borrowed and must outlive every page allocated through this owner,
// including pages retained after a Database closes.
class GxOwnedPage {
public:
    GxOwnedPage();
    ~GxOwnedPage();

    GxOwnedPage(const GxOwnedPage&) = delete;
    GxOwnedPage& operator=(const GxOwnedPage&) = delete;

    // Configures the exact supported page bound without allocating. An
    // explicit complete allocator is required; there is no hosted fallback.
    GxStatus init(const GxAllocator* allocator, uint32_t pageSize);
    // Configures an empty temporary owner with the same allocator identity and
    // page bound as `source`.
    GxStatus initLike(const GxOwnedPage& source);

    // Validates one complete encoded page with the established SQL page
    // inspector, allocates a full-page image through this owner's allocator,
    // then publishes the replacement only after the copy is complete.
    GxStatus materialize(const uint8_t* bytes, size_t byteCount,
                         uint64_t expectedPageId);

    // Deep-copies into the explicitly selected allocator domain. The source
    // remains unchanged; destination replacement occurs only after allocation
    // and validation succeed.
    GxStatus cloneTo(const GxAllocator* destinationAllocator,
                     GxOwnedPage& destination) const;

    // Transfers the bytes and their allocator provenance together. A bound
    // destination must use the same callbacks, context, and page size; a
    // different domain is rejected before either owner changes.
    GxStatus moveTo(GxOwnedPage& destination);

    // Releases page storage but retains allocator configuration for reuse.
    GxStatus clear();
    // Releases storage and configuration so the object may be initialized
    // again with another allocation domain.
    void destroy();

    bool isInitialized() const { return _initialized; }
    bool isValid() const { return _valid; }
    bool isConfiguredFor(uint32_t pageSize) const {
        return _initialized && _pageSize == pageSize;
    }
    uint32_t pageSize() const { return _pageSize; }
    uint64_t pageId() const { return _info.pageId; }
    uint16_t pageType() const { return _info.pageType; }
    uint16_t flags() const { return _info.flags; }
    uint32_t payloadSize() const { return _info.payloadSize; }
    uint32_t payloadCapacity() const;
    uint32_t generation() const { return _info.generation; }
    size_t byteCount() const { return _byteCount; }
    size_t allocatedCapacity() const { return _capacity; }
    const uint8_t* encodedBytes() const { return _bytes; }
    const GxAllocator& allocator() const { return _allocator; }

    // Reuses the authoritative length-, type-, identity-, bounds-, and
    // checksum-aware page validator against the retained bytes and metadata.
    GxStatus validate() const;

private:
    void releaseStorage();
    void resetConfiguration();
    void replaceFrom(GxOwnedPage& source);

    GxAllocator _allocator;
    uint8_t* _bytes;
    size_t _byteCount;
    size_t _capacity;
    uint32_t _pageSize;
    GxDatabasePageInfo _info;
    bool _initialized;
    bool _valid;
};

#endif
