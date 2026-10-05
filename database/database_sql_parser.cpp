#include "database_sql_parser.h"

namespace gxos {
namespace db {

const char* sqlStatementTypeName(SqlStatementType type) {
    switch (type) {
    case SqlStatementType::CreateTable: return "CREATE TABLE";
    case SqlStatementType::CreateIndex: return "CREATE INDEX";
    case SqlStatementType::Insert: return "INSERT";
    case SqlStatementType::Select: return "SELECT";
    case SqlStatementType::Update: return "UPDATE";
    case SqlStatementType::Delete: return "DELETE";
    case SqlStatementType::Begin: return "BEGIN";
    case SqlStatementType::Commit: return "COMMIT";
    case SqlStatementType::Rollback: return "ROLLBACK";
    case SqlStatementType::DropIndex: return "DROP INDEX";
    case SqlStatementType::DropTable: return "DROP TABLE";
    case SqlStatementType::AlterTableAddColumn: return "ALTER TABLE ADD COLUMN";
    case SqlStatementType::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* sqlJoinTypeName(SqlJoinType type) {
    switch (type) {
    case SqlJoinType::Inner: return "INNER JOIN";
    case SqlJoinType::Left: return "LEFT JOIN";
    }
    return "JOIN";
}

const char* sqlAggregateKindName(SqlAggregateKind kind) {
    switch (kind) {
    case SqlAggregateKind::Count: return "COUNT";
    case SqlAggregateKind::Sum: return "SUM";
    case SqlAggregateKind::Avg: return "AVG";
    case SqlAggregateKind::Min: return "MIN";
    case SqlAggregateKind::Max: return "MAX";
    }
    return "AGGREGATE";
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

// Words that are not SQL7 keywords but must never be swallowed as a bare table
// alias, so unsupported join forms (RIGHT/FULL/CROSS JOIN) are rejected instead
// of being reinterpreted as `FROM A AS RIGHT JOIN B`.
bool isReservedAliasWord(const std::string& text) {
    if (text.size() != 4 && text.size() != 5) {
        return false;
    }
    std::string upper;
    upper.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        upper.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return upper == "RIGHT" || upper == "FULL" || upper == "CROSS";
}

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
    case SqlTokenKind::Create: {
        const SqlTokenKind next = peek(1).kind;
        if (next == SqlTokenKind::Index || next == SqlTokenKind::Unique) {
            out.type = SqlStatementType::CreateIndex;
            return parseCreateIndex(out, error);
        }
        out.type = SqlStatementType::CreateTable;
        return parseCreateTable(out, error);
    }
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
    case SqlTokenKind::Drop: {
        const SqlTokenKind next = peek(1).kind;
        if (next == SqlTokenKind::Index) {
            out.type = SqlStatementType::DropIndex;
            return parseDropIndex(out, error);
        }
        if (next == SqlTokenKind::Table) {
            out.type = SqlStatementType::DropTable;
            return parseDropTable(out, error);
        }
        std::string message = "expected INDEX or TABLE after DROP, found ";
        message += sqlTokenKindName(next);
        error = makeError(SqlErrorCode::SyntaxError, message, peek(1));
        return false;
    }
    case SqlTokenKind::Alter:
        out.type = SqlStatementType::AlterTableAddColumn;
        return parseAlterTableAddColumn(out, error);
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
        if (check(SqlTokenKind::Foreign)) {
            break;
        }
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

    // SQL8: table-level FOREIGN KEY constraints.
    while (match(SqlTokenKind::Foreign)) {
        if (!expect(SqlTokenKind::Key, error, "KEY after FOREIGN")) {
            return false;
        }
        if (!expect(SqlTokenKind::LeftParen, error, "'(' after FOREIGN KEY")) {
            return false;
        }
        SqlForeignKeyAst fk;
        fk.line = peek().line;
        fk.column = peek().column;
        if (!parseIdentifier(fk.childColumn, error, "child column name")) {
            return false;
        }
        if (check(SqlTokenKind::Comma)) {
            error = makeError(SqlErrorCode::Unsupported,
                              "composite foreign keys are not supported", peek());
            return false;
        }
        if (!expect(SqlTokenKind::RightParen, error, "')' after child column")) {
            return false;
        }
        if (!expect(SqlTokenKind::References, error, "REFERENCES after child column")) {
            return false;
        }
        if (!parseIdentifier(fk.parentTable, error, "parent table name")) {
            return false;
        }
        if (!expect(SqlTokenKind::LeftParen, error, "'(' after parent table name")) {
            return false;
        }
        if (!parseIdentifier(fk.parentColumn, error, "parent column name")) {
            return false;
        }
        if (check(SqlTokenKind::Comma)) {
            error = makeError(SqlErrorCode::Unsupported,
                              "composite foreign keys are not supported", peek());
            return false;
        }
        if (!expect(SqlTokenKind::RightParen, error, "')' after parent column")) {
            return false;
        }
        // Reject unsupported referential actions.
        if (check(SqlTokenKind::On)) {
            error = makeError(SqlErrorCode::Unsupported,
                              "ON DELETE/UPDATE referential actions are not supported", peek());
            return false;
        }
        out.createTable.foreignKeys.push_back(fk);
        if (!match(SqlTokenKind::Comma)) {
            break;
        }
    }

    if (!expect(SqlTokenKind::RightParen, error, "')' after column list")) {
        return false;
    }
    return true;
}

bool SqlParser::parseColumnDef(SqlColumnDefAst& out, SqlError& error) {
    out.primaryKey = false;
    out.unique = false;
    out.hasDefault = false;
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
    bool explicitNull = false;
    if (match(SqlTokenKind::Not)) {
        if (!expect(SqlTokenKind::Null, error, "NULL after NOT")) {
            return false;
        }
        out.nullable = false;
    } else if (match(SqlTokenKind::Null)) {
        out.nullable = true;
        explicitNull = true;
    }

    // SQL6 column constraints. PRIMARY KEY implies NOT NULL and rejects a
    // contradictory explicit NULL declaration.
    for (;;) {
        if (match(SqlTokenKind::Primary)) {
            if (!expect(SqlTokenKind::Key, error, "KEY after PRIMARY")) {
                return false;
            }
            if (out.primaryKey) {
                error = makeError(SqlErrorCode::SemanticError,
                                  "duplicate PRIMARY KEY constraint", peek());
                return false;
            }
            if (explicitNull) {
                error = makeError(SqlErrorCode::SemanticError,
                                  "PRIMARY KEY column cannot be declared NULL", peek());
                return false;
            }
            out.primaryKey = true;
            out.nullable = false;
        } else if (match(SqlTokenKind::Unique)) {
            out.unique = true;
        } else if (match(SqlTokenKind::Default)) {
            if (out.hasDefault) {
                error = makeError(SqlErrorCode::SemanticError,
                                  "duplicate DEFAULT constraint", peek());
                return false;
            }
            if (!parseLiteral(out.defaultValue, error)) {
                return false;
            }
            out.hasDefault = true;
        } else {
            break;
        }
    }

    // SQL8: NOT NULL + DEFAULT NULL is rejected.
    if (!out.nullable && out.hasDefault && out.defaultValue.kind == SqlLiteralKind::Null) {
        error = makeError(SqlErrorCode::SemanticError,
                          "NOT NULL column cannot have DEFAULT NULL", peek());
        return false;
    }
    return true;
}

bool SqlParser::parseCreateIndex(SqlStatementAst& out, SqlError& error) {
    advance(); // CREATE
    bool unique = false;
    if (match(SqlTokenKind::Unique)) {
        unique = true;
    }
    if (!expect(SqlTokenKind::Index, error, "INDEX after CREATE")) {
        return false;
    }
    if (!parseIdentifier(out.createIndex.index, error, "index name")) {
        return false;
    }
    if (!expect(SqlTokenKind::On, error, "ON after index name")) {
        return false;
    }
    if (!parseIdentifier(out.createIndex.table, error, "table name")) {
        return false;
    }
    if (!expect(SqlTokenKind::LeftParen, error, "'(' after table name")) {
        return false;
    }
    if (!parseIdentifier(out.createIndex.columnName, error, "column name")) {
        return false;
    }
    if (check(SqlTokenKind::Comma)) {
        error = makeError(SqlErrorCode::Unsupported,
                          "composite indexes are not supported", peek());
        return false;
    }
    if (!expect(SqlTokenKind::RightParen, error, "')' after column name")) {
        return false;
    }
    out.createIndex.unique = unique;
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

    // SQL8: optional column list. A '(' immediately after the table name can
    // only be a column list, because the no-column-list form is followed by the
    // VALUES keyword.
    if (check(SqlTokenKind::LeftParen)) {
        advance(); // (
        out.insert.hasColumnList = true;
        SqlIdentifier col;
        if (!parseIdentifier(col, error, "column name")) {
            return false;
        }
        out.insert.columnList.push_back(col);
        while (match(SqlTokenKind::Comma)) {
            if (out.insert.columnList.size() >= kSqlMaxColumnsPerCreate) {
                error = makeError(SqlErrorCode::ResourceLimit,
                                  "column list count exceeds the SQL maximum", peek());
                return false;
            }
            if (!parseIdentifier(col, error, "column name")) {
                return false;
            }
            out.insert.columnList.push_back(col);
        }
        if (!expect(SqlTokenKind::RightParen, error, "')' after column list")) {
            return false;
        }
    }
    if (!expect(SqlTokenKind::Values, error, "VALUES after table name")) {
        return false;
    }
    if (!expect(SqlTokenKind::LeftParen, error, "'(' after VALUES")) {
        return false;
    }
    SqlLiteralAst literal;
    if (!parseInsertValue(literal, error)) {
        return false;
    }
    out.insert.values.push_back(literal);
    while (match(SqlTokenKind::Comma)) {
        if (out.insert.values.size() >= kSqlMaxValuesPerInsert) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "value count exceeds the SQL maximum", peek());
            return false;
        }
        if (!parseInsertValue(literal, error)) {
            return false;
        }
        out.insert.values.push_back(literal);
    }
    if (!expect(SqlTokenKind::RightParen, error, "')' after value list")) {
        return false;
    }
    return true;
}

bool SqlParser::parseInsertValue(SqlLiteralAst& out, SqlError& error) {
    if (check(SqlTokenKind::Default)) {
        const SqlToken& token = peek();
        out.kind = SqlLiteralKind::Null;
        out.isDefault = true;
        out.line = token.line;
        out.column = token.column;
        advance();
        return true;
    }
    return parseLiteral(out, error);
}

bool SqlParser::parseSelect(SqlStatementAst& out, SqlError& error) {
    advance(); // SELECT
    if (match(SqlTokenKind::Distinct)) {
        out.select.distinct = true;
    }
    if (!parseSelectList(out.select, error)) {
        return false;
    }
    if (!expect(SqlTokenKind::From, error, "FROM after projection")) {
        return false;
    }
    if (!parseTableRef(out.select.from, error)) {
        return false;
    }
    // The legacy base-table view mirrors the FROM relation name.
    out.select.table = out.select.from.table;
    if (!parseJoinClauses(out.select, error)) {
        return false;
    }
    if (!parseWhereClause(out.select.where, error)) {
        return false;
    }
    if (match(SqlTokenKind::Group)) {
        if (!expect(SqlTokenKind::By, error, "BY after GROUP")) {
            return false;
        }
        if (!parseGroupBy(out.select.groupBy, error)) {
            return false;
        }
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

bool SqlParser::parseSelectList(SqlSelectAst& out, SqlError& error) {
    if (match(SqlTokenKind::Star)) {
        out.star = true;
        return true;
    }
    for (;;) {
        if (out.projection.size() >= kSqlMaxSelectColumns) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "projection exceeds the SQL maximum", peek());
            return false;
        }
        SqlProjectionItemAst item;
        if (!parseProjectionItem(item, error)) {
            return false;
        }
        if (item.kind == SqlProjectionKind::Column) {
            out.columns.push_back(item.columnRef);
        }
        out.projection.push_back(item);
        if (!match(SqlTokenKind::Comma)) {
            break;
        }
    }
    return true;
}

bool SqlParser::parseProjectionItem(SqlProjectionItemAst& out, SqlError& error) {
    const SqlToken& token = peek();
    out.line = token.line;
    out.column = token.column;
    switch (token.kind) {
    case SqlTokenKind::Count:
    case SqlTokenKind::Sum:
    case SqlTokenKind::Avg:
    case SqlTokenKind::Min:
    case SqlTokenKind::Max:
        return parseAggregate(out, error);
    default:
        break;
    }
    out.kind = SqlProjectionKind::Column;
    if (!parseColumnRef(out.columnRef, error, "column name or '*'")) {
        return false;
    }
    return parseProjectionAlias(out, error);
}

bool SqlParser::parseAggregate(SqlProjectionItemAst& out, SqlError& error) {
    const SqlToken& token = peek();
    out.line = token.line;
    out.column = token.column;
    switch (token.kind) {
    case SqlTokenKind::Count: out.aggregate.kind = SqlAggregateKind::Count; break;
    case SqlTokenKind::Sum: out.aggregate.kind = SqlAggregateKind::Sum; break;
    case SqlTokenKind::Avg: out.aggregate.kind = SqlAggregateKind::Avg; break;
    case SqlTokenKind::Min: out.aggregate.kind = SqlAggregateKind::Min; break;
    case SqlTokenKind::Max: out.aggregate.kind = SqlAggregateKind::Max; break;
    default:
        error = makeError(SqlErrorCode::SyntaxError,
                          "expected an aggregate function", token);
        return false;
    }
    advance();
    if (!expect(SqlTokenKind::LeftParen, error, "'(' after aggregate name")) {
        return false;
    }
    if (out.aggregate.kind == SqlAggregateKind::Count &&
        match(SqlTokenKind::Star)) {
        out.aggregate.star = true;
    } else {
        if (!parseColumnRef(out.aggregate.column, error, "aggregate column name")) {
            return false;
        }
    }
    if (!expect(SqlTokenKind::RightParen, error, "')' after aggregate argument")) {
        return false;
    }
    out.kind = SqlProjectionKind::Aggregate;
    return parseProjectionAlias(out, error);
}

bool SqlParser::parseProjectionAlias(SqlProjectionItemAst& out, SqlError& error) {
    if (match(SqlTokenKind::As)) {
        if (!parseIdentifier(out.alias, error, "projection alias")) {
            return false;
        }
        out.hasAlias = true;
        return true;
    }
    // A bare trailing identifier is a projection alias when one is present.
    if (check(SqlTokenKind::Identifier)) {
        if (!parseIdentifier(out.alias, error, "projection alias")) {
            return false;
        }
        out.hasAlias = true;
    }
    return true;
}

bool SqlParser::parseTableRef(SqlTableRefAst& out, SqlError& error) {
    if (!parseIdentifier(out.table, error, "table name")) {
        return false;
    }
    if (match(SqlTokenKind::As)) {
        if (!parseIdentifier(out.alias, error, "table alias")) {
            return false;
        }
        out.hasAlias = true;
        return true;
    }
    // `FROM Users u` is unambiguous: only an identifier can follow a table name
    // where a join, WHERE, GROUP, ORDER, LIMIT, OFFSET or terminator may appear.
    // Reserved words for unsupported join forms are never taken as aliases.
    if (check(SqlTokenKind::Identifier) && !isReservedAliasWord(peek().text)) {
        if (!parseIdentifier(out.alias, error, "table alias")) {
            return false;
        }
        out.hasAlias = true;
    }
    return true;
}

bool SqlParser::parseJoinClauses(SqlSelectAst& out, SqlError& error) {
    for (;;) {
        SqlJoinAst join;
        const SqlToken& token = peek();
        join.line = token.line;
        join.column = token.column;
        if (check(SqlTokenKind::Join)) {
            advance();
            join.type = SqlJoinType::Inner;
        } else if (check(SqlTokenKind::Inner)) {
            advance();
            if (!expect(SqlTokenKind::Join, error, "JOIN after INNER")) {
                return false;
            }
            join.type = SqlJoinType::Inner;
        } else if (check(SqlTokenKind::Left)) {
            advance();
            match(SqlTokenKind::Outer);
            if (!expect(SqlTokenKind::Join, error, "JOIN after LEFT")) {
                return false;
            }
            join.type = SqlJoinType::Left;
        } else {
            break;
        }
        if (out.joins.size() + 1 >= kSqlMaxJoinSources) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "relation source count exceeds the SQL7 maximum", token);
            return false;
        }
        if (!parseTableRef(join.table, error)) {
            return false;
        }
        if (!expect(SqlTokenKind::On, error, "ON after joined relation")) {
            return false;
        }
        join.on.nodes.clear();
        join.on.root = -1;
        join.on.present = true;
        int32_t root = -1;
        if (!parseOrExpr(join.on, root, error)) {
            return false;
        }
        join.on.root = root;
        out.joins.push_back(join);
    }
    return true;
}

bool SqlParser::parseGroupBy(std::vector<SqlIdentifier>& out, SqlError& error) {
    for (;;) {
        if (out.size() >= kSqlMaxGroupByColumns) {
            error = makeError(SqlErrorCode::ResourceLimit,
                              "GROUP BY term count exceeds the SQL7 maximum", peek());
            return false;
        }
        SqlIdentifier identifier;
        if (!parseColumnRef(identifier, error, "column name")) {
            return false;
        }
        out.push_back(identifier);
        if (!match(SqlTokenKind::Comma)) {
            break;
        }
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

bool SqlParser::parseDropIndex(SqlStatementAst& out, SqlError& error) {
    advance(); // DROP
    if (!expect(SqlTokenKind::Index, error, "INDEX after DROP")) {
        return false;
    }
    if (!parseIdentifier(out.dropIndex.index, error, "index name")) {
        return false;
    }
    return true;
}

bool SqlParser::parseDropTable(SqlStatementAst& out, SqlError& error) {
    advance(); // DROP
    if (!expect(SqlTokenKind::Table, error, "TABLE after DROP")) {
        return false;
    }
    if (!parseIdentifier(out.dropTable.table, error, "table name")) {
        return false;
    }
    return true;
}

bool SqlParser::parseAlterTableAddColumn(SqlStatementAst& out, SqlError& error) {
    advance(); // ALTER
    if (!expect(SqlTokenKind::Table, error, "TABLE after ALTER")) {
        return false;
    }
    if (!parseIdentifier(out.alterTableAddColumn.table, error, "table name")) {
        return false;
    }
    if (!expect(SqlTokenKind::Add, error, "ADD after table name")) {
        return false;
    }
    if (!expect(SqlTokenKind::Column, error, "COLUMN after ADD")) {
        return false;
    }
    if (!parseColumnDef(out.alterTableAddColumn.columnDef, error)) {
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
    if (!parseColumnRef(term.column, error, "column name")) {
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
        if (!parseColumnRef(next.column, error, "column name")) {
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
    out.isDefault = false;
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
    out.qualifier.clear();
    out.hasQualifier = false;
    out.line = token.line;
    out.column = token.column;
    advance();
    return true;
}

bool SqlParser::parseColumnRef(SqlIdentifier& out, SqlError& error,
                               const char* what) {
    if (!parseIdentifier(out, error, what)) {
        return false;
    }
    if (match(SqlTokenKind::Dot)) {
        SqlIdentifier column;
        if (!parseIdentifier(column, error, "column name after '.'")) {
            return false;
        }
        out.hasQualifier = true;
        out.qualifier = out.name;
        out.name = column.name;
        out.line = column.line;
        out.column = column.column;
    }
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
        SqlIdentifier identifier;
        if (!parseColumnRef(identifier, error, "column name")) {
            return false;
        }
        arena.nodes[index].kind = SqlExprKind::ColumnRef;
        arena.nodes[index].identifier = identifier;
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
