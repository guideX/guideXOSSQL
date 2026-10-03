#include "database_sql_parser.h"

namespace gxos {
namespace db {

const char* sqlStatementTypeName(SqlStatementType type) {
    switch (type) {
    case SqlStatementType::CreateTable: return "CREATE TABLE";
    case SqlStatementType::Insert: return "INSERT";
    case SqlStatementType::Select: return "SELECT";
    case SqlStatementType::Begin: return "BEGIN";
    case SqlStatementType::Commit: return "COMMIT";
    case SqlStatementType::Rollback: return "ROLLBACK";
    case SqlStatementType::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

SqlParser::SqlParser(const std::vector<SqlToken>& tokens)
    : _tokens(tokens), _index(0) {}

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
                              "column count exceeds the SQL4 maximum", peek());
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
                          "type length or precision declarations are not supported in "
                          "SQL4",
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
                              "value count exceeds the SQL4 maximum", peek());
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
                                  "projection exceeds the SQL4 maximum", peek());
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

} // namespace db
} // namespace gxos
