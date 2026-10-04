#include "database_sql.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "database_format.h"
#include "database_heap.h"
#include "database_index.h"
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

// ---------------------------------------------------------------------------
// SQL6 access-path selection and row sources.

struct AccessPathChoice {
    enum class Kind { FullScan, IndexLookup, IndexRange };
    Kind kind;
    const Catalog::IndexRecord* index;
    uint32_t columnOrdinal;
    SqlCompareOp op;
    DbValue value;
    int score;

    AccessPathChoice()
        : kind(Kind::FullScan), index(nullptr), columnOrdinal(0),
          op(SqlCompareOp::Eq), value(), score(0) {}
};

int compareOpScore(SqlCompareOp op) {
    return op == SqlCompareOp::Eq ? 2 : 1;
}

void considerIndexCandidate(const std::vector<SqlExprNode>& nodes, int32_t index,
                            const std::vector<ColumnDefinition>& columns,
                            Database& db, uint32_t tableId, AccessPathChoice& best) {
    if (index < 0 || static_cast<size_t>(index) >= nodes.size()) {
        return;
    }
    const SqlExprNode& node = nodes[static_cast<size_t>(index)];
    if (node.kind == SqlExprKind::And) {
        considerIndexCandidate(nodes, node.left, columns, db, tableId, best);
        considerIndexCandidate(nodes, node.right, columns, db, tableId, best);
        return;
    }
    if (node.kind != SqlExprKind::Compare || node.left < 0 || node.right < 0) {
        return;
    }
    const SqlExprNode& lhs = nodes[static_cast<size_t>(node.left)];
    const SqlExprNode& rhs = nodes[static_cast<size_t>(node.right)];
    const SqlExprNode* colNode = nullptr;
    const SqlExprNode* litNode = nullptr;
    SqlCompareOp op = node.compareOp;
    if (lhs.kind == SqlExprKind::ColumnRef && rhs.kind == SqlExprKind::Literal) {
        colNode = &lhs;
        litNode = &rhs;
    } else if (lhs.kind == SqlExprKind::Literal &&
               rhs.kind == SqlExprKind::ColumnRef) {
        colNode = &rhs;
        litNode = &lhs;
        switch (op) {
        case SqlCompareOp::Lt: op = SqlCompareOp::Gt; break;
        case SqlCompareOp::Le: op = SqlCompareOp::Ge; break;
        case SqlCompareOp::Gt: op = SqlCompareOp::Lt; break;
        case SqlCompareOp::Ge: op = SqlCompareOp::Le; break;
        default: break;
        }
    } else {
        return;
    }
    if (op == SqlCompareOp::Ne || litNode->literal.kind == SqlLiteralKind::Null) {
        return;
    }
    size_t ordinal = columns.size();
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == colNode->identifier.name) {
            ordinal = i;
            break;
        }
    }
    if (ordinal == columns.size() || !isIndexableType(columns[ordinal].type)) {
        return;
    }
    const Catalog::IndexRecord* idx =
        db.findIndexForColumn(tableId, static_cast<uint32_t>(ordinal));
    if (idx == nullptr) {
        return;
    }
    std::vector<DbValue> coerced;
    SqlError error;
    if (!coerceLiteral(litNode->literal, columns[ordinal], coerced, error) ||
        coerced.empty() || coerced[0].isNull()) {
        return;
    }
    std::vector<uint8_t> probe;
    if (!encodeIndexKey(columns[ordinal].type, coerced[0], probe)) {
        return;
    }
    const int score = compareOpScore(op);
    if (score > best.score) {
        best.kind = (op == SqlCompareOp::Eq) ? AccessPathChoice::Kind::IndexLookup
                                             : AccessPathChoice::Kind::IndexRange;
        best.index = idx;
        best.columnOrdinal = static_cast<uint32_t>(ordinal);
        best.op = op;
        best.value = coerced[0];
        best.score = score;
    }
}

AccessPathChoice selectAccessPath(const SqlPredicateAst& where,
                                  const std::vector<ColumnDefinition>& columns,
                                  Database& db, uint32_t tableId) {
    AccessPathChoice best;
    if (!where.present) {
        return best;
    }
    considerIndexCandidate(where.nodes, where.root, columns, db, tableId, best);
    return best;
}

// A uniform row source so SELECT can stream from either a heap scan or an
// index candidate list without changing predicate/projection/ordering code.
class RowSource {
public:
    virtual ~RowSource() {}
    virtual bool next(std::vector<DbValue>& row) = 0;
    virtual DbResult status() const = 0;
};

class ScanRowSource : public RowSource {
public:
    explicit ScanRowSource(std::unique_ptr<TableScan> scan) : _scan(std::move(scan)) {}
    bool next(std::vector<DbValue>& row) override { return _scan->next(row); }
    DbResult status() const override { return _scan->status(); }

private:
    std::unique_ptr<TableScan> _scan;
};

class LocatorRowSource : public RowSource {
public:
    LocatorRowSource(Table* table, const std::vector<RowLocator>& locators)
        : _table(table), _locators(locators), _index(0), _status(DbResult::ok()) {}
    bool next(std::vector<DbValue>& row) override {
        if (!_status.isOk() || _index >= _locators.size()) {
            return false;
        }
        DbResult result = _table->fetchRow(_locators[_index], row);
        ++_index;
        if (!result.isOk()) {
            _status = result;
            return false;
        }
        return true;
    }
    DbResult status() const override { return _status; }

private:
    Table* _table;
    std::vector<RowLocator> _locators;
    size_t _index;
    DbResult _status;
};

// ---------------------------------------------------------------------------
// SQL7 joined/aggregate query machinery.
//
// SQL7 executes a left-deep chain of relation sources in written order. A
// joined row is a flat vector of every source's decoded columns, concatenated
// in relation order; a LEFT JOIN null-extends the right source in place. The
// SQL5 predicate engine is reused unchanged, so ON and WHERE share the same
// three-valued semantics and column-to-column comparisons already work.

SqlErrorCode mapStatusToSqlCode(DbStatus status) {
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

SqlError executionError(const DbResult& result, uint32_t line, uint32_t column) {
    SqlError error;
    error.code = mapStatusToSqlCode(result.status());
    error.message = result.message().empty() ? std::string(dbStatusName(result.status()))
                                             : result.message();
    error.line = line;
    error.column = column;
    error.hasLocation = (line != 0 || column != 0);
    return error;
}

SqlError resourceError(const std::string& message, uint32_t line, uint32_t column) {
    SqlError error;
    error.code = SqlErrorCode::ResourceLimit;
    error.message = message;
    error.line = line;
    error.column = column;
    error.hasLocation = (line != 0 || column != 0);
    return error;
}

bool isNumericDbType(DbType type) {
    return type == DbType::Int32 || type == DbType::Int64 || type == DbType::Float64;
}

bool isOrderableDbType(DbType type) {
    return type == DbType::Boolean || type == DbType::Int32 || type == DbType::Int64 ||
           type == DbType::Float64 || type == DbType::Text;
}

// True when an equality between `leftType` and `rightType` can be probed
// through a B+ tree on `rightType`. Numeric widening toward the indexed type is
// allowed; the predicate engine performs the same promotion.
bool joinComparableTypes(DbType leftType, DbType rightType) {
    if (leftType == rightType) {
        return true;
    }
    if (leftType == DbType::Int32 || leftType == DbType::Int64) {
        if (rightType == DbType::Int32 || rightType == DbType::Int64) {
            return true;
        }
        if (rightType == DbType::Float64) {
            return true;
        }
    }
    if (leftType == DbType::Float64 && rightType == DbType::Float64) {
        return true;
    }
    return false;
}

// Widens a left probe value to the indexed column type. Returns false when the
// value cannot match any key of that type (e.g. an Int64 outside Int32 range),
// in which case the probe is skipped and no candidates are produced.
bool coerceJoinProbe(const DbValue& value, DbType target, DbValue& out) {
    if (value.isNull()) {
        out = DbValue::null();
        return true;
    }
    if (value.type() == target) {
        out = value;
        return true;
    }
    switch (target) {
    case DbType::Int32:
        if (value.type() == DbType::Int64) {
            const int64_t v = value.int64Value();
            if (v < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
                v > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
                return false;
            }
            out = DbValue::int32(static_cast<int32_t>(v));
            return true;
        }
        return false;
    case DbType::Int64:
        if (value.type() == DbType::Int32) {
            out = DbValue::int64(static_cast<int64_t>(value.int32Value()));
            return true;
        }
        return false;
    case DbType::Float64:
        if (value.type() == DbType::Int32) {
            out = DbValue::float64(static_cast<double>(value.int32Value()));
            return true;
        }
        if (value.type() == DbType::Int64) {
            out = DbValue::float64(static_cast<double>(value.int64Value()));
            return true;
        }
        return false;
    default:
        return false;
    }
}

int compareTextBytes(const std::string& a, const std::string& b) {
    const size_t count = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < count; ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

// Total order over SQL result values used by DISTINCT and GROUP BY. NULLs are
// equal to each other for duplicate/grouping purposes (unlike SQL `=`, which is
// UNKNOWN for NULL); -0.0 and +0.0 compare equal, matching IEEE equality.
int compareResultValues(const DbValue& a, const DbValue& b) {
    if (a.isNull() || b.isNull()) {
        if (a.isNull() && b.isNull()) {
            return 0;
        }
        return a.isNull() ? -1 : 1;
    }
    if (a.type() != b.type()) {
        return a.type() < b.type() ? -1 : 1;
    }
    switch (a.type()) {
    case DbType::Boolean: {
        const int va = a.booleanValue() ? 1 : 0;
        const int vb = b.booleanValue() ? 1 : 0;
        return va < vb ? -1 : (va > vb ? 1 : 0);
    }
    case DbType::Int32:
        return a.int32Value() < b.int32Value()
                   ? -1
                   : (a.int32Value() > b.int32Value() ? 1 : 0);
    case DbType::Int64:
        return a.int64Value() < b.int64Value()
                   ? -1
                   : (a.int64Value() > b.int64Value() ? 1 : 0);
    case DbType::Float64:
        return a.float64Value() < b.float64Value()
                   ? -1
                   : (a.float64Value() > b.float64Value() ? 1 : 0);
    case DbType::Text:
        return compareTextBytes(a.textValue(), b.textValue());
    case DbType::Blob: {
        const std::vector<uint8_t>& x = a.blobValue();
        const std::vector<uint8_t>& y = b.blobValue();
        const size_t count = x.size() < y.size() ? x.size() : y.size();
        for (size_t i = 0; i < count; ++i) {
            if (x[i] != y[i]) {
                return x[i] < y[i] ? -1 : 1;
            }
        }
        if (x.size() == y.size()) {
            return 0;
        }
        return x.size() < y.size() ? -1 : 1;
    }
    default:
        return 0;
    }
}

struct ResultRowLess {
    bool operator()(const std::vector<DbValue>& a,
                    const std::vector<DbValue>& b) const {
        const size_t count = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < count; ++i) {
            const int cmp = compareResultValues(a[i], b[i]);
            if (cmp != 0) {
                return cmp < 0;
            }
        }
        return a.size() < b.size();
    }
};

struct BoundSource {
    std::string tableName;
    std::string qualifier; // alias when present, else the table name
    Table* table;
    const std::vector<ColumnDefinition>* columns;
    uint32_t baseOrdinal;
    uint32_t columnCount;
    bool outerNullable; // right side of a LEFT JOIN

    BoundSource()
        : table(nullptr), columns(nullptr), baseOrdinal(0), columnCount(0),
          outerNullable(false) {}
};

// Resolves column references across the visible relation sources of one SELECT
// scope. An explicit alias replaces the table name as the qualifier.
class SourceColumnResolver : public SqlColumnResolver {
public:
    explicit SourceColumnResolver(const std::vector<BoundSource>& sources)
        : _sources(sources) {}

    bool resolveColumn(const SqlIdentifier& identifier, size_t& ordinal,
                       DbType& type, SqlError& error) const override {
        if (identifier.hasQualifier) {
            const BoundSource* source = nullptr;
            for (size_t i = 0; i < _sources.size(); ++i) {
                if (_sources[i].qualifier == identifier.qualifier) {
                    source = &_sources[i];
                    break;
                }
            }
            if (source == nullptr) {
                error = makeSemantic("unknown relation qualifier '" +
                                          identifier.qualifier + "'",
                                      identifier.line, identifier.column);
                return false;
            }
            for (size_t j = 0; j < source->columns->size(); ++j) {
                if ((*source->columns)[j].name == identifier.name) {
                    ordinal = source->baseOrdinal + j;
                    type = (*source->columns)[j].type;
                    return true;
                }
            }
            error = makeSemantic("unknown column '" + identifier.qualifier + "." +
                                      identifier.name + "'",
                                  identifier.line, identifier.column);
            return false;
        }

        int matches = 0;
        size_t foundOrdinal = 0;
        DbType foundType = DbType::Unknown;
        for (size_t i = 0; i < _sources.size(); ++i) {
            const std::vector<ColumnDefinition>& columns = *_sources[i].columns;
            for (size_t j = 0; j < columns.size(); ++j) {
                if (columns[j].name == identifier.name) {
                    ++matches;
                    foundOrdinal = _sources[i].baseOrdinal + j;
                    foundType = columns[j].type;
                }
            }
        }
        if (matches == 0) {
            error = makeSemantic("unknown column '" + identifier.name + "'",
                                  identifier.line, identifier.column);
            return false;
        }
        if (matches > 1) {
            error = makeSemantic("ambiguous column '" + identifier.name +
                                      "'; qualify it with a relation name or alias",
                                  identifier.line, identifier.column);
            return false;
        }
        ordinal = foundOrdinal;
        type = foundType;
        return true;
    }

private:
    const std::vector<BoundSource>& _sources;
};

// Classifies a column reference into (source index, local ordinal) without
// raising an error. Used by the join planner.
bool classifyColumn(const std::vector<BoundSource>& sources,
                    const SqlIdentifier& identifier, size_t& sourceIndex,
                    size_t& localOrdinal) {
    if (identifier.hasQualifier) {
        for (size_t i = 0; i < sources.size(); ++i) {
            if (sources[i].qualifier != identifier.qualifier) {
                continue;
            }
            for (size_t j = 0; j < sources[i].columns->size(); ++j) {
                if ((*sources[i].columns)[j].name == identifier.name) {
                    sourceIndex = i;
                    localOrdinal = j;
                    return true;
                }
            }
            return false;
        }
        return false;
    }
    int matches = 0;
    for (size_t i = 0; i < sources.size(); ++i) {
        for (size_t j = 0; j < sources[i].columns->size(); ++j) {
            if ((*sources[i].columns)[j].name == identifier.name) {
                sourceIndex = i;
                localOrdinal = j;
                ++matches;
            }
        }
    }
    return matches == 1;
}

bool nullableForOrdinal(const std::vector<BoundSource>& sources, size_t ordinal) {
    for (size_t i = 0; i < sources.size(); ++i) {
        const BoundSource& source = sources[i];
        if (ordinal >= source.baseOrdinal &&
            ordinal < static_cast<size_t>(source.baseOrdinal) + source.columnCount) {
            return (*source.columns)[ordinal - source.baseOrdinal].nullable ||
                   source.outerNullable;
        }
    }
    return true;
}

// ---- Projection / aggregate / order binding -------------------------------

struct ProjectionItemBound {
    SqlProjectionKind kind;
    size_t ordinal; // Column or aggregate argument
    DbType type;    // output type
    bool nullable;
    std::string name;
    SqlAggregateKind aggKind;
    bool aggStar;
    DbType aggInputType;

    ProjectionItemBound()
        : kind(SqlProjectionKind::Column), ordinal(0), type(DbType::Unknown),
          nullable(true), aggKind(SqlAggregateKind::Count), aggStar(false),
          aggInputType(DbType::Unknown) {}
};

struct OrderTermBound {
    bool useProjection;
    size_t ordinal;
    DbType type;
    bool descending;

    OrderTermBound()
        : useProjection(false), ordinal(0), type(DbType::Unknown),
          descending(false) {}
};

bool bindAggregateItem(const SqlProjectionItemAst& item,
                       const SourceColumnResolver& resolver,
                       ProjectionItemBound& out, SqlError& error) {
    out.kind = SqlProjectionKind::Aggregate;
    out.aggKind = item.aggregate.kind;
    out.aggStar = false;
    out.nullable = true;

    if (item.aggregate.star) {
        if (item.aggregate.kind != SqlAggregateKind::Count) {
            error = makeSemantic("'*' is only valid for COUNT", item.line, item.column);
            return false;
        }
        out.aggStar = true;
        out.type = DbType::Int64;
        out.nullable = false;
        out.name = item.hasAlias ? item.alias.name : "COUNT(*)";
        return true;
    }

    size_t ordinal = 0;
    DbType type = DbType::Unknown;
    if (!resolver.resolveColumn(item.aggregate.column, ordinal, type, error)) {
        return false;
    }
    out.ordinal = ordinal;
    out.aggInputType = type;

    switch (item.aggregate.kind) {
    case SqlAggregateKind::Count:
        out.type = DbType::Int64;
        out.nullable = false;
        break;
    case SqlAggregateKind::Sum:
        if (!isNumericDbType(type)) {
            error = makeSemantic("SUM requires a numeric column", item.line,
                                  item.column);
            return false;
        }
        out.type = (type == DbType::Float64) ? DbType::Float64 : DbType::Int64;
        break;
    case SqlAggregateKind::Avg:
        if (!isNumericDbType(type)) {
            error = makeSemantic("AVG requires a numeric column", item.line,
                                  item.column);
            return false;
        }
        out.type = DbType::Float64;
        break;
    case SqlAggregateKind::Min:
    case SqlAggregateKind::Max:
        if (!isOrderableDbType(type)) {
            error = makeSemantic(
                std::string(sqlAggregateKindName(item.aggregate.kind)) +
                    " requires an orderable column",
                item.line, item.column);
            return false;
        }
        out.type = type;
        break;
    }
    out.name = item.hasAlias
                   ? item.alias.name
                   : (std::string(sqlAggregateKindName(item.aggregate.kind)) + "(" +
                      item.aggregate.column.name + ")");
    return true;
}

// ---- Join planning ---------------------------------------------------------

struct JoinPathChoice {
    enum class Kind { NestedLoop, IndexNestedLoop };
    Kind kind;
    const Catalog::IndexRecord* index;
    size_t probeOrdinal; // left column ordinal in the accumulated joined row
    DbType indexType;
    int score;

    JoinPathChoice()
        : kind(Kind::NestedLoop), index(nullptr), probeOrdinal(0),
          indexType(DbType::Unknown), score(0) {}
};

void considerJoinCandidate(const std::vector<SqlExprNode>& nodes, int32_t index,
                           const std::vector<BoundSource>& sources,
                           size_t rightIndex, Database& db,
                           JoinPathChoice& best) {
    if (index < 0 || static_cast<size_t>(index) >= nodes.size()) {
        return;
    }
    const SqlExprNode& node = nodes[static_cast<size_t>(index)];
    if (node.kind == SqlExprKind::And) {
        considerJoinCandidate(nodes, node.left, sources, rightIndex, db, best);
        considerJoinCandidate(nodes, node.right, sources, rightIndex, db, best);
        return;
    }
    if (node.kind != SqlExprKind::Compare || node.compareOp != SqlCompareOp::Eq ||
        node.left < 0 || node.right < 0) {
        return;
    }
    const SqlExprNode& lhs = nodes[static_cast<size_t>(node.left)];
    const SqlExprNode& rhs = nodes[static_cast<size_t>(node.right)];
    if (lhs.kind != SqlExprKind::ColumnRef || rhs.kind != SqlExprKind::ColumnRef) {
        return;
    }
    size_t ls = 0;
    size_t ll = 0;
    size_t rs = 0;
    size_t rl = 0;
    if (!classifyColumn(sources, lhs.identifier, ls, ll) ||
        !classifyColumn(sources, rhs.identifier, rs, rl)) {
        return;
    }
    size_t leftSource = 0;
    size_t leftLocal = 0;
    size_t rightLocal = 0;
    if (rs == rightIndex && ls < rightIndex) {
        leftSource = ls;
        leftLocal = ll;
        rightLocal = rl;
    } else if (ls == rightIndex && rs < rightIndex) {
        leftSource = rs;
        leftLocal = rl;
        rightLocal = ll;
    } else {
        return;
    }

    const BoundSource& right = sources[rightIndex];
    if (rightLocal >= right.columns->size() ||
        leftLocal >= sources[leftSource].columns->size()) {
        return;
    }
    const ColumnDefinition& rightColumn = (*right.columns)[rightLocal];
    const ColumnDefinition& leftColumn = (*sources[leftSource].columns)[leftLocal];
    if (!isIndexableType(rightColumn.type) ||
        !joinComparableTypes(leftColumn.type, rightColumn.type)) {
        return;
    }
    const Catalog::IndexRecord* idx = db.findIndexForColumn(
        right.table->tableId(), static_cast<uint32_t>(rightLocal));
    if (idx == nullptr) {
        return;
    }
    const int score = (idx->primaryKey() || idx->unique()) ? 3 : 2;
    if (score > best.score) {
        best.kind = JoinPathChoice::Kind::IndexNestedLoop;
        best.index = idx;
        best.probeOrdinal = sources[leftSource].baseOrdinal + leftLocal;
        best.indexType = rightColumn.type;
        best.score = score;
    }
}

// ---- Joined-row production -------------------------------------------------

bool appendJoinedRow(std::vector<std::vector<DbValue> >& rows,
                     const std::vector<DbValue>& row, size_t& bytes,
                     SqlError& error, uint32_t line, uint32_t column) {
    if (rows.size() >= kSqlMaxJoinedRows) {
        error = resourceError(
            "joined intermediate row count exceeds the SQL7 maximum", line, column);
        return false;
    }
    const size_t rowSize = rowBytes(row);
    if (bytes + rowSize > kSqlMaxJoinedBytes) {
        error = resourceError(
            "joined intermediate working set exceeds the SQL7 byte limit", line,
            column);
        return false;
    }
    bytes += rowSize;
    rows.push_back(row);
    return true;
}

bool materializeSourceRows(const BoundSource& source,
                           std::vector<std::vector<DbValue> >& out, SqlError& error,
                           uint32_t line, uint32_t column) {
    std::unique_ptr<TableScan> scan;
    DbResult result = source.table->scanStart(scan);
    if (!result.isOk()) {
        error = executionError(result, line, column);
        return false;
    }
    std::vector<DbValue> row;
    size_t bytes = 0;
    while (scan->next(row)) {
        if (!appendJoinedRow(out, row, bytes, error, line, column)) {
            return false;
        }
    }
    if (!scan->status().isOk()) {
        error = executionError(scan->status(), line, column);
        return false;
    }
    return true;
}

bool runJoinStep(const BoundSource& right, SqlJoinType type,
                 const BoundPredicate& on, const JoinPathChoice& path,
                 std::vector<std::vector<DbValue> >& current, size_t leftWidth,
                 SqlJoinDiagnostics& diag, SqlError& error, uint32_t line,
                 uint32_t column, Database& db) {
    diag.joinType = (type == SqlJoinType::Left) ? "LEFT JOIN" : "INNER JOIN";
    diag.rightTable = right.tableName;
    diag.rightAlias = right.qualifier;

    std::vector<std::vector<DbValue> > next;
    size_t bytes = 0;

    if (path.kind == JoinPathChoice::Kind::IndexNestedLoop) {
        diag.accessPath = "IndexNestedLoop";
        diag.indexName = path.index->name;
        for (size_t li = 0; li < current.size(); ++li) {
            ++diag.leftRowsProcessed;
            const std::vector<DbValue>& leftRow = current[li];
            bool matched = false;
            std::vector<RowLocator> locators;
            if (path.probeOrdinal < leftRow.size() &&
                !leftRow[path.probeOrdinal].isNull()) {
                DbValue probe;
                if (coerceJoinProbe(leftRow[path.probeOrdinal], path.indexType,
                                    probe)) {
                    std::vector<uint8_t> key;
                    if (encodeIndexKey(path.indexType, probe, key)) {
                        ++diag.indexProbes;
                        DbResult result =
                            db.indexLookup(path.index->indexId, key, locators);
                        if (!result.isOk()) {
                            error = executionError(result, line, column);
                            return false;
                        }
                    }
                }
            }
            for (size_t ci = 0; ci < locators.size(); ++ci) {
                std::vector<DbValue> rightRow;
                DbResult fetched = right.table->fetchRow(locators[ci], rightRow);
                if (!fetched.isOk()) {
                    error = executionError(fetched, line, column);
                    return false;
                }
                ++diag.candidatesFetched;
                std::vector<DbValue> candidate;
                candidate.reserve(leftRow.size() + rightRow.size());
                candidate.insert(candidate.end(), leftRow.begin(), leftRow.end());
                candidate.insert(candidate.end(), rightRow.begin(), rightRow.end());
                if (on.evaluate(candidate) == SqlTruth::True) {
                    matched = true;
                    ++diag.matchesEmitted;
                    if (!appendJoinedRow(next, candidate, bytes, error, line,
                                         column)) {
                        return false;
                    }
                }
            }
            if (!matched && type == SqlJoinType::Left) {
                std::vector<DbValue> extended = leftRow;
                extended.resize(leftWidth + right.columnCount, DbValue::null());
                ++diag.nullExtendedRows;
                if (!appendJoinedRow(next, extended, bytes, error, line, column)) {
                    return false;
                }
            }
        }
    } else {
        diag.accessPath = "NestedLoop";
        std::vector<std::vector<DbValue> > rightRows;
        if (!materializeSourceRows(right, rightRows, error, line, column)) {
            return false;
        }
        for (size_t li = 0; li < current.size(); ++li) {
            ++diag.leftRowsProcessed;
            const std::vector<DbValue>& leftRow = current[li];
            bool matched = false;
            for (size_t ri = 0; ri < rightRows.size(); ++ri) {
                std::vector<DbValue> candidate;
                candidate.reserve(leftRow.size() + rightRows[ri].size());
                candidate.insert(candidate.end(), leftRow.begin(), leftRow.end());
                candidate.insert(candidate.end(), rightRows[ri].begin(),
                                 rightRows[ri].end());
                if (on.evaluate(candidate) == SqlTruth::True) {
                    matched = true;
                    ++diag.matchesEmitted;
                    if (!appendJoinedRow(next, candidate, bytes, error, line,
                                         column)) {
                        return false;
                    }
                }
            }
            if (!matched && type == SqlJoinType::Left) {
                std::vector<DbValue> extended = leftRow;
                extended.resize(leftWidth + right.columnCount, DbValue::null());
                ++diag.nullExtendedRows;
                if (!appendJoinedRow(next, extended, bytes, error, line, column)) {
                    return false;
                }
            }
        }
    }

    current.swap(next);
    return true;
}

// ---- Aggregate accumulation ------------------------------------------------

bool int64AddChecked(int64_t a, int64_t b, int64_t& out) {
    if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<int64_t>::min() - b)) {
        return false;
    }
    out = a + b;
    return true;
}

struct AggregateAccumulator {
    SqlAggregateKind kind;
    DbType inputType;
    DbType outputType;
    bool countStar;
    bool overflow;
    uint64_t rowCount;
    uint64_t valueCount;
    bool hasValue;
    int64_t intSum;
    double floatSum;
    bool extremeInit;
    DbValue extreme;

    AggregateAccumulator()
        : kind(SqlAggregateKind::Count), inputType(DbType::Unknown),
          outputType(DbType::Unknown), countStar(false), overflow(false),
          rowCount(0), valueCount(0), hasValue(false), intSum(0), floatSum(0.0),
          extremeInit(false), extreme() {}

    void update(const std::vector<DbValue>& row, size_t ordinal) {
        if (countStar) {
            if (rowCount == std::numeric_limits<uint64_t>::max()) {
                overflow = true;
            } else {
                ++rowCount;
            }
            return;
        }
        if (ordinal >= row.size()) {
            return;
        }
        const DbValue& value = row[ordinal];
        if (value.isNull()) {
            return;
        }
        if (valueCount == std::numeric_limits<uint64_t>::max()) {
            overflow = true;
            return;
        }
        ++valueCount;
        switch (kind) {
        case SqlAggregateKind::Count:
            break;
        case SqlAggregateKind::Sum:
        case SqlAggregateKind::Avg:
            if (inputType == DbType::Float64) {
                floatSum += value.float64Value();
            } else {
                const int64_t x = (inputType == DbType::Int32)
                                      ? static_cast<int64_t>(value.int32Value())
                                      : value.int64Value();
                if (!int64AddChecked(intSum, x, intSum)) {
                    overflow = true;
                }
            }
            hasValue = true;
            break;
        case SqlAggregateKind::Min:
        case SqlAggregateKind::Max: {
            if (!extremeInit) {
                extreme = value;
                extremeInit = true;
            } else {
                const int cmp = compareOrderValues(extreme, value, inputType);
                if ((kind == SqlAggregateKind::Min && cmp > 0) ||
                    (kind == SqlAggregateKind::Max && cmp < 0)) {
                    extreme = value;
                }
            }
            break;
        }
        }
    }

    DbValue finalize() {
        switch (kind) {
        case SqlAggregateKind::Count:
            return DbValue::int64(countStar ? static_cast<int64_t>(rowCount)
                                            : static_cast<int64_t>(valueCount));
        case SqlAggregateKind::Sum:
            if (!hasValue) {
                return DbValue::null();
            }
            if (outputType == DbType::Float64) {
                return DbValue::float64(floatSum);
            }
            return DbValue::int64(intSum);
        case SqlAggregateKind::Avg:
            if (!hasValue || valueCount == 0) {
                return DbValue::null();
            }
            if (inputType == DbType::Float64) {
                return DbValue::float64(floatSum /
                                        static_cast<double>(valueCount));
            }
            return DbValue::float64(static_cast<double>(intSum) /
                                    static_cast<double>(valueCount));
        case SqlAggregateKind::Min:
        case SqlAggregateKind::Max:
            if (!extremeInit) {
                return DbValue::null();
            }
            return extreme;
        }
        return DbValue::null();
    }
};

AggregateAccumulator makeAccumulator(const ProjectionItemBound& item) {
    AggregateAccumulator accumulator;
    accumulator.kind = item.aggKind;
    accumulator.inputType = item.aggInputType;
    accumulator.outputType = item.type;
    accumulator.countStar = item.aggStar;
    return accumulator;
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
      transactionIdAfter(0), accessPath(), accessIndexName(),
      candidateRowsVisited(0) {}

SqlExecutionResult::SqlExecutionResult()
    : ok(false), error(), statements(), failedStatementIndex(0), stoppedEarly(false),
      transactionActiveAfter(false) {}

// ---------------------------------------------------------------------------
// Engine.

SqlEngine::SqlEngine(Database& db) : _db(db), _tx(), _forceNestedLoop(false) {}

SqlEngine::~SqlEngine() {
    if (_tx && _tx->isActive()) {
        _tx->rollback();
    }
}

uint64_t SqlEngine::transactionId() const { return _tx ? _tx->id() : 0; }

SqlErrorCode SqlEngine::codeForStatus(DbStatus status) {
    return mapStatusToSqlCode(status);
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
    case SqlStatementType::CreateIndex:
        ok = executeCreateIndex(stmt.createIndex, out);
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
        if (ast.columns[i].primaryKey) {
            definition.indexes.push_back(IndexDefinition(
                std::string(), 0, static_cast<uint32_t>(i), IndexKind::PrimaryKey));
        } else if (ast.columns[i].unique) {
            definition.indexes.push_back(IndexDefinition(
                std::string(), 0, static_cast<uint32_t>(i), IndexKind::Unique));
        }
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

bool SqlEngine::executeCreateIndex(const SqlCreateIndexAst& ast,
                                   SqlStatementResult& out) {
    const Catalog::TableRecord* table = _db.catalog().findTable(ast.table.name);
    if (table == nullptr) {
        out.error = makeSemantic("unknown table '" + ast.table.name + "'",
                                 ast.table.line, ast.table.column);
        return false;
    }
    size_t ordinal = table->columns.size();
    for (size_t i = 0; i < table->columns.size(); ++i) {
        if (table->columns[i].name == ast.columnName.name) {
            ordinal = i;
            break;
        }
    }
    if (ordinal == table->columns.size()) {
        out.error = makeSemantic(
            "unknown column '" + ast.columnName.name + "' in table '" + ast.table.name + "'",
            ast.columnName.line, ast.columnName.column);
        return false;
    }
    if (!isIndexableType(table->columns[ordinal].type)) {
        out.error = makeSemantic(
            "column '" + ast.columnName.name + "' has a type that cannot be indexed",
            ast.columnName.line, ast.columnName.column);
        return false;
    }

    IndexDefinition def;
    def.name = ast.index.name;
    def.tableId = table->tableId;
    def.columnOrdinal = static_cast<uint32_t>(ordinal);
    def.kind = ast.unique ? IndexKind::Unique : IndexKind::Ordinary;

    const bool explicitTransaction = (_tx != nullptr);
    TransactionSavepoint savepoint;
    if (explicitTransaction) {
        _tx->beginStatement(savepoint);
    }
    uint32_t indexId = 0;
    DbResult result = _db.createIndex(def, indexId);
    if (!result.isOk()) {
        if (explicitTransaction) {
            _tx->rollbackStatement(savepoint);
        }
        out.error = errorFromResult(result, ast.index.line, ast.index.column);
        return false;
    }
    if (explicitTransaction) {
        _tx->releaseStatement(savepoint);
    }
    out.objectName = ast.index.name;
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

    // ---- 1. Bind relation sources (FROM plus every JOIN). ------------------
    std::vector<BoundSource> sources;
    sources.reserve(1 + ast.joins.size());
    {
        Table* table = openTableCached(ast.from.table.name, ctx, error,
                                       ast.from.table.line, ast.from.table.column);
        if (table == nullptr) {
            out.error = error;
            return false;
        }
        BoundSource source;
        source.tableName = ast.from.table.name;
        source.qualifier =
            ast.from.hasAlias ? ast.from.alias.name : ast.from.table.name;
        source.table = table;
        source.columns = &table->columns();
        source.baseOrdinal = 0;
        source.columnCount = static_cast<uint32_t>(source.columns->size());
        source.outerNullable = false;
        sources.push_back(source);
    }
    for (size_t i = 0; i < ast.joins.size(); ++i) {
        const SqlTableRefAst& ref = ast.joins[i].table;
        Table* table = openTableCached(ref.table.name, ctx, error, ref.table.line,
                                       ref.table.column);
        if (table == nullptr) {
            out.error = error;
            return false;
        }
        BoundSource source;
        source.tableName = ref.table.name;
        source.qualifier = ref.hasAlias ? ref.alias.name : ref.table.name;
        source.table = table;
        source.columns = &table->columns();
        source.baseOrdinal = sources.back().baseOrdinal + sources.back().columnCount;
        source.columnCount = static_cast<uint32_t>(source.columns->size());
        source.outerNullable = (ast.joins[i].type == SqlJoinType::Left);
        sources.push_back(source);
    }

    // Self-joins require distinct aliases: qualifiers are query-source
    // identifiers and must be unique within the SELECT scope.
    for (size_t i = 0; i < sources.size(); ++i) {
        for (size_t j = i + 1; j < sources.size(); ++j) {
            if (sources[i].qualifier == sources[j].qualifier) {
                uint32_t line = ast.line;
                uint32_t column = ast.column;
                if (j >= 1 && j - 1 < ast.joins.size()) {
                    line = ast.joins[j - 1].line;
                    column = ast.joins[j - 1].column;
                }
                out.error = makeSemantic(
                    "duplicate relation alias '" + sources[i].qualifier +
                        "'; every joined source needs a unique alias",
                    line, column);
                return false;
            }
        }
    }

    SourceColumnResolver resolver(sources);

    // ---- 2. Bind WHERE and ON predicates. ---------------------------------
    BoundPredicate where;
    if (!where.bind(ast.where.nodes, ast.where.present ? ast.where.root : -1,
                    resolver, error)) {
        out.error = error;
        return false;
    }
    std::vector<BoundPredicate> onPredicates(ast.joins.size());
    for (size_t i = 0; i < ast.joins.size(); ++i) {
        // ON may only reference the relations joined so far plus the current
        // right relation (left-deep scope).
        std::vector<BoundSource> prefix(sources.begin(), sources.begin() + i + 2);
        SourceColumnResolver onResolver(prefix);
        if (!onPredicates[i].bind(ast.joins[i].on.nodes, ast.joins[i].on.root,
                                  onResolver, error)) {
            out.error = error;
            return false;
        }
    }

    // ---- 3. Bind projection. ----------------------------------------------
    std::vector<ProjectionItemBound> projection;
    bool hasAggregate = false;
    if (ast.star) {
        for (size_t s = 0; s < sources.size(); ++s) {
            const BoundSource& source = sources[s];
            for (size_t j = 0; j < source.columns->size(); ++j) {
                ProjectionItemBound item;
                item.kind = SqlProjectionKind::Column;
                item.ordinal = source.baseOrdinal + static_cast<uint32_t>(j);
                item.type = (*source.columns)[j].type;
                item.nullable =
                    (*source.columns)[j].nullable || source.outerNullable;
                item.name = (*source.columns)[j].name;
                projection.push_back(item);
            }
        }
    } else {
        for (size_t i = 0; i < ast.projection.size(); ++i) {
            const SqlProjectionItemAst& item = ast.projection[i];
            ProjectionItemBound bound;
            if (item.kind == SqlProjectionKind::Aggregate) {
                hasAggregate = true;
                if (!bindAggregateItem(item, resolver, bound, error)) {
                    out.error = error;
                    return false;
                }
            } else {
                size_t ordinal = 0;
                DbType type = DbType::Unknown;
                if (!resolver.resolveColumn(item.columnRef, ordinal, type, error)) {
                    out.error = error;
                    return false;
                }
                bound.kind = SqlProjectionKind::Column;
                bound.ordinal = ordinal;
                bound.type = type;
                bound.nullable = nullableForOrdinal(sources, ordinal);
                bound.name = item.hasAlias ? item.alias.name : item.columnRef.name;
            }
            projection.push_back(bound);
        }
    }

    // ---- 4. Bind GROUP BY and validate grouped projections. ---------------
    std::vector<size_t> groupOrdinals;
    for (size_t i = 0; i < ast.groupBy.size(); ++i) {
        size_t ordinal = 0;
        DbType type = DbType::Unknown;
        if (!resolver.resolveColumn(ast.groupBy[i], ordinal, type, error)) {
            out.error = error;
            return false;
        }
        groupOrdinals.push_back(ordinal);
    }
    const bool hasGroup = !groupOrdinals.empty();
    if (hasAggregate || hasGroup) {
        for (size_t p = 0; p < projection.size(); ++p) {
            if (projection[p].kind != SqlProjectionKind::Column) {
                continue;
            }
            bool inGroup = false;
            for (size_t g = 0; g < groupOrdinals.size(); ++g) {
                if (groupOrdinals[g] == projection[p].ordinal) {
                    inGroup = true;
                    break;
                }
            }
            if (!inGroup) {
                out.error = makeSemantic(
                    "column '" + projection[p].name +
                        "' must appear in GROUP BY or be used in an aggregate",
                    ast.line, ast.column);
                return false;
            }
        }
    }

    // ---- 5. Bind ORDER BY. ------------------------------------------------
    std::vector<OrderTermBound> orderTerms;
    for (size_t i = 0; i < ast.orderBy.size(); ++i) {
        const SqlOrderTermAst& term = ast.orderBy[i];
        OrderTermBound bound;
        bound.descending = term.descending;
        bool matchedProjection = false;
        // An unqualified name that matches a unique projection alias (or output
        // column name) resolves to that output; the alias wins.
        if (!term.column.hasQualifier) {
            int matches = 0;
            size_t index = 0;
            for (size_t p = 0; p < projection.size(); ++p) {
                if (projection[p].name == term.column.name) {
                    ++matches;
                    index = p;
                }
            }
            if (matches == 1) {
                bound.useProjection = true;
                bound.ordinal = index;
                bound.type = projection[index].type;
                matchedProjection = true;
            } else if (matches > 1) {
                out.error = makeSemantic(
                    "ambiguous ORDER BY name '" + term.column.name +
                        "'; use a unique projection alias",
                    term.column.line, term.column.column);
                return false;
            }
        }
        // Otherwise resolve as a source column. If that column is projected,
        // order by the projected output (so DISTINCT/aggregate queries can order
        // by a grouped output column).
        bool resolved = false;
        size_t sourceOrdinal = 0;
        DbType sourceType = DbType::Unknown;
        if (!matchedProjection) {
            resolved =
                resolver.resolveColumn(term.column, sourceOrdinal, sourceType, error);
            if (resolved) {
                for (size_t p = 0; p < projection.size(); ++p) {
                    if (projection[p].kind == SqlProjectionKind::Column &&
                        projection[p].ordinal == sourceOrdinal) {
                        bound.useProjection = true;
                        bound.ordinal = p;
                        bound.type = projection[p].type;
                        matchedProjection = true;
                        break;
                    }
                }
            }
        }
        if (!matchedProjection && (hasAggregate || hasGroup) && !ast.distinct &&
            resolved) {
            // An aggregate query may order by a GROUP BY column that is not
            // itself projected; the group key carries that value.
            for (size_t g = 0; g < groupOrdinals.size(); ++g) {
                if (groupOrdinals[g] == sourceOrdinal) {
                    bound.useProjection = false;
                    bound.ordinal = g;
                    bound.type = sourceType;
                    matchedProjection = true;
                    break;
                }
            }
        }
        if (!matchedProjection) {
            if (ast.distinct || hasAggregate || hasGroup) {
                out.error = makeSemantic(
                    "ORDER BY must reference a projected column or alias in a "
                    "DISTINCT or aggregate query",
                    term.column.line, term.column.column);
                return false;
            }
            if (!resolved) {
                out.error = error;
                return false;
            }
            if (sourceType == DbType::Blob) {
                out.error = makeSemantic(
                    "ORDER BY is not supported for Blob column '" +
                        term.column.name + "'",
                    term.column.line, term.column.column);
                return false;
            }
            bound.useProjection = false;
            bound.ordinal = sourceOrdinal;
            bound.type = sourceType;
        }
        orderTerms.push_back(bound);
    }

    // ---- 6. Produce base rows (SQL6 access path for a single relation). ---
    std::vector<std::vector<DbValue> > current;
    {
        const BoundSource& base = sources[0];
        if (sources.size() == 1) {
            AccessPathChoice path = selectAccessPath(ast.where, *base.columns, _db,
                                                     base.table->tableId());
            std::unique_ptr<RowSource> source;
            if (path.kind == AccessPathChoice::Kind::FullScan) {
                std::unique_ptr<TableScan> scan;
                DbResult result = base.table->scanStart(scan);
                if (!result.isOk()) {
                    out.error = errorFromResult(result, ast.from.table.line,
                                                ast.from.table.column);
                    return false;
                }
                source.reset(new ScanRowSource(std::move(scan)));
                out.accessPath = "FullScan";
            } else {
                std::vector<RowLocator> locators;
                DbResult result = DbResult::ok();
                const DbType indexType = (*base.columns)[path.columnOrdinal].type;
                if (path.kind == AccessPathChoice::Kind::IndexLookup) {
                    std::vector<uint8_t> key;
                    if (!encodeIndexKey(indexType, path.value, key)) {
                        out.error = makeSemantic("value cannot be indexed", ast.line,
                                                 ast.column);
                        return false;
                    }
                    result = _db.indexLookup(path.index->indexId, key, locators);
                    out.accessPath = "IndexLookup";
                } else {
                    IndexRangeBound bound;
                    switch (path.op) {
                    case SqlCompareOp::Lt:
                        bound.hasUpper = true;
                        bound.upperInclusive = false;
                        bound.upper = path.value;
                        break;
                    case SqlCompareOp::Le:
                        bound.hasUpper = true;
                        bound.upperInclusive = true;
                        bound.upper = path.value;
                        break;
                    case SqlCompareOp::Gt:
                        bound.hasLower = true;
                        bound.lowerInclusive = false;
                        bound.lower = path.value;
                        break;
                    case SqlCompareOp::Ge:
                        bound.hasLower = true;
                        bound.lowerInclusive = true;
                        bound.lower = path.value;
                        break;
                    default:
                        break;
                    }
                    result = _db.indexRange(path.index->indexId, bound, locators);
                    out.accessPath = "IndexRange";
                }
                if (!result.isOk()) {
                    out.error = errorFromResult(result, ast.from.table.line,
                                                ast.from.table.column);
                    return false;
                }
                out.accessIndexName = path.index->name;
                out.candidateRowsVisited = static_cast<uint64_t>(locators.size());
                source.reset(new LocatorRowSource(base.table, locators));
            }
            std::vector<DbValue> row;
            size_t bytes = 0;
            while (source->next(row)) {
                if (!appendJoinedRow(current, row, bytes, error, ast.line,
                                     ast.column)) {
                    out.error = error;
                    return false;
                }
            }
            if (!source->status().isOk()) {
                out.error = errorFromResult(source->status(), ast.from.table.line,
                                            ast.from.table.column);
                return false;
            }
        } else {
            out.accessPath = "FullScan";
            if (!materializeSourceRows(base, current, error, ast.from.table.line,
                                       ast.from.table.column)) {
                out.error = error;
                return false;
            }
        }
    }

    // ---- 7. Execute joins in written left-to-right order. -----------------
    for (size_t i = 0; i < ast.joins.size(); ++i) {
        const size_t rightIndex = i + 1;
        JoinPathChoice path;
        considerJoinCandidate(ast.joins[i].on.nodes, ast.joins[i].on.root, sources,
                              rightIndex, _db, path);
        if (_forceNestedLoop) {
            path.kind = JoinPathChoice::Kind::NestedLoop;
            path.index = nullptr;
            path.score = 0;
        }
        SqlJoinDiagnostics diag;
        const size_t leftWidth = sources[rightIndex].baseOrdinal;
        if (!runJoinStep(sources[rightIndex], ast.joins[i].type, onPredicates[i],
                         path, current, leftWidth, diag, error, ast.joins[i].line,
                         ast.joins[i].column, _db)) {
            out.error = error;
            return false;
        }
        out.joins.push_back(diag);
    }

    // ---- 8. Apply WHERE. --------------------------------------------------
    if (ast.where.present) {
        std::vector<std::vector<DbValue> > filtered;
        for (size_t i = 0; i < current.size(); ++i) {
            if (where.evaluate(current[i]) == SqlTruth::True) {
                filtered.push_back(current[i]);
            }
        }
        current.swap(filtered);
    }

    // ---- 9. Result-column metadata. ---------------------------------------
    std::vector<SqlColumn> resultColumns;
    resultColumns.reserve(projection.size());
    for (size_t p = 0; p < projection.size(); ++p) {
        resultColumns.push_back(SqlColumn(projection[p].name, projection[p].type,
                                          projection[p].nullable));
    }
    out.resultSet.setColumns(resultColumns);

    // ---- 10. Projection / aggregation. ------------------------------------
    struct OutputRow {
        std::vector<DbValue> values;
        std::vector<DbValue> source;
    };
    std::vector<OutputRow> rows;

    if (hasAggregate || hasGroup) {
        struct GroupState {
            std::vector<DbValue> keyValues;
            std::vector<AggregateAccumulator> accumulators;
        };
        std::map<std::vector<DbValue>, size_t, ResultRowLess> groupIndex;
        std::vector<GroupState> groups;
        size_t groupBytes = 0;

        for (size_t r = 0; r < current.size(); ++r) {
            std::vector<DbValue> key;
            key.reserve(groupOrdinals.size());
            for (size_t g = 0; g < groupOrdinals.size(); ++g) {
                key.push_back(current[r][groupOrdinals[g]]);
            }
            std::map<std::vector<DbValue>, size_t, ResultRowLess>::iterator it =
                groupIndex.find(key);
            size_t groupId = 0;
            if (it == groupIndex.end()) {
                if (groups.size() >= kSqlMaxGroupCount) {
                    out.error = resourceError(
                        "GROUP BY group count exceeds the SQL7 maximum", ast.line,
                        ast.column);
                    return false;
                }
                GroupState state;
                state.keyValues = key;
                state.accumulators.resize(projection.size());
                for (size_t p = 0; p < projection.size(); ++p) {
                    if (projection[p].kind == SqlProjectionKind::Aggregate) {
                        state.accumulators[p] = makeAccumulator(projection[p]);
                    }
                }
                groupId = groups.size();
                groups.push_back(state);
                groupIndex.insert(std::make_pair(key, groupId));
                groupBytes += rowBytes(key) + sizeof(GroupState);
                if (groupBytes > kSqlMaxGroupBytes) {
                    out.error = resourceError(
                        "GROUP BY working set exceeds the SQL7 byte limit", ast.line,
                        ast.column);
                    return false;
                }
            } else {
                groupId = it->second;
            }
            GroupState& state = groups[groupId];
            for (size_t p = 0; p < projection.size(); ++p) {
                if (projection[p].kind == SqlProjectionKind::Aggregate) {
                    state.accumulators[p].update(current[r], projection[p].ordinal);
                }
            }
        }

        if (!hasGroup && groups.empty()) {
            // An aggregate over an empty input still yields exactly one row.
            GroupState state;
            state.accumulators.resize(projection.size());
            for (size_t p = 0; p < projection.size(); ++p) {
                if (projection[p].kind == SqlProjectionKind::Aggregate) {
                    state.accumulators[p] = makeAccumulator(projection[p]);
                }
            }
            groups.push_back(state);
        }

        for (size_t g = 0; g < groups.size(); ++g) {
            OutputRow row;
            row.values.resize(projection.size());
            // Order terms that reference a non-projected GROUP BY column index
            // into this group-key vector.
            row.source = groups[g].keyValues;
            for (size_t p = 0; p < projection.size(); ++p) {
                if (projection[p].kind == SqlProjectionKind::Aggregate) {
                    if (groups[g].accumulators[p].overflow) {
                        out.error = makeSemantic(
                            "integer overflow while computing an aggregate",
                            ast.line, ast.column);
                        out.error.code = SqlErrorCode::ExecutionError;
                        return false;
                    }
                    row.values[p] = groups[g].accumulators[p].finalize();
                } else {
                    size_t position = 0;
                    bool found = false;
                    for (size_t k = 0; k < groupOrdinals.size(); ++k) {
                        if (groupOrdinals[k] == projection[p].ordinal) {
                            position = k;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        out.error = makeSemantic(
                            "internal: grouped column missing from GROUP BY",
                            ast.line, ast.column);
                        return false;
                    }
                    row.values[p] = groups[g].keyValues[position];
                }
            }
            rows.push_back(row);
        }
    } else {
        for (size_t r = 0; r < current.size(); ++r) {
            OutputRow row;
            row.values.resize(projection.size());
            for (size_t p = 0; p < projection.size(); ++p) {
                row.values[p] = current[r][projection[p].ordinal];
            }
            row.source = current[r];
            rows.push_back(row);
        }
    }

    // ---- 11. DISTINCT (after projection/aggregation). ---------------------
    if (ast.distinct) {
        std::set<std::vector<DbValue>, ResultRowLess> seen;
        std::vector<OutputRow> dedup;
        size_t bytes = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!seen.insert(rows[i].values).second) {
                continue;
            }
            if (dedup.size() >= kSqlMaxDistinctRows) {
                out.error = resourceError(
                    "DISTINCT row count exceeds the SQL7 maximum", ast.line,
                    ast.column);
                return false;
            }
            bytes += rowBytes(rows[i].values);
            if (bytes > kSqlMaxDistinctBytes) {
                out.error = resourceError(
                    "DISTINCT working set exceeds the SQL7 byte limit", ast.line,
                    ast.column);
                return false;
            }
            dedup.push_back(rows[i]);
        }
        rows.swap(dedup);
    }

    // ---- 12. ORDER BY (before OFFSET/LIMIT). ------------------------------
    if (!orderTerms.empty()) {
        std::stable_sort(rows.begin(), rows.end(),
                         [&orderTerms](const OutputRow& a, const OutputRow& b) {
                             for (size_t i = 0; i < orderTerms.size(); ++i) {
                                 const OrderTermBound& term = orderTerms[i];
                                 const std::vector<DbValue>& av =
                                     term.useProjection ? a.values : a.source;
                                 const std::vector<DbValue>& bv =
                                     term.useProjection ? b.values : b.source;
                                 int cmp = compareOrderValues(av[term.ordinal],
                                                              bv[term.ordinal],
                                                              term.type);
                                 if (term.descending) {
                                     cmp = -cmp;
                                 }
                                 if (cmp != 0) {
                                     return cmp < 0;
                                 }
                             }
                             return false;
                         });
    }

    // ---- 13. OFFSET / LIMIT. ----------------------------------------------
    const uint64_t offset = ast.hasOffset ? ast.offset : 0;
    const uint64_t limit =
        ast.hasLimit ? ast.limit : std::numeric_limits<uint64_t>::max();
    const uint64_t total = static_cast<uint64_t>(rows.size());
    const uint64_t start = offset < total ? offset : total;
    uint64_t end = total;
    if (limit != std::numeric_limits<uint64_t>::max()) {
        const uint64_t remaining = end - start;
        if (limit < remaining) {
            end = start + limit;
        }
    }
    for (uint64_t i = start; i < end; ++i) {
        if (!out.resultSet.addRow(rows[static_cast<size_t>(i)].values)) {
            out.error = makeSemantic(
                "result set exceeds the SQL7 materialization limit", ast.line,
                ast.column);
            out.error.code = SqlErrorCode::ResourceLimit;
            return false;
        }
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
