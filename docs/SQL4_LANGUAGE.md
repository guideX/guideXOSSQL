# guideXOS SQL — Phase SQL4: SQL Language Layer

SQL4 is the first SQL *language* phase. It adds a small, deterministic SQL
dialect on top of the relational and transaction engine proven in SQL1–SQL3:

```
SQL text
   |
SqlTokenizer      bounded, iterative lexer
   |
SqlParser         recursive-descent, bounded depth
   |
AST               owned value nodes
   |
SqlEngine         semantic validation + execution
   |
Database / Transaction / Table / Catalog
   |
WAL / Buffer / Pages
```

The parser never manipulates page IDs, heap pages, catalog serialization, WAL
records or `.gxdb` bytes, and the storage layer never parses SQL. SQL4 requires
**no `.gxdb` or `.gxwal` format change**.

## Supported statements

```sql
CREATE TABLE name ( column_def, ... );
INSERT INTO name VALUES ( value, ... );
SELECT * FROM name;
SELECT column, column FROM name;
BEGIN;
COMMIT;
ROLLBACK;
```

`;` terminates a statement. Multiple statements may appear in one input string.
For the final statement the terminating `;` is optional. Runs of `;` with no
statement between them are accepted and ignored (empty statements).

## Grammar summary

```
input        := (statement? ';')* statement? EOF
statement    := create_table | insert | select | begin | commit | rollback

create_table := CREATE TABLE identifier
                '(' column_def (',' column_def)* ')'
column_def   := identifier type nullability?
nullability  := NULL | NOT NULL          -- omitted => nullable

insert       := INSERT INTO identifier
                VALUES '(' value (',' value)* ')'

select       := SELECT ('*' | identifier (',' identifier)*)
                FROM identifier

begin        := BEGIN
commit       := COMMIT
rollback     := ROLLBACK

value        := literal | TRUE | FALSE | NULL
type         := BOOLEAN | BOOL | INT | INT32 | INT64 | BIGINT
              | DOUBLE | FLOAT64 | TEXT | BLOB
```

## Keywords and identifiers

- Keywords are **ASCII case-insensitive**: `select`, `SELECT` and `SeLeCt` are
  the same token.
- Identifiers are **not normalized** and are compared **byte-wise and
  case-sensitively**, exactly as in SQL2. `Users`, `users` and `USERS` are
  three distinct table names.
- Identifiers must be non-empty, valid UTF-8 and at most 64 bytes. Identifier
  validation for `CREATE TABLE` is enforced by the SQL2 catalog; the tokenizer
  enforces the 64-byte bound for every identifier.
- Quoted identifiers (`"My Table"`) are **not** supported in SQL4.

## Literals

| Literal | Syntax | Notes |
| --- | --- | --- |
| Integer | `0`, `42`, `-42`, `9223372036854775807` | signed 64-bit; overflow rejected |
| Float | `3.14159`, `-0.5`, `1.0`, `.5`, `1e3`, `-2.5e-2` | IEEE 754 binary64; non-finite rejected |
| String | `'Alice'`, `'Leon''s database'`, `''` | single quotes; a doubled quote is one quote; no backslash escapes |
| Blob | `X'001122AABBCC'`, `X''` | hex digits, case-insensitive, even count required |
| Boolean | `TRUE`, `FALSE` | maps to `DbType::Boolean` |
| NULL | `NULL` | nullable columns only |

## Type aliases

| SQL4 type | SQL2 type |
| --- | --- |
| `BOOLEAN`, `BOOL` | `DbType::Boolean` |
| `INT`, `INT32` | `DbType::Int32` |
| `INT64`, `BIGINT` | `DbType::Int64` |
| `DOUBLE`, `FLOAT64` | `DbType::Float64` |
| `TEXT` | `DbType::Text` |
| `BLOB` | `DbType::Blob` |

Precision/length declarations are rejected rather than approximated:
`VARCHAR(100)`, `CHAR(20)`, `DECIMAL(10,2)` and `TEXT(10)` are errors.

## NULL behavior

A column is **nullable by default**. `NOT NULL` forbids `NULL`. Inserting
`NULL` into a `NOT NULL` column is a semantic error. The SQL2 nullability rules
remain authoritative; the SQL engine performs no coercion beyond literal
fitting.

## INSERT value fitting

Values are checked against the destination column type before the row is handed
to the relational API (which re-validates):

- integer literal → `Int32` (range checked), `Int64`, or `Float64`
- float literal → `Float64`
- string literal → `Text`
- blob literal → `Blob`
- `TRUE`/`FALSE` → `Boolean`
- `NULL` → any nullable column

A mismatch, a wrong value count, an out-of-range integer, an oversized value or
a `NULL` into `NOT NULL` is a semantic error.

## SELECT semantics

- `SELECT *` returns every column in schema order.
- `SELECT a, b` returns exactly the requested columns in the requested order,
  even when that differs from the physical schema order.
- Every selected column is resolved against the durable schema, case-sensitively.
  An unknown table or unknown column is a semantic error.
- SQL4 has no `WHERE`, expressions, joins, aggregates, `ORDER BY` or `LIMIT`.

## Result sets

`SqlResultSet` exposes column names, column types, column count, row count and
typed `DbValue` cells including NULL state. Results are **materialized** with
explicit bounds (see resource limits); a cursor/streaming result can replace
this later without changing the facade.

## Transactions

`BEGIN`, `COMMIT` and `ROLLBACK` map directly to the SQL3 single-writer
transaction:

- `BEGIN` starts one real transaction (`TransactionAlreadyActive` if one is
  already open).
- While a transaction is active, `CREATE TABLE` and `INSERT` participate in it;
  they do **not** start independent implicit transactions.
- `SELECT` inside a transaction reads the transaction view, so it observes the
  caller's own uncommitted writes (read-your-writes).
- `COMMIT` persists through the WAL; `ROLLBACK` leaves no persistent state.
- Outside `BEGIN`, each mutating statement is wrapped in its own atomic
  implicit transaction, so SQL execution is never weaker than the native API.
- `COMMIT`/`ROLLBACK` with no active transaction is an error.
- If a statement fails inside an active transaction, the transaction **stays
  active**. The caller may continue or issue `ROLLBACK`. SQL4 does not adopt a
  PostgreSQL-style "failed transaction" state.

## Multi-statement behavior

- Tokenization runs over the whole input first; a tokenizer error aborts the
  entire input with **no** statements executed.
- Parsing and execution proceed statement by statement.
- Execution stops at the first error and returns the results of all prior
  completed statements plus the failing error. Statements already committed
  (implicit transactions or an explicit `COMMIT`) are **not** rolled back.

## Error taxonomy

`SqlErrorCode`: `TokenizerError`, `SyntaxError`, `SemanticError`,
`ExecutionError`, `TransactionError`, `ResourceLimit`, `Unsupported`.

Errors carry a message and, where available, the source byte offset, line and
column. Internal C++ exceptions are never exposed to callers; the engine
returns structured results.

## Resource limits

SQL input is untrusted. Every bounded quantity is capped:

| Limit | Value |
| --- | --- |
| Maximum input size | 1 MiB (`kSqlMaxInputBytes`) |
| Maximum token count | 200,000 (`kSqlMaxTokens`) |
| Maximum identifier length | 64 bytes (`kSqlMaxIdentifierBytes`) |
| Maximum string literal length | 65,536 bytes (`kSqlMaxStringLiteralBytes`) |
| Maximum blob literal length | 65,536 bytes (`kSqlMaxBlobLiteralBytes`) |
| Maximum numeric literal length | 64 bytes (`kSqlMaxNumericLiteralBytes`) |
| Maximum columns per `CREATE TABLE` | 64 (`kSqlMaxColumnsPerCreate`) |
| Maximum values per `INSERT` | 64 (`kSqlMaxValuesPerInsert`) |
| Maximum projection entries | 64 (`kSqlMaxSelectColumns`) |
| Maximum statements per input | 4,096 (`kSqlMaxStatements`) |
| Maximum parser nesting depth | 32 (`kSqlMaxNestingDepth`) |
| Maximum materialized rows per `SELECT` | 100,000 (`kSqlMaxResultRows`) |
| Maximum materialized result bytes per `SELECT` | 64 MiB (`kSqlMaxResultBytes`) |

The tokenizer is iterative and never allocates based on an unbounded length
discovered in hostile input. Over-limit input is rejected explicitly.

## Whitespace and comments

Whitespace: space, tab, CR, LF. Comments:

```sql
-- line comment
/* block comment */
```

Block comments are **not** nested. An unterminated block comment or string is a
tokenization error.

## Unsupported syntax (reserved for SQL5+)

`UPDATE`, `DELETE`, `DROP TABLE`, `ALTER TABLE`, `WHERE`, `JOIN`, `ORDER BY`,
`GROUP BY`, `HAVING`, `LIMIT`, `OFFSET`, `DISTINCT`, `PRIMARY KEY`,
`FOREIGN KEY`, `UNIQUE`, `CHECK`, `DEFAULT`, `CREATE INDEX`, `DROP INDEX`,
views, subqueries, functions, aggregates, query optimization, prepared
statements, parameters, stored procedures and triggers are intentionally not
implemented. Column-list `INSERT` is also deferred.

## Hosted CLI

```sh
build/gxdb_cli sql app.gxdb "SELECT * FROM Users;"
build/gxdb_cli run app.gxdb schema.sql
build/gxdb_cli shell app.gxdb
```

The CLI is a thin formatter over `SqlEngine`; no SQL logic lives in it.
