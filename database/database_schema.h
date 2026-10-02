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

    ColumnDefinition() : type(DbType::Unknown), nullable(false), ordinal(0) {}
    ColumnDefinition(const std::string& nameIn, DbType typeIn, bool nullableIn)
        : name(nameIn), type(typeIn), nullable(nullableIn), ordinal(0) {}
};

struct TableDefinition {
    std::string name;
    std::vector<ColumnDefinition> columns;

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

} // namespace db
} // namespace gxos
