#pragma once
// guideXOS SQL -- Phase SQL4
// Public SQL execution facade.
//
//   SQL text -> SqlTokenizer -> SqlParser -> AST -> SqlEngine -> Database
//
// SqlEngine is a thin language/execution layer: it validates semantics against
// the durable catalog and then delegates every mutation to the existing
// relational and transaction APIs. It never manipulates pages, heap layout,
// catalog serialization or WAL records directly.
//
// An SqlEngine instance may span multiple execute() calls so an explicit
// BEGIN ... COMMIT can be issued across calls. It must not outlive the
// Database it was constructed with.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "database_relational.h"
#include "database_result.h"
#include "database_sql_ast.h"
#include "database_sql_token.h"
#include "database_types.h"

namespace gxos {
namespace db {

class Table;
class Transaction;

// One output column of a SELECT result.
struct SqlColumn {
    std::string name;
    DbType type;

    SqlColumn() : type(DbType::Unknown) {}
    SqlColumn(const std::string& nameIn, DbType typeIn)
        : name(nameIn), type(typeIn) {}
};

// A materialized SELECT result. SQL4 materializes results with explicit bounds
// (kSqlMaxResultRows / kSqlMaxResultBytes) rather than streaming; this keeps
// multi-statement result ownership simple. Future phases can replace it with a
// cursor without changing the engine facade.
class SqlResultSet {
public:
    SqlResultSet();

    bool hasResult() const { return _hasResult; }
    size_t columnCount() const { return _columns.size(); }
    size_t rowCount() const { return _rows.size(); }

    const SqlColumn& column(size_t index) const { return _columns[index]; }
    const std::vector<SqlColumn>& columns() const { return _columns; }
    const std::vector<DbValue>& row(size_t index) const { return _rows[index]; }
    const std::vector<std::vector<DbValue> >& rows() const { return _rows; }

    const DbValue& value(size_t rowIndex, size_t columnIndex) const {
        return _rows[rowIndex][columnIndex];
    }
    bool isNull(size_t rowIndex, size_t columnIndex) const {
        return _rows[rowIndex][columnIndex].isNull();
    }

    void setColumns(const std::vector<SqlColumn>& columns);

    // Appends a row. Returns false (and appends nothing) when the row would
    // exceed the SQL4 materialization bounds.
    bool addRow(const std::vector<DbValue>& row);

    void clear();

private:
    bool _hasResult;
    std::vector<SqlColumn> _columns;
    std::vector<std::vector<DbValue> > _rows;
    size_t _bytes;
};

// The structured result of one statement.
struct SqlStatementResult {
    SqlStatementType type;
    bool ok;
    SqlError error;
    uint64_t affectedRows;
    std::string objectName;
    SqlResultSet resultSet;
    bool transactionActiveAfter;
    uint64_t transactionIdAfter;

    SqlStatementResult();
};

// The structured result of one execute() call (possibly many statements).
struct SqlExecutionResult {
    bool ok;
    SqlError error;
    std::vector<SqlStatementResult> statements;
    size_t failedStatementIndex;
    bool stoppedEarly;
    bool transactionActiveAfter;

    SqlExecutionResult();
};

class SqlEngine {
public:
    explicit SqlEngine(Database& db);
    ~SqlEngine();

    SqlEngine(const SqlEngine&) = delete;
    SqlEngine& operator=(const SqlEngine&) = delete;

    // Tokenizes, parses and executes `sql`. Execution stops at the first
    // failure; results for all completed statements are returned. A tokenizer
    // error aborts the whole input before any statement executes. A parse or
    // execution error stops after the preceding statements have run.
    SqlExecutionResult execute(const std::string& sql);

    bool inTransaction() const { return _tx != nullptr; }
    uint64_t transactionId() const;

    Database& database() { return _db; }

private:
    struct ExecContext {
        std::map<std::string, std::unique_ptr<Table> > tables;
    };

    bool executeStatement(const SqlStatementAst& stmt, ExecContext& ctx,
                          SqlStatementResult& out);
    bool executeCreateTable(const SqlCreateTableAst& ast, SqlStatementResult& out);
    bool executeInsert(const SqlInsertAst& ast, ExecContext& ctx,
                       SqlStatementResult& out);
    bool executeSelect(const SqlSelectAst& ast, ExecContext& ctx,
                       SqlStatementResult& out);
    bool executeBegin(SqlStatementResult& out);
    bool executeCommit(SqlStatementResult& out);
    bool executeRollback(SqlStatementResult& out);

    Table* openTableCached(const std::string& name, ExecContext& ctx, SqlError& error,
                           uint32_t line, uint32_t column);

    static SqlErrorCode codeForStatus(DbStatus status);
    static SqlError errorFromResult(const DbResult& result, uint32_t line,
                                    uint32_t column);

    Database& _db;
    std::unique_ptr<Transaction> _tx;
};

} // namespace db
} // namespace gxos
