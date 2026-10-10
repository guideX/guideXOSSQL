# SQL9 prepared statements API

## Status and scope

SQL9 adds a caller-owned prepared-statement API above the SQL1–SQL8 executor.
Prepared statements are runtime objects. SQL text containing parameters is
tokenized and parsed once when `SqlEngine::prepare()` is called; repeated
`execute()` calls bind copied typed values into a copy of the retained AST and
then use the normal SQL executor, relational APIs, transaction layer,
constraints, indexes, savepoints, and WAL path. Parameter values are never
written into SQL text or sent back through the tokenizer.

This implementation reuses the parsed statement and its inferred parameter
and result metadata. The current executor still binds predicates and selects
the physical access path on every execution. It does **not** yet retain a
pre-bound physical plan, so access-path reuse is a remaining SQL9 acceptance
gap. Global SQL text caches and persistent plans are not implemented.

No `.gxdb`, catalog, heap, B+ tree, or WAL format changed. Prepared statements
are local to one `SqlEngine` and are not thread-safe. Use one engine and its
statements from one thread at a time.

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

Prepared execution uses the ordinary SQL6/SQL7 planner and exposes the same
`accessPath`, index name, and join diagnostics as ordinary SQL. A bound
primary-key value can therefore use `IndexLookup`, ranges can use
`IndexRange`, and equality joins can use `IndexNestedLoop`. This implementation
re-runs that lightweight physical selection for each execution rather than
retaining it in the prepared object; this is the principal remaining plan-reuse
gap.

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
- Full physical plan caching and reuse across repeated execution.
- Prepared-specific SQL3 crash fault matrices have not yet been added; prepared
  mutations use the existing executor and WAL path, but SQL9-specific crash
  recovery proof remains outstanding.
- Native guideXOS VFS and .NET/REXX bindings.

Prepared DDL without parameters can be parsed and executed. Some DDL semantic
errors are reported by execution rather than `prepare()`. Prepared DDL is
invalidated by its own successful schema change along with every other
prepared statement.
