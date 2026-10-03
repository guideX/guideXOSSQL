#pragma once
// guideXOS SQL -- Phase SQL2/SQL6
// Catalog: the durable relational system catalog.
//
// The catalog is authoritative for schema metadata. It is stored as a chain
// of catalog pages rooted at the database's root catalog page:
//
//   root catalog page  ->  continuation page  ->  continuation page -> ...
//
// Catalog v2 (SQL2) holds a sequence of self-bounded table records; every table
// record embeds its column records. Catalog v3 (SQL6) adds index/constraint
// records in the same record stream, distinguished by a one-byte record type.
// A v3 catalog is written only once the database has at least one index; a
// database without indexes keeps the byte-identical v2 layout, so SQL1-SQL5
// databases and their corruption tests continue to behave exactly as before.
//
// All fields are explicitly serialized little-endian. Every parse treats disk
// contents as untrusted: bounds, uniqueness, type ids, name encoding and chain
// structure are all validated.
//
// On-disk v2 table record:
//   u32 tableId, u32 schemaVersion, u32 columnCount, u64 firstHeapPageId,
//   u32 heapPageCount, u64 rowCount, u32 flags, u32 nameLength, name bytes,
//   then columnCount column records.
//
// On-disk v3 record stream: a u8 record type (1 = table, 2 = index) followed
// by the record payload. The table payload is identical to v2.
//
// On-disk index record payload:
//   u32 indexId, u32 tableId, u32 columnOrdinal, u64 rootPageId,
//   u16 flags (bit0 unique, bit1 primary key), u16 formatVersion,
//   u64 entryCount, u32 nameLength, name bytes.

#include <cstdint>
#include <string>
#include <vector>

#include "database_pagestore.h"
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

    struct IndexRecord {
        uint32_t indexId;
        std::string name;
        uint32_t tableId;
        uint32_t columnOrdinal;
        uint64_t rootPageId; // 0 means "empty index, no pages allocated yet"
        uint16_t flags;      // bit0 unique, bit1 primary key
        uint16_t formatVersion;
        uint64_t entryCount;

        IndexRecord()
            : indexId(0), tableId(0), columnOrdinal(0), rootPageId(0), flags(0),
              formatVersion(1), entryCount(0) {}

        bool unique() const { return (flags & 0x0001u) != 0; }
        bool primaryKey() const { return (flags & 0x0002u) != 0; }
        IndexKind kind() const {
            if (primaryKey()) return IndexKind::PrimaryKey;
            if (unique()) return IndexKind::Unique;
            return IndexKind::Ordinary;
        }
    };

    Catalog();

    // Loads the catalog from the root page and its continuation chain.
    DbResult load(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize,
                  uint64_t pageCount);

    // Rewrites the whole catalog (root + continuations) from the in-memory
    // records. Reuses existing continuation pages where possible and allocates
    // new ones when the catalog grows.
    DbResult save(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize);

    const TableRecord* findTable(const std::string& name) const;
    const TableRecord* findTable(uint32_t tableId) const;
    TableRecord* findTableMutable(uint32_t tableId);

    const std::vector<TableRecord>& tables() const { return _tables; }
    uint32_t tableCount() const { return static_cast<uint32_t>(_tables.size()); }
    uint32_t nextTableId() const { return _nextTableId; }
    uint32_t catalogPageCount() const {
        return 1 + static_cast<uint32_t>(_continuationPageIds.size());
    }

    // ---- SQL6 indexes ---------------------------------------------------
    const IndexRecord* findIndex(const std::string& name) const;
    const IndexRecord* findIndex(uint32_t indexId) const;
    IndexRecord* findIndexMutable(uint32_t indexId);
    const std::vector<IndexRecord>& indexes() const { return _indexes; }
    uint32_t indexCount() const { return static_cast<uint32_t>(_indexes.size()); }
    uint32_t nextIndexId() const { return _nextIndexId; }
    bool indexNameExists(const std::string& name) const;
    bool indexOnColumnExists(uint32_t tableId, uint32_t columnOrdinal) const;
    std::vector<IndexRecord> indexesForTable(uint32_t tableId) const;
    uint16_t version() const { return _version; }

    // Validates and appends a new index record, assigning its id. Fails with
    // AlreadyExists on a duplicate name or a second index on the same column.
    // Does not touch the disk; call save() to persist.
    DbResult addIndex(const IndexDefinition& def, uint32_t pageSize,
                      uint32_t& outIndexId);

    // Updates the durable root page id of an index after a root split.
    void setIndexRoot(uint32_t indexId, uint64_t rootPageId);

    // Adjusts the diagnostic entry count of an index.
    void adjustIndexEntryCount(uint32_t indexId, int64_t delta);
    void setIndexEntryCount(uint32_t indexId, uint64_t count);

    // Validates and appends a new table record, assigning its id. Fails with
    // AlreadyExists on a duplicate name. Also appends any index records carried
    // by the definition (PRIMARY KEY / UNIQUE constraints). Does not touch the
    // disk; call save() to persist. `pageSize` is used to verify the serialized
    // records fit in a catalog page.
    DbResult addTable(const TableDefinition& def, uint32_t pageSize, uint32_t& outTableId);

    // Serialized size of the table record for `def` (validation not included).
    uint32_t recordSizeForDefinition(const TableDefinition& def) const;

private:
    struct PageWrite {
        std::vector<uint8_t> payload;
        uint32_t recordCount;
    };

    DbResult loadV2(PageAccess& pages, const std::vector<uint8_t>& rootPayload,
                    uint64_t rootPageId, uint32_t pageSize, uint64_t pageCount);
    DbResult loadV3(PageAccess& pages, const std::vector<uint8_t>& rootPayload,
                    uint64_t rootPageId, uint32_t pageSize, uint64_t pageCount);
    DbResult saveV2(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize);
    DbResult saveV3(PageAccess& pages, uint64_t rootPageId, uint32_t pageSize);

    DbResult parseTableRecord(const uint8_t* bytes, size_t length, size_t& offset,
                              TableRecord& out);
    DbResult parseIndexRecord(const uint8_t* bytes, size_t length, size_t& offset,
                              IndexRecord& out);
    DbResult parseColumnRecord(const uint8_t* bytes, size_t length, size_t& offset,
                               ColumnDefinition& out);
    void serializeTableRecord(const TableRecord& rec, std::vector<uint8_t>& out) const;
    void serializeColumnRecord(const ColumnDefinition& col,
                               std::vector<uint8_t>& out) const;
    void serializeIndexRecord(const IndexRecord& rec, std::vector<uint8_t>& out) const;
    bool validateName(const std::string& name, uint32_t maxBytes) const;
    bool tableNameExists(const std::string& name) const;

    std::vector<TableRecord> _tables;
    std::vector<IndexRecord> _indexes;
    uint32_t _nextTableId;
    uint32_t _nextIndexId;
    uint16_t _version;
    std::vector<uint64_t> _continuationPageIds;
};

} // namespace db
} // namespace gxos
