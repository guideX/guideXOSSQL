# guideXOS SQL — Phase SQL7: JOINs, Aliases, Aggregates, GROUP BY and DISTINCT

SQL7 adds multi-relation and aggregate query semantics on top of the SQL1–SQL6
engine. It is a **query-layer-only** phase: no `.gxdb`, catalog, B+ tree, heap or
WAL format changed. JOIN and aggregate execution never touch pages, index
structures, catalog serialization or WAL records directly; indexed JOIN probes go
through the same authoritative SQL6 index APIs as ordinary indexed SELECTs.

```
SQL text -> tokenizer -> parser/AST -> binder -> join planner
         -> joined/aggregate executor -> table scans / SQL6 indexes
         -> transaction view -> heap / B+ tree / WAL / storage
```

## 1. New grammar

```
SELECT [DISTINCT] select_list
FROM table_ref
     join_clause*
[WHERE predicate]
[GROUP BY column_ref (, column_ref)*]
[ORDER BY order_term (, order_term)*]
[LIMIT n] [OFFSET n]

select_list  := '*' | select_item (',' select_item)*
select_item  := column_ref [AS alias | alias]
              | aggregate  [AS alias | alias]
aggregate    := (COUNT|SUM|AVG|MIN|MAX) '(' ( '*' | column_ref ) ')'
column_ref   := [identifier '.'] identifier
table_ref    := identifier [AS alias | alias]
join_clause  := [INNER] JOIN table_ref ON predicate
              | LEFT [OUTER] JOIN table_ref ON predicate
```

New keywords: `AS`, `JOIN`, `INNER`, `LEFT`, `OUTER`, `DISTINCT`, `COUNT`,
`SUM`, `AVG`, `MIN`, `MAX`, `GROUP`. `.` is a new punctuation token. `JOIN`
without a qualifier means `INNER JOIN`. `BY` and `ON` already existed.

## 2. Qualified identifiers and aliases

A column reference may carry a qualifier: `Users.Id` or `u.Id`. The qualifier is
the relation's **query-local qualifier**:

* Without an alias, the qualifier is the table name (`Users.Id`).
* With an explicit alias (`FROM Users AS u` or `FROM Users u`), the alias
  **replaces** the table name. After `FROM Users AS u`, `Users.Id` is an
  "unknown relation qualifier" error; use `u.Id`.
* Aliases are query-local and do not survive the statement.
* Identifier comparison is byte-wise and **case-sensitive** (the SQL2 rule), so
  `u` and `U` are distinct qualifiers and distinct aliases.

Bound column references are resolved once, before scanning, into
`(source ordinal, column ordinal, type)`. No name resolution happens per result
row.

## 3. Ambiguity rules

* An unqualified name is legal when exactly one visible source exposes a column
  with that name.
* If two or more sources expose it, the statement fails with a SemanticError
  ("ambiguous column"); the engine never silently chooses the leftmost relation.
  Qualify the reference (`A.Id`) or use aliases.
* A qualified reference to an unknown qualifier, or to a column not present in
  that relation, fails before execution.
* Every relation source must have a unique qualifier within one SELECT scope.
  Two sources of the same table therefore require distinct aliases (self-joins).
* `ORDER BY Id` on a joined query is ambiguous and fails if more than one visible
  source exposes `Id`; `ORDER BY u.Id` works.

## 4. JOIN semantics

### INNER JOIN

A pair is emitted only when the complete `ON` predicate evaluates to TRUE.
`FALSE` and `UNKNOWN` do not match. In particular `NULL = NULL` is UNKNOWN and
never creates an equality-join match, consistent with SQL5 three-valued logic.

### LEFT JOIN

For each left-side row, every matching right-side row is emitted; if none match,
exactly one **null-extended** row is emitted in which every column of the right
relation is NULL — even if the underlying storage column is `NOT NULL`.

### Null extension and result nullability

A joined row is a flat vector of every source's decoded columns in relation
order. A null-extended relation is represented in place with typed
`DbValue::Null` values; no persistent row is manufactured. Because a LEFT JOIN
can produce NULL for a physically `NOT NULL` column, result metadata
(`SqlColumn::nullable`) marks outer-joined columns nullable and never assumes
storage nullability equals query-result nullability.

### Multi-join chains

Execution is strictly left-deep in written order. `INNER`/`LEFT` may be mixed:

```sql
SELECT ...
FROM A
JOIN B ON ...
LEFT JOIN C ON ...
JOIN D ON ...;
```

At most 16 relation sources (FROM plus JOINs) are accepted; a larger chain is a
`ResourceLimit` parse error.

## 5. ON predicate

`ON` reuses the SQL5 predicate engine unchanged: qualified/unqualified column
references, literals, comparisons, `AND`, `OR`, `NOT`, `IS [NOT] NULL` and
parentheses, with the same three-valued truth tables. Comparisons may be
column-to-column (`o.UserId = u.Id`) or column-to-literal. There is no separate
JOIN Boolean evaluator. An `ON` predicate may reference the relations joined so
far plus the current right relation (left-deep scope); references to later
sources are rejected.

## 6. Join planner and access paths

SQL7 uses a small deterministic, statistics-free planner. For each written join
step it chooses one of:

* `NestedLoop` — the correctness-first fallback. The right relation is
  materialized once and reused for every left row; the complete `ON` predicate is
  evaluated for each candidate pair.
* `IndexNestedLoop` — selected when an `ON` equality conjunct is
  `already-joined.column = right.indexed.column`.

Deterministic rule:

1. Scan the `ON` conjuncts (flattening `AND`) for an equality between a column of
   the current right relation and a column of an already-joined source.
2. The right column must be indexable and the two column types equality
   comparable (same type, integer/integer, or numeric widened to a Float64 index).
3. Prefer a PRIMARY KEY / UNIQUE index; otherwise take the first applicable
   ordinary index; otherwise use `NestedLoop`.

Joins are **never reordered** and no cost estimation or statistics are used.

### IndexNestedLoop

For each left row the engine:

1. takes the left probe value (from the already-joined sources);
2. skips the probe entirely when the value is NULL (ordinary equality can never
   be TRUE), which for LEFT JOIN produces the null-extended row;
3. encodes the key and probes the SQL6 B+ tree with `Database::indexLookup`;
4. fetches each candidate right row by its locator via `Table::fetchRow`;
5. evaluates the **complete** `ON` predicate and emits actual matches.

The index is a candidate generator only; it never replaces predicate semantics.
Non-unique indexes may return 0, 1 or many candidates, and every candidate is
checked. UNIQUE/PRIMARY KEY probes with a non-NULL key yield at most one
candidate, but this never changes semantics.

### Corruption and transaction visibility

If the chosen index is structurally corrupt, `indexLookup` (or the locator
fetch) returns the SQL6 corruption failure; SQL7 propagates it and never silently
falls back to `NestedLoop`. JOIN probes use the active SQL3 transaction view, so
transaction-local INSERT/UPDATE/DELETE — including transaction-private B+ tree
pages — are visible inside a transaction and disappear after rollback/reopen.

### Diagnostics

`SqlStatementResult::joins` exposes read-only per-step metadata: join type, right
table/alias, access path, selected index name, left rows processed, index probes,
candidates fetched, matches emitted and null-extended rows. The CLI prints it
when `GXDB_CLI_EXPLAIN` is set.

## 7. Aggregates

```
COUNT(*)  COUNT(column)
SUM(column)  AVG(column)  MIN(column)  MAX(column)
```

The argument is a single column reference (or `*` for COUNT only). Arbitrary
expressions such as `SUM(A + B)` are out of scope. Nested aggregates
(`SUM(COUNT(*))`) and aggregates in `WHERE`/`ON` are rejected; `HAVING` is not
implemented.

Result types:

| Aggregate | Input | Output | NULL when no non-NULL input |
|-----------|-------|--------|------------------------------|
| `COUNT(*)` | any | Int64 | never (returns 0) |
| `COUNT(col)` | any | Int64 | never (returns 0) |
| `SUM` | Int32/Int64 | Int64 | yes |
| `SUM` | Float64 | Float64 | yes |
| `AVG` | Int32/Int64/Float64 | Float64 | yes |
| `MIN`/`MAX` | Boolean/Int32/Int64/Float64/Text | same as input | yes |

* `COUNT(*)` counts every input row; `COUNT(column)` counts non-NULL values.
* All aggregates except `COUNT(*)` ignore NULL inputs.
* Integer `SUM` and `AVG` accumulation is overflow-checked; overflow returns a
  structured `ExecutionError`, never a silent wrap.
* `MIN`/`MAX` use the SQL5/SQL6 ordering: FALSE < TRUE, numeric order, unsigned
  byte-wise UTF-8 for Text. `MIN`/`MAX` over Blob are rejected (SQL5 defines BLOB
  equality but not ordering).
* `SUM(TextColumn)` and `AVG(TextColumn)` are rejected before scanning.
* `SUM`/`AVG` for integer input accumulate in a checked Int64 before conversion
  and division; `AVG` result is Float64.

### Aggregates without GROUP BY

If an aggregate is present and no `GROUP BY` is given, the whole filtered/joined
input is one implicit group and exactly one result row is returned, even for zero
matching rows:

```sql
SELECT COUNT(*) FROM EmptyTable;  -- 0
SELECT SUM(V)   FROM EmptyTable;  -- NULL
```

## 8. GROUP BY

`GROUP BY column_ref [, column_ref ...]` — column references only. NULLs in the
same key position form one group (unlike SQL `NULL = NULL`, which is UNKNOWN).
Composite keys are compared with a type-aware, length-safe total order; raw value
bytes are never concatenated. For an aggregate query with GROUP BY, every
non-aggregate projected column must appear in the GROUP BY list, otherwise the
statement is rejected (no arbitrary row is chosen). `SELECT COUNT(*) FROM T
GROUP BY A` is legal and returns one row per group without projecting `A`. Output
order without `ORDER BY` is unspecified; the implementation preserves first-seen
group order for diagnostics only, and tests compare logical rows as
sets/multisets.

## 9. DISTINCT

`SELECT DISTINCT` eliminates duplicate rows from the final projected values
(after aggregation). Equality for duplicate elimination treats two NULLs in the
same position as equal, uses exact byte-wise Text/Blob equality and documented
numeric equality (`-0.0` and `+0.0` are equal). Rows are never stringified for
deduplication. With `DISTINCT`, `ORDER BY` must reference a projected column or
alias (section 10).

Operation order:

```
FROM / JOIN -> WHERE -> GROUP BY / aggregate -> projection
            -> DISTINCT -> ORDER BY -> OFFSET -> LIMIT
```

## 10. ORDER BY

SQL5 `ORDER BY` is extended to qualified joined columns and projection aliases:

* An unqualified name that matches a unique projection alias or output column
  name resolves to that output; the alias wins if it conflicts with a source
  column.
* Otherwise the name resolves to a source column (with the usual ambiguity
  rules). Non-projected source columns may be used for ordinary non-DISTINCT,
  non-aggregate queries, preserving SQL5 behavior.
* In a DISTINCT query, `ORDER BY` must reference a projected column or alias.
* In an aggregate/GROUP BY query, `ORDER BY` may reference a projected column or
  alias, or a GROUP BY column (even if it is not projected).
* `ORDER BY` over Blob is rejected.

## 11. Working-set bounds

SQL7 materializes bounded intermediate rows/groups in memory; there is no disk
spill, external sort, hash join or merge join. Limits (all reported as
`ResourceLimit`, never silent truncation):

| Bound | Value |
|-------|-------|
| relation sources per SELECT | 16 |
| GROUP BY terms | 64 |
| projection items | 64 |
| ORDER BY terms | 64 |
| joined/intermediate rows | 100,000 |
| joined/intermediate bytes | 64 MiB |
| group count | 100,000 |
| group key + aggregate-state bytes | 64 MiB |
| DISTINCT surviving rows | 100,000 |
| DISTINCT working-set bytes | 64 MiB |
| result rows / result bytes | 100,000 / 64 MiB |

## 12. Float64 edge policy

SQL7 does not invent a second Float64 model; it follows the existing SQL5/SQL6
rules:

* Float literals are parsed with `strtod` and rejected if non-finite, so NaN and
  ±Infinity cannot enter through SQL text. The SQL layer never generates
  non-finite Float64 values, so aggregates and grouping operate on finite values.
* `-0.0` and `+0.0` compare equal, so they form one DISTINCT/group value and
  `MIN`/`MAX` may return either zero representation.
* `SUM`/`AVG` accumulate in `double`; `MIN`/`MAX` use the SQL5 ordering
  comparison. If non-finite values were introduced through the native API, the
  host `double` behavior applies and is not special-cased — but this is not
  reachable from SQL.

## 13. Unsupported in SQL7

`RIGHT JOIN`, `FULL JOIN`, `CROSS JOIN`, comma joins, join reordering, hash join,
merge join, cost-based optimization, statistics/histograms, `HAVING`, `UNION`,
`INTERSECT`, `EXCEPT`, subqueries, CTEs, window functions, scalar arithmetic
expressions, `CASE`, `LIKE`, `IN`, `BETWEEN`, scalar/date functions, foreign
keys, `DROP INDEX`, `ALTER TABLE`, `DROP TABLE`, triggers, views, stored
procedures, prepared statements, parameters, MVCC, concurrent writers, network
protocol, authentication, REXX/Navigator integration and GUI tooling remain out
of scope.

## 14. No persistent-format change

SQL7 is entirely read/query semantics. It requires no `.gxdb` format change, no
catalog version change, no B+ tree format change and no WAL format change. SQL6
index corruption detection, locator validation, transaction-private index
visibility and indexed/full-scan equivalence remain mandatory and are reused
unchanged.

## 15. Test coverage

`tests/database_sql7_test.cpp` covers tokenizer/parser additions, hostile and
malformed input, qualified/alias/ambiguity rules, INNER/LEFT/self/multi-table
joins, indexed-vs-forced-NestedLoop equivalence, aggregate functions and NULL
semantics, integer overflow, GROUP BY (including NULL grouping), DISTINCT,
DISTINCT+ORDER/LIMIT ordering, aggregate-over-JOIN, transaction visibility
through JOIN, join-index corruption, Float64 edges, result metadata, a
deterministic 1000/3000/8000-row multi-table workload with an independent model,
and a 250-lifecycle joined/aggregate transaction workload.
