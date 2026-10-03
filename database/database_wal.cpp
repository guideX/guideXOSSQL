#include "database_wal.h"

#include <cstring>

#include "database_checksum.h"
#include "database_endian.h"

namespace gxos {
namespace db {

const uint8_t kWalMagic[8] = {'G', 'X', 'W', 'A', 'L', '\r', '\n', 0x1A};

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

uint32_t computeRecordCrc(const uint8_t* prefix, const uint8_t* payload,
                          uint32_t payloadLength) {
    uint32_t crc = crc32Init();
    crc = crc32Update(crc, prefix, wal_record_offset::RecordCrc32);
    const uint8_t zero[4] = {0, 0, 0, 0};
    crc = crc32Update(crc, zero, sizeof(zero));
    crc = crc32Update(crc, prefix + wal_record_offset::RecordCrc32 + 4,
                      kWalRecordPrefixSize - (wal_record_offset::RecordCrc32 + 4));
    if (payloadLength > 0 && payload != nullptr) {
        crc = crc32Update(crc, payload, payloadLength);
    }
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

bool isKnownRecordType(uint8_t type) {
    return type == kWalRecordBegin || type == kWalRecordPageImage ||
           type == kWalRecordHeaderImage || type == kWalRecordCommit;
}

} // namespace

WriteAheadLog::WriteAheadLog()
    : _file(), _readOnly(false), _pageSize(0), _generation(1), _lastTransactionId(0),
      _writeOffset(0), _bytesSinceCheckpoint(0), _lastError() {
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
    close();
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

    const uint64_t size = _file->size();
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

void WriteAheadLog::close() {
    if (_file) {
        _file->close();
        _file.reset();
    }
    _writeOffset = 0;
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
    if (_writeOffset + kWalRecordPrefixSize + payloadLength > kMaxWalFileBytes) {
        return fail(DbStatus::TransactionTooLarge, "WAL file size limit exceeded");
    }

    std::vector<uint8_t> record(kWalRecordPrefixSize + payloadLength, 0);
    uint8_t* prefix = record.data();
    prefix[wal_record_offset::RecordType] = type;
    prefix[wal_record_offset::Flags] = 0;
    storeLe16(prefix + wal_record_offset::Reserved, 0);
    storeLe32(prefix + wal_record_offset::PayloadLength, payloadLength);
    storeLe64(prefix + wal_record_offset::TransactionId, transactionId);
    if (payloadLength > 0 && payload != nullptr) {
        std::memcpy(record.data() + kWalRecordPrefixSize, payload, payloadLength);
    }
    const uint32_t crc = computeRecordCrc(prefix, record.data() + kWalRecordPrefixSize,
                                          payloadLength);
    storeLe32(prefix + wal_record_offset::RecordCrc32, crc);

    if (!_file->writeAt(_writeOffset, record.data(), record.size())) {
        return fail(DbStatus::IoError, "failed to append WAL record");
    }
    _writeOffset += record.size();
    _bytesSinceCheckpoint += record.size();
    return DbResult::ok();
}

DbResult WriteAheadLog::appendBegin(uint64_t transactionId) {
    return appendRecord(kWalRecordBegin, transactionId, nullptr, 0);
}

DbResult WriteAheadLog::appendPageImage(uint64_t transactionId, const DatabasePage& page) {
    std::vector<uint8_t> bytes;
    DbResult result = page.serialize(bytes, _pageSize);
    if (!result.isOk()) {
        return result;
    }
    std::vector<uint8_t> payload(wal_page_offset::PageBytes + bytes.size(), 0);
    storeLe16(payload.data() + wal_page_offset::PageType,
              static_cast<uint16_t>(page.type));
    storeLe16(payload.data() + wal_page_offset::Reserved, 0);
    storeLe64(payload.data() + wal_page_offset::PageId, page.pageId);
    std::memcpy(payload.data() + wal_page_offset::PageBytes, bytes.data(), bytes.size());
    return appendRecord(kWalRecordPageImage, transactionId, payload.data(),
                        static_cast<uint32_t>(payload.size()));
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

uint64_t WriteAheadLog::size() {
    return _file ? _file->size() : 0;
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

    const uint64_t size = _file->size();
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

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!_file->readAt(0, bytes.data(), bytes.size())) {
        outState = WalState::Corrupt;
        outMessage = "failed to read WAL";
        return DbResult::error(DbStatus::IoError, outMessage);
    }

    const uint8_t* data = bytes.data();
    if (std::memcmp(data + wal_header_offset::Magic, kWalMagic, 8) != 0) {
        outState = WalState::Corrupt;
        outMessage = "WAL signature mismatch";
        return DbResult::error(DbStatus::WalCorrupt, outMessage);
    }
    const uint16_t version = loadLe16(data + wal_header_offset::FormatVersion);
    if (version != kWalFormatVersion) {
        outState = WalState::UnsupportedVersion;
        outMessage = "unsupported WAL format version";
        return DbResult::error(DbStatus::WalUnsupportedVersion, outMessage);
    }
    const uint32_t pageSize = loadLe32(data + wal_header_offset::PageSize);
    if (pageSize != _pageSize) {
        outState = WalState::Foreign;
        outMessage = "WAL page size does not match database";
        return DbResult::error(DbStatus::WalDatabaseMismatch, outMessage);
    }
    if (std::memcmp(data + wal_header_offset::DatabaseId, _databaseId, 16) != 0) {
        outState = WalState::Foreign;
        outMessage = "WAL database identity does not match database";
        return DbResult::error(DbStatus::WalDatabaseMismatch, outMessage);
    }
    if (loadLe32(data + wal_header_offset::HeaderCrc32) != computeHeaderCrc(data)) {
        outState = WalState::Corrupt;
        outMessage = "WAL header checksum mismatch";
        return DbResult::error(DbStatus::WalCorrupt, outMessage);
    }
    _generation = loadLe64(data + wal_header_offset::Generation);
    _lastTransactionId = loadLe64(data + wal_header_offset::LastTransactionId);

    size_t offset = kWalHeaderSize;
    bool haveTx = false;
    WalTransaction current;
    bool incompleteTail = false;
    uint64_t maxCommittedId = 0;

    while (offset < bytes.size()) {
        const size_t remaining = bytes.size() - offset;
        if (remaining < kWalRecordPrefixSize) {
            incompleteTail = true;
            break;
        }
        const uint8_t* prefix = data + offset;
        const uint8_t type = prefix[wal_record_offset::RecordType];
        const uint32_t payloadLength = loadLe32(prefix + wal_record_offset::PayloadLength);
        if (!isKnownRecordType(type)) {
            outState = WalState::Corrupt;
            outMessage = "unknown WAL record type";
            return DbResult::error(DbStatus::WalCorrupt, outMessage);
        }
        if (payloadLength > kMaxWalRecordPayload) {
            outState = WalState::Corrupt;
            outMessage = "WAL record payload length exceeds maximum";
            return DbResult::error(DbStatus::WalCorrupt, outMessage);
        }
        if (remaining < kWalRecordPrefixSize + static_cast<size_t>(payloadLength)) {
            incompleteTail = true;
            break;
        }
        const uint8_t* payload = prefix + kWalRecordPrefixSize;
        const uint32_t storedCrc = loadLe32(prefix + wal_record_offset::RecordCrc32);
        if (storedCrc != computeRecordCrc(prefix, payload, payloadLength)) {
            outState = WalState::Corrupt;
            outMessage = "WAL record checksum mismatch";
            return DbResult::error(DbStatus::WalCorrupt, outMessage);
        }
        const uint64_t txId = loadLe64(prefix + wal_record_offset::TransactionId);

        if (type == kWalRecordBegin) {
            if (haveTx) {
                outState = WalState::Corrupt;
                outMessage = "nested WAL BEGIN record";
                return DbResult::error(DbStatus::WalCorrupt, outMessage);
            }
            haveTx = true;
            current = WalTransaction();
            current.transactionId = txId;
        } else if (type == kWalRecordPageImage) {
            if (!haveTx || txId != current.transactionId) {
                outState = WalState::Corrupt;
                outMessage = "WAL PAGE_IMAGE outside its transaction";
                return DbResult::error(DbStatus::WalCorrupt, outMessage);
            }
            if (payloadLength != wal_page_offset::PageBytes + _pageSize) {
                outState = WalState::Corrupt;
                outMessage = "WAL PAGE_IMAGE payload size mismatch";
                return DbResult::error(DbStatus::WalCorrupt, outMessage);
            }
            const uint64_t pageId = loadLe64(payload + wal_page_offset::PageId);
            DatabasePage page;
            DbResult parsed = DatabasePage::parse(payload + wal_page_offset::PageBytes,
                                                  _pageSize, pageId, page);
            if (!parsed.isOk()) {
                outState = WalState::Corrupt;
                outMessage = "WAL PAGE_IMAGE is not a valid page: " + parsed.message();
                return DbResult::error(DbStatus::WalCorrupt, outMessage);
            }
            current.pages.push_back(page);
        } else if (type == kWalRecordHeaderImage) {
            if (!haveTx || txId != current.transactionId) {
                outState = WalState::Corrupt;
                outMessage = "WAL DB_HEADER outside its transaction";
                return DbResult::error(DbStatus::WalCorrupt, outMessage);
            }
            if (payloadLength != kHeaderSize) {
                outState = WalState::Corrupt;
                outMessage = "WAL DB_HEADER payload size mismatch";
                return DbResult::error(DbStatus::WalCorrupt, outMessage);
            }
            DatabaseHeader header;
            DbResult parsed = DatabaseHeader::parse(payload, kHeaderSize, header);
            if (!parsed.isOk()) {
                outState = WalState::Corrupt;
                outMessage = "WAL DB_HEADER is not a valid header: " + parsed.message();
                return DbResult::error(DbStatus::WalCorrupt, outMessage);
            }
            if (std::memcmp(header.databaseId, _databaseId, 16) != 0) {
                outState = WalState::Foreign;
                outMessage = "WAL DB_HEADER identity does not match database";
                return DbResult::error(DbStatus::WalDatabaseMismatch, outMessage);
            }
            current.hasHeaderImage = true;
            current.headerImage = header;
        } else { // kWalRecordCommit
            if (!haveTx || txId != current.transactionId) {
                outState = WalState::Corrupt;
                outMessage = "WAL COMMIT outside its transaction";
                return DbResult::error(DbStatus::WalCorrupt, outMessage);
            }
            outTransactions.push_back(current);
            if (current.transactionId > maxCommittedId) {
                maxCommittedId = current.transactionId;
            }
            current = WalTransaction();
            haveTx = false;
        }

        offset += kWalRecordPrefixSize + static_cast<size_t>(payloadLength);
    }

    if (haveTx) {
        incompleteTail = true;
    }
    if (maxCommittedId > _lastTransactionId) {
        _lastTransactionId = maxCommittedId;
    }

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
