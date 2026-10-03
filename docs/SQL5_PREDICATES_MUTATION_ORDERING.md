# guideXOS SQL — Phase SQL5: Predicates, UPDATE, DELETE, ORDER BY, LIMIT/OFFSET

SQL5 turns the SQL4 read-only language into a data-manipulation language while
keeping the engine deliberately full-scan and index-free. It adds:

```sql
WHERE   UPDATE   SET   DELETE
ORDER BY   ASC   DESC   LIMIT   OFFSET
AND   OR   NOT   IS   NULL   TRUE   FALSE
=   <>   !=   <   <=   >   >=
```

No `.gxdb` and no `.gxwal` format change was required.

## Layering

```
SQL text -> Tokenizer -> Parser -> AST -> Bound predicate / executor
        -> relational row locators + mutation primitives
        -> transactions (private overlay) -> WAL -> storage
```

The SQL layer never touches page bytes, catalog serialization, WAL records or
headers. UPDATE/DELETE are expressed as generic relational primitives: a scan
produces internal row locators, the SQL layer plans a bounded set of
replacements/deletions, and the relational layer applies them.

## Expression grammar

```
select      := SELECT projection FROM table
               [WHERE expr] [ORDER BY order] [LIMIT int] [OFFSET int]
projection  := '*' | ident (',' ident)*
order       := orderterm (',' orderterm)*
orderterm   := ident [ASC | DESC]
update      := UPDATE table SET assignment (',' assignment)* [WHERE expr]
assignment  := ident '=' literal
delete      := DELETE FROM table [WHERE expr]

expr        := orExpr
orExpr      := andExpr (OR andExpr)*
andExpr     := notExpr (AND notExpr)*
notExpr     := NOT notExpr | comparison
comparison  := primary [ (compareOp primary) | (IS [NOT] NULL) ]
primary     := '(' expr ')' | columnRef | literal
literal     := NULL | TRUE | FALSE | integer | float | string | blob
```

Assignment right-hand sides are literals only. No arithmetic, `LIKE`, `IN`,
`BETWEEN`, `CASE`, functions, aggregates, joins or subqueries exist in SQL5.

## Precedence

Highest to lowest:

```
parenthesized expression
comparison / IS NULL
NOT
AND
OR
```

`A = 1 OR B = 2 AND C = 3` means `A = 1 OR (B = 2 AND C = 3)`.
`NOT A = 1 AND B = 2` means `(NOT (A = 1)) AND (B = 2)`.

## Expression AST and resource limits

A predicate is a flat arena (`std::vector<SqlExprNode>` with integer child
indices). It stays copyable and makes bounding the node count independent of
nesting depth. Limits:

| Limit | Value |
| --- | --- |
| Parser nesting depth (`(` and `NOT`) | 32 |
| Expression nodes per predicate | 4096 |
| ORDER BY terms | 64 |
| UPDATE assignments | 64 |
| Mutation target rows | 100000 |
| Mutation plan bytes | 64 MiB |
| ORDER BY working-set rows | 100000 |
| ORDER BY working-set bytes | 64 MiB |

Hostile input (`((((...`, a flat query with thousands of `OR` terms, huge
assignment/ORDER BY lists) fails with `ResourceLimit`, never a stack overflow or
unbounded allocation.

## Semantic binding

Before any row is scanned, every column reference is resolved against the
durable schema to an ordinal and type. Unknown columns and invalid comparison
semantics are rejected *before* data access or mutation. Identifiers keep
SQL2/SQL4 semantics (valid UTF-8, max 64 bytes, byte-wise case-sensitive, no
normalization); keywords are ASCII case-insensitive.

## Three-valued logic

Truth values are `TRUE`, `FALSE`, `UNKNOWN`.

```
FALSE AND UNKNOWN = FALSE     TRUE  AND UNKNOWN = UNKNOWN
TRUE  OR  UNKNOWN = TRUE      FALSE OR  UNKNOWN = UNKNOWN
NOT UNKNOWN = UNKNOWN
```

A row passes `WHERE` only when the predicate is exactly `TRUE`. Both `FALSE` and
`UNKNOWN` are filtered out.

## NULL behavior

Any ordinary comparison with `NULL` is `UNKNOWN`:

```sql
NULL = NULL     -- UNKNOWN
Value = NULL    -- UNKNOWN
```

`= NULL` is **not** rewritten to `IS NULL`. Use `IS NULL` / `IS NOT NULL` to
test for nullness.

## Comparison typing

| Operand types | Operators | Comparison |
| --- | --- | --- |
| Boolean / Boolean | `=`, `<>` | value equality; ordering rejected |
| Int32, Int64 | all | promoted to Int64 |
| any numeric with Float64 | all | promoted to Float64 (see precision note) |
| Text / Text | all | byte-wise unsigned UTF-8 ordering |
| Blob / Blob | `=`, `<>` | byte-wise equality; ordering rejected |
| NULL involved | all | `UNKNOWN` |
| incompatible types | — | `SemanticError`, never an implicit coercion |

There is no implicit text-to-number conversion. Integer literals are `Int64`;
float literals are `Float64`. An out-of-range integer compared to an `Int32`
column is a valid `Int64` comparison, not an error.

**Precision note.** Int64 vs Float64 comparisons convert the integer to a
`double`. Integers above `2^53` may not be exactly representable, so
`Big = 9007199254740992.0` matches both `2^53` and `2^53 + 1`. Int64-vs-Int64
comparisons remain exact.

## WHERE

The same bound predicate evaluator drives `SELECT`, `UPDATE` and `DELETE`. A
bare Boolean column or literal is accepted as a predicate (`WHERE Enabled`).
`WHERE TRUE` keeps every row; `WHERE FALSE` / `WHERE NULL` keep none.

## ORDER BY

```sql
ORDER BY Column [ASC | DESC] [, ...]
```

Order columns are resolved before scanning and may differ from the projection.
Ordering rules:

- Int32/Int64: numeric
- Float64: numeric
- Boolean: `FALSE < TRUE`
- Text: byte-wise unsigned UTF-8
- Blob: rejected with `SemanticError`
- ASC: NULL before non-NULL; DESC: NULL after non-NULL
- equal keys: stable, preserving table-scan order

Because physical row order is not a SQL guarantee, physical relocation after an
expanding UPDATE may change scan order; use `ORDER BY` for deterministic output.

## LIMIT / OFFSET

Nonnegative integer literals only. Negative values are rejected. `OFFSET`
without `LIMIT` is accepted. `LIMIT 0` returns zero rows. Execution order is:

```
FROM -> WHERE -> ORDER BY -> OFFSET -> LIMIT -> projection/delivery
```

Without `ORDER BY`, a bounded `LIMIT` may stop scanning once enough qualifying
rows have passed `OFFSET`; results are still bounded by the SQL4 materialization
limits (100000 rows / 64 MiB). With `ORDER BY`, the bounded qualifying set is
materialized and stably sorted; exceeding the sort bounds returns
`ResourceLimit` rather than silently truncating.

## UPDATE

```sql
UPDATE table SET col = literal [, col = literal ...] [WHERE predicate]
```

Without `WHERE`, every row is targeted. Before mutating anything the executor
validates: table and columns exist, no column is assigned twice, each literal
fits the destination type, `NULL` respects nullability, and TEXT/BLOB sizes are
within limits. The statement returns the affected (matched) row count.

## DELETE

```sql
DELETE FROM table [WHERE predicate]
```

Without `WHERE`, every row is deleted; the table schema survives. The statement
returns the affected row count. `DROP TABLE` remains out of scope.

## Row locators and mutation architecture

A scan yields internal locators `{pageId, slot}`. They are ephemeral, valid only
for the relevant scan/generation, and are never a SQL-visible key. UPDATE/DELETE
use a strict two-stage design:

```
SCAN / PLAN  ->  bounded locators + replacement rows  ->  APPLY
```

Every original row is considered exactly once, so an UPDATE can never update a
row twice because its physical location changed.

## Physical UPDATE strategy

Affected heap pages are rebuilt in place. Replacement rows are validated and
encoded first. If all resulting rows fit the page, the page is rewritten with
new slot metadata and its original `next` link. If not, the prefix that fits is
kept and the overflow rows are relocated to the existing tail page, or to a
newly allocated heap page. Relocation is applied only after every affected page
has been rewritten, so a relocated row is never rewritten twice. Values are
never truncated to keep a row in place.

## Physical DELETE strategy

Deleted slots are dropped and each affected transaction-private heap page is
compacted with its surviving rows, preserving the chain. Empty pages are left
allocated and chained; scanning an empty page is valid and bounded. No free-page
reuse, chain surgery or VACUUM is introduced.

## Row counts

`rowCount` stays exact for clean committed operations: UPDATE leaves it
unchanged, DELETE subtracts the deleted rows, rollback restores the
pre-transaction value, and crash recovery yields the logical committed count.

## Statement atomicity

One SQL mutation statement is atomic inside an already-active transaction.
Before an UPDATE/DELETE inside an explicit transaction, the engine takes an
internal `TransactionSavepoint`: a snapshot of the transaction-private overlay,
catalog and next-page counter. If the statement fails (transaction page limit,
row-size problem, allocation failure, resource limit, relational failure), the
savepoint is restored and the prior statements of the transaction remain
intact. No user-visible `SAVEPOINT` syntax is exposed.

Outside `BEGIN`, each UPDATE/DELETE uses the existing implicit transaction path
and is atomic as one SQL3 transaction. `COMMIT` produces one atomic WAL commit
for all accumulated modifications.

## Transaction / WAL interaction

UPDATE and DELETE only change transaction-private pages and catalog metadata, so
they reuse the SQL3 machinery unchanged: `BEGIN`, complete `PAGE_IMAGE` records,
`DB_HEADER` when the page count changes, `COMMIT`, and the WAL flush durability
point. Recovery therefore exposes either the complete pre-statement state or the
complete committed state, never a mixture.

## Unsupported syntax (SQL5)

No indexes/B+trees, primary keys, UNIQUE, foreign keys, CHECK, DEFAULT, ALTER
TABLE, DROP TABLE, INSERT column lists, arithmetic, LIKE, IN, BETWEEN, CASE,
functions, aggregates, GROUP BY, HAVING, DISTINCT, joins, subqueries, aliases,
quoted identifiers, collations, prepared statements, parameters, views,
triggers, free-page reuse, VACUUM, MVCC, concurrent writers, savepoint syntax,
network protocol, authentication, or guideXOS/REXX/Navigator integrations.
