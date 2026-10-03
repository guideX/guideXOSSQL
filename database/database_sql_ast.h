#pragma once
// guideXOS SQL -- Phase SQL4/SQL5
// The SQL abstract syntax tree.
//
// The AST owns all of its strings and bytes, so it remains valid after the
// token vector is discarded. Nodes are plain value types with no inheritance:
// the tree stays easy to read, copy and extend.
//
// SQL5 adds a bounded predicate expression tree. Expressions are stored as a
// flat arena (a vector of nodes whose children are integer indices) rather than
// owning pointers. This keeps every AST node trivially copyable and makes
// bounding the total node count independent of nesting depth trivial.
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
    Update,
    Delete,
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

// A literal value in an INSERT ... VALUES list or an expression.
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

// ---- SQL5 predicate expressions -------------------------------------------

enum class SqlExprKind {
    Invalid = 0,
    ColumnRef, // identifier
    Literal,   // literal
    Compare,   // compareOp, left, right
    And,       // left, right
    Or,        // left, right
    Not,       // left
    IsNull     // left; negated == true means IS NOT NULL
};

enum class SqlCompareOp { Eq = 0, Ne, Lt, Le, Gt, Ge };

const char* sqlCompareOpName(SqlCompareOp op);

// One node of the flat expression arena. Children are indices into the arena's
// node vector; -1 means "no child".
struct SqlExprNode {
    SqlExprKind kind;
    SqlIdentifier identifier; // ColumnRef
    SqlLiteralAst literal;    // Literal
    SqlCompareOp compareOp;   // Compare
    bool negated;             // IsNull
    int32_t left;
    int32_t right;
    uint32_t line;
    uint32_t column;

    SqlExprNode()
        : kind(SqlExprKind::Invalid), compareOp(SqlCompareOp::Eq), negated(false),
          left(-1), right(-1), line(0), column(0) {}
};

// A WHERE predicate: a flat expression arena plus the root node index. When
// `present` is false there is no predicate (all rows qualify).
struct SqlPredicateAst {
    std::vector<SqlExprNode> nodes;
    int32_t root;
    bool present;

    SqlPredicateAst() : nodes(), root(-1), present(false) {}
};

struct SqlOrderTermAst {
    SqlIdentifier column;
    bool descending;

    SqlOrderTermAst() : descending(false) {}
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
    SqlPredicateAst where;
    std::vector<SqlOrderTermAst> orderBy;
    bool hasLimit;
    uint64_t limit;
    bool hasOffset;
    uint64_t offset;
    uint32_t line;
    uint32_t column;

    SqlSelectAst()
        : star(false), hasLimit(false), limit(0), hasOffset(false), offset(0),
          line(0), column(0) {}
};

struct SqlAssignmentAst {
    SqlIdentifier column;
    SqlLiteralAst value;

    SqlAssignmentAst() {}
};

struct SqlUpdateAst {
    SqlIdentifier table;
    std::vector<SqlAssignmentAst> assignments;
    SqlPredicateAst where;
    uint32_t line;
    uint32_t column;

    SqlUpdateAst() : line(0), column(0) {}
};

struct SqlDeleteAst {
    SqlIdentifier table;
    SqlPredicateAst where;
    uint32_t line;
    uint32_t column;

    SqlDeleteAst() : line(0), column(0) {}
};

// One parsed statement. Exactly one of the payload members is meaningful,
// selected by `type`.
struct SqlStatementAst {
    SqlStatementType type;
    SqlCreateTableAst createTable;
    SqlInsertAst insert;
    SqlSelectAst select;
    SqlUpdateAst update;
    SqlDeleteAst deleteStatement;
    uint32_t line;
    uint32_t column;

    SqlStatementAst() : type(SqlStatementType::Unknown), line(0), column(0) {}
};

} // namespace db
} // namespace gxos
