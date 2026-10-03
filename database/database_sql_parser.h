#pragma once
// guideXOS SQL -- Phase SQL4/SQL5
// Recursive-descent SQL parser.
//
// The parser is bounded: nesting depth is limited by kSqlMaxNestingDepth and
// the total number of expression nodes by kSqlMaxExprNodes, so no hostile input
// can exhaust the stack or allocate an unbounded tree. Statements are produced
// one at a time so the executor can run them incrementally and stop at the
// first failure.
//
// It never executes while parsing; it only builds AST nodes.

#include <vector>

#include "database_sql_ast.h"
#include "database_sql_token.h"

namespace gxos {
namespace db {

class SqlParser {
public:
    explicit SqlParser(const std::vector<SqlToken>& tokens);

    // Produces the next statement. Returns false and fills `error` on a syntax
    // or resource error. When the input is exhausted, returns true with
    // `done == true` and leaves `out` untouched.
    bool next(SqlStatementAst& out, SqlError& error, bool& done);

private:
    const SqlToken& peek(size_t lookahead = 0) const;
    const SqlToken& advance();
    bool check(SqlTokenKind kind) const;
    bool match(SqlTokenKind kind);
    bool expect(SqlTokenKind kind, SqlError& error, const char* what);

    SqlError makeError(SqlErrorCode code, const std::string& message,
                       const SqlToken& token) const;

    bool parseStatement(SqlStatementAst& out, SqlError& error);
    bool parseCreateTable(SqlStatementAst& out, SqlError& error);
    bool parseCreateIndex(SqlStatementAst& out, SqlError& error);
    bool parseInsert(SqlStatementAst& out, SqlError& error);
    bool parseSelect(SqlStatementAst& out, SqlError& error);
    bool parseUpdate(SqlStatementAst& out, SqlError& error);
    bool parseDelete(SqlStatementAst& out, SqlError& error);
    bool parseColumnDef(SqlColumnDefAst& out, SqlError& error);
    bool parseLiteral(SqlLiteralAst& out, SqlError& error);
    bool parseIdentifier(SqlIdentifier& out, SqlError& error, const char* what);

    // ---- SQL5 predicate expressions ------------------------------------
    // `orExpr` is the entry point. Precedence (low to high):
    //   OR, AND, NOT, comparison/IS NULL, parenthesized/primary.
    bool parseOrExpr(SqlPredicateAst& arena, int32_t& out, SqlError& error);
    bool parseAndExpr(SqlPredicateAst& arena, int32_t& out, SqlError& error);
    bool parseNotExpr(SqlPredicateAst& arena, int32_t& out, SqlError& error);
    bool parseComparison(SqlPredicateAst& arena, int32_t& out, SqlError& error);
    bool parsePrimary(SqlPredicateAst& arena, int32_t& out, SqlError& error);

    // Appends a node to `arena` and returns its index, or -1 on resource limit.
    int32_t allocExpr(SqlPredicateAst& arena, const SqlToken& token,
                      SqlError& error);

    bool parseWhereClause(SqlPredicateAst& out, SqlError& error);
    bool parseOrderBy(std::vector<SqlOrderTermAst>& out, SqlError& error);
    bool parseLimitOffset(SqlSelectAst& out, SqlError& error);
    bool parseAssignments(std::vector<SqlAssignmentAst>& out, SqlError& error);

    const std::vector<SqlToken>& _tokens;
    size_t _index;
    size_t _depth;
};

} // namespace db
} // namespace gxos
