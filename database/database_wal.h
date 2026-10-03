#pragma once
// guideXOS SQL -- Phase SQL3
// WriteAheadLog: the sidecar redo log that makes multi-page relational
// transactions crash-atomic.
//
// File naming: for `database.gxdb` the log is `database.gxwal`.
//
// The log is a versioned, explicitly serialized byte stream. C++ structs are
// never dumped directly. Every multi-byte field is little-endian. The log is
// structurally tied to exactly one database identity (the SQL1 database UUID):
// a log is never replayed against a database with a different UUID.
//
// Stream layout:
//
//   [ WAL header (64 bytes) ]
//   [ record ] [ record ] ...
//
// A committed transaction is the sequence:
//
//   BEGIN, PAGE_IMAGE*, DB_HEADER?, COMMIT
//
// The COMMIT record is only considered durable after sync() (the durability
// point). A transaction without a fully validated COMMIT is discarded on
// recovery; because SQL3 forbids writing uncommitted pages to the .gxdb, no
// undo is ever required.
//
// See docs/SQL3_WAL_TRANSACTIONS.md for the exact byte layout.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "database_diagnostics.h"
#include "database_header.h"
#include "database_io.h"
#include "database_page.h"
#include "database_result.h"

namespace gxos {
namespace db {

// Returns the sidecar WAL path for a database path: the final extension is
// replaced with ".gxwal" (or the suffix is appended when there is none).
std::string walPathFor(const std::string& databasePath);

// Recognizable 8-byte signature: "GXWAL" followed by CR LF SUB LF.
extern const uint8_t kWalMagic[8];

const uint16_t kWalFormatVersion = 1;
const uint32_t kWalHeaderSize = 64;
const uint32_t kWalRecordPrefixSize = 20;

// Record type ids (u8 in the record prefix; kept as u32 constants).
const uint8_t kWalRecordBegin = 1;
const uint8_t kWalRecordPageImage = 2;
const uint8_t kWalRecordHeaderImage = 3;
const uint8_t kWalRecordCommit = 4;

// Record integrity bounds. A record prefix claiming more than this payload is
// rejected as corruption rather than treated as a torn tail. The bound covers a
// full-page redo image (page-size payload plus its small record header).
const uint32_t kMaxWalRecordPayload = kMaxPageSize + 64;
const uint64_t kMaxWalFileBytes = 64ull * 1024ull * 1024ull;
const uint64_t kAutoCheckpointWalBytes = 16ull * 1024ull * 1024ull;

// WAL header field offsets.
namespace wal_header_offset {
enum : uint32_t {
    Magic = 0,             // 8 bytes
    FormatVersion = 8,     // u16
    HeaderSize = 10,       // u16
    PageSize = 12,         // u32
    DatabaseId = 16,       // 16 bytes
    Generation = 32,       // u64
    LastTransactionId = 40,// u64
    Flags = 48,            // u32
    Reserved0 = 52,        // u32
    HeaderCrc32 = 56,      // u32
    Reserved1 = 60         // u32
};
} // namespace wal_header_offset

// Record prefix field offsets.
namespace wal_record_offset {
enum : uint32_t {
    RecordType = 0,     // u8
    Flags = 1,          // u8
    Reserved = 2,       // u16
    PayloadLength = 4,  // u32
    TransactionId = 8,  // u64
    RecordCrc32 = 16    // u32
};
} // namespace wal_record_offset

// PAGE_IMAGE payload field offsets.
namespace wal_page_offset {
enum : uint32_t {
    PageType = 0,   // u16
    Reserved = 2,   // u16
    PageId = 4,     // u64
    PageBytes = 12  // pageSize bytes
};
} // namespace wal_page_offset

// One fully committed transaction recovered from the log.
struct WalTransaction {
    uint64_t transactionId;
    bool hasHeaderImage;
    DatabaseHeader headerImage;
    std::vector<DatabasePage> pages;

    WalTransaction() : transactionId(0), hasHeaderImage(false) {}
};

class WriteAheadLog {
public:
    WriteAheadLog();
    ~WriteAheadLog();

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    // Takes ownership of an already-open file. When writable, an empty or
    // partially written header is replaced with a fresh clean header.
    DbResult open(std::unique_ptr<IDatabaseFile> file, const uint8_t databaseId[16],
                  uint32_t pageSize, bool readOnly);
    void close();
    bool isOpen() const { return _file != nullptr; }
    bool isReadOnly() const { return _readOnly; }

    // Append records for one transaction. Call sync() after appendCommit() to
    // reach the durability point.
    DbResult appendBegin(uint64_t transactionId);
    DbResult appendPageImage(uint64_t transactionId, const DatabasePage& page);
    DbResult appendHeaderImage(uint64_t transactionId, const DatabaseHeader& header);
    DbResult appendCommit(uint64_t transactionId);
    DbResult sync();

    // Parses the whole log. `outTransactions` receives every fully committed
    // transaction in order. Returns an error status for corrupt / foreign /
    // unsupported logs (also reflected in `outState`).
    DbResult scan(WalState& outState, std::vector<WalTransaction>& outTransactions,
                  std::string& outMessage);

    // Truncates to a fresh clean header (generation advanced) and syncs.
    DbResult checkpoint();

    uint64_t size();
    uint64_t bytesSinceCheckpoint() const { return _bytesSinceCheckpoint; }
    uint64_t generation() const { return _generation; }
    uint64_t lastTransactionId() const { return _lastTransactionId; }

private:
    DbResult appendRecord(uint8_t type, uint64_t transactionId, const uint8_t* payload,
                          uint32_t payloadLength);
    DbResult writeHeader(uint64_t generation, uint64_t lastTransactionId);
    DbResult fail(DbStatus status, const std::string& message);

    std::unique_ptr<IDatabaseFile> _file;
    bool _readOnly;
    uint32_t _pageSize;
    uint8_t _databaseId[16];
    uint64_t _generation;
    uint64_t _lastTransactionId;
    uint64_t _writeOffset;
    uint64_t _bytesSinceCheckpoint;
    DbResult _lastError;
};

} // namespace db
} // namespace gxos
