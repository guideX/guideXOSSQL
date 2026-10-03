#pragma once
// guideXOS SQL -- Phase SQL4
// SQL language front-end primitives: resource limits, the SQL error taxonomy,
// the token classes and the token container produced by the tokenizer.
//
// This header is deliberately independent of the storage/relational layer
// except for DbType (so a type keyword can be mapped to a concrete column
// type). It contains no page, WAL or catalog concepts.

#include <cstdint>
#include <string>
#include <vector>

#include "database_types.h"

namespace gxos {
namespace db {

// ---------------------------------------------------------------------------
// Resource limits (SQL4 section 6).
//
// SQL text is untrusted. Every bounded quantity the tokenizer or parser can
// grow is capped here so hostile input cannot force unbounded allocation or
// unbounded recursion.
// ---------------------------------------------------------------------------

// Maximum size of a single SQL input string in bytes.
const size_t kSqlMaxInputBytes = 1024u * 1024u; // 1 MiB

// Maximum number of tokens produced for one input string.
const size_t kSqlMaxTokens = 200000u;

// Maximum identifier length in bytes (matches the SQL2 policy).
const size_t kSqlMaxIdentifierBytes = 64u;

// Maximum decoded string literal length in bytes (matches kMaxTextBytes).
const size_t kSqlMaxStringLiteralBytes = 65536u;

// Maximum decoded blob literal length in bytes (matches kMaxBlobBytes).
const size_t kSqlMaxBlobLiteralBytes = 65536u;

// Maximum numeric literal length in bytes.
const size_t kSqlMaxNumericLiteralBytes = 64u;

// Maximum column definitions in one CREATE TABLE (matches kMaxColumnsPerTable).
const size_t kSqlMaxColumnsPerCreate = 64u;

// Maximum values in one INSERT ... VALUES (...).
const size_t kSqlMaxValuesPerInsert = 64u;

// Maximum projection entries in one SELECT.
const size_t kSqlMaxSelectColumns = 64u;

// Maximum number of statements accepted in one input string.
const size_t kSqlMaxStatements = 4096u;

// Maximum parser nesting depth (parenthesized expressions and NOT chains).
const size_t kSqlMaxNestingDepth = 32u;

// Maximum number of expression AST nodes in one predicate (SQL5 section 6).
// Depth alone is not enough: a flat 1 MiB query could otherwise allocate an
// unbounded number of nodes.
const size_t kSqlMaxExprNodes = 4096u;

// Maximum ORDER BY terms in one SELECT.
const size_t kSqlMaxOrderByColumns = 64u;

// Maximum assignments in one UPDATE SET list.
const size_t kSqlMaxUpdateAssignments = 64u;

// Maximum rows materialized for one SELECT result set.
const size_t kSqlMaxResultRows = 100000u;

// Maximum approximate payload bytes materialized for one SELECT result set.
const size_t kSqlMaxResultBytes = 64u * 1024u * 1024u;

// Maximum number of rows/locators one UPDATE/DELETE statement may target.
const size_t kSqlMaxMutationTargets = 100000u;

// Maximum approximate bytes held while planning one UPDATE/DELETE statement
// (replacement row payloads). Bounds a compact statement that matches a huge
// number of large rows.
const size_t kSqlMaxMutationBytes = 64u * 1024u * 1024u;

// Maximum approximate bytes held by the ORDER BY working set. ORDER BY
// materializes qualifying rows before sorting; this bounds that buffer.
const size_t kSqlMaxSortBytes = 64u * 1024u * 1024u;

// Maximum qualifying rows materialized for an ORDER BY working set.
const size_t kSqlMaxSortRows = 100000u;

// ---------------------------------------------------------------------------
// Error taxonomy (SQL4 section 20).
// ---------------------------------------------------------------------------

enum class SqlErrorCode {
    Ok = 0,
    TokenizerError,
    SyntaxError,
    SemanticError,
    ExecutionError,
    TransactionError,
    ResourceLimit,
    Unsupported
};

const char* sqlErrorCodeName(SqlErrorCode code);

// A bounded SQL error carrying an optional source location. Callers branch on
// `code`; `message` is for diagnostics only.
struct SqlError {
    SqlErrorCode code;
    std::string message;
    uint32_t offset; // zero-based byte offset into the SQL input
    uint32_t line;   // one-based line, 0 when unknown
    uint32_t column; // one-based column, 0 when unknown
    bool hasLocation;

    SqlError()
        : code(SqlErrorCode::Ok), offset(0), line(0), column(0),
          hasLocation(false) {}

    bool isOk() const { return code == SqlErrorCode::Ok; }

    // Renders "Code: message (line L, column C)".
    std::string describe() const;
};

// ---------------------------------------------------------------------------
// Token classes (SQL4 section 5).
// ---------------------------------------------------------------------------

enum class SqlTokenKind {
    // Keywords.
    Create,
    Table,
    Insert,
    Into,
    Values,
    Select,
    From,
    Begin,
    Commit,
    Rollback,
    Not,
    Null,
    Boolean,
    Bool,
    Int,
    Int32,
    Int64,
    BigInt,
    Double,
    Float64,
    Text,
    Blob,
    True,
    False,
    // SQL5 predicate and mutation keywords.
    Where,
    Update,
    Set,
    Delete,
    Order,
    By,
    Asc,
    Desc,
    Limit,
    Offset,
    And,
    Or,
    Is,
    // Literals.
    Identifier,
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,
    BlobLiteral,
    // Punctuation.
    Comma,
    LeftParen,
    RightParen,
    Semicolon,
    Star,
    // SQL5 comparison operators.
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    // End of input.
    EndOfInput
};

const char* sqlTokenKindName(SqlTokenKind kind);

// True for every reserved keyword token.
bool sqlTokenIsKeyword(SqlTokenKind kind);

// True for the type-name keywords (BOOLEAN, INT64, TEXT, ...).
bool sqlTokenIsTypeKeyword(SqlTokenKind kind);

// Maps a type-name keyword to its concrete SQL2 type. Returns false for
// non-type tokens.
bool sqlTypeKeywordToDbType(SqlTokenKind kind, DbType& out);

// A single token with source location. `text` holds the original lexeme for
// keywords and identifiers and the decoded contents for string literals.
// `bytes` holds the decoded payload for blob literals. Numeric literals carry
// their checked value.
struct SqlToken {
    SqlTokenKind kind;
    std::string text;
    std::vector<uint8_t> bytes;
    int64_t int64Value;
    double float64Value;
    uint32_t offset;
    uint32_t line;
    uint32_t column;

    SqlToken()
        : kind(SqlTokenKind::EndOfInput), int64Value(0), float64Value(0.0),
          offset(0), line(1), column(1) {}
};

} // namespace db
} // namespace gxos
