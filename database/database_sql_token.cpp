#include "database_sql_token.h"

namespace gxos {
namespace db {

const char* sqlErrorCodeName(SqlErrorCode code) {
    switch (code) {
    case SqlErrorCode::Ok: return "Ok";
    case SqlErrorCode::TokenizerError: return "TokenizerError";
    case SqlErrorCode::SyntaxError: return "SyntaxError";
    case SqlErrorCode::SemanticError: return "SemanticError";
    case SqlErrorCode::ExecutionError: return "ExecutionError";
    case SqlErrorCode::TransactionError: return "TransactionError";
    case SqlErrorCode::ResourceLimit: return "ResourceLimit";
    case SqlErrorCode::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

std::string SqlError::describe() const {
    std::string out = sqlErrorCodeName(code);
    if (!message.empty()) {
        out += ": ";
        out += message;
    }
    if (hasLocation) {
        out += " (line ";
        out += std::to_string(static_cast<unsigned long long>(line));
        out += ", column ";
        out += std::to_string(static_cast<unsigned long long>(column));
        out += ")";
    }
    return out;
}

const char* sqlTokenKindName(SqlTokenKind kind) {
    switch (kind) {
    case SqlTokenKind::Create: return "CREATE";
    case SqlTokenKind::Table: return "TABLE";
    case SqlTokenKind::Insert: return "INSERT";
    case SqlTokenKind::Into: return "INTO";
    case SqlTokenKind::Values: return "VALUES";
    case SqlTokenKind::Select: return "SELECT";
    case SqlTokenKind::From: return "FROM";
    case SqlTokenKind::Begin: return "BEGIN";
    case SqlTokenKind::Commit: return "COMMIT";
    case SqlTokenKind::Rollback: return "ROLLBACK";
    case SqlTokenKind::Not: return "NOT";
    case SqlTokenKind::Null: return "NULL";
    case SqlTokenKind::Boolean: return "BOOLEAN";
    case SqlTokenKind::Bool: return "BOOL";
    case SqlTokenKind::Int: return "INT";
    case SqlTokenKind::Int32: return "INT32";
    case SqlTokenKind::Int64: return "INT64";
    case SqlTokenKind::BigInt: return "BIGINT";
    case SqlTokenKind::Double: return "DOUBLE";
    case SqlTokenKind::Float64: return "FLOAT64";
    case SqlTokenKind::Text: return "TEXT";
    case SqlTokenKind::Blob: return "BLOB";
    case SqlTokenKind::True: return "TRUE";
    case SqlTokenKind::False: return "FALSE";
    case SqlTokenKind::Where: return "WHERE";
    case SqlTokenKind::Update: return "UPDATE";
    case SqlTokenKind::Set: return "SET";
    case SqlTokenKind::Delete: return "DELETE";
    case SqlTokenKind::Order: return "ORDER";
    case SqlTokenKind::By: return "BY";
    case SqlTokenKind::Asc: return "ASC";
    case SqlTokenKind::Desc: return "DESC";
    case SqlTokenKind::Limit: return "LIMIT";
    case SqlTokenKind::Offset: return "OFFSET";
    case SqlTokenKind::And: return "AND";
    case SqlTokenKind::Or: return "OR";
    case SqlTokenKind::Is: return "IS";
    case SqlTokenKind::Index: return "INDEX";
    case SqlTokenKind::Unique: return "UNIQUE";
    case SqlTokenKind::Primary: return "PRIMARY";
    case SqlTokenKind::Key: return "KEY";
    case SqlTokenKind::On: return "ON";
    case SqlTokenKind::As: return "AS";
    case SqlTokenKind::Join: return "JOIN";
    case SqlTokenKind::Inner: return "INNER";
    case SqlTokenKind::Left: return "LEFT";
    case SqlTokenKind::Outer: return "OUTER";
    case SqlTokenKind::Distinct: return "DISTINCT";
    case SqlTokenKind::Count: return "COUNT";
    case SqlTokenKind::Sum: return "SUM";
    case SqlTokenKind::Avg: return "AVG";
    case SqlTokenKind::Min: return "MIN";
    case SqlTokenKind::Max: return "MAX";
    case SqlTokenKind::Group: return "GROUP";
    case SqlTokenKind::Identifier: return "identifier";
    case SqlTokenKind::IntegerLiteral: return "integer literal";
    case SqlTokenKind::FloatLiteral: return "float literal";
    case SqlTokenKind::StringLiteral: return "string literal";
    case SqlTokenKind::BlobLiteral: return "blob literal";
    case SqlTokenKind::Comma: return "','";
    case SqlTokenKind::LeftParen: return "'('";
    case SqlTokenKind::RightParen: return "')'";
    case SqlTokenKind::Semicolon: return "';'";
    case SqlTokenKind::Star: return "'*'";
    case SqlTokenKind::Dot: return "'.'";
    case SqlTokenKind::Eq: return "'='";
    case SqlTokenKind::Ne: return "'<>'";
    case SqlTokenKind::Lt: return "'<'";
    case SqlTokenKind::Le: return "'<='";
    case SqlTokenKind::Gt: return "'>'";
    case SqlTokenKind::Ge: return "'>='";
    case SqlTokenKind::EndOfInput: return "end of input";
    }
    return "token";
}

bool sqlTokenIsKeyword(SqlTokenKind kind) {
    return kind >= SqlTokenKind::Create && kind <= SqlTokenKind::Group;
}

bool sqlTokenIsTypeKeyword(SqlTokenKind kind) {
    switch (kind) {
    case SqlTokenKind::Boolean:
    case SqlTokenKind::Bool:
    case SqlTokenKind::Int:
    case SqlTokenKind::Int32:
    case SqlTokenKind::Int64:
    case SqlTokenKind::BigInt:
    case SqlTokenKind::Double:
    case SqlTokenKind::Float64:
    case SqlTokenKind::Text:
    case SqlTokenKind::Blob:
        return true;
    default:
        return false;
    }
}

bool sqlTypeKeywordToDbType(SqlTokenKind kind, DbType& out) {
    switch (kind) {
    case SqlTokenKind::Boolean:
    case SqlTokenKind::Bool:
        out = DbType::Boolean;
        return true;
    case SqlTokenKind::Int:
    case SqlTokenKind::Int32:
        out = DbType::Int32;
        return true;
    case SqlTokenKind::Int64:
    case SqlTokenKind::BigInt:
        out = DbType::Int64;
        return true;
    case SqlTokenKind::Double:
    case SqlTokenKind::Float64:
        out = DbType::Float64;
        return true;
    case SqlTokenKind::Text:
        out = DbType::Text;
        return true;
    case SqlTokenKind::Blob:
        out = DbType::Blob;
        return true;
    default:
        return false;
    }
}

} // namespace db
} // namespace gxos
