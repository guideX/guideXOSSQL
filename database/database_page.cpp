#include "database_page.h"

#include <cstring>

#include "database_checksum.h"
#include "database_endian.h"

namespace gxos {
namespace db {

namespace {

// CRC32 over an entire page with the stored CRC field treated as zero.
uint32_t computePageCrc(const uint8_t* bytes, uint32_t pageSize) {
    uint32_t crc = crc32Init();
    crc = crc32Update(crc, bytes, page_offset::PageCrc32);
    const uint8_t zero[4] = {0, 0, 0, 0};
    crc = crc32Update(crc, zero, sizeof(zero));
    const uint32_t tailOffset = page_offset::PageCrc32 + 4;
    crc = crc32Update(crc, bytes + tailOffset, pageSize - tailOffset);
    return crc32Final(crc);
}

} // namespace

DatabasePage::DatabasePage()
    : pageId(0),
      type(PageType::Unknown),
      flags(0),
      payloadSize(0),
      generation(0) {}

uint32_t DatabasePage::payloadCapacity(uint32_t pageSize) {
    if (pageSize <= kPageHeaderSize) {
        return 0;
    }
    return pageSize - kPageHeaderSize;
}

DbResult DatabasePage::serialize(std::vector<uint8_t>& out, uint32_t pageSize) const {
    if (!isSupportedPageSize(pageSize)) {
        return DbResult::error(DbStatus::InvalidArgument, "unsupported page size");
    }
    const uint32_t capacity = payloadCapacity(pageSize);
    if (payload.size() > capacity) {
        return DbResult::error(DbStatus::InvalidArgument, "payload exceeds page capacity");
    }

    out.assign(pageSize, 0);

    storeLe64(out.data() + page_offset::PageId, pageId);
    storeLe16(out.data() + page_offset::PageType, static_cast<uint16_t>(type));
    storeLe16(out.data() + page_offset::Flags, flags);
    storeLe32(out.data() + page_offset::PayloadSize,
              static_cast<uint32_t>(payload.size()));
    storeLe32(out.data() + page_offset::Generation, generation);
    if (!payload.empty()) {
        std::memcpy(out.data() + kPageHeaderSize, payload.data(), payload.size());
    }

    const uint32_t crc = computePageCrc(out.data(), pageSize);
    storeLe32(out.data() + page_offset::PageCrc32, crc);
    return DbResult::ok();
}

DbResult DatabasePage::parse(const uint8_t* bytes, uint32_t pageSize,
                             uint64_t expectedPageId, DatabasePage& out) {
    if (bytes == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "null page buffer");
    }
    if (!isSupportedPageSize(pageSize)) {
        return DbResult::error(DbStatus::InvalidArgument, "unsupported page size");
    }

    const uint64_t storedPageId = loadLe64(bytes + page_offset::PageId);
    if (storedPageId != expectedPageId) {
        return DbResult::error(DbStatus::CorruptPage, "page id does not match its index");
    }

    const uint32_t payloadSize = loadLe32(bytes + page_offset::PayloadSize);
    const uint32_t capacity = payloadCapacity(pageSize);
    if (payloadSize > capacity) {
        return DbResult::error(DbStatus::CorruptPage, "payload size exceeds page capacity");
    }

    const uint32_t storedCrc = loadLe32(bytes + page_offset::PageCrc32);
    if (storedCrc != computePageCrc(bytes, pageSize)) {
        return DbResult::error(DbStatus::CorruptPage, "page checksum mismatch");
    }

    DatabasePage page;
    page.pageId = storedPageId;
    page.type = static_cast<PageType>(loadLe16(bytes + page_offset::PageType));
    page.flags = loadLe16(bytes + page_offset::Flags);
    page.payloadSize = payloadSize;
    page.generation = loadLe32(bytes + page_offset::Generation);
    if (payloadSize > 0) {
        page.payload.assign(bytes + kPageHeaderSize, bytes + kPageHeaderSize + payloadSize);
    }

    out = page;
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
