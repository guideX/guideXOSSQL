# SQL9 prepared statements API

## Status and scope

SQL9 adds a caller-owned prepared-statement API above the SQL1–SQL8 executor.
Prepared statements are runtime objects. SQL text containing parameters is
tokenized and parsed once when `SqlEngine::prepare()` is called. The original
SQL9 implementation reused the AST and metadata but selected access paths on
every execution; SQL9R closes that plan-reuse gap as described below. Parameter
values are never written into SQL text or sent back through the tokenizer.

No `.gxdb`, catalog, heap, B+ tree, or WAL format changed. Prepared statements
are local to one `SqlEngine` and are not thread-safe. Use one engine and its
statements from one thread at a time.

## SQL9R: bound physical plan reuse

Preparation builds a statement-local plan with four distinct parts:

1. **Parsed AST.** The single parsed statement retains parameter ordinals.
2. **Semantic bindings.** The plan binds table IDs, column ordinals, predicate
   types, projection and result metadata, JOIN relations, aggregate outputs,
   GROUP BY columns, and ORDER BY terms.
3. **Physical access plan.** It retains the chosen FullScan, IndexLookup, or
   IndexRange path, the selected index ID and key column, plus each JOIN step's
   NestedLoop or IndexNestedLoop choice and index ID.
4. **Per-execution state.** Every call gets fresh parameter values, encoded
   search keys, scans, JOIN candidates, aggregate/group state, result rows,
   LIMIT/OFFSET counters, row locators, mutation plans, and statement savepoints.

While the captured schema generation remains current, rebinding values does
not rebuild the physical plan. Runtime values supply the current index key and
predicate operands to the retained descriptors. A NULL lookup keeps the same
plan and returns no rows for `column = NULL`, consistent with SQL three-valued
logic.

Prepared INSERT/UPDATE/DELETE retain the table ID and column ordinals. They
create a fresh table facade per execution and rediscover matching rows through
the retained index or scan choice. They do not retain row locators or physical
page pointers. Every execution still checks current transaction-visible rows,
foreign keys, uniqueness, defaults, and the existing statement savepoint and
WAL path.

`SqlPreparedStatement::diagnostics()` exposes per-statement
`tokenizeCount`, `parseCount`, `semanticBindCount`,
`physicalPlanBuildCount`, `executionCount`, and `physicalPlanReuseCount`, along
with the selected access path and index IDs. The SQL9R test suite checks one
plan across 5,000 changing indexed SELECT keys, plus range, JOIN, aggregate,
UPDATE/DELETE reuse and a mixed workload exceeding 5,000 prepared executions.

Prepared DML crash matrices use the SQL3 byte-budgeted file provider. They
destroy the engine/database state, reopen the persisted files, and check that
recovery exposes exactly the pre-commit or post-commit rows and valid indexes.
Prepared statements add no WAL record types or on-disk format changes.

Prepare-time validation covers value-driven DML and SELECT semantics. DDL
syntax is parsed at prepare time, but catalog-dependent DDL checks such as an
unknown target, duplicate object, or drop dependency remain execution-time
checks. This SQL9R repair does not move those checks or perform data-dependent
constraint validation at prepare time.

## Prepare one statement

```cpp
SqlEngine connection(database);
SqlPreparedStatement query;
SqlError error = connection.prepare(
    "SELECT Name FROM Users WHERE Id = ?;", query);
if (!error.isOk()) {
    // Handle error.describe().
}
```

`prepare()` accepts exactly one non-empty statement. Leading, trailing, and
repeated empty semicolons are ignored, but two non-empty statements are
rejected. It parses without executing DML or DDL. SELECT and value-driven DML
receive schema/name/type validation during preparation; DDL syntax is parsed
but some DDL catalog checks remain execution-time checks.

The SQL engine accepts at most 256 `?` markers per input. Parameter ordinals
are zero-based and follow lexical order. Markers inside strings and comments
are ordinary text. Named parameters and identifier parameters are unsupported.
Markers are accepted in INSERT values, UPDATE assignments, comparison
operands in WHERE/JOIN ON, LIMIT, and OFFSET. A marker must have a type that
can be inferred from a target column, a typed comparison operand, or the
Int64 LIMIT/OFFSET context. Ambiguous uses fail preparation.

## Parameter metadata and binding

`SqlPreparedStatement::parameters()` returns each ordinal, inferred `DbType`,
whether NULL is allowed in all uses of that ordinal, its source line/column,
and whether LIMIT/OFFSET requires a nonnegative value.

```cpp
statement.bindInt64(0, 42);
statement.bindText(1, "Alice");
statement.bindBoolean(2, true);
statement.bindFloat64(3, 1.25);
statement.bindBlob(4, bytes, byteCount);
statement.bindNull(5);
// Or bind an owned DbValue:
statement.bind(0, DbValue::int64(42));
```

Binding copies Text and BLOB data. Embedded NUL bytes in Text are retained as
bytes. Text and BLOB values are limited to 65,536 bytes. Oversized values,
invalid indexes, disallowed NULLs, non-finite Float64 values, and type
mismatches return `ParameterError`; failed binds do not replace a previous
valid binding. Exact types are required, except Int32 can be promoted to an
Int64 parameter. Int64 is never silently narrowed; when the destination is
Int32, the existing checked SQL conversion rejects out-of-range values.
Text is never parsed into a number.

Comparison parameters infer the compared column's type. NULL is accepted in a
comparison context and retains SQL three-valued logic (`column = NULL` is
UNKNOWN). INSERT/UPDATE parameters inherit destination nullability; NULL for a
NOT NULL destination is rejected at bind time. LIMIT/OFFSET parameters are
non-null, nonnegative Int64 values.

Every parameter must be bound before execution. Rebinding replaces that
ordinal. `clearBindings()` makes all parameters unbound. A failed execution,
including a constraint failure, leaves the statement available for rebinding
and reuse.

## Execute and results

```cpp
SqlPreparedStatement insert;
connection.prepare("INSERT INTO Users (Id, Name) VALUES (?, ?);", insert);
insert.bindInt64(0, 1);
insert.bindText(1, "Alice");
SqlStatementResult write = insert.execute();

SqlPreparedStatement find;
connection.prepare("SELECT Name FROM Users WHERE Id = ?;", find);
find.bindInt64(0, 1);
SqlStatementResult read = find.executeQuery();
```

`execute()` returns the same `SqlStatementResult` and `SqlResultSet` used by
ordinary SQL. DML affected-row counts are per execution. SELECT result-column
names, types, and nullability are available from `resultColumns()` after
prepare; actual rows are returned by execution. Result sets are materialized
and owned by the returned result object. They do not retain a table cursor or
page pointer.

Prepared statements participate in transactions owned by the same
`SqlEngine`. `BEGIN`, prepared DML/SELECT, `COMMIT`, and `ROLLBACK` therefore
use the established transaction visibility and statement-savepoint behavior.
Each standalone DML execution continues to use the engine's existing implicit
transaction behavior.

## Schema changes and lifetime

Each prepared statement records the database's volatile, global schema
generation. Any successful CREATE/DROP/ALTER operation advances it, including
transaction-local DDL. Rollback does not restore the previous generation.
Thus any schema change conservatively stales every prepared statement, even
when it affects an unrelated table. A stale statement returns
`SchemaChanged`; prepare it again to obtain current schema and indexes.

The generation is process/runtime state and is not stored in the database.
Execution after `Database::close()` returns `DatabaseClosed`. The statement
must not outlive its `SqlEngine` or `Database`; it is movable and non-copyable.
There is no automatic reprepare after a schema change.

## Access paths, diagnostics, and security

Prepared execution exposes the selected `accessPath`, index name, and join
diagnostics as ordinary SQL does. A bound primary-key value can use
`IndexLookup`, ranges can use `IndexRange`, and equality joins can use
`IndexNestedLoop`. `physicalPlanBuildCount` distinguishes the single planning
pass from repeated execution.

The prepared object retains an AST containing explicit parameter ordinals.
Execution copies that AST and converts those nodes to typed in-memory literal
nodes for the established executor. It never escapes, quotes, or concatenates
parameter bytes into SQL. For example, `"'; DROP TABLE Users; --"` bound to a
Text value is data and cannot become another statement.

## Unsupported features and current limitations

- Named, numbered, array, and table-valued parameters.
- Parameters in identifiers, type names, schema objects, or DDL defaults.
- Multi-statement prepare, persistent plans, and a global plan cache.
- Automatic reprepare, fine-grained schema dependencies, or concurrent use.
- Native guideXOS VFS and .NET/REXX bindings.

Prepared DDL without parameters can be parsed and executed. Some DDL semantic
errors are reported by execution rather than `prepare()`. Prepared DDL is
invalidated by its own successful schema change along with every other
prepared statement.
