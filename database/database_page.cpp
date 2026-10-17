#include "database_page.h"

#include <cstring>
#include <utility>

#include "database_endian.h"
#include "portable/gx_page_codec.h"

namespace gxos {
namespace db {

namespace {

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

    out.resize(pageSize);
    const GxStatus encodeStatus = gxDatabasePageEncode(
        out.data(), pageSize, pageId, static_cast<uint16_t>(type), flags,
        generation, payload.empty() ? nullptr : payload.data(), payload.size());
    if (encodeStatus != GxStatus::Ok) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "database page encoding rejected its input");
    }
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

    GxDatabasePageInfo info = {};
    const GxStatus inspectStatus = gxDatabasePageInspect(
        bytes, pageSize, expectedPageId, &info);
    if (inspectStatus == GxStatus::CorruptData) {
        return DbResult::error(DbStatus::CorruptPage,
                               "page identity, payload bounds, or checksum is invalid");
    }
    if (inspectStatus != GxStatus::Ok) {
        return DbResult::error(DbStatus::InvalidArgument,
                               "page image inspection rejected its input");
    }

    DatabasePage page;
    page.pageId = info.pageId;
    page.type = static_cast<PageType>(info.pageType);
    page.flags = info.flags;
    page.payloadSize = info.payloadSize;
    page.generation = info.generation;
    if (info.payloadSize > 0) {
        page.payload.assign(bytes + kPageHeaderSize,
                            bytes + kPageHeaderSize + info.payloadSize);
    }

    out = std::move(page);
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
