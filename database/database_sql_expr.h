#pragma once
// guideXOS SQL -- Phase SQL5
// Bound predicate expressions and SQL three-valued logic.
//
// A parsed SqlPredicateAst is bound once against a table's durable schema
// before any row is scanned: every column reference becomes a resolved ordinal
// and type, every literal is type-checked and coerced, and every comparison is
// validated for its operand types. Evaluation then runs against decoded rows
// with no further name lookup and no runtime type errors.
//
// The single evaluator defined here drives SELECT ... WHERE, UPDATE ... WHERE
// and DELETE ... WHERE so SQL Boolean semantics live in exactly one place.
//
// This module depends only on the AST, the schema and the value model. It has
// no page, heap, catalog, WAL or transaction concepts.

#include <cstddef>
#include <vector>

#include "database_schema.h"
#include "database_sql_ast.h"
#include "database_sql_token.h"
#include "database_types.h"

namespace gxos {
namespace db {

// SQL three-valued truth: TRUE, FALSE and UNKNOWN.
enum class SqlTruth { False = 0, True = 1, Unknown = 2 };

// Combines two truth values with AND / OR, and negates with NOT, using the
// standard SQL truth tables. Exposed so the truth tables can be unit tested
// directly.
SqlTruth sqlTruthAnd(SqlTruth a, SqlTruth b);
SqlTruth sqlTruthOr(SqlTruth a, SqlTruth b);
SqlTruth sqlTruthNot(SqlTruth a);

const char* sqlTruthName(SqlTruth truth);

// Resolves a (possibly qualified) column reference against the visible relation
// sources of one SELECT scope. SQL5 binds against a single table; SQL7 binds
// against a joined schema and must additionally detect ambiguous unqualified
// references and unknown qualifiers. The executor supplies the implementation.
class SqlColumnResolver {
public:
    virtual ~SqlColumnResolver() {}
    virtual bool resolveColumn(const SqlIdentifier& identifier, size_t& ordinal,
                               DbType& type, SqlError& error) const = 0;
};

// A predicate bound to a concrete schema.
class BoundPredicate {
public:
    BoundPredicate();

    // Binds the predicate rooted at `root` in `nodes` against `columns`
    // (single-table SQL5 path). Returns false and fills `error` with a
    // SemanticError on any type error, unknown column or unsupported operator. A
    // predicate with no WHERE clause binds successfully and always evaluates
    // TRUE.
    bool bind(const std::vector<SqlExprNode>& nodes, int32_t root,
              const std::vector<ColumnDefinition>& columns, SqlError& error);

    // SQL7: binds against a resolver that may resolve qualified names across
    // multiple relation sources and report ambiguity.
    bool bind(const std::vector<SqlExprNode>& nodes, int32_t root,
              const SqlColumnResolver& resolver, SqlError& error);

    bool isBound() const { return _bound; }

    // Evaluates the bound predicate for one decoded row. Only safe after a
    // successful bind().
    SqlTruth evaluate(const std::vector<DbValue>& row) const;

private:
    enum class Kind {
        Column,
        Literal,
        Compare,
        And,
        Or,
        Not,
        IsNull
    };

    struct Node {
        Kind kind;
        // Column
        size_t ordinal;
        DbType type;
        // Literal
        DbValue literal;
        // Compare
        SqlCompareOp compareOp;
        DbType compareType;
        bool alwaysUnknown;
        // IsNull
        bool negated;
        // Children
        int32_t left;
        int32_t right;

        Node()
            : kind(Kind::Literal), ordinal(0), type(DbType::Unknown),
              compareOp(SqlCompareOp::Eq), compareType(DbType::Unknown),
              alwaysUnknown(false), negated(false), left(-1), right(-1) {}
    };

    bool bindInternal(const std::vector<SqlExprNode>& nodes, int32_t root,
                      const SqlColumnResolver& resolver, SqlError& error);
    bool bindNode(const std::vector<SqlExprNode>& nodes, int32_t index,
                  const SqlColumnResolver& resolver, int32_t& outIndex,
                  SqlError& error);
    bool isTruthNode(int32_t index) const;

    SqlTruth evalTruth(int32_t index, const std::vector<DbValue>& row) const;
    DbValue evalValue(int32_t index, const std::vector<DbValue>& row) const;

    std::vector<Node> _nodes;
    int32_t _root;
    bool _bound;
};

} // namespace db
} // namespace gxos
