#pragma once
// guideXOS SQL -- Phase SQL4
// The SQL4 abstract syntax tree.
//
// The AST owns all of its strings and bytes, so it remains valid after the
// token vector is discarded. Nodes are plain value types with no inheritance:
// SQL4 is intentionally small and a tagged union keeps the tree easy to read
// and extend.
//
// The AST contains no page, heap, WAL or catalog concepts. It is the only
// output of the parser and the only input to the executor.

#include <cstdint>
#include <string>
#include <vector>

#include "database_types.h"

namespace gxos {
namespace db {

enum class SqlStatementType {
    Unknown = 0,
    CreateTable,
    Insert,
    Select,
    Begin,
    Commit,
    Rollback
};

const char* sqlStatementTypeName(SqlStatementType type);

// An identifier plus the source location of its first byte.
struct SqlIdentifier {
    std::string name;
    uint32_t line;
    uint32_t column;

    SqlIdentifier() : line(0), column(0) {}
};

// A literal value in an INSERT ... VALUES list.
enum class SqlLiteralKind { Null, Boolean, Integer, Float, String, Blob };

struct SqlLiteralAst {
    SqlLiteralKind kind;
    bool boolValue;
    int64_t int64Value;
    double float64Value;
    std::string textValue;
    std::vector<uint8_t> blobValue;
    uint32_t line;
    uint32_t column;

    SqlLiteralAst()
        : kind(SqlLiteralKind::Null), boolValue(false), int64Value(0),
          float64Value(0.0), line(0), column(0) {}
};

struct SqlColumnDefAst {
    SqlIdentifier name;
    DbType type;
    bool nullable;

    SqlColumnDefAst() : type(DbType::Unknown), nullable(true) {}
};

struct SqlCreateTableAst {
    SqlIdentifier table;
    std::vector<SqlColumnDefAst> columns;
    uint32_t line;
    uint32_t column;

    SqlCreateTableAst() : line(0), column(0) {}
};

struct SqlInsertAst {
    SqlIdentifier table;
    std::vector<SqlLiteralAst> values;
    uint32_t line;
    uint32_t column;

    SqlInsertAst() : line(0), column(0) {}
};

struct SqlSelectAst {
    bool star;
    std::vector<SqlIdentifier> columns;
    SqlIdentifier table;
    uint32_t line;
    uint32_t column;

    SqlSelectAst() : star(false), line(0), column(0) {}
};

// One parsed statement. Exactly one of the payload members is meaningful,
// selected by `type`.
struct SqlStatementAst {
    SqlStatementType type;
    SqlCreateTableAst createTable;
    SqlInsertAst insert;
    SqlSelectAst select;
    uint32_t line;
    uint32_t column;

    SqlStatementAst() : type(SqlStatementType::Unknown), line(0), column(0) {}
};

} // namespace db
} // namespace gxos
