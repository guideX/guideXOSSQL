#include "database_sql.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "database_format.h"
#include "database_heap.h"
#include "database_sql_expr.h"
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

size_t valueBytes(const DbValue& value) {
    size_t bytes = sizeof(DbValue);
    if (value.type() == DbType::Text) {
        bytes += value.textValue().size();
    } else if (value.type() == DbType::Blob) {
        bytes += value.blobValue().size();
    }
    return bytes;
}

size_t rowBytes(const std::vector<DbValue>& row) {
    size_t bytes = 0;
    for (size_t i = 0; i < row.size(); ++i) {
        bytes += valueBytes(row[i]);
    }
    return bytes;
}

// Compares two ORDER BY key values. NULL sorts before every non-NULL value;
// callers reverse the result for DESC so NULL ends up last.
int compareOrderValues(const DbValue& a, const DbValue& b, DbType type) {
    const bool aNull = a.isNull();
    const bool bNull = b.isNull();
    if (aNull || bNull) {
        if (aNull && bNull) {
            return 0;
        }
        return aNull ? -1 : 1;
    }
    switch (type) {
    case DbType::Boolean: {
        const int va = a.booleanValue() ? 1 : 0;
        const int vb = b.booleanValue() ? 1 : 0;
        return va < vb ? -1 : (va > vb ? 1 : 0);
    }
    case DbType::Int32: {
        const int32_t va = a.int32Value();
        const int32_t vb = b.int32Value();
        return va < vb ? -1 : (va > vb ? 1 : 0);
    }
    case DbType::Int64: {
        const int64_t va = a.int64Value();
        const int64_t vb = b.int64Value();
        return va < vb ? -1 : (va > vb ? 1 : 0);
    }
    case DbType::Float64: {
        const double va = a.float64Value();
        const double vb = b.float64Value();
        return va < vb ? -1 : (va > vb ? 1 : 0);
    }
    case DbType::Text: {
        const std::string& sa = a.textValue();
        const std::string& sb = b.textValue();
        const size_t count = sa.size() < sb.size() ? sa.size() : sb.size();
        for (size_t i = 0; i < count; ++i) {
            const unsigned char ca = static_cast<unsigned char>(sa[i]);
            const unsigned char cb = static_cast<unsigned char>(sb[i]);
            if (ca != cb) {
                return ca < cb ? -1 : 1;
            }
        }
        if (sa.size() == sb.size()) {
            return 0;
        }
        return sa.size() < sb.size() ? -1 : 1;
    }
    default:
        return 0;
    }
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
    case DbStatus::TransactionTooLarge:
    case DbStatus::NoSpace:
        return SqlErrorCode::ResourceLimit;
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
    case SqlStatementType::Update:
        ok = executeUpdate(stmt.update, ctx, out);
        break;
    case SqlStatementType::Delete:
        ok = executeDelete(stmt.deleteStatement, ctx, out);
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

    // Bind the WHERE predicate against the durable schema before scanning.
    BoundPredicate predicate;
    if (!predicate.bind(ast.where.nodes, ast.where.present ? ast.where.root : -1,
                        columns, error)) {
        out.error = error;
        return false;
    }

    // Bind ORDER BY terms to column ordinals before scanning.
    struct OrderKey {
        size_t ordinal;
        DbType type;
        bool descending;
    };
    std::vector<OrderKey> orderKeys;
    for (size_t i = 0; i < ast.orderBy.size(); ++i) {
        const SqlOrderTermAst& term = ast.orderBy[i];
        size_t found = columns.size();
        for (size_t j = 0; j < columns.size(); ++j) {
            if (columns[j].name == term.column.name) {
                found = j;
                break;
            }
        }
        if (found == columns.size()) {
            out.error = makeSemantic(
                "unknown column '" + term.column.name + "' in ORDER BY",
                term.column.line, term.column.column);
            return false;
        }
        if (columns[found].type == DbType::Blob) {
            out.error = makeSemantic(
                "ORDER BY is not supported for Blob column '" + term.column.name + "'",
                term.column.line, term.column.column);
            return false;
        }
        OrderKey key;
        key.ordinal = found;
        key.type = columns[found].type;
        key.descending = term.descending;
        orderKeys.push_back(key);
    }

    std::unique_ptr<TableScan> scan;
    DbResult result = table->scanStart(scan);
    if (!result.isOk()) {
        out.error = errorFromResult(result, ast.table.line, ast.table.column);
        return false;
    }

    out.resultSet.setColumns(resultColumns);

    const uint64_t offset = ast.hasOffset ? ast.offset : 0;
    const uint64_t limit =
        ast.hasLimit ? ast.limit : std::numeric_limits<uint64_t>::max();

    std::vector<DbValue> row;
    std::vector<DbValue> projected;

    if (orderKeys.empty()) {
        // No ordering: stream, applying OFFSET then LIMIT, and stop scanning
        // once a bounded LIMIT has been satisfied.
        uint64_t skipped = 0;
        uint64_t emitted = 0;
        while (limit != 0 && scan->next(row)) {
            if (predicate.evaluate(row) != SqlTruth::True) {
                continue;
            }
            if (skipped < offset) {
                ++skipped;
                continue;
            }
            projected.clear();
            projected.reserve(projection.size());
            for (size_t i = 0; i < projection.size(); ++i) {
                projected.push_back(row[projection[i]]);
            }
            if (!out.resultSet.addRow(projected)) {
                out.error = makeSemantic(
                    "result set exceeds the SQL5 materialization limit", ast.line,
                    ast.column);
                out.error.code = SqlErrorCode::ResourceLimit;
                return false;
            }
            ++emitted;
            if (emitted >= limit) {
                break;
            }
        }
    } else {
        // ORDER BY: materialize the bounded qualifying set, sort, then apply
        // OFFSET/LIMIT.
        struct Materialized {
            std::vector<DbValue> projected;
            std::vector<DbValue> keys;
        };
        std::vector<Materialized> rows;
        size_t bytes = 0;
        while (scan->next(row)) {
            if (predicate.evaluate(row) != SqlTruth::True) {
                continue;
            }
            if (rows.size() >= kSqlMaxSortRows) {
                out.error = makeSemantic(
                    "ORDER BY working set exceeds the SQL5 row limit", ast.line,
                    ast.column);
                out.error.code = SqlErrorCode::ResourceLimit;
                return false;
            }
            Materialized item;
            item.projected.reserve(projection.size());
            for (size_t i = 0; i < projection.size(); ++i) {
                item.projected.push_back(row[projection[i]]);
            }
            item.keys.reserve(orderKeys.size());
            for (size_t i = 0; i < orderKeys.size(); ++i) {
                item.keys.push_back(row[orderKeys[i].ordinal]);
            }
            bytes += rowBytes(item.projected) + rowBytes(item.keys);
            if (bytes > kSqlMaxSortBytes) {
                out.error = makeSemantic(
                    "ORDER BY working set exceeds the SQL5 byte limit", ast.line,
                    ast.column);
                out.error.code = SqlErrorCode::ResourceLimit;
                return false;
            }
            rows.push_back(std::move(item));
        }

        std::stable_sort(
            rows.begin(), rows.end(),
            [&orderKeys](const Materialized& a, const Materialized& b) {
                for (size_t i = 0; i < orderKeys.size(); ++i) {
                    int cmp = compareOrderValues(a.keys[i], b.keys[i], orderKeys[i].type);
                    if (orderKeys[i].descending) {
                        cmp = -cmp;
                    }
                    if (cmp != 0) {
                        return cmp < 0;
                    }
                }
                return false;
            });

        const uint64_t start =
            offset < static_cast<uint64_t>(rows.size()) ? offset
                                                        : static_cast<uint64_t>(rows.size());
        uint64_t end = static_cast<uint64_t>(rows.size());
        if (limit != std::numeric_limits<uint64_t>::max()) {
            const uint64_t remaining = end - start;
            if (limit < remaining) {
                end = start + limit;
            }
        }
        for (uint64_t i = start; i < end; ++i) {
            if (!out.resultSet.addRow(rows[static_cast<size_t>(i)].projected)) {
                out.error = makeSemantic(
                    "result set exceeds the SQL5 materialization limit", ast.line,
                    ast.column);
                out.error.code = SqlErrorCode::ResourceLimit;
                return false;
            }
        }
    }

    if (!scan->status().isOk()) {
        out.error = errorFromResult(scan->status(), ast.table.line, ast.table.column);
        return false;
    }
    return true;
}

bool SqlEngine::applyMutationPlan(Table* table, ExecContext& ctx,
                                  const std::vector<RowMutation>& plan,
                                  SqlStatementResult& out, uint32_t line,
                                  uint32_t column) {
    const bool explicitTransaction = (_tx != nullptr);
    TransactionSavepoint savepoint;
    if (explicitTransaction) {
        _tx->beginStatement(savepoint);
    }

    DbResult result = table->applyMutations(plan);
    if (!result.isOk()) {
        if (explicitTransaction) {
            _tx->rollbackStatement(savepoint);
            // Cached Table objects may reference pages that no longer exist
            // after the overlay was restored.
            ctx.tables.clear();
        }
        out.error = errorFromResult(result, line, column);
        return false;
    }

    if (explicitTransaction) {
        _tx->releaseStatement(savepoint);
    }
    return true;
}

bool SqlEngine::executeUpdate(const SqlUpdateAst& ast, ExecContext& ctx,
                              SqlStatementResult& out) {
    SqlError error;
    Table* table =
        openTableCached(ast.table.name, ctx, error, ast.table.line, ast.table.column);
    if (table == nullptr) {
        out.error = error;
        return false;
    }
    const std::vector<ColumnDefinition>& columns = table->columns();

    // Statement-level semantic validation before any row is touched.
    std::vector<size_t> assignmentOrdinals;
    std::vector<DbValue> assignmentValues;
    for (size_t i = 0; i < ast.assignments.size(); ++i) {
        const SqlAssignmentAst& assignment = ast.assignments[i];
        size_t found = columns.size();
        for (size_t j = 0; j < columns.size(); ++j) {
            if (columns[j].name == assignment.column.name) {
                found = j;
                break;
            }
        }
        if (found == columns.size()) {
            out.error = makeSemantic(
                "unknown column '" + assignment.column.name + "' in UPDATE",
                assignment.column.line, assignment.column.column);
            return false;
        }
        for (size_t k = 0; k < assignmentOrdinals.size(); ++k) {
            if (assignmentOrdinals[k] == found) {
                out.error = makeSemantic(
                    "column '" + assignment.column.name +
                        "' is assigned more than once",
                    assignment.column.line, assignment.column.column);
                return false;
            }
        }
        std::vector<DbValue> coerced;
        if (!coerceLiteral(assignment.value, columns[found], coerced, error)) {
            out.error = error;
            return false;
        }
        assignmentOrdinals.push_back(found);
        assignmentValues.push_back(coerced[0]);
    }

    BoundPredicate predicate;
    if (!predicate.bind(ast.where.nodes, ast.where.present ? ast.where.root : -1,
                        columns, error)) {
        out.error = error;
        return false;
    }

    std::unique_ptr<TableScan> scan;
    DbResult result = table->scanStart(scan);
    if (!result.isOk()) {
        out.error = errorFromResult(result, ast.table.line, ast.table.column);
        return false;
    }

    std::vector<RowMutation> plan;
    size_t planBytes = 0;
    std::vector<DbValue> row;
    while (scan->next(row)) {
        if (predicate.evaluate(row) != SqlTruth::True) {
            continue;
        }
        if (plan.size() >= kSqlMaxMutationTargets) {
            out.error = makeSemantic(
                "UPDATE target set exceeds the SQL5 row limit", ast.line, ast.column);
            out.error.code = SqlErrorCode::ResourceLimit;
            return false;
        }
        RowMutation mutation;
        mutation.locator =
            RowLocator(scan->currentPageId(), scan->currentSlotIndex());
        mutation.deleted = false;
        mutation.values = row;
        for (size_t i = 0; i < assignmentOrdinals.size(); ++i) {
            mutation.values[assignmentOrdinals[i]] = assignmentValues[i];
        }
        planBytes += rowBytes(mutation.values);
        if (planBytes > kSqlMaxMutationBytes) {
            out.error = makeSemantic(
                "UPDATE plan exceeds the SQL5 byte limit", ast.line, ast.column);
            out.error.code = SqlErrorCode::ResourceLimit;
            return false;
        }
        plan.push_back(std::move(mutation));
    }
    if (!scan->status().isOk()) {
        out.error = errorFromResult(scan->status(), ast.table.line, ast.table.column);
        return false;
    }

    out.objectName = ast.table.name;
    if (plan.empty()) {
        out.affectedRows = 0;
        return true;
    }
    if (!applyMutationPlan(table, ctx, plan, out, ast.table.line, ast.table.column)) {
        return false;
    }
    out.affectedRows = plan.size();
    return true;
}

bool SqlEngine::executeDelete(const SqlDeleteAst& ast, ExecContext& ctx,
                              SqlStatementResult& out) {
    SqlError error;
    Table* table =
        openTableCached(ast.table.name, ctx, error, ast.table.line, ast.table.column);
    if (table == nullptr) {
        out.error = error;
        return false;
    }
    const std::vector<ColumnDefinition>& columns = table->columns();

    BoundPredicate predicate;
    if (!predicate.bind(ast.where.nodes, ast.where.present ? ast.where.root : -1,
                        columns, error)) {
        out.error = error;
        return false;
    }

    std::unique_ptr<TableScan> scan;
    DbResult result = table->scanStart(scan);
    if (!result.isOk()) {
        out.error = errorFromResult(result, ast.table.line, ast.table.column);
        return false;
    }

    std::vector<RowMutation> plan;
    std::vector<DbValue> row;
    while (scan->next(row)) {
        if (predicate.evaluate(row) != SqlTruth::True) {
            continue;
        }
        if (plan.size() >= kSqlMaxMutationTargets) {
            out.error = makeSemantic(
                "DELETE target set exceeds the SQL5 row limit", ast.line, ast.column);
            out.error.code = SqlErrorCode::ResourceLimit;
            return false;
        }
        RowMutation mutation;
        mutation.locator =
            RowLocator(scan->currentPageId(), scan->currentSlotIndex());
        mutation.deleted = true;
        plan.push_back(mutation);
    }
    if (!scan->status().isOk()) {
        out.error = errorFromResult(scan->status(), ast.table.line, ast.table.column);
        return false;
    }

    out.objectName = ast.table.name;
    if (plan.empty()) {
        out.affectedRows = 0;
        return true;
    }
    if (!applyMutationPlan(table, ctx, plan, out, ast.table.line, ast.table.column)) {
        return false;
    }
    out.affectedRows = plan.size();
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
