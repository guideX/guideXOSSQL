#include "database_wal.h"

#include <cstring>
#include <new>
#include <utility>

#include "database_checksum.h"
#include "database_endian.h"
#include "portable/gx_allocator_hosted.h"
#include "portable/gx_array.h"
#include "portable/gx_wal_page_image.h"
#include "portable/gx_wal_record.h"
#include "portable/gx_wal_scan.h"

namespace gxos {
namespace db {

const uint8_t kWalMagic[8] = {'G', 'X', 'W', 'A', 'L', '\r', '\n', 0x1A};

static_assert(kWalRecordPrefixSize == GX_WAL_RECORD_PREFIX_SIZE,
              "portable WAL record prefix must match SQL3 framing");
static_assert(kWalRecordBegin == GX_WAL_SCAN_RECORD_BEGIN &&
                  kWalRecordPageImage == GX_WAL_SCAN_RECORD_PAGE_IMAGE &&
                  kWalRecordHeaderImage == GX_WAL_SCAN_RECORD_DB_HEADER &&
                  kWalRecordCommit == GX_WAL_SCAN_RECORD_COMMIT,
              "portable WAL scanner record ids must match SQL3 framing");
static_assert(static_cast<uint32_t>(wal_page_offset::PageBytes) ==
                  static_cast<uint32_t>(GX_WAL_PAGE_IMAGE_HEADER_SIZE),
              "portable PAGE_IMAGE header must match SQL3 framing");

std::string walPathFor(const std::string& databasePath) {
    const size_t slash = databasePath.find_last_of("/\\");
    const size_t dot = databasePath.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) {
        return databasePath.substr(0, dot) + ".gxwal";
    }
    return databasePath + ".gxwal";
}

namespace {

uint32_t computeHeaderCrc(const uint8_t* bytes) {
    uint32_t crc = crc32Init();
    crc = crc32Update(crc, bytes, wal_header_offset::HeaderCrc32);
    const uint8_t zero[4] = {0, 0, 0, 0};
    crc = crc32Update(crc, zero, sizeof(zero));
    crc = crc32Update(crc, bytes + wal_header_offset::HeaderCrc32 + 4,
                      kWalHeaderSize - (wal_header_offset::HeaderCrc32 + 4));
    return crc32Final(crc);
}

void serializeWalHeader(uint8_t* out, uint32_t pageSize, const uint8_t dbId[16],
                        uint64_t generation, uint64_t lastTransactionId) {
    std::memset(out, 0, kWalHeaderSize);
    std::memcpy(out + wal_header_offset::Magic, kWalMagic, 8);
    storeLe16(out + wal_header_offset::FormatVersion, kWalFormatVersion);
    storeLe16(out + wal_header_offset::HeaderSize, static_cast<uint16_t>(kWalHeaderSize));
    storeLe32(out + wal_header_offset::PageSize, pageSize);
    std::memcpy(out + wal_header_offset::DatabaseId, dbId, 16);
    storeLe64(out + wal_header_offset::Generation, generation);
    storeLe64(out + wal_header_offset::LastTransactionId, lastTransactionId);
    storeLe32(out + wal_header_offset::Flags, 0);
    storeLe32(out + wal_header_offset::HeaderCrc32, computeHeaderCrc(out));
}

bool readWalBytes(void* context, uint64_t offset, uint8_t* buffer,
                  size_t length) {
    IDatabaseFile* file = static_cast<IDatabaseFile*>(context);
    return file != nullptr && file->readAt(offset, buffer, length);
}

struct WalRecordScanContext {
    uint32_t pageSize;
    const uint8_t* databaseId;
    bool haveTransaction;
    WalTransaction current;
    std::vector<WalTransaction>* transactions;
    uint64_t maxCommittedId;
    const char* errorMessage;

    WalRecordScanContext(uint32_t pageSizeIn, const uint8_t databaseIdIn[16],
                         std::vector<WalTransaction>* transactionsIn)
        : pageSize(pageSizeIn), databaseId(databaseIdIn),
          haveTransaction(false), current(), transactions(transactionsIn),
          maxCommittedId(0), errorMessage(nullptr) {}
};

GxWalScanStatus visitWalRecord(void* opaque, uint8_t type, uint64_t txId,
                               const uint8_t* payload,
                               uint32_t payloadLength) {
    WalRecordScanContext& context = *static_cast<WalRecordScanContext*>(opaque);
    try {
        if (type == kWalRecordBegin) {
            if (context.haveTransaction) {
                context.errorMessage = "nested WAL BEGIN record";
                return GxWalScanStatus_Corrupt;
            }
            context.haveTransaction = true;
            context.current = WalTransaction();
            context.current.transactionId = txId;
        } else if (type == kWalRecordPageImage) {
            if (!context.haveTransaction ||
                txId != context.current.transactionId) {
                context.errorMessage = "WAL PAGE_IMAGE outside its transaction";
                return GxWalScanStatus_Corrupt;
            }
            if (payloadLength != wal_page_offset::PageBytes + context.pageSize) {
                context.errorMessage = "WAL PAGE_IMAGE payload size mismatch";
                return GxWalScanStatus_Corrupt;
            }
            const uint64_t pageId = loadLe64(payload + wal_page_offset::PageId);
            DatabasePage page;
            DbResult parsed = DatabasePage::parse(
                payload + wal_page_offset::PageBytes, context.pageSize, pageId,
                page);
            if (!parsed.isOk()) {
                context.errorMessage = "WAL PAGE_IMAGE is not a valid page";
                return GxWalScanStatus_Corrupt;
            }
            context.current.pages.push_back(std::move(page));
        } else if (type == kWalRecordHeaderImage) {
            if (!context.haveTransaction ||
                txId != context.current.transactionId) {
                context.errorMessage = "WAL DB_HEADER outside its transaction";
                return GxWalScanStatus_Corrupt;
            }
            if (payloadLength != kHeaderSize) {
                context.errorMessage = "WAL DB_HEADER payload size mismatch";
                return GxWalScanStatus_Corrupt;
            }
            DatabaseHeader header;
            DbResult parsed = DatabaseHeader::parse(payload, kHeaderSize, header);
            if (!parsed.isOk()) {
                context.errorMessage = "WAL DB_HEADER is not a valid header";
                return GxWalScanStatus_Corrupt;
            }
            if (std::memcmp(header.databaseId, context.databaseId, 16) != 0) {
                context.errorMessage = "WAL DB_HEADER identity does not match database";
                return GxWalScanStatus_Foreign;
            }
            context.current.hasHeaderImage = true;
            context.current.headerImage = header;
        } else { // kWalRecordCommit
            if (!context.haveTransaction ||
                txId != context.current.transactionId) {
                context.errorMessage = "WAL COMMIT outside its transaction";
                return GxWalScanStatus_Corrupt;
            }
            context.transactions->push_back(std::move(context.current));
            if (txId > context.maxCommittedId) {
                context.maxCommittedId = txId;
            }
            context.current = WalTransaction();
            context.haveTransaction = false;
        }
    } catch (const std::bad_alloc&) {
        context.errorMessage = "WAL recovery transaction allocation failed";
        return GxWalScanStatus_NoSpace;
    }
    return GxWalScanStatus_Ok;
}

} // namespace

WriteAheadLog::WriteAheadLog()
    : _file(), _readOnly(false), _pageSize(0), _generation(1), _lastTransactionId(0),
      _writeOffset(0), _bytesSinceCheckpoint(0), _lastError(), _recordAllocator() {
    std::memset(_databaseId, 0, sizeof(_databaseId));
    gxHostedAllocatorInit(&_recordAllocator, kWalRecordPrefixSize +
                                                 kMaxWalRecordPayload);
}

WriteAheadLog::WriteAheadLog(const GxAllocator& recordAllocator)
    : _file(), _readOnly(false), _pageSize(0), _generation(1), _lastTransactionId(0),
      _writeOffset(0), _bytesSinceCheckpoint(0), _lastError(),
      _recordAllocator(recordAllocator) {
    std::memset(_databaseId, 0, sizeof(_databaseId));
}

WriteAheadLog::~WriteAheadLog() { close(); }

DbResult WriteAheadLog::fail(DbStatus status, const std::string& message) {
    _lastError = DbResult::error(status, message);
    return _lastError;
}

DbResult WriteAheadLog::open(std::unique_ptr<IDatabaseFile> file,
                             const uint8_t databaseId[16], uint32_t pageSize,
                             bool readOnly) {
    DbResult closeResult = close();
    if (!closeResult.isOk()) {
        return fail(DbStatus::IoError, "failed to close prior WAL handle: " +
                                           closeResult.describe());
    }
    if (!file || !file->isOpen()) {
        return fail(DbStatus::InvalidArgument, "WAL file must be open");
    }
    if (!isSupportedPageSize(pageSize)) {
        return fail(DbStatus::InvalidArgument, "unsupported WAL page size");
    }
    _file = std::move(file);
    _readOnly = readOnly;
    _pageSize = pageSize;
    std::memcpy(_databaseId, databaseId, 16);
    _generation = 1;
    _lastTransactionId = 0;
    _bytesSinceCheckpoint = 0;

    uint64_t size = 0;
    DbResult sizeResult = _file->getSize(size);
    if (!sizeResult.isOk()) {
        return fail(DbStatus::IoError, "failed to query WAL size: " + sizeResult.describe());
    }
    if (size < kWalHeaderSize) {
        if (_readOnly) {
            _writeOffset = size;
            return DbResult::ok();
        }
        DbResult result = writeHeader(1, 0);
        if (!result.isOk()) {
            return result;
        }
        return DbResult::ok();
    }

    _writeOffset = size;
    uint8_t header[kWalHeaderSize];
    if (!_file->readAt(0, header, kWalHeaderSize)) {
        return fail(DbStatus::IoError, "failed to read WAL header");
    }
    if (std::memcmp(header + wal_header_offset::Magic, kWalMagic, 8) != 0) {
        // Leave as-is; scan() reports corruption. Do not silently discard.
        return DbResult::ok();
    }
    if (loadLe16(header + wal_header_offset::FormatVersion) != kWalFormatVersion) {
        return DbResult::ok();
    }
    _generation = loadLe64(header + wal_header_offset::Generation);
    _lastTransactionId = loadLe64(header + wal_header_offset::LastTransactionId);
    return DbResult::ok();
}

DbResult WriteAheadLog::close() {
    DbResult result = DbResult::ok();
    if (_file) {
        result = _file->close();
        _file.reset();
    }
    _writeOffset = 0;
    return result;
}

DbResult WriteAheadLog::writeHeader(uint64_t generation, uint64_t lastTransactionId) {
    uint8_t header[kWalHeaderSize];
    serializeWalHeader(header, _pageSize, _databaseId, generation, lastTransactionId);
    if (!_file->writeAt(0, header, kWalHeaderSize)) {
        return fail(DbStatus::IoError, "failed to write WAL header");
    }
    if (!_file->truncateTo(kWalHeaderSize)) {
        return fail(DbStatus::IoError, "failed to truncate WAL to clean header");
    }
    _generation = generation;
    _lastTransactionId = lastTransactionId;
    _writeOffset = kWalHeaderSize;
    _bytesSinceCheckpoint = 0;
    return DbResult::ok();
}

DbResult WriteAheadLog::appendRecord(uint8_t type, uint64_t transactionId,
                                     const uint8_t* payload, uint32_t payloadLength) {
    if (!_file || _readOnly) {
        return fail(DbStatus::ReadOnly, "WAL is not writable");
    }
    if (payloadLength > kMaxWalRecordPayload) {
        return fail(DbStatus::TransactionTooLarge, "WAL record payload exceeds maximum");
    }
    const uint64_t recordSize =
        static_cast<uint64_t>(kWalRecordPrefixSize) + payloadLength;
    if (_writeOffset > kMaxWalFileBytes ||
        recordSize > kMaxWalFileBytes - _writeOffset) {
        return fail(DbStatus::TransactionTooLarge, "WAL file size limit exceeded");
    }

    GxArray record = {};
    const size_t maximumRecordBytes = kWalRecordPrefixSize + payloadLength;
    GxStatus status = gxArrayInit(&record, &_recordAllocator, 1,
                                  maximumRecordBytes);
    if (status != GxStatus::Ok) {
        return fail(DbStatus::NoSpace, "WAL record buffer initialization failed");
    }
    status = gxWalRecordEncode(&record, type, transactionId, payload,
                               payloadLength, kMaxWalRecordPayload);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&record);
        const DbStatus dbStatus = status == GxStatus::Overflow
                                      ? DbStatus::OutOfBounds
                                      : status == GxStatus::InvalidArgument
                                            ? DbStatus::InvalidArgument
                                            : DbStatus::NoSpace;
        return fail(dbStatus, "WAL record buffer allocation failed");
    }

    DbResult appendResult = appendEncodedRecord(record.data,
                                                gxArraySize(&record));
    gxArrayDestroy(&record);
    return appendResult;
}

DbResult WriteAheadLog::appendEncodedRecord(const uint8_t* bytes,
                                            size_t recordSize) {
    if (!_file || _readOnly) {
        return fail(DbStatus::ReadOnly, "WAL is not writable");
    }
    if (bytes == nullptr || recordSize < kWalRecordPrefixSize ||
        _writeOffset > kMaxWalFileBytes ||
        recordSize > kMaxWalFileBytes - _writeOffset) {
        return fail(DbStatus::TransactionTooLarge, "WAL file size limit exceeded");
    }
    if (!_file->writeAt(_writeOffset, bytes, recordSize)) {
        return fail(DbStatus::IoError, "failed to append WAL record");
    }
    _writeOffset += recordSize;
    _bytesSinceCheckpoint += recordSize;
    return DbResult::ok();
}

DbResult WriteAheadLog::appendBegin(uint64_t transactionId) {
    return appendRecord(kWalRecordBegin, transactionId, nullptr, 0);
}

DbResult WriteAheadLog::appendPageImage(uint64_t transactionId, const DatabasePage& page) {
    if (!_file || _readOnly) {
        return fail(DbStatus::ReadOnly, "WAL is not writable");
    }
    if (!isSupportedPageSize(_pageSize)) {
        return fail(DbStatus::InvalidArgument, "unsupported WAL page size");
    }

    GxArray record = {};
    const size_t maximumRecordBytes = kWalRecordPrefixSize +
                                      wal_page_offset::PageBytes +
                                      static_cast<size_t>(_pageSize);
    GxStatus status = gxArrayInit(&record, &_recordAllocator, 1,
                                  maximumRecordBytes);
    if (status != GxStatus::Ok) {
        return fail(DbStatus::NoSpace, "WAL page-image buffer initialization failed");
    }
    status = gxWalPageImageRecordEncode(
        &record, transactionId, page.pageId, static_cast<uint16_t>(page.type),
        page.flags, page.generation,
        page.payload.empty() ? nullptr : page.payload.data(), page.payload.size(),
        _pageSize, kMaxWalRecordPayload);
    if (status != GxStatus::Ok) {
        gxArrayDestroy(&record);
        const DbStatus dbStatus = status == GxStatus::Overflow
                                      ? DbStatus::OutOfBounds
                                      : status == GxStatus::InvalidArgument
                                            ? DbStatus::InvalidArgument
                                            : status == GxStatus::ResourceLimit
                                                  ? DbStatus::TransactionTooLarge
                                                  : DbStatus::NoSpace;
        return fail(dbStatus, "WAL page-image encoding failed");
    }
    DbResult appendResult = appendEncodedRecord(record.data,
                                                gxArraySize(&record));
    gxArrayDestroy(&record);
    return appendResult;
}

DbResult WriteAheadLog::appendHeaderImage(uint64_t transactionId,
                                          const DatabaseHeader& header) {
    uint8_t payload[kHeaderSize];
    header.serialize(payload);
    return appendRecord(kWalRecordHeaderImage, transactionId, payload, kHeaderSize);
}

DbResult WriteAheadLog::appendCommit(uint64_t transactionId) {
    DbResult result = appendRecord(kWalRecordCommit, transactionId, nullptr, 0);
    if (result.isOk() && transactionId > _lastTransactionId) {
        _lastTransactionId = transactionId;
    }
    return result;
}

DbResult WriteAheadLog::sync() {
    if (!_file || _readOnly) {
        return fail(DbStatus::ReadOnly, "WAL is not writable");
    }
    if (!_file->flush()) {
        return fail(DbStatus::IoError, "failed to flush WAL");
    }
    return DbResult::ok();
}

DbResult WriteAheadLog::getSize(uint64_t& outSize) {
    if (!_file) {
        return DbResult::error(DbStatus::NotOpen, "WAL is not open");
    }
    return _file->getSize(outSize);
}

DbResult WriteAheadLog::scan(WalState& outState,
                             std::vector<WalTransaction>& outTransactions,
                             std::string& outMessage) {
    outTransactions.clear();
    outMessage.clear();
    outState = WalState::Clean;

    if (!_file) {
        outState = WalState::Absent;
        return DbResult::error(DbStatus::NotOpen, "WAL is not open");
    }

    uint64_t size = 0;
    DbResult sizeResult = _file->getSize(size);
    if (!sizeResult.isOk()) {
        outMessage = "failed to query WAL size";
        return fail(DbStatus::IoError, outMessage + ": " + sizeResult.describe());
    }
    if (size == 0) {
        outState = WalState::Clean;
        return DbResult::ok();
    }
    if (size > kMaxWalFileBytes) {
        outState = WalState::Corrupt;
        outMessage = "WAL exceeds the maximum supported size";
        return DbResult::error(DbStatus::WalCorrupt, outMessage);
    }
    if (size < kWalHeaderSize) {
        // A truncated header can only be the residue of an interrupted WAL
        // creation; there can be no committed transaction in it.
        outState = WalState::Incomplete;
        outMessage = "WAL header is truncated";
        return DbResult::ok();
    }

    uint8_t header[kWalHeaderSize];
    if (!readWalBytes(_file.get(), 0, header, sizeof(header))) {
        outMessage = "failed to read WAL header";
        return DbResult::error(DbStatus::IoError, outMessage);
    }
    if (std::memcmp(header + wal_header_offset::Magic, kWalMagic, 8) != 0) {
        outState = WalState::Corrupt;
        outMessage = "WAL signature mismatch";
        return DbResult::error(DbStatus::WalCorrupt, outMessage);
    }
    const uint16_t version = loadLe16(header + wal_header_offset::FormatVersion);
    if (version != kWalFormatVersion) {
        outState = WalState::UnsupportedVersion;
        outMessage = "unsupported WAL format version";
        return DbResult::error(DbStatus::WalUnsupportedVersion, outMessage);
    }
    const uint32_t pageSize = loadLe32(header + wal_header_offset::PageSize);
    if (pageSize != _pageSize) {
        outState = WalState::Foreign;
        outMessage = "WAL page size does not match database";
        return DbResult::error(DbStatus::WalDatabaseMismatch, outMessage);
    }
    if (std::memcmp(header + wal_header_offset::DatabaseId, _databaseId, 16) != 0) {
        outState = WalState::Foreign;
        outMessage = "WAL database identity does not match database";
        return DbResult::error(DbStatus::WalDatabaseMismatch, outMessage);
    }
    if (loadLe32(header + wal_header_offset::HeaderCrc32) != computeHeaderCrc(header)) {
        outState = WalState::Corrupt;
        outMessage = "WAL header checksum mismatch";
        return DbResult::error(DbStatus::WalCorrupt, outMessage);
    }
    _generation = loadLe64(header + wal_header_offset::Generation);
    _lastTransactionId = loadLe64(header + wal_header_offset::LastTransactionId);

    static_assert(GX_WAL_SCAN_PREFIX_SIZE == kWalRecordPrefixSize,
                  "portable WAL scanner prefix must match SQL3 framing");
    static_assert(GX_WAL_SCAN_MAX_PAYLOAD == kMaxWalRecordPayload,
                  "portable WAL scanner bound must match SQL3 policy");
    WalRecordScanContext context(_pageSize, _databaseId, &outTransactions);
    GxWalScanResult scanResult = {};
    const GxWalScanStatus scanStatus = gxWalScanRecords(
        kWalHeaderSize, size, kMaxWalFileBytes, kMaxWalRecordPayload,
        readWalBytes, _file.get(), &_recordAllocator, visitWalRecord, &context,
        &scanResult);
    if (scanStatus != GxWalScanStatus_Ok) {
        outTransactions.clear();
        if (scanStatus == GxWalScanStatus_IoError) {
            outMessage = "failed to read WAL record";
            return DbResult::error(DbStatus::IoError, outMessage);
        }
        if (scanStatus == GxWalScanStatus_NoSpace) {
            outMessage = context.errorMessage ? context.errorMessage
                                              : "WAL recovery scan allocation failed";
            return DbResult::error(DbStatus::NoSpace, outMessage);
        }
        if (scanStatus == GxWalScanStatus_Foreign) {
            outState = WalState::Foreign;
            outMessage = context.errorMessage ? context.errorMessage
                                              : "WAL database identity mismatch";
            return DbResult::error(DbStatus::WalDatabaseMismatch, outMessage);
        }
        if (scanStatus == GxWalScanStatus_Corrupt) {
            outState = WalState::Corrupt;
            outMessage = context.errorMessage ? context.errorMessage
                                              : "invalid WAL record prefix or checksum";
            return DbResult::error(DbStatus::WalCorrupt, outMessage);
        }
        outMessage = "WAL scan rejected invalid input";
        return DbResult::error(DbStatus::Internal, outMessage);
    }

    if (context.maxCommittedId > _lastTransactionId) {
        _lastTransactionId = context.maxCommittedId;
    }

    const bool incompleteTail = scanResult.incompleteTail || context.haveTransaction;
    if (!outTransactions.empty()) {
        outState = WalState::Committed;
    } else if (incompleteTail) {
        outState = WalState::Incomplete;
        outMessage = "WAL contains no committed transaction";
    } else {
        outState = WalState::Clean;
    }
    return DbResult::ok();
}

DbResult WriteAheadLog::checkpoint() {
    if (!_file || _readOnly) {
        return fail(DbStatus::ReadOnly, "WAL is not writable");
    }
    DbResult result = writeHeader(_generation + 1, _lastTransactionId);
    if (!result.isOk()) {
        return result;
    }
    if (!_file->flush()) {
        return fail(DbStatus::IoError, "failed to flush checkpointed WAL");
    }
    return DbResult::ok();
}

} // namespace db
} // namespace gxos
