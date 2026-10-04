#include "database_sql_expr.h"

#include <cstring>

namespace gxos {
namespace db {

SqlTruth sqlTruthAnd(SqlTruth a, SqlTruth b) {
    if (a == SqlTruth::False || b == SqlTruth::False) {
        return SqlTruth::False;
    }
    if (a == SqlTruth::Unknown || b == SqlTruth::Unknown) {
        return SqlTruth::Unknown;
    }
    return SqlTruth::True;
}

SqlTruth sqlTruthOr(SqlTruth a, SqlTruth b) {
    if (a == SqlTruth::True || b == SqlTruth::True) {
        return SqlTruth::True;
    }
    if (a == SqlTruth::Unknown || b == SqlTruth::Unknown) {
        return SqlTruth::Unknown;
    }
    return SqlTruth::False;
}

SqlTruth sqlTruthNot(SqlTruth a) {
    switch (a) {
    case SqlTruth::True: return SqlTruth::False;
    case SqlTruth::False: return SqlTruth::True;
    case SqlTruth::Unknown: return SqlTruth::Unknown;
    }
    return SqlTruth::Unknown;
}

const char* sqlTruthName(SqlTruth truth) {
    switch (truth) {
    case SqlTruth::True: return "TRUE";
    case SqlTruth::False: return "FALSE";
    case SqlTruth::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

namespace {

bool isNumericType(DbType type) {
    return type == DbType::Int32 || type == DbType::Int64 || type == DbType::Float64;
}

DbType literalKindToType(SqlLiteralKind kind) {
    switch (kind) {
    case SqlLiteralKind::Boolean: return DbType::Boolean;
    case SqlLiteralKind::Integer: return DbType::Int64;
    case SqlLiteralKind::Float: return DbType::Float64;
    case SqlLiteralKind::String: return DbType::Text;
    case SqlLiteralKind::Blob: return DbType::Blob;
    case SqlLiteralKind::Null: return DbType::Unknown;
    }
    return DbType::Unknown;
}

DbValue literalToValue(const SqlLiteralAst& literal) {
    switch (literal.kind) {
    case SqlLiteralKind::Boolean:
        return DbValue::boolean(literal.boolValue);
    case SqlLiteralKind::Integer:
        return DbValue::int64(literal.int64Value);
    case SqlLiteralKind::Float:
        return DbValue::float64(literal.float64Value);
    case SqlLiteralKind::String:
        return DbValue::text(literal.textValue);
    case SqlLiteralKind::Blob:
        return DbValue::blob(literal.blobValue.empty() ? nullptr : literal.blobValue.data(),
                             literal.blobValue.size());
    case SqlLiteralKind::Null:
        break;
    }
    return DbValue::null();
}

SqlError semanticError(const std::string& message, uint32_t line, uint32_t column) {
    SqlError error;
    error.code = SqlErrorCode::SemanticError;
    error.message = message;
    error.line = line;
    error.column = column;
    error.hasLocation = (line != 0 || column != 0);
    return error;
}

// Compares two byte strings lexicographically using unsigned byte values, which
// defines the deterministic byte-wise UTF-8 ordering for SQL5 Text.
int compareBytesUnsigned(const std::string& a, const std::string& b) {
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

int compareBlobs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t count = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

bool coerceValue(const DbValue& value, DbType target, DbValue& out) {
    switch (target) {
    case DbType::Boolean:
    case DbType::Text:
    case DbType::Blob:
        if (value.type() != target) {
            return false;
        }
        out = value;
        return true;
    case DbType::Int64:
        if (value.type() == DbType::Int32) {
            out = DbValue::int64(static_cast<int64_t>(value.int32Value()));
            return true;
        }
        if (value.type() == DbType::Int64) {
            out = value;
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
        if (value.type() == DbType::Float64) {
            out = value;
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool valuesEqual(DbType type, const DbValue& a, const DbValue& b) {
    switch (type) {
    case DbType::Boolean: return a.booleanValue() == b.booleanValue();
    case DbType::Int64: return a.int64Value() == b.int64Value();
    case DbType::Float64: return a.float64Value() == b.float64Value();
    case DbType::Text: return a.textValue() == b.textValue();
    case DbType::Blob: return a.blobValue() == b.blobValue();
    default: return false;
    }
}

bool compareOrdered(SqlCompareOp op, DbType type, const DbValue& a, const DbValue& b) {
    int cmp = 0;
    switch (type) {
    case DbType::Int64:
        cmp = (a.int64Value() < b.int64Value()) ? -1 : (a.int64Value() > b.int64Value() ? 1 : 0);
        break;
    case DbType::Float64:
        cmp = (a.float64Value() < b.float64Value())
                  ? -1
                  : (a.float64Value() > b.float64Value() ? 1 : 0);
        break;
    case DbType::Text:
        cmp = compareBytesUnsigned(a.textValue(), b.textValue());
        break;
    case DbType::Blob:
        cmp = compareBlobs(a.blobValue(), b.blobValue());
        break;
    default:
        return false;
    }
    switch (op) {
    case SqlCompareOp::Lt: return cmp < 0;
    case SqlCompareOp::Le: return cmp <= 0;
    case SqlCompareOp::Gt: return cmp > 0;
    case SqlCompareOp::Ge: return cmp >= 0;
    default: return false;
    }
}

bool compareValues(SqlCompareOp op, DbType type, const DbValue& a, const DbValue& b) {
    if (op == SqlCompareOp::Eq) {
        return valuesEqual(type, a, b);
    }
    if (op == SqlCompareOp::Ne) {
        return !valuesEqual(type, a, b);
    }
    return compareOrdered(op, type, a, b);
}

// SQL5 single-table resolver: matches by column name and ignores any qualifier.
class ColumnListResolver : public SqlColumnResolver {
public:
    explicit ColumnListResolver(const std::vector<ColumnDefinition>& columns)
        : _columns(columns) {}

    bool resolveColumn(const SqlIdentifier& identifier, size_t& ordinal,
                       DbType& type, SqlError& error) const override {
        for (size_t i = 0; i < _columns.size(); ++i) {
            if (_columns[i].name == identifier.name) {
                ordinal = i;
                type = _columns[i].type;
                return true;
            }
        }
        error = semanticError("unknown column '" + identifier.name + "' in predicate",
                              identifier.line, identifier.column);
        return false;
    }

private:
    const std::vector<ColumnDefinition>& _columns;
};

} // namespace

BoundPredicate::BoundPredicate() : _nodes(), _root(-1), _bound(false) {}

bool BoundPredicate::bind(const std::vector<SqlExprNode>& nodes, int32_t root,
                          const std::vector<ColumnDefinition>& columns,
                          SqlError& error) {
    ColumnListResolver resolver(columns);
    return bindInternal(nodes, root, resolver, error);
}

bool BoundPredicate::bind(const std::vector<SqlExprNode>& nodes, int32_t root,
                          const SqlColumnResolver& resolver, SqlError& error) {
    return bindInternal(nodes, root, resolver, error);
}

bool BoundPredicate::bindInternal(const std::vector<SqlExprNode>& nodes,
                                  int32_t root, const SqlColumnResolver& resolver,
                                  SqlError& error) {
    _nodes.clear();
    _root = -1;
    _bound = false;

    if (root < 0) {
        // No WHERE clause: every row qualifies.
        _bound = true;
        return true;
    }

    int32_t boundRoot = -1;
    if (!bindNode(nodes, root, resolver, boundRoot, error)) {
        return false;
    }
    if (!isTruthNode(boundRoot)) {
        error = semanticError(
            "predicate must be a Boolean expression (comparison, IS NULL or "
            "logical combination)",
            nodes[root].line, nodes[root].column);
        return false;
    }
    _root = boundRoot;
    _bound = true;
    return true;
}

bool BoundPredicate::bindNode(const std::vector<SqlExprNode>& nodes, int32_t index,
                              const SqlColumnResolver& resolver, int32_t& outIndex,
                              SqlError& error) {
    if (index < 0 || static_cast<size_t>(index) >= nodes.size()) {
        error = semanticError("malformed predicate expression", 0, 0);
        return false;
    }
    const SqlExprNode& src = nodes[static_cast<size_t>(index)];

    switch (src.kind) {
    case SqlExprKind::ColumnRef: {
        size_t found = 0;
        DbType type = DbType::Unknown;
        if (!resolver.resolveColumn(src.identifier, found, type, error)) {
            return false;
        }
        Node node;
        node.kind = Kind::Column;
        node.ordinal = found;
        node.type = type;
        _nodes.push_back(node);
        outIndex = static_cast<int32_t>(_nodes.size() - 1);
        return true;
    }

    case SqlExprKind::Literal: {
        Node node;
        node.kind = Kind::Literal;
        node.type = literalKindToType(src.literal.kind);
        node.literal = literalToValue(src.literal);
        _nodes.push_back(node);
        outIndex = static_cast<int32_t>(_nodes.size() - 1);
        return true;
    }

    case SqlExprKind::Compare: {
        int32_t left = -1;
        int32_t right = -1;
        if (!bindNode(nodes, src.left, resolver, left, error) ||
            !bindNode(nodes, src.right, resolver, right, error)) {
            return false;
        }
        const Node& ln = _nodes[static_cast<size_t>(left)];
        const Node& rn = _nodes[static_cast<size_t>(right)];
        if ((ln.kind != Kind::Column && ln.kind != Kind::Literal) ||
            (rn.kind != Kind::Column && rn.kind != Kind::Literal)) {
            error = semanticError(
                "comparison operands must be a column or literal in SQL5",
                src.line, src.column);
            return false;
        }

        Node node;
        node.kind = Kind::Compare;
        node.compareOp = src.compareOp;
        node.left = left;
        node.right = right;

        const bool leftNull = (ln.kind == Kind::Literal && ln.literal.isNull());
        const bool rightNull = (rn.kind == Kind::Literal && rn.literal.isNull());
        if (leftNull || rightNull) {
            // Any comparison with the NULL literal is UNKNOWN (never TRUE).
            node.alwaysUnknown = true;
            _nodes.push_back(node);
            outIndex = static_cast<int32_t>(_nodes.size() - 1);
            return true;
        }

        const DbType lt = ln.type;
        const DbType rt = rn.type;
        if (!isConcreteDbType(lt) || !isConcreteDbType(rt)) {
            error = semanticError("unsupported comparison operand type", src.line,
                                  src.column);
            return false;
        }

        if (lt == DbType::Boolean && rt == DbType::Boolean) {
            if (src.compareOp != SqlCompareOp::Eq && src.compareOp != SqlCompareOp::Ne) {
                error = semanticError(
                    "ordering comparisons are not supported for Boolean values",
                    src.line, src.column);
                return false;
            }
            node.compareType = DbType::Boolean;
        } else if (isNumericType(lt) && isNumericType(rt)) {
            node.compareType =
                (lt == DbType::Float64 || rt == DbType::Float64) ? DbType::Float64
                                                                 : DbType::Int64;
        } else if (lt == DbType::Text && rt == DbType::Text) {
            node.compareType = DbType::Text;
        } else if (lt == DbType::Blob && rt == DbType::Blob) {
            if (src.compareOp != SqlCompareOp::Eq && src.compareOp != SqlCompareOp::Ne) {
                error = semanticError(
                    "ordering comparisons are not supported for Blob values",
                    src.line, src.column);
                return false;
            }
            node.compareType = DbType::Blob;
        } else {
            std::string message = "type mismatch in comparison: ";
            message += dbTypeName(lt);
            message += " vs ";
            message += dbTypeName(rt);
            error = semanticError(message, src.line, src.column);
            return false;
        }

        _nodes.push_back(node);
        outIndex = static_cast<int32_t>(_nodes.size() - 1);
        return true;
    }

    case SqlExprKind::And:
    case SqlExprKind::Or: {
        int32_t left = -1;
        int32_t right = -1;
        if (!bindNode(nodes, src.left, resolver, left, error) ||
            !bindNode(nodes, src.right, resolver, right, error)) {
            return false;
        }
        if (!isTruthNode(left) || !isTruthNode(right)) {
            error = semanticError("logical operands must be Boolean expressions",
                                  src.line, src.column);
            return false;
        }
        Node node;
        node.kind = (src.kind == SqlExprKind::And) ? Kind::And : Kind::Or;
        node.left = left;
        node.right = right;
        _nodes.push_back(node);
        outIndex = static_cast<int32_t>(_nodes.size() - 1);
        return true;
    }

    case SqlExprKind::Not: {
        int32_t operand = -1;
        if (!bindNode(nodes, src.left, resolver, operand, error)) {
            return false;
        }
        if (!isTruthNode(operand)) {
            error = semanticError("NOT operand must be a Boolean expression", src.line,
                                  src.column);
            return false;
        }
        Node node;
        node.kind = Kind::Not;
        node.left = operand;
        _nodes.push_back(node);
        outIndex = static_cast<int32_t>(_nodes.size() - 1);
        return true;
    }

    case SqlExprKind::IsNull: {
        int32_t operand = -1;
        if (!bindNode(nodes, src.left, resolver, operand, error)) {
            return false;
        }
        const Node& on = _nodes[static_cast<size_t>(operand)];
        if (on.kind != Kind::Column && on.kind != Kind::Literal) {
            error = semanticError("IS NULL operand must be a column or literal",
                                  src.line, src.column);
            return false;
        }
        Node node;
        node.kind = Kind::IsNull;
        node.negated = src.negated;
        node.left = operand;
        _nodes.push_back(node);
        outIndex = static_cast<int32_t>(_nodes.size() - 1);
        return true;
    }

    case SqlExprKind::Invalid:
    default:
        error = semanticError("unsupported predicate expression", src.line, src.column);
        return false;
    }
}

bool BoundPredicate::isTruthNode(int32_t index) const {
    if (index < 0 || static_cast<size_t>(index) >= _nodes.size()) {
        return false;
    }
    const Node& node = _nodes[static_cast<size_t>(index)];
    switch (node.kind) {
    case Kind::Compare:
    case Kind::And:
    case Kind::Or:
    case Kind::Not:
    case Kind::IsNull:
        return true;
    case Kind::Column:
        return node.type == DbType::Boolean;
    case Kind::Literal:
        return node.literal.isNull() || node.type == DbType::Boolean;
    }
    return false;
}

SqlTruth BoundPredicate::evaluate(const std::vector<DbValue>& row) const {
    if (!_bound) {
        return SqlTruth::Unknown;
    }
    if (_root < 0) {
        return SqlTruth::True;
    }
    return evalTruth(_root, row);
}

SqlTruth BoundPredicate::evalTruth(int32_t index, const std::vector<DbValue>& row) const {
    const Node& node = _nodes[static_cast<size_t>(index)];
    switch (node.kind) {
    case Kind::Column: {
        if (node.ordinal >= row.size()) {
            return SqlTruth::Unknown;
        }
        const DbValue& value = row[node.ordinal];
        if (value.isNull()) {
            return SqlTruth::Unknown;
        }
        return value.booleanValue() ? SqlTruth::True : SqlTruth::False;
    }
    case Kind::Literal:
        if (node.literal.isNull()) {
            return SqlTruth::Unknown;
        }
        return node.literal.booleanValue() ? SqlTruth::True : SqlTruth::False;
    case Kind::Compare: {
        if (node.alwaysUnknown) {
            return SqlTruth::Unknown;
        }
        const DbValue left = evalValue(node.left, row);
        const DbValue right = evalValue(node.right, row);
        if (left.isNull() || right.isNull()) {
            return SqlTruth::Unknown;
        }
        DbValue coercedLeft;
        DbValue coercedRight;
        if (!coerceValue(left, node.compareType, coercedLeft) ||
            !coerceValue(right, node.compareType, coercedRight)) {
            return SqlTruth::Unknown;
        }
        return compareValues(node.compareOp, node.compareType, coercedLeft, coercedRight)
                   ? SqlTruth::True
                   : SqlTruth::False;
    }
    case Kind::And:
        return sqlTruthAnd(evalTruth(node.left, row), evalTruth(node.right, row));
    case Kind::Or:
        return sqlTruthOr(evalTruth(node.left, row), evalTruth(node.right, row));
    case Kind::Not:
        return sqlTruthNot(evalTruth(node.left, row));
    case Kind::IsNull: {
        const DbValue value = evalValue(node.left, row);
        bool isNull = value.isNull();
        if (node.negated) {
            isNull = !isNull;
        }
        return isNull ? SqlTruth::True : SqlTruth::False;
    }
    }
    return SqlTruth::Unknown;
}

DbValue BoundPredicate::evalValue(int32_t index, const std::vector<DbValue>& row) const {
    const Node& node = _nodes[static_cast<size_t>(index)];
    if (node.kind == Kind::Column) {
        if (node.ordinal >= row.size()) {
            return DbValue::null();
        }
        return row[node.ordinal];
    }
    return node.literal;
}

} // namespace db
} // namespace gxos
