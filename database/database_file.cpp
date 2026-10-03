#include "database_file.h"

#include <cstring>
#include <utility>

#include "database_endian.h"

namespace gxos {
namespace db {

namespace {

std::vector<uint8_t> makeCatalogPayload() {
    // SQL2 root catalog header: empty catalog, next table id 1.
    std::vector<uint8_t> payload(kCatalogRootHeaderSize, 0);
    storeLe32(payload.data() + 0, kCatalogMagic);
    storeLe16(payload.data() + 4, kCatalogVersion);
    storeLe16(payload.data() + 6, 0);  // reserved
    storeLe32(payload.data() + 8, 0);  // tableCount
    storeLe32(payload.data() + 12, 1); // nextTableId
    storeLe32(payload.data() + 16, 0); // continuationCount
    storeLe32(payload.data() + 20, 0); // firstContinuationPageId
    storeLe32(payload.data() + 24, 0); // reserved
    storeLe32(payload.data() + 28, 0); // reserved
    return payload;
}

} // namespace

DatabaseFile::DatabaseFile()
    : _header(),
      _diagnostics(),
      _lastError(),
      _open(false),
      _readOnly(false),
      _headerDirty(false) {}

DatabaseFile::~DatabaseFile() {
    if (_open) {
        close();
    }
}

DatabaseFile::DatabaseFile(DatabaseFile&& other) noexcept
    : _file(std::move(other._file)),
      _header(other._header),
      _diagnostics(std::move(other._diagnostics)),
      _lastError(std::move(other._lastError)),
      _open(other._open),
      _readOnly(other._readOnly),
      _headerDirty(other._headerDirty) {
    other._open = false;
    other._readOnly = false;
    other._headerDirty = false;
}

DatabaseFile& DatabaseFile::operator=(DatabaseFile&& other) noexcept {
    if (this != &other) {
        if (_open) {
            close();
        }
        _file = std::move(other._file);
        _header = other._header;
        _diagnostics = std::move(other._diagnostics);
        _lastError = std::move(other._lastError);
        _open = other._open;
        _readOnly = other._readOnly;
        _headerDirty = other._headerDirty;
        other._open = false;
        other._readOnly = false;
        other._headerDirty = false;
    }
    return *this;
}

DbResult DatabaseFile::fail(DbStatus status, const std::string& message) {
    _lastError = DbResult::error(status, message);
    _diagnostics.state = status;
    _diagnostics.lastValidationFailure = _lastError.describe();
    _diagnostics.open = _open;
    _diagnostics.readOnly = _readOnly;
    return _lastError;
}

void DatabaseFile::refreshDiagnostics() {
    _diagnostics.open = _open;
    _diagnostics.readOnly = _readOnly;
    _diagnostics.formatMajor = _header.formatMajor;
    _diagnostics.formatMinor = _header.formatMinor;
    _diagnostics.pageSize = _header.pageSize;
    _diagnostics.pageCount = _header.pageCount;
    _diagnostics.rootPageId = _header.rootPageId;
    _diagnostics.databaseId = formatDatabaseId(_header.databaseId);
    _diagnostics.fileSizeBytes = _file ? _file->size() : 0;
    _diagnostics.state = _lastError.status();
}

bool DatabaseFile::computePageOffset(uint64_t pageId, uint64_t& outOffset) const {
    if (pageId > kMaxPageCount) {
        return false;
    }
    uint64_t offset = 0;
    if (!checkedMulU64(pageId, static_cast<uint64_t>(_header.pageSize), offset)) {
        return false;
    }
    uint64_t end = 0;
    if (!checkedAddU64(offset, static_cast<uint64_t>(_header.pageSize), end)) {
        return false;
    }
    outOffset = offset;
    return true;
}

DbResult DatabaseFile::writeHeaderPage() {
    std::vector<uint8_t> page0(_header.pageSize, 0);
    _header.serialize(page0.data());
    if (!_file->writeAt(0, page0.data(), page0.size())) {
        return DbResult::error(DbStatus::IoError, "failed to write header page");
    }
    return DbResult::ok();
}

DbResult DatabaseFile::readPageBytes(uint64_t pageId, std::vector<uint8_t>& out) {
    if (pageId >= _header.pageCount) {
        return DbResult::error(DbStatus::OutOfBounds, "page id outside allocated range");
    }
    uint64_t offset = 0;
    if (!computePageOffset(pageId, offset)) {
        return DbResult::error(DbStatus::CorruptHeader, "page offset overflow");
    }
    out.assign(_header.pageSize, 0);
    if (!_file->readAt(offset, out.data(), out.size())) {
        return DbResult::error(DbStatus::Truncated, "short page read");
    }
    return DbResult::ok();
}

DbResult DatabaseFile::validateBootstrap(const DatabasePage& root) {
    if (root.type != PageType::Catalog) {
        return DbResult::error(DbStatus::CorruptPage, "root page is not a catalog page");
    }
    if (root.payloadSize < kCatalogPayloadSize) {
        return DbResult::error(DbStatus::CorruptPage, "catalog payload too small");
    }
    if (loadLe32(root.payload.data() + 0) != kCatalogMagic) {
        return DbResult::error(DbStatus::CorruptPage, "catalog signature mismatch");
    }
    const uint16_t catalogVersion = loadLe16(root.payload.data() + 4);
    if (catalogVersion == kCatalogVersionLegacy) {
        // SQL1 database: empty catalog, no tables.
        return DbResult::ok();
    }
    if (catalogVersion != kCatalogVersion) {
        return DbResult::error(DbStatus::UnsupportedVersion,
                               "unsupported catalog version " +
                                   std::to_string(static_cast<unsigned>(catalogVersion)));
    }
    if (root.payloadSize < kCatalogRootHeaderSize) {
        return DbResult::error(DbStatus::CorruptPage, "catalog root header truncated");
    }
    return DbResult::ok();
}

DbResult DatabaseFile::create(std::unique_ptr<IDatabaseFile> file,
                              const DatabaseCreateOptions& options,
                              std::unique_ptr<DatabaseFile>& out) {
    if (!file || !file->isOpen()) {
        return DbResult::error(DbStatus::InvalidArgument, "file must be open");
    }
    if (file->isReadOnly()) {
        return DbResult::error(DbStatus::InvalidArgument, "file must be writable");
    }
    if (!isSupportedPageSize(options.pageSize)) {
        return DbResult::error(DbStatus::InvalidArgument, "unsupported page size");
    }

    std::unique_ptr<DatabaseFile> db(new DatabaseFile());
    db->_file = std::move(file);
    db->_open = true;
    db->_readOnly = false;
    db->_headerDirty = false;

    uint8_t id[16];
    if (options.generateDatabaseId) {
        generateDatabaseId(id);
    } else {
        std::memcpy(id, options.databaseId, 16);
    }
    const uint64_t creationTime = options.creationTimeUnixNanos != 0
                                      ? options.creationTimeUnixNanos
                                      : currentUnixTimeNanos();
    db->_header = DatabaseHeader::makeDefault(options.pageSize, id, creationTime);

    // Write the bootstrap page first, then let flush() write the header last.
    DatabasePage root;
    root.pageId = kBootstrapPageId;
    root.type = PageType::Catalog;
    root.payload = makeCatalogPayload();
    root.payloadSize = static_cast<uint32_t>(root.payload.size());

    std::vector<uint8_t> rootBytes;
    DbResult result = root.serialize(rootBytes, options.pageSize);
    if (!result.isOk()) {
        db->_file->close();
        db->_open = false;
        return result;
    }

    uint64_t rootOffset = 0;
    if (!db->computePageOffset(kBootstrapPageId, rootOffset) ||
        !db->_file->writeAt(rootOffset, rootBytes.data(), rootBytes.size())) {
        db->_file->close();
        db->_open = false;
        return DbResult::error(DbStatus::IoError, "failed to write bootstrap page");
    }

    db->_headerDirty = true;
    result = db->flush();
    if (!result.isOk()) {
        db->_file->close();
        db->_open = false;
        return result;
    }

    db->refreshDiagnostics();
    out = std::move(db);
    return DbResult::ok();
}

DbResult DatabaseFile::open(std::unique_ptr<IDatabaseFile> file,
                            const DatabaseOpenOptions& options,
                            std::unique_ptr<DatabaseFile>& out) {
    if (!file || !file->isOpen()) {
        return DbResult::error(DbStatus::InvalidArgument, "file must be open");
    }

    std::unique_ptr<DatabaseFile> db(new DatabaseFile());
    db->_file = std::move(file);
    db->_open = true;
    db->_readOnly = options.readOnly;
    db->_headerDirty = false;

    const uint64_t fileSize = db->_file->size();
    db->_diagnostics.fileSizeBytes = fileSize;
    if (fileSize < kHeaderSize) {
        return db->fail(DbStatus::Truncated, "file smaller than database header");
    }

    uint8_t headerBytes[kHeaderSize];
    if (!db->_file->readAt(0, headerBytes, kHeaderSize)) {
        return db->fail(DbStatus::IoError, "failed to read database header");
    }

    DatabaseHeader header;
    DbResult result = DatabaseHeader::parse(headerBytes, kHeaderSize, header);
    if (!result.isOk()) {
        return db->fail(result.status(), result.message());
    }
    db->_header = header;

    const uint32_t pageSize = header.pageSize;

    uint64_t requiredBytes = 0;
    if (!checkedMulU64(header.pageCount, static_cast<uint64_t>(pageSize), requiredBytes)) {
        return db->fail(DbStatus::CorruptHeader, "page count overflows file size");
    }
    if (fileSize < requiredBytes) {
        return db->fail(DbStatus::Truncated, "file smaller than allocated page count");
    }
    if (fileSize % pageSize != 0) {
        return db->fail(DbStatus::CorruptHeader, "file size is not page aligned");
    }
    // A larger file means uncommitted orphan pages from an interrupted write;
    // they are ignored and reused by the next append.

    std::vector<uint8_t> rootBytes;
    result = db->readPageBytes(header.rootPageId, rootBytes);
    if (!result.isOk()) {
        return db->fail(result.status(), result.message());
    }

    DatabasePage root;
    result = DatabasePage::parse(rootBytes.data(), pageSize, header.rootPageId, root);
    if (!result.isOk()) {
        return db->fail(result.status(), result.message());
    }
    result = db->validateBootstrap(root);
    if (!result.isOk()) {
        return db->fail(result.status(), result.message());
    }

    if (options.validateAllPages) {
        for (uint64_t pageId = 1; pageId < header.pageCount; ++pageId) {
            std::vector<uint8_t> bytes;
            result = db->readPageBytes(pageId, bytes);
            if (!result.isOk()) {
                return db->fail(result.status(), result.message());
            }
            DatabasePage page;
            result = DatabasePage::parse(bytes.data(), pageSize, pageId, page);
            if (!result.isOk()) {
                return db->fail(result.status(), result.message());
            }
        }
    }

    db->_headerDirty = false;
    db->refreshDiagnostics();
    out = std::move(db);
    return DbResult::ok();
}

DbResult DatabaseFile::close() {
    if (!_open) {
        return DbResult::ok();
    }
    DbResult result = DbResult::ok();
    if (!_readOnly && _headerDirty) {
        result = flush();
    }
    _file->close();
    _open = false;
    _headerDirty = false;
    _diagnostics.open = false;
    return result;
}

DbResult DatabaseFile::readPage(uint64_t pageId, DatabasePage& out) {
    _lastError = DbResult::ok();
    if (!_open) {
        return fail(DbStatus::NotOpen, "database is not open");
    }
    if (pageId == kHeaderPageId) {
        return fail(DbStatus::InvalidArgument, "page 0 is the header page");
    }
    if (pageId >= _header.pageCount) {
        return fail(DbStatus::OutOfBounds, "page id outside allocated range");
    }

    std::vector<uint8_t> bytes;
    DbResult result = readPageBytes(pageId, bytes);
    if (!result.isOk()) {
        return fail(result.status(), result.message());
    }

    DatabasePage page;
    result = DatabasePage::parse(bytes.data(), _header.pageSize, pageId, page);
    if (!result.isOk()) {
        return fail(result.status(), result.message());
    }

    out = page;
    return DbResult::ok();
}

DbResult DatabaseFile::writePage(const DatabasePage& page) {
    _lastError = DbResult::ok();
    if (!_open) {
        return fail(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return fail(DbStatus::ReadOnly, "database opened read-only");
    }
    if (page.pageId == kHeaderPageId || page.pageId >= _header.pageCount) {
        return fail(DbStatus::OutOfBounds, "page id outside allocated range");
    }

    std::vector<uint8_t> bytes;
    DbResult result = page.serialize(bytes, _header.pageSize);
    if (!result.isOk()) {
        return fail(result.status(), result.message());
    }

    uint64_t offset = 0;
    if (!computePageOffset(page.pageId, offset)) {
        return fail(DbStatus::OutOfBounds, "page offset overflow");
    }
    if (!_file->writeAt(offset, bytes.data(), bytes.size())) {
        return fail(DbStatus::IoError, "failed to write page");
    }
    return DbResult::ok();
}

DbResult DatabaseFile::allocatePage(PageType type, uint64_t& outPageId) {
    _lastError = DbResult::ok();
    if (!_open) {
        return fail(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return fail(DbStatus::ReadOnly, "database opened read-only");
    }

    const uint64_t newPageId = _header.pageCount;
    if (newPageId >= kMaxPageCount) {
        return fail(DbStatus::NoSpace, "database reached the page count limit");
    }

    DatabasePage page;
    page.pageId = newPageId;
    page.type = type;
    page.flags = 0;
    page.payloadSize = 0;
    page.generation = 0;

    std::vector<uint8_t> bytes;
    DbResult result = page.serialize(bytes, _header.pageSize);
    if (!result.isOk()) {
        return fail(result.status(), result.message());
    }

    uint64_t offset = 0;
    if (!computePageOffset(newPageId, offset)) {
        return fail(DbStatus::NoSpace, "page offset overflow");
    }
    if (!_file->writeAt(offset, bytes.data(), bytes.size())) {
        return fail(DbStatus::IoError, "failed to write allocated page");
    }

    _header.pageCount = newPageId + 1;
    _headerDirty = true;
    outPageId = newPageId;
    refreshDiagnostics();
    return DbResult::ok();
}

DbResult DatabaseFile::setPageCount(uint64_t pageCount) {
    _lastError = DbResult::ok();
    if (!_open) {
        return fail(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return fail(DbStatus::ReadOnly, "database opened read-only");
    }
    if (pageCount < 2 || pageCount > kMaxPageCount) {
        return fail(DbStatus::CorruptHeader, "page count outside policy range");
    }
    if (pageCount < _header.pageCount) {
        return fail(DbStatus::InvalidArgument, "page count cannot shrink");
    }
    _header.pageCount = pageCount;
    _headerDirty = true;
    refreshDiagnostics();
    return DbResult::ok();
}

DbResult DatabaseFile::applyHeaderImage(const DatabaseHeader& image) {
    _lastError = DbResult::ok();
    if (!_open) {
        return fail(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return fail(DbStatus::ReadOnly, "database opened read-only");
    }
    if (image.pageSize != _header.pageSize) {
        return fail(DbStatus::CorruptHeader, "WAL header page size does not match database");
    }
    if (image.pageCount < 2 || image.pageCount > kMaxPageCount) {
        return fail(DbStatus::CorruptHeader, "WAL header page count outside policy range");
    }
    if (image.rootPageId < 1 || image.rootPageId >= image.pageCount) {
        return fail(DbStatus::CorruptHeader, "WAL root page id outside allocated range");
    }
    _header.pageCount = image.pageCount;
    _header.rootPageId = image.rootPageId;
    _headerDirty = true;
    refreshDiagnostics();
    return DbResult::ok();
}

DbResult DatabaseFile::flush() {
    _lastError = DbResult::ok();
    if (!_open) {
        return fail(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::ok();
    }

    if (_headerDirty) {
        DbResult result = writeHeaderPage();
        if (!result.isOk()) {
            return fail(result.status(), result.message());
        }
        _headerDirty = false;
    }

    if (!_file->flush()) {
        return fail(DbStatus::IoError, "failed to flush database file");
    }
    refreshDiagnostics();
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
