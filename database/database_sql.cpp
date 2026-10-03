#include "database_sql.h"

#include <limits>

#include "database_format.h"
#include "database_heap.h"
#include "database_sql_parser.h"
#include "database_sql_tokenizer.h"
#include "database_transaction.h"

namespace gxos {
namespace db {

namespace {

const char* literalKindName(SqlLiteralKind kind) {
    switch (kind) {
    case SqlLiteralKind::Null: return "NULL";
    case SqlLiteralKind::Boolean: return "BOOLEAN";
    case SqlLiteralKind::Integer: return "INTEGER";
    case SqlLiteralKind::Float: return "FLOAT";
    case SqlLiteralKind::String: return "TEXT";
    case SqlLiteralKind::Blob: return "BLOB";
    }
    return "value";
}

SqlError makeSemantic(const std::string& message, uint32_t line, uint32_t column) {
    SqlError error;
    error.code = SqlErrorCode::SemanticError;
    error.message = message;
    error.line = line;
    error.column = column;
    error.hasLocation = (line != 0 || column != 0);
    return error;
}

bool coerceLiteral(const SqlLiteralAst& literal, const ColumnDefinition& column,
                   std::vector<DbValue>& out, SqlError& error) {
    if (literal.kind == SqlLiteralKind::Null) {
        if (!column.nullable) {
            error = makeSemantic("NULL assigned to NOT NULL column '" + column.name + "'",
                                 literal.line, literal.column);
            return false;
        }
        out.push_back(DbValue::null());
        return true;
    }

    switch (literal.kind) {
    case SqlLiteralKind::Boolean:
        if (column.type != DbType::Boolean) {
            break;
        }
        out.push_back(DbValue::boolean(literal.boolValue));
        return true;
    case SqlLiteralKind::Integer:
        if (column.type == DbType::Int32) {
            if (literal.int64Value <
                    static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
                literal.int64Value >
                    static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
                error = makeSemantic("integer literal out of range for Int32 column '" +
                                         column.name + "'",
                                     literal.line, literal.column);
                return false;
            }
            out.push_back(DbValue::int32(static_cast<int32_t>(literal.int64Value)));
            return true;
        }
        if (column.type == DbType::Int64) {
            out.push_back(DbValue::int64(literal.int64Value));
            return true;
        }
        if (column.type == DbType::Float64) {
            out.push_back(DbValue::float64(static_cast<double>(literal.int64Value)));
            return true;
        }
        break;
    case SqlLiteralKind::Float:
        if (column.type != DbType::Float64) {
            break;
        }
        out.push_back(DbValue::float64(literal.float64Value));
        return true;
    case SqlLiteralKind::String:
        if (column.type != DbType::Text) {
            break;
        }
        if (literal.textValue.size() > kMaxTextBytes) {
            error = makeSemantic("text value exceeds the maximum length for column '" +
                                     column.name + "'",
                                 literal.line, literal.column);
            return false;
        }
        out.push_back(DbValue::text(literal.textValue));
        return true;
    case SqlLiteralKind::Blob:
        if (column.type != DbType::Blob) {
            break;
        }
        if (literal.blobValue.size() > kMaxBlobBytes) {
            error = makeSemantic("blob value exceeds the maximum length for column '" +
                                     column.name + "'",
                                 literal.line, literal.column);
            return false;
        }
        out.push_back(DbValue::blob(literal.blobValue.empty() ? nullptr
                                                              : literal.blobValue.data(),
                                    literal.blobValue.size()));
        return true;
    case SqlLiteralKind::Null:
        break;
    }

    std::string message = "value type mismatch for column '";
    message += column.name;
    message += "': cannot assign ";
    message += literalKindName(literal.kind);
    message += " to ";
    message += dbTypeName(column.type);
    error = makeSemantic(message, literal.line, literal.column);
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Result types.

SqlResultSet::SqlResultSet() : _hasResult(false), _columns(), _rows(), _bytes(0) {}

void SqlResultSet::setColumns(const std::vector<SqlColumn>& columns) {
    _columns = columns;
    _hasResult = true;
}

bool SqlResultSet::addRow(const std::vector<DbValue>& row) {
    if (_rows.size() >= kSqlMaxResultRows) {
        return false;
    }
    size_t bytes = sizeof(DbValue) * row.size();
    for (size_t i = 0; i < row.size(); ++i) {
        if (row[i].type() == DbType::Text) {
            bytes += row[i].textValue().size();
        } else if (row[i].type() == DbType::Blob) {
            bytes += row[i].blobValue().size();
        }
    }
    if (_bytes + bytes > kSqlMaxResultBytes) {
        return false;
    }
    _rows.push_back(row);
    _bytes += bytes;
    return true;
}

void SqlResultSet::clear() {
    _hasResult = false;
    _columns.clear();
    _rows.clear();
    _bytes = 0;
}

SqlStatementResult::SqlStatementResult()
    : type(SqlStatementType::Unknown), ok(false), error(), affectedRows(0),
      objectName(), resultSet(), transactionActiveAfter(false),
      transactionIdAfter(0) {}

SqlExecutionResult::SqlExecutionResult()
    : ok(false), error(), statements(), failedStatementIndex(0), stoppedEarly(false),
      transactionActiveAfter(false) {}

// ---------------------------------------------------------------------------
// Engine.

SqlEngine::SqlEngine(Database& db) : _db(db), _tx() {}

SqlEngine::~SqlEngine() {
    if (_tx && _tx->isActive()) {
        _tx->rollback();
    }
}

uint64_t SqlEngine::transactionId() const { return _tx ? _tx->id() : 0; }

SqlErrorCode SqlEngine::codeForStatus(DbStatus status) {
    switch (status) {
    case DbStatus::AlreadyExists:
    case DbStatus::InvalidArgument:
        return SqlErrorCode::SemanticError;
    case DbStatus::TransactionAlreadyActive:
    case DbStatus::NoActiveTransaction:
    case DbStatus::CommitFailed:
    case DbStatus::RollbackFailed:
        return SqlErrorCode::TransactionError;
    default:
        return SqlErrorCode::ExecutionError;
    }
}

SqlError SqlEngine::errorFromResult(const DbResult& result, uint32_t line,
                                    uint32_t column) {
    SqlError error;
    error.code = codeForStatus(result.status());
    error.message = result.message().empty() ? std::string(dbStatusName(result.status()))
                                             : result.message();
    error.line = line;
    error.column = column;
    error.hasLocation = (line != 0 || column != 0);
    return error;
}

SqlExecutionResult SqlEngine::execute(const std::string& sql) {
    SqlExecutionResult result;
    result.ok = true;
    result.stoppedEarly = false;
    result.failedStatementIndex = 0;

    if (sql.size() > kSqlMaxInputBytes) {
        result.ok = false;
        result.stoppedEarly = true;
        result.error.code = SqlErrorCode::ResourceLimit;
        result.error.message = "SQL input exceeds the maximum allowed size";
        result.transactionActiveAfter = inTransaction();
        return result;
    }

    std::vector<SqlToken> tokens;
    SqlError error;
    SqlTokenizer tokenizer;
    if (!tokenizer.tokenize(sql, tokens, error)) {
        result.ok = false;
        result.stoppedEarly = true;
        result.error = error;
        result.transactionActiveAfter = inTransaction();
        return result;
    }

    SqlParser parser(tokens);
    ExecContext ctx;
    bool done = false;
    size_t statementCount = 0;

    while (true) {
        SqlStatementAst statement;
        if (!parser.next(statement, error, done)) {
            SqlStatementResult failure;
            failure.ok = false;
            failure.error = error;
            failure.type = SqlStatementType::Unknown;
            failure.transactionActiveAfter = inTransaction();
            failure.transactionIdAfter = transactionId();
            result.failedStatementIndex = result.statements.size();
            result.statements.push_back(failure);
            result.ok = false;
            result.error = error;
            result.stoppedEarly = true;
            break;
        }
        if (done) {
            break;
        }
        if (statementCount >= kSqlMaxStatements) {
            SqlError limitError;
            limitError.code = SqlErrorCode::ResourceLimit;
            limitError.message = "statement count exceeds the SQL4 maximum";
            limitError.line = statement.line;
            limitError.column = statement.column;
            limitError.hasLocation = true;
            SqlStatementResult failure;
            failure.ok = false;
            failure.error = limitError;
            failure.type = statement.type;
            failure.transactionActiveAfter = inTransaction();
            failure.transactionIdAfter = transactionId();
            result.failedStatementIndex = result.statements.size();
            result.statements.push_back(failure);
            result.ok = false;
            result.error = limitError;
            result.stoppedEarly = true;
            break;
        }
        ++statementCount;

        SqlStatementResult statementResult;
        executeStatement(statement, ctx, statementResult);
        result.statements.push_back(statementResult);
        if (!statementResult.ok) {
            result.failedStatementIndex = result.statements.size() - 1;
            result.ok = false;
            result.error = statementResult.error;
            result.stoppedEarly = true;
            break;
        }
    }

    result.transactionActiveAfter = inTransaction();
    return result;
}

bool SqlEngine::executeStatement(const SqlStatementAst& stmt, ExecContext& ctx,
                                 SqlStatementResult& out) {
    out.type = stmt.type;
    out.ok = false;

    bool ok = false;
    switch (stmt.type) {
    case SqlStatementType::CreateTable:
        ok = executeCreateTable(stmt.createTable, out);
        break;
    case SqlStatementType::Insert:
        ok = executeInsert(stmt.insert, ctx, out);
        break;
    case SqlStatementType::Select:
        ok = executeSelect(stmt.select, ctx, out);
        break;
    case SqlStatementType::Begin:
        ctx.tables.clear();
        ok = executeBegin(out);
        break;
    case SqlStatementType::Commit:
        ctx.tables.clear();
        ok = executeCommit(out);
        break;
    case SqlStatementType::Rollback:
        ctx.tables.clear();
        ok = executeRollback(out);
        break;
    case SqlStatementType::Unknown:
        out.error = makeSemantic("unsupported statement", stmt.line, stmt.column);
        out.error.code = SqlErrorCode::Unsupported;
        ok = false;
        break;
    }

    if (!ok && !out.error.hasLocation) {
        out.error.line = stmt.line;
        out.error.column = stmt.column;
        out.error.hasLocation = (stmt.line != 0 || stmt.column != 0);
    }
    out.ok = ok;
    out.transactionActiveAfter = inTransaction();
    out.transactionIdAfter = transactionId();
    return ok;
}

bool SqlEngine::executeCreateTable(const SqlCreateTableAst& ast,
                                   SqlStatementResult& out) {
    TableDefinition definition;
    definition.name = ast.table.name;
    for (size_t i = 0; i < ast.columns.size(); ++i) {
        definition.columns.push_back(ColumnDefinition(
            ast.columns[i].name.name, ast.columns[i].type, ast.columns[i].nullable));
    }

    uint32_t tableId = 0;
    DbResult result = _db.createTable(definition, tableId);
    if (!result.isOk()) {
        out.error = errorFromResult(result, ast.table.line, ast.table.column);
        return false;
    }
    out.objectName = definition.name;
    out.affectedRows = 0;
    return true;
}

bool SqlEngine::executeInsert(const SqlInsertAst& ast, ExecContext& ctx,
                              SqlStatementResult& out) {
    SqlError error;
    Table* table =
        openTableCached(ast.table.name, ctx, error, ast.table.line, ast.table.column);
    if (table == nullptr) {
        out.error = error;
        return false;
    }

    const std::vector<ColumnDefinition>& columns = table->columns();
    if (ast.values.size() != columns.size()) {
        std::string message = "wrong value count for table '";
        message += ast.table.name;
        message += "': expected ";
        message += std::to_string(static_cast<unsigned long long>(columns.size()));
        message += ", got ";
        message += std::to_string(static_cast<unsigned long long>(ast.values.size()));
        out.error = makeSemantic(message, ast.table.line, ast.table.column);
        return false;
    }

    std::vector<DbValue> values;
    values.reserve(columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
        if (!coerceLiteral(ast.values[i], columns[i], values, error)) {
            out.error = error;
            return false;
        }
    }

    DbResult result = table->insert(values);
    if (!result.isOk()) {
        out.error = errorFromResult(result, ast.table.line, ast.table.column);
        return false;
    }
    out.objectName = ast.table.name;
    out.affectedRows = 1;
    return true;
}

bool SqlEngine::executeSelect(const SqlSelectAst& ast, ExecContext& ctx,
                              SqlStatementResult& out) {
    SqlError error;
    Table* table =
        openTableCached(ast.table.name, ctx, error, ast.table.line, ast.table.column);
    if (table == nullptr) {
        out.error = error;
        return false;
    }

    const std::vector<ColumnDefinition>& columns = table->columns();
    std::vector<size_t> projection;
    std::vector<SqlColumn> resultColumns;

    if (ast.star) {
        for (size_t i = 0; i < columns.size(); ++i) {
            projection.push_back(i);
            resultColumns.push_back(SqlColumn(columns[i].name, columns[i].type));
        }
    } else {
        for (size_t i = 0; i < ast.columns.size(); ++i) {
            const SqlIdentifier& identifier = ast.columns[i];
            size_t found = columns.size();
            for (size_t j = 0; j < columns.size(); ++j) {
                if (columns[j].name == identifier.name) {
                    found = j;
                    break;
                }
            }
            if (found == columns.size()) {
                out.error = makeSemantic(
                    "unknown column '" + identifier.name + "' in table '" +
                        ast.table.name + "'",
                    identifier.line, identifier.column);
                return false;
            }
            projection.push_back(found);
            resultColumns.push_back(
                SqlColumn(columns[found].name, columns[found].type));
        }
    }

    std::unique_ptr<TableScan> scan;
    DbResult result = table->scanStart(scan);
    if (!result.isOk()) {
        out.error = errorFromResult(result, ast.table.line, ast.table.column);
        return false;
    }

    out.resultSet.setColumns(resultColumns);

    std::vector<DbValue> row;
    std::vector<DbValue> projected;
    while (scan->next(row)) {
        projected.clear();
        projected.reserve(projection.size());
        for (size_t i = 0; i < projection.size(); ++i) {
            projected.push_back(row[projection[i]]);
        }
        if (!out.resultSet.addRow(projected)) {
            out.error = makeSemantic(
                "result set exceeds the SQL4 materialization limit",
                ast.line, ast.column);
            out.error.code = SqlErrorCode::ResourceLimit;
            return false;
        }
    }
    if (!scan->status().isOk()) {
        out.error = errorFromResult(scan->status(), ast.table.line, ast.table.column);
        return false;
    }
    return true;
}

bool SqlEngine::executeBegin(SqlStatementResult& out) {
    if (_tx != nullptr) {
        out.error.code = SqlErrorCode::TransactionError;
        out.error.message = "a transaction is already active";
        return false;
    }
    std::unique_ptr<Transaction> transaction;
    DbResult result = _db.beginTransaction(transaction);
    if (!result.isOk()) {
        out.error = errorFromResult(result, 0, 0);
        return false;
    }
    _tx = std::move(transaction);
    return true;
}

bool SqlEngine::executeCommit(SqlStatementResult& out) {
    if (_tx == nullptr) {
        out.error.code = SqlErrorCode::TransactionError;
        out.error.message = "no active transaction to commit";
        return false;
    }
    DbResult result = _tx->commit();
    _tx.reset();
    if (!result.isOk()) {
        out.error = errorFromResult(result, 0, 0);
        return false;
    }
    return true;
}

bool SqlEngine::executeRollback(SqlStatementResult& out) {
    if (_tx == nullptr) {
        out.error.code = SqlErrorCode::TransactionError;
        out.error.message = "no active transaction to roll back";
        return false;
    }
    DbResult result = _tx->rollback();
    _tx.reset();
    if (!result.isOk()) {
        out.error = errorFromResult(result, 0, 0);
        return false;
    }
    return true;
}

Table* SqlEngine::openTableCached(const std::string& name, ExecContext& ctx,
                                  SqlError& error, uint32_t line, uint32_t column) {
    std::map<std::string, std::unique_ptr<Table> >::iterator it = ctx.tables.find(name);
    if (it != ctx.tables.end()) {
        return it->second.get();
    }
    std::unique_ptr<Table> table;
    DbResult result = _db.openTable(name, table);
    if (!result.isOk()) {
        if (result.status() == DbStatus::InvalidArgument) {
            error = makeSemantic("unknown table '" + name + "'", line, column);
        } else {
            error = errorFromResult(result, line, column);
        }
        return nullptr;
    }
    Table* raw = table.get();
    ctx.tables[name] = std::move(table);
    return raw;
}

} // namespace db
} // namespace gxos
