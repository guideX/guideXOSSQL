#pragma once
// guideXOS SQL -- Phase SQL4
// Recursive-descent SQL parser.
//
// The parser is bounded: nesting depth is limited by kSqlMaxNestingDepth and
// the grammar of SQL4 has no expressions, so no hostile input can exhaust the
// stack. Statements are produced one at a time so the executor can run them
// incrementally and stop at the first failure.
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
    bool parseInsert(SqlStatementAst& out, SqlError& error);
    bool parseSelect(SqlStatementAst& out, SqlError& error);
    bool parseColumnDef(SqlColumnDefAst& out, SqlError& error);
    bool parseLiteral(SqlLiteralAst& out, SqlError& error);
    bool parseIdentifier(SqlIdentifier& out, SqlError& error, const char* what);

    const std::vector<SqlToken>& _tokens;
    size_t _index;
};

} // namespace db
} // namespace gxos
