#include "database_sql_parser.h"

namespace gxos {
namespace db {

const char* sqlStatementTypeName(SqlStatementType type) {
    switch (type) {
    case SqlStatementType::CreateTable: return "CREATE TABLE";
    case SqlStatementType::Insert: return "INSERT";
    case SqlStatementType::Select: return "SELECT";
    case SqlStatementType::Update: return "UPDATE";
    case SqlStatementType::Delete: return "DELETE";
    case SqlStatementType::Begin: return "BEGIN";
    case SqlStatementType::Commit: return "COMMIT";
    case SqlStatementType::Rollback: return "ROLLBACK";
    case SqlStatementType::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* sqlCompareOpName(SqlCompareOp op) {
    switch (op) {
    case SqlCompareOp::Eq: return "=";
    case SqlCompareOp::Ne: return "<>";
    case SqlCompareOp::Lt: return "<";
    case SqlCompareOp::Le: return "<=";
    case SqlCompareOp::Gt: return ">";
    case SqlCompareOp::Ge: return ">=";
    }
    return "?";
}

namespace {

bool comparisonToken(SqlTokenKind kind, SqlCompareOp& out) {
    switch (kind) {
    case SqlTokenKind::Eq: out = SqlCompareOp::Eq; return true;
    case SqlTokenKind::Ne: out = SqlCompareOp::Ne; return true;
    case SqlTokenKind::Lt: out = SqlCompareOp::Lt; return true;
    case SqlTokenKind::Le: out = SqlCompareOp::Le; return true;
    case SqlTokenKind::Gt: out = SqlCompareOp::Gt; return true;
    case SqlTokenKind::Ge: out = SqlCompareOp::Ge; return true;
    default: return false;
    }
}

} // namespace

SqlParser::SqlParser(const std::vector<SqlToken>& tokens)
    : _tokens(tokens), _index(0), _depth(0) {}

const SqlToken& SqlParser::peek(size_t lookahead) const {
    const size_t at = _index + lookahead;
    if (at >= _tokens.size()) {
        return _tokens.back();
    }
    return _tokens[at];
}

const SqlToken& SqlParser::advance() {
    const SqlToken& token = peek();
    if (_index < _tokens.size()) {
        ++_index;
    }
    return token;
}

bool SqlParser::check(SqlTokenKind kind) const { return peek().kind == kind; }

bool SqlParser::match(SqlTokenKind kind) {
    if (check(kind)) {
        advance();
        return true;
    }
    return false;
}

SqlError SqlParser::makeError(SqlErrorCode code, const std::string& message,
                              const SqlToken& token) const {
    SqlError error;
    error.code = code;
    error.message = message;
    error.offset = token.offset;
    error.line = token.line;
    error.column = token.column;
    error.hasLocation = true;
    return error;
}

bool SqlParser::expect(SqlTokenKind kind, SqlError& error, const char* what) {
    if (check(kind)) {
        advance();
        return true;
    }
    std::string message = "expected ";
    message += what;
    message += ", found ";
    message += sqlTokenKindName(peek().kind);
    error = makeError(SqlErrorCode::SyntaxError, message, peek());
    return false;
}

bool SqlParser::next(SqlStatementAst& out, SqlError& error, bool& done) {
    // Skip empty statements: runs of ';' are accepted and ignored.
    while (check(SqlTokenKind::Semicolon)) {
        advance();
    }
    if (check(SqlTokenKind::EndOfInput)) {
        done = true;
        return true;
    }
    // Reset the target so callers may safely reuse one AST object.
    out = SqlStatementAst();
    if (!parseStatement(out, error)) {
        return false;
    }
    if (check(SqlTokenKind::Semicolon)) {
        advance();
    } else if (!check(SqlTokenKind::EndOfInput)) {
        std::string message = "expected ';' or end of input after statement, found ";
        message += sqlTokenKindName(peek().kind);
        error = makeError(SqlErrorCode::SyntaxError, message, peek());
        return false;
    }
    done = false;
    return true;
}

bool SqlParser::parseStatement(SqlStatementAst& out, SqlError& error) {
    const SqlToken& token = peek();
    out.line = token.line;
    out.column = token.column;
    switch (token.kind) {
    case SqlTokenKind::Create:
        out.type = SqlStatementType::CreateTable;
        return parseCreateTable(out, error);
    case SqlTokenKind::Insert:
        out.type = SqlStatementType::Insert;
        return parseInsert(out, error);
    case SqlTokenKind::Select:
        out.type = SqlStatementType::Select;
        return parseSelect(out, error);
    case SqlTokenKind::Update:
        out.type = SqlStatementType::Update;
        return parseUpdate(out, error);
    case SqlTokenKind::Delete:
        out.type = SqlStatementType::Delete;
        return parseDelete(out, error);
    case SqlTokenKind::Begin:
        advance();
        out.type = SqlStatementType::Begin;
        return true;
    case SqlTokenKind::Commit:
        advance();
        out.type = SqlStatementType::Commit;
        return true;
    case SqlTokenKind::Rollback:
        advance();
        out.type = SqlStatementType::Rollback;
        return true;
    default: {
        std::string message = "unexpected ";
        message += sqlTokenKindName(token.kind);
        message += " at start of statement";
        error = makeError(SqlErrorCode::SyntaxError, message, token);
        return false;
    }
    }
}

bool SqlParser::parseCreateTable(SqlStatementAst& out, SqlError& error) {
    advance(); // CREATE
    if (!expect(SqlTokenKind::Table, error, "TABLE after CREATE")) {
        return false;
    }
    if (!parseIdentifier(out.createTable.table, error, "table name")) {
        return false;
    }
    if (!expect(SqlTokenKind::LeftParen, error, "'(' after table name")) {
        return false;
    }
    SqlColumnDefAst column;
    if (!parseColumnDef(column, error)) {
        return false;
    }
    out.createTable.columns.push_back(column);
    while (match(SqlTokenKind::Comma)) {
        if (out.createTable.columns.size() >= kSqlMaxColumnsPerCreate) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "column count exceeds the SQL maximum", peek());
            return false;
        }
        if (!parseColumnDef(column, error)) {
            return false;
        }
        out.createTable.columns.push_back(column);
    }
    if (!expect(SqlTokenKind::RightParen, error, "')' after column list")) {
        return false;
    }
    return true;
}

bool SqlParser::parseColumnDef(SqlColumnDefAst& out, SqlError& error) {
    if (!parseIdentifier(out.name, error, "column name")) {
        return false;
    }
    if (!sqlTokenIsTypeKeyword(peek().kind)) {
        std::string message = "expected a column type, found ";
        message += sqlTokenKindName(peek().kind);
        error = makeError(SqlErrorCode::SyntaxError, message, peek());
        return false;
    }
    sqlTypeKeywordToDbType(peek().kind, out.type);
    advance();
    if (check(SqlTokenKind::LeftParen)) {
        error = makeError(SqlErrorCode::Unsupported,
                          "type length or precision declarations are not supported",
                          peek());
        return false;
    }
    out.nullable = true;
    if (match(SqlTokenKind::Not)) {
        if (!expect(SqlTokenKind::Null, error, "NULL after NOT")) {
            return false;
        }
        out.nullable = false;
    } else if (match(SqlTokenKind::Null)) {
        out.nullable = true;
    }
    return true;
}

bool SqlParser::parseInsert(SqlStatementAst& out, SqlError& error) {
    advance(); // INSERT
    if (!expect(SqlTokenKind::Into, error, "INTO after INSERT")) {
        return false;
    }
    if (!parseIdentifier(out.insert.table, error, "table name")) {
        return false;
    }
    if (!expect(SqlTokenKind::Values, error, "VALUES after table name")) {
        return false;
    }
    if (!expect(SqlTokenKind::LeftParen, error, "'(' after VALUES")) {
        return false;
    }
    SqlLiteralAst literal;
    if (!parseLiteral(literal, error)) {
        return false;
    }
    out.insert.values.push_back(literal);
    while (match(SqlTokenKind::Comma)) {
        if (out.insert.values.size() >= kSqlMaxValuesPerInsert) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "value count exceeds the SQL maximum", peek());
            return false;
        }
        if (!parseLiteral(literal, error)) {
            return false;
        }
        out.insert.values.push_back(literal);
    }
    if (!expect(SqlTokenKind::RightParen, error, "')' after value list")) {
        return false;
    }
    return true;
}

bool SqlParser::parseSelect(SqlStatementAst& out, SqlError& error) {
    advance(); // SELECT
    if (match(SqlTokenKind::Star)) {
        out.select.star = true;
    } else {
        SqlIdentifier identifier;
        if (!parseIdentifier(identifier, error, "column name or '*'")) {
            return false;
        }
        out.select.columns.push_back(identifier);
        while (match(SqlTokenKind::Comma)) {
            if (out.select.columns.size() >= kSqlMaxSelectColumns) {
                error = makeError(SqlErrorCode::ResourceLimit,
                                  "projection exceeds the SQL maximum", peek());
                return false;
            }
            if (!parseIdentifier(identifier, error, "column name")) {
                return false;
            }
            out.select.columns.push_back(identifier);
        }
    }
    if (!expect(SqlTokenKind::From, error, "FROM after projection")) {
        return false;
    }
    if (!parseIdentifier(out.select.table, error, "table name")) {
        return false;
    }
    if (!parseWhereClause(out.select.where, error)) {
        return false;
    }
    if (match(SqlTokenKind::Order)) {
        if (!expect(SqlTokenKind::By, error, "BY after ORDER")) {
            return false;
        }
        if (!parseOrderBy(out.select.orderBy, error)) {
            return false;
        }
    }
    if (!parseLimitOffset(out.select, error)) {
        return false;
    }
    return true;
}

bool SqlParser::parseUpdate(SqlStatementAst& out, SqlError& error) {
    advance(); // UPDATE
    if (!parseIdentifier(out.update.table, error, "table name")) {
        return false;
    }
    if (!expect(SqlTokenKind::Set, error, "SET after table name")) {
        return false;
    }
    if (!parseAssignments(out.update.assignments, error)) {
        return false;
    }
    if (!parseWhereClause(out.update.where, error)) {
        return false;
    }
    return true;
}

bool SqlParser::parseDelete(SqlStatementAst& out, SqlError& error) {
    advance(); // DELETE
    if (!expect(SqlTokenKind::From, error, "FROM after DELETE")) {
        return false;
    }
    if (!parseIdentifier(out.deleteStatement.table, error, "table name")) {
        return false;
    }
    if (!parseWhereClause(out.deleteStatement.where, error)) {
        return false;
    }
    return true;
}

bool SqlParser::parseAssignments(std::vector<SqlAssignmentAst>& out,
                                 SqlError& error) {
    SqlAssignmentAst assignment;
    if (!parseIdentifier(assignment.column, error, "column name")) {
        return false;
    }
    if (!expect(SqlTokenKind::Eq, error, "'=' after column name")) {
        return false;
    }
    if (!parseLiteral(assignment.value, error)) {
        return false;
    }
    out.push_back(assignment);
    while (match(SqlTokenKind::Comma)) {
        if (out.size() >= kSqlMaxUpdateAssignments) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "assignment count exceeds the SQL5 maximum", peek());
            return false;
        }
        SqlAssignmentAst next;
        if (!parseIdentifier(next.column, error, "column name")) {
            return false;
        }
        if (!expect(SqlTokenKind::Eq, error, "'=' after column name")) {
            return false;
        }
        if (!parseLiteral(next.value, error)) {
            return false;
        }
        out.push_back(next);
    }
    return true;
}

bool SqlParser::parseOrderBy(std::vector<SqlOrderTermAst>& out, SqlError& error) {
    SqlOrderTermAst term;
    if (!parseIdentifier(term.column, error, "column name")) {
        return false;
    }
    if (match(SqlTokenKind::Asc)) {
        term.descending = false;
    } else if (match(SqlTokenKind::Desc)) {
        term.descending = true;
    }
    out.push_back(term);
    while (match(SqlTokenKind::Comma)) {
        if (out.size() >= kSqlMaxOrderByColumns) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "ORDER BY column count exceeds the SQL5 maximum", peek());
            return false;
        }
        SqlOrderTermAst next;
        if (!parseIdentifier(next.column, error, "column name")) {
            return false;
        }
        if (match(SqlTokenKind::Asc)) {
            next.descending = false;
        } else if (match(SqlTokenKind::Desc)) {
            next.descending = true;
        }
        out.push_back(next);
    }
    return true;
}

bool SqlParser::parseLimitOffset(SqlSelectAst& out, SqlError& error) {
    if (match(SqlTokenKind::Limit)) {
        if (!check(SqlTokenKind::IntegerLiteral)) {
            std::string message = "expected a nonnegative integer after LIMIT, found ";
            message += sqlTokenKindName(peek().kind);
            error = makeError(SqlErrorCode::SyntaxError, message, peek());
            return false;
        }
        const int64_t value = peek().int64Value;
        if (value < 0) {
            error = makeError(SqlErrorCode::SyntaxError,
                              "LIMIT must be a nonnegative integer", peek());
            return false;
        }
        out.hasLimit = true;
        out.limit = static_cast<uint64_t>(value);
        advance();
    }
    if (match(SqlTokenKind::Offset)) {
        if (!check(SqlTokenKind::IntegerLiteral)) {
            std::string message = "expected a nonnegative integer after OFFSET, found ";
            message += sqlTokenKindName(peek().kind);
            error = makeError(SqlErrorCode::SyntaxError, message, peek());
            return false;
        }
        const int64_t value = peek().int64Value;
        if (value < 0) {
            error = makeError(SqlErrorCode::SyntaxError,
                              "OFFSET must be a nonnegative integer", peek());
            return false;
        }
        out.hasOffset = true;
        out.offset = static_cast<uint64_t>(value);
        advance();
    }
    return true;
}

bool SqlParser::parseLiteral(SqlLiteralAst& out, SqlError& error) {
    const SqlToken& token = peek();
    out.line = token.line;
    out.column = token.column;
    switch (token.kind) {
    case SqlTokenKind::Null:
        out.kind = SqlLiteralKind::Null;
        advance();
        return true;
    case SqlTokenKind::True:
        out.kind = SqlLiteralKind::Boolean;
        out.boolValue = true;
        advance();
        return true;
    case SqlTokenKind::False:
        out.kind = SqlLiteralKind::Boolean;
        out.boolValue = false;
        advance();
        return true;
    case SqlTokenKind::IntegerLiteral:
        out.kind = SqlLiteralKind::Integer;
        out.int64Value = token.int64Value;
        advance();
        return true;
    case SqlTokenKind::FloatLiteral:
        out.kind = SqlLiteralKind::Float;
        out.float64Value = token.float64Value;
        advance();
        return true;
    case SqlTokenKind::StringLiteral:
        out.kind = SqlLiteralKind::String;
        out.textValue = token.text;
        advance();
        return true;
    case SqlTokenKind::BlobLiteral:
        out.kind = SqlLiteralKind::Blob;
        out.blobValue = token.bytes;
        advance();
        return true;
    default: {
        std::string message = "expected a literal value, found ";
        message += sqlTokenKindName(token.kind);
        error = makeError(SqlErrorCode::SyntaxError, message, token);
        return false;
    }
    }
}

bool SqlParser::parseIdentifier(SqlIdentifier& out, SqlError& error,
                                const char* what) {
    if (!check(SqlTokenKind::Identifier)) {
        std::string message = "expected ";
        message += what;
        message += ", found ";
        message += sqlTokenKindName(peek().kind);
        error = makeError(SqlErrorCode::SyntaxError, message, peek());
        return false;
    }
    const SqlToken& token = peek();
    out.name = token.text;
    out.line = token.line;
    out.column = token.column;
    advance();
    return true;
}

// ---------------------------------------------------------------------------
// SQL5 predicate expressions.

int32_t SqlParser::allocExpr(SqlPredicateAst& arena, const SqlToken& token,
                             SqlError& error) {
    if (arena.nodes.size() >= kSqlMaxExprNodes) {
        error = makeError(SqlErrorCode::ResourceLimit,
                          "expression exceeds the SQL5 node limit", token);
        return -1;
    }
    SqlExprNode node;
    node.line = token.line;
    node.column = token.column;
    arena.nodes.push_back(node);
    return static_cast<int32_t>(arena.nodes.size() - 1);
}

bool SqlParser::parseOrExpr(SqlPredicateAst& arena, int32_t& out, SqlError& error) {
    int32_t left = -1;
    if (!parseAndExpr(arena, left, error)) {
        return false;
    }
    while (check(SqlTokenKind::Or)) {
        const SqlToken& token = peek();
        advance();
        int32_t right = -1;
        if (!parseAndExpr(arena, right, error)) {
            return false;
        }
        const int32_t index = allocExpr(arena, token, error);
        if (index < 0) {
            return false;
        }
        arena.nodes[index].kind = SqlExprKind::Or;
        arena.nodes[index].left = left;
        arena.nodes[index].right = right;
        left = index;
    }
    out = left;
    return true;
}

bool SqlParser::parseAndExpr(SqlPredicateAst& arena, int32_t& out, SqlError& error) {
    int32_t left = -1;
    if (!parseNotExpr(arena, left, error)) {
        return false;
    }
    while (check(SqlTokenKind::And)) {
        const SqlToken& token = peek();
        advance();
        int32_t right = -1;
        if (!parseNotExpr(arena, right, error)) {
            return false;
        }
        const int32_t index = allocExpr(arena, token, error);
        if (index < 0) {
            return false;
        }
        arena.nodes[index].kind = SqlExprKind::And;
        arena.nodes[index].left = left;
        arena.nodes[index].right = right;
        left = index;
    }
    out = left;
    return true;
}

bool SqlParser::parseNotExpr(SqlPredicateAst& arena, int32_t& out, SqlError& error) {
    if (check(SqlTokenKind::Not)) {
        const SqlToken token = peek();
        if (_depth >= kSqlMaxNestingDepth) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "expression nesting exceeds the SQL5 maximum", token);
            return false;
        }
        advance();
        ++_depth;
        int32_t operand = -1;
        const bool ok = parseNotExpr(arena, operand, error);
        --_depth;
        if (!ok) {
            return false;
        }
        const int32_t index = allocExpr(arena, token, error);
        if (index < 0) {
            return false;
        }
        arena.nodes[index].kind = SqlExprKind::Not;
        arena.nodes[index].left = operand;
        out = index;
        return true;
    }
    return parseComparison(arena, out, error);
}

bool SqlParser::parseComparison(SqlPredicateAst& arena, int32_t& out,
                                SqlError& error) {
    int32_t left = -1;
    if (!parsePrimary(arena, left, error)) {
        return false;
    }

    SqlCompareOp op = SqlCompareOp::Eq;
    if (comparisonToken(peek().kind, op)) {
        const SqlToken token = peek();
        advance();
        int32_t right = -1;
        if (!parsePrimary(arena, right, error)) {
            return false;
        }
        const int32_t index = allocExpr(arena, token, error);
        if (index < 0) {
            return false;
        }
        arena.nodes[index].kind = SqlExprKind::Compare;
        arena.nodes[index].compareOp = op;
        arena.nodes[index].left = left;
        arena.nodes[index].right = right;
        out = index;
        return true;
    }

    if (check(SqlTokenKind::Is)) {
        const SqlToken token = peek();
        advance();
        bool negated = false;
        if (match(SqlTokenKind::Not)) {
            negated = true;
        }
        if (!expect(SqlTokenKind::Null, error, "NULL after IS")) {
            return false;
        }
        const int32_t index = allocExpr(arena, token, error);
        if (index < 0) {
            return false;
        }
        arena.nodes[index].kind = SqlExprKind::IsNull;
        arena.nodes[index].negated = negated;
        arena.nodes[index].left = left;
        out = index;
        return true;
    }

    out = left;
    return true;
}

bool SqlParser::parsePrimary(SqlPredicateAst& arena, int32_t& out, SqlError& error) {
    const SqlToken& token = peek();

    if (token.kind == SqlTokenKind::LeftParen) {
        if (_depth >= kSqlMaxNestingDepth) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "expression nesting exceeds the SQL5 maximum", token);
            return false;
        }
        advance();
        ++_depth;
        int32_t inner = -1;
        const bool ok = parseOrExpr(arena, inner, error);
        --_depth;
        if (!ok) {
            return false;
        }
        if (!expect(SqlTokenKind::RightParen, error, "')' after expression")) {
            return false;
        }
        out = inner;
        return true;
    }

    if (token.kind == SqlTokenKind::Identifier) {
        const int32_t index = allocExpr(arena, token, error);
        if (index < 0) {
            return false;
        }
        arena.nodes[index].kind = SqlExprKind::ColumnRef;
        arena.nodes[index].identifier.name = token.text;
        arena.nodes[index].identifier.line = token.line;
        arena.nodes[index].identifier.column = token.column;
        advance();
        out = index;
        return true;
    }

    SqlLiteralAst literal;
    if (!parseLiteral(literal, error)) {
        return false;
    }
    const int32_t index = allocExpr(arena, token, error);
    if (index < 0) {
        return false;
    }
    arena.nodes[index].kind = SqlExprKind::Literal;
    arena.nodes[index].literal = literal;
    out = index;
    return true;
}

bool SqlParser::parseWhereClause(SqlPredicateAst& out, SqlError& error) {
    out.nodes.clear();
    out.root = -1;
    out.present = false;
    if (!match(SqlTokenKind::Where)) {
        return true;
    }
    out.present = true;
    int32_t root = -1;
    if (!parseOrExpr(out, root, error)) {
        return false;
    }
    out.root = root;
    return true;
}

} // namespace db
} // namespace gxos
