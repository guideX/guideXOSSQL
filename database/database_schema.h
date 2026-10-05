#pragma once
// guideXOS SQL -- Phase SQL2
// Relational schema model: table/column definitions and discovery info.
//
// Identifier policy (SQL2):
//   * Table and column names are non-empty UTF-8 byte strings.
//   * Maximum length is 64 bytes (kMaxTableNameBytes / kMaxColumnNameBytes).
//   * Malformed UTF-8 is rejected (InvalidArgument).
//   * Comparison is byte-wise and CASE-SENSITIVE: "Users", "users" and
//     "USERS" are three distinct identifiers. This rule is deterministic and
//     does not inherit host-filesystem case semantics.

#include <cstdint>
#include <string>
#include <vector>

#include "database_types.h"

namespace gxos {
namespace db {

struct ColumnDefinition {
    std::string name;
    DbType type;
    bool nullable;
    uint32_t ordinal; // zero-based position, assigned at creation time
    bool hasDefault;
    DbValue defaultValue;

    ColumnDefinition() : type(DbType::Unknown), nullable(false), ordinal(0),
                        hasDefault(false) {}
    ColumnDefinition(const std::string& nameIn, DbType typeIn, bool nullableIn)
        : name(nameIn), type(typeIn), nullable(nullableIn), ordinal(0),
          hasDefault(false) {}
};

// ---- SQL6 indexes and constraints -----------------------------------------
//
// An index is always single-column in SQL6. `kind` records why the index
// exists so future schema introspection can distinguish an ordinary index from
// a UNIQUE constraint or a PRIMARY KEY backing index. PRIMARY KEY additionally
// implies NOT NULL at the column level; that is enforced on the table
// definition, not by the index layer.
enum class IndexKind : uint16_t {
    Ordinary = 0,
    Unique = 1,
    PrimaryKey = 2
};

inline const char* indexKindName(IndexKind kind) {
    switch (kind) {
    case IndexKind::Ordinary: return "Index";
    case IndexKind::Unique: return "Unique";
    case IndexKind::PrimaryKey: return "PrimaryKey";
    }
    return "Index";
}

inline bool indexKindIsUnique(IndexKind kind) {
    return kind == IndexKind::Unique || kind == IndexKind::PrimaryKey;
}

// SQL6 indexable types: every ordered type except Blob (SQL5 defines BLOB
// equality but not ordering, and a B+ tree requires a total ordered domain).
inline bool isIndexableType(DbType type) {
    switch (type) {
    case DbType::Boolean:
    case DbType::Int32:
    case DbType::Int64:
    case DbType::Float64:
    case DbType::Text:
        return true;
    default:
        return false;
    }
}

// A requested index. For CREATE INDEX the name is user supplied; for a
// constraint-backed index the catalog assigns a reserved, deterministic name
// and stable index id. `tableId` is zero when the definition is nested in a
// TableDefinition (the catalog fills it in on creation).
//
// SQL8 adds ownership: `ownerForeignKeyId` is non-zero only for FK support
// indexes (system-owned, not user-droppable). `systemOwned` marks any index
// the user must not drop directly (PK, UNIQUE, FK support).
struct IndexDefinition {
    std::string name;
    uint32_t tableId;
    uint32_t columnOrdinal;
    IndexKind kind;
    uint32_t ownerForeignKeyId; // 0 = not an FK support index
    bool systemOwned;

    IndexDefinition()
        : tableId(0), columnOrdinal(0), kind(IndexKind::Ordinary),
          ownerForeignKeyId(0), systemOwned(false) {}
    IndexDefinition(const std::string& nameIn, uint32_t tableIdIn,
                    uint32_t columnOrdinalIn, IndexKind kindIn)
        : name(nameIn), tableId(tableIdIn), columnOrdinal(columnOrdinalIn),
          kind(kindIn), ownerForeignKeyId(0), systemOwned(false) {}
};

// ---- SQL8 foreign keys -----------------------------------------------------
//
// A single-column FOREIGN KEY constraint. The child column references exactly
// one column in exactly one parent table, which must be backed by a PRIMARY
// KEY or UNIQUE constraint/index. SQL8 uses RESTRICT/NO ACTION semantics only.
struct ForeignKeyDefinition {
    uint32_t foreignKeyId;
    uint32_t childTableId;
    uint32_t childColumnOrdinal;
    uint32_t parentTableId;
    uint32_t parentColumnOrdinal;
    uint32_t referencedIndexId; // PK/UNIQUE index on the parent column
    uint32_t supportIndexId;    // system-owned non-unique index on child column

    ForeignKeyDefinition()
        : foreignKeyId(0), childTableId(0), childColumnOrdinal(0), parentTableId(0),
          parentColumnOrdinal(0), referencedIndexId(0), supportIndexId(0) {}
};

struct TableDefinition {
    std::string name;
    std::vector<ColumnDefinition> columns;
    std::vector<IndexDefinition> indexes; // SQL6 constraints/indexes
    std::vector<ForeignKeyDefinition> foreignKeys; // SQL8

    TableDefinition() {}
    explicit TableDefinition(const std::string& nameIn) : name(nameIn) {}
};

// Discovery record returned by listTables().
struct TableInfo {
    uint32_t tableId;
    std::string name;
    uint32_t columnCount;
    uint32_t heapPageCount;
    uint64_t rowCount;

    TableInfo()
        : tableId(0), columnCount(0), heapPageCount(0), rowCount(0) {}
};

// Discovery record returned by listIndexes()/describeIndex().
struct IndexInfo {
    uint32_t indexId;
    std::string name;
    uint32_t tableId;
    std::string tableName;
    uint32_t columnOrdinal;
    std::string columnName;
    DbType columnType;
    IndexKind kind;
    uint64_t rootPageId;
    uint64_t entryCount;
    uint16_t formatVersion;

    IndexInfo()
        : indexId(0), tableId(0), columnOrdinal(0), columnType(DbType::Unknown),
          kind(IndexKind::Ordinary), rootPageId(0), entryCount(0),
          formatVersion(0) {}

    bool unique() const { return indexKindIsUnique(kind); }
    bool primaryKey() const { return kind == IndexKind::PrimaryKey; }
};

// Discovery record for a foreign key (SQL8).
struct ForeignKeyInfo {
    uint32_t foreignKeyId;
    uint32_t childTableId;
    std::string childTableName;
    uint32_t childColumnOrdinal;
    std::string childColumnName;
    uint32_t parentTableId;
    std::string parentTableName;
    uint32_t parentColumnOrdinal;
    std::string parentColumnName;
    uint32_t referencedIndexId;
    uint32_t supportIndexId;

    ForeignKeyInfo()
        : foreignKeyId(0), childTableId(0), childColumnOrdinal(0), parentTableId(0),
          parentColumnOrdinal(0), referencedIndexId(0), supportIndexId(0) {}
};

} // namespace db
} // namespace gxos
