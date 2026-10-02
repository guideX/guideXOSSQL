#pragma once
// guideXOS SQL -- Phase SQL2
// Catalog: the durable relational system catalog.
//
// The catalog is authoritative for schema metadata. It is stored as a chain
// of catalog pages rooted at the database's root catalog page:
//
//   root catalog page  ->  continuation page  ->  continuation page -> ...
//
// Each page holds a sequence of self-bounded table records; every table
// record embeds its column records. All fields are explicitly serialized
// little-endian. Every parse treats disk contents as untrusted: bounds,
// uniqueness, type ids, name encoding and chain structure are all validated.
//
// On-disk table record:
//   u32 tableId, u32 schemaVersion, u32 columnCount, u64 firstHeapPageId,
//   u32 heapPageCount, u64 rowCount, u32 flags, u32 nameLength, name bytes,
//   then columnCount column records.
//
// On-disk column record:
//   u32 ordinal, u16 type, u16 nullable, u32 flags, u32 nameLength, name bytes.

#include <cstdint>
#include <string>
#include <vector>

#include "database_buffer.h"
#include "database_file.h"
#include "database_result.h"
#include "database_schema.h"

namespace gxos {
namespace db {

class Catalog {
public:
    struct TableRecord {
        uint32_t tableId;
        uint32_t schemaVersion;
        std::string name;
        uint64_t firstHeapPageId;
        uint32_t heapPageCount;
        uint64_t rowCount;
        std::vector<ColumnDefinition> columns;

        TableRecord()
            : tableId(0), schemaVersion(1), firstHeapPageId(0), heapPageCount(0),
              rowCount(0) {}
    };

    Catalog();

    // Loads the catalog from the root page and its continuation chain.
    DbResult load(BufferManager& buffer, uint64_t rootPageId, uint32_t pageSize,
                  uint64_t pageCount);

    // Rewrites the whole catalog (root + continuations) from the in-memory
    // records. Reuses existing continuation pages where possible and allocates
    // new ones when the catalog grows.
    DbResult save(BufferManager& buffer, DatabaseFile& file, uint64_t rootPageId,
                  uint32_t pageSize);

    const TableRecord* findTable(const std::string& name) const;
    const TableRecord* findTable(uint32_t tableId) const;
    TableRecord* findTableMutable(uint32_t tableId);

    const std::vector<TableRecord>& tables() const { return _tables; }
    uint32_t tableCount() const { return static_cast<uint32_t>(_tables.size()); }
    uint32_t nextTableId() const { return _nextTableId; }
    uint32_t catalogPageCount() const {
        return 1 + static_cast<uint32_t>(_continuationPageIds.size());
    }

    // Validates and appends a new table record, assigning its id. Fails with
    // AlreadyExists on a duplicate name. Does not touch the disk; call save()
    // to persist. `pageSize` is used to verify the serialized record fits in
    // a catalog page.
    DbResult addTable(const TableDefinition& def, uint32_t pageSize, uint32_t& outTableId);

    // Serialized size of the table record for `def` (validation not included).
    uint32_t recordSizeForDefinition(const TableDefinition& def) const;

private:
    struct PageWrite {
        std::vector<uint8_t> payload;
        uint32_t recordCount;
    };

    DbResult parseTableRecord(const uint8_t* bytes, size_t length, size_t& offset,
                              TableRecord& out);
    DbResult parseColumnRecord(const uint8_t* bytes, size_t length, size_t& offset,
                               ColumnDefinition& out);
    void serializeTableRecord(const TableRecord& rec, std::vector<uint8_t>& out) const;
    void serializeColumnRecord(const ColumnDefinition& col,
                               std::vector<uint8_t>& out) const;
    bool validateName(const std::string& name, uint32_t maxBytes) const;
    bool tableNameExists(const std::string& name) const;

    std::vector<TableRecord> _tables;
    uint32_t _nextTableId;
    std::vector<uint64_t> _continuationPageIds;
};

} // namespace db
} // namespace gxos
