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

// Per-table relational diagnostics (read-only).
struct TableDiagnostics {
    uint32_t tableId;
    std::string name;
    uint32_t columnCount;
    uint32_t heapPageCount;
    uint64_t rowCount;

    TableDiagnostics()
        : tableId(0), columnCount(0), heapPageCount(0), rowCount(0) {}
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
          bufferDirty(0) {}

    void reset() {
        *this = DatabaseDiagnostics();
    }
};

} // namespace db
} // namespace gxos
