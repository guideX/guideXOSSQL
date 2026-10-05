#pragma once
// guideXOS SQL -- Phase SQL1/SQL2
// Read-only diagnostics describing an opened (or inspected) database.
// Intended for later Database Manager / Developer Studio tooling. No GUI here.

#include <cstdint>
#include <string>
#include <vector>

#include "database_result.h"

namespace gxos {
namespace db {

// Observed state of the sidecar write-ahead log (read-only description).
enum class WalState : int {
    Absent = 0,        // no .gxwal file
    Clean,             // valid header, no transaction records
    Committed,         // at least one fully committed transaction needs redo
    Incomplete,        // valid records but the last transaction has no COMMIT
    Corrupt,           // header/record integrity failure
    Foreign,           // WAL identity does not match this database
    UnsupportedVersion // WAL format version not understood
};

inline const char* walStateName(WalState state) {
    switch (state) {
    case WalState::Absent: return "Absent";
    case WalState::Clean: return "Clean";
    case WalState::Committed: return "Committed";
    case WalState::Incomplete: return "Incomplete";
    case WalState::Corrupt: return "Corrupt";
    case WalState::Foreign: return "Foreign";
    case WalState::UnsupportedVersion: return "UnsupportedVersion";
    }
    return "Unknown";
}

// Per-column relational diagnostics (read-only). SQL8.
struct ColumnDiagnostics {
    std::string name;
    std::string typeName;
    bool nullable;
    bool hasDefault;
    std::string defaultValue; // human-readable; empty when no default

    ColumnDiagnostics() : nullable(false), hasDefault(false) {}
};

// Per-table relational diagnostics (read-only).
struct TableDiagnostics {
    uint32_t tableId;
    std::string name;
    uint32_t schemaVersion;
    uint32_t columnCount;
    uint32_t heapPageCount;
    uint64_t rowCount;
    std::vector<ColumnDiagnostics> columns;

    TableDiagnostics()
        : tableId(0), schemaVersion(0), columnCount(0), heapPageCount(0), rowCount(0) {}
};

// Per-index relational diagnostics (read-only). SQL6. SQL8 adds ownership.
struct IndexDiagnostics {
    uint32_t indexId;
    std::string name;
    uint32_t tableId;
    std::string tableName;
    std::string columnName;
    bool unique;
    bool primaryKey;
    bool systemOwned;
    uint32_t ownerForeignKeyId;
    uint64_t rootPageId;
    uint64_t entryCount;
    uint16_t formatVersion;

    IndexDiagnostics()
        : indexId(0), tableId(0), unique(false), primaryKey(false), systemOwned(false),
          ownerForeignKeyId(0), rootPageId(0), entryCount(0), formatVersion(0) {}
};

// Per-foreign-key diagnostics (read-only). SQL8.
struct ForeignKeyDiagnostics {
    uint32_t foreignKeyId;
    uint32_t childTableId;
    std::string childTableName;
    std::string childColumnName;
    uint32_t parentTableId;
    std::string parentTableName;
    std::string parentColumnName;
    uint32_t referencedIndexId;
    uint32_t supportIndexId;

    ForeignKeyDiagnostics()
        : foreignKeyId(0), childTableId(0), parentTableId(0), referencedIndexId(0),
          supportIndexId(0) {}
};

struct DatabaseDiagnostics {
    bool open;
    bool readOnly;
    uint16_t formatMajor;
    uint16_t formatMinor;
    uint32_t pageSize;
    uint64_t pageCount;
    uint64_t rootPageId;
    uint64_t fileSizeBytes;
    std::string databaseId;          // canonical 8-4-4-4-12 string
    DbStatus state;                  // Ok when the last operation succeeded
    std::string lastValidationFailure;

    // SQL2 relational diagnostics.
    uint32_t tableCount;
    uint32_t catalogPageCount;
    uint32_t bufferCapacity;
    uint32_t bufferResident;
    uint32_t bufferDirty;
    std::vector<TableDiagnostics> tables;

    // SQL6 index diagnostics.
    uint32_t indexCount;
    std::vector<IndexDiagnostics> indexes;

    // SQL8 foreign-key diagnostics.
    uint32_t foreignKeyCount;
    std::vector<ForeignKeyDiagnostics> foreignKeys;

    // SQL3 transaction / write-ahead-log diagnostics (read-only).
    bool transactionActive;
    uint64_t transactionId;
    uint32_t transactionModifiedPages;
    bool walPresent;
    WalState walState;
    uint64_t walBytes;
    bool recoveryRequired;
    std::string lastRecoveryResult;
    uint64_t pagesRedone;

    DatabaseDiagnostics()
        : open(false),
          readOnly(false),
          formatMajor(0),
          formatMinor(0),
          pageSize(0),
          pageCount(0),
          rootPageId(0),
          fileSizeBytes(0),
          state(DbStatus::NotOpen),
          tableCount(0),
          catalogPageCount(0),
          bufferCapacity(0),
          bufferResident(0),
          bufferDirty(0),
          indexCount(0),
          foreignKeyCount(0),
          transactionActive(false),
          transactionId(0),
          transactionModifiedPages(0),
          walPresent(false),
          walState(WalState::Absent),
          walBytes(0),
          recoveryRequired(false),
          pagesRedone(0) {}

    void reset() {
        *this = DatabaseDiagnostics();
    }
};

} // namespace db
} // namespace gxos
