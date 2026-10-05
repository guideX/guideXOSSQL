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
    CreateIndex,
    Insert,
    Select,
    Update,
    Delete,
    Begin,
    Commit,
    Rollback,
    // SQL8 schema lifecycle.
    DropIndex,
    DropTable,
    AlterTableAddColumn
};

const char* sqlStatementTypeName(SqlStatementType type);

// An identifier plus the source location of its first byte. SQL7 adds an
// optional qualifier so a column reference can be written `Users.Id` or `u.Id`.
// `name` is always the column/object name; when `hasQualifier` is set,
// `qualifier` holds the relation name or alias written before the dot.
struct SqlIdentifier {
    std::string name;
    std::string qualifier;
    bool hasQualifier;
    uint32_t line;
    uint32_t column;

    SqlIdentifier() : hasQualifier(false), line(0), column(0) {}
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
    bool isDefault; // SQL8: true when this is the DEFAULT keyword in VALUES
    uint32_t line;
    uint32_t column;

    SqlLiteralAst()
        : kind(SqlLiteralKind::Null), boolValue(false), int64Value(0),
          float64Value(0.0), isDefault(false), line(0), column(0) {}
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

// ---- SQL7 joins, aliases, aggregates and grouping --------------------------

// A relation reference in FROM or after JOIN: a table name plus an optional
// query-local alias. `hasAlias` selects whether `alias` or `table.name` is the
// qualifier for that source in the SELECT scope.
struct SqlTableRefAst {
    SqlIdentifier table;
    bool hasAlias;
    SqlIdentifier alias;

    SqlTableRefAst() : hasAlias(false) {}
};

enum class SqlJoinType { Inner = 0, Left };

const char* sqlJoinTypeName(SqlJoinType type);

// One `JOIN table_ref ON predicate` clause.
struct SqlJoinAst {
    SqlJoinType type;
    SqlTableRefAst table;
    SqlPredicateAst on;
    uint32_t line;
    uint32_t column;

    SqlJoinAst() : type(SqlJoinType::Inner), line(0), column(0) {}
};

enum class SqlAggregateKind { Count = 0, Sum, Avg, Min, Max };

const char* sqlAggregateKindName(SqlAggregateKind kind);

// An aggregate call. `star` is true only for COUNT(*); otherwise `column` is the
// single column argument.
struct SqlAggregateAst {
    SqlAggregateKind kind;
    bool star;
    SqlIdentifier column;

    SqlAggregateAst() : kind(SqlAggregateKind::Count), star(false) {}
};

enum class SqlProjectionKind { Star = 0, Column, Aggregate };

// One SELECT projection entry: `*`, a column reference, or an aggregate call,
// each with an optional output alias.
struct SqlProjectionItemAst {
    SqlProjectionKind kind;
    SqlIdentifier columnRef;    // Column
    SqlAggregateAst aggregate;  // Aggregate
    bool hasAlias;
    SqlIdentifier alias;
    uint32_t line;
    uint32_t column;

    SqlProjectionItemAst()
        : kind(SqlProjectionKind::Column), hasAlias(false), line(0), column(0) {}
};

struct SqlColumnDefAst {
    SqlIdentifier name;
    DbType type;
    bool nullable;
    bool primaryKey;
    bool unique;
    bool hasDefault;
    SqlLiteralAst defaultValue;

    SqlColumnDefAst()
        : type(DbType::Unknown), nullable(true), primaryKey(false), unique(false),
          hasDefault(false) {}
};

// SQL8 table-level FOREIGN KEY constraint.
struct SqlForeignKeyAst {
    SqlIdentifier childColumn;
    SqlIdentifier parentTable;
    SqlIdentifier parentColumn;
    uint32_t line;
    uint32_t column;

    SqlForeignKeyAst() : line(0), column(0) {}
};

struct SqlCreateTableAst {
    SqlIdentifier table;
    std::vector<SqlColumnDefAst> columns;
    std::vector<SqlForeignKeyAst> foreignKeys;
    uint32_t line;
    uint32_t column;

    SqlCreateTableAst() : line(0), column(0) {}
};

// SQL8 DROP INDEX.
struct SqlDropIndexAst {
    SqlIdentifier index;
    uint32_t line;
    uint32_t column;

    SqlDropIndexAst() : line(0), column(0) {}
};

// SQL8 DROP TABLE.
struct SqlDropTableAst {
    SqlIdentifier table;
    uint32_t line;
    uint32_t column;

    SqlDropTableAst() : line(0), column(0) {}
};

// SQL8 ALTER TABLE ADD COLUMN.
struct SqlAlterTableAddColumnAst {
    SqlIdentifier table;
    SqlColumnDefAst columnDef;
    uint32_t line;
    uint32_t column;

    SqlAlterTableAddColumnAst() : line(0), column(0) {}
};

// CREATE [UNIQUE] INDEX name ON table (column)
struct SqlCreateIndexAst {
    SqlIdentifier index;
    SqlIdentifier table;
    SqlIdentifier columnName;
    bool unique;
    uint32_t line;
    uint32_t column;

    SqlCreateIndexAst() : unique(false), line(0), column(0) {}
};

struct SqlInsertAst {
    SqlIdentifier table;
    std::vector<SqlLiteralAst> values;
    bool hasColumnList;
    std::vector<SqlIdentifier> columnList;
    uint32_t line;
    uint32_t column;

    SqlInsertAst() : hasColumnList(false), line(0), column(0) {}
};

struct SqlSelectAst {
    bool distinct;
    bool star;
    // Legacy simple-column view of the projection. Populated for every
    // SqlProjectionKind::Column item so SQL4/SQL5 callers keep working.
    std::vector<SqlIdentifier> columns;
    // Authoritative SQL7 projection list.
    std::vector<SqlProjectionItemAst> projection;
    // Legacy base-table view of FROM. Populated with the base relation name.
    SqlIdentifier table;
    // Authoritative base relation reference (table + optional alias).
    SqlTableRefAst from;
    std::vector<SqlJoinAst> joins;
    SqlPredicateAst where;
    std::vector<SqlIdentifier> groupBy;
    std::vector<SqlOrderTermAst> orderBy;
    bool hasLimit;
    uint64_t limit;
    bool hasOffset;
    uint64_t offset;
    uint32_t line;
    uint32_t column;

    SqlSelectAst()
        : distinct(false), star(false), hasLimit(false), limit(0), hasOffset(false),
          offset(0), line(0), column(0) {}
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
    SqlCreateIndexAst createIndex;
    SqlInsertAst insert;
    SqlSelectAst select;
    SqlUpdateAst update;
    SqlDeleteAst deleteStatement;
    SqlDropIndexAst dropIndex;
    SqlDropTableAst dropTable;
    SqlAlterTableAddColumnAst alterTableAddColumn;
    uint32_t line;
    uint32_t column;

    SqlStatementAst() : type(SqlStatementType::Unknown), line(0), column(0) {}
};

} // namespace db
} // namespace gxos
