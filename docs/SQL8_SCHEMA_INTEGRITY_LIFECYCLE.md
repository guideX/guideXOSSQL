# guideXOS SQL — Phase SQL8: Schema Lifecycle and Relational Integrity

SQL8 moves the engine from query expressiveness to **schema lifecycle and
relational integrity**. It adds:

- typed column `DEFAULT` literals;
- `INSERT` column lists and the `DEFAULT` keyword in `VALUES`;
- single-column `FOREIGN KEY ... REFERENCES ...` constraints with
  RESTRICT / NO ACTION semantics;
- system-owned foreign-key support indexes;
- `DROP INDEX`;
- `DROP TABLE`;
- `ALTER TABLE ... ADD COLUMN`.

The acceptance question is whether guideXOS SQL can preserve cross-table
referential integrity and safely evolve or remove schema objects
transactionally without creating dangling catalog metadata, stale indexes,
invalid rows, or crash-visible partial schema state. The hosted proof answers
**yes**.

Layering is preserved: the SQL parser/AST produces schema operations; a DDL
semantic layer validates them; the relational catalog/schema manager records
them; table/index/FK maintenance runs through the SQL5 transaction-private page
path; the SQL3 WAL makes the change durable. The parser never touches catalog
bytes, heap pages, B+ tree pages, WAL records or page allocation.

---

## 1. Catalog v4

SQL6 introduced catalog v3. SQL8 introduces an explicitly versioned **catalog
v4** to persist column defaults, foreign-key records, dependency ownership and
system/support-index ownership. Backward compatibility is preserved:

| Catalog | Phase     | Opens in SQL8 |
| ------- | --------- | ------------- |
| v1      | SQL1      | yes (empty)   |
| v2      | SQL2–SQL5 | yes           |
| v3      | SQL6–SQL7 | yes           |
| v4      | SQL8      | yes           |

A database that does not use SQL8 features is not rewritten: creating plain
tables and indexes still produces a v2/v3 catalog. The first SQL8 schema feature
(any column `DEFAULT`, any foreign key, or `ALTER ADD COLUMN`) upgrades the
catalog to v4 transactionally. Older bytes are never reinterpreted
ambiguously — the version field selects the parser, and v4 serialization is
version-gated (v2/v3 records carry no default or ownership metadata).

### v4 record layout

The v4 root header is 48 bytes (v3 is 40). The continuation header is
byte-identical to v3. Records are type-tagged:

```
u8 recordType (1 = table, 2 = index, 3 = foreign key), then the payload
```

- **table record**: unchanged 40-byte header + columns; each column record
  carries an explicit `hasDefault` flag and, when set, a typed default payload.
- **index record**: v4 adds `u32 ownerForeignKeyId`; the `flags` word gains a
  `system-owned` bit (bit 2).
- **foreign-key record** (36 bytes):
  `u32 foreignKeyId, u32 childTableId, u32 childColumnOrdinal, u32 parentTableId,
   u32 parentColumnOrdinal, u32 referencedIndexId, u32 supportIndexId,
   u16 flags, u16 formatVersion, u32 reserved`.

---

## 2. DEFAULT metadata

A column may have no default or exactly one **typed literal** default. The
logical value is serialized explicitly; SQL source text is never persisted and
reparsed.

| Type    | Serialized payload                     |
| ------- | -------------------------------------- |
| NULL    | type tag = column type, null flag = 1  |
| Boolean | 1 byte                                 |
| Int32   | 4 bytes little-endian                  |
| Int64   | 8 bytes little-endian                  |
| Float64 | 8 bytes IEEE-754 little-endian         |
| Text    | u32 length + UTF-8 bytes               |
| Blob    | u32 length + bytes                     |

A `NULL` default stores the column's declared type so the record is
self-describing. Corruption of the type tag, length, payload or NULL/default
flags is rejected safely and no allocation is made from an untrusted length.

### Scope and rules

DEFAULT values are **literal constants only**. `CURRENT_TIMESTAMP`,
`CURRENT_DATE`, `UUID()`, functions, expressions, sequence values and computed
columns are not supported. A default must type-check against the destination
column when the schema is created or altered; invalid defaults are rejected
before any catalog mutation.

- `Value INT64 NOT NULL DEFAULT NULL` is rejected.
- `Value INT64 NULL DEFAULT NULL` is accepted.
- A `NOT NULL` column may have no default, but an `INSERT` that omits it then
  fails.
- Duplicate `DEFAULT` and conflicting `NULL NOT NULL` attributes are rejected.

---

## 3. INSERT column lists and DEFAULT

`INSERT INTO t (c1, c2, ...) VALUES (v1, v2, ...)` maps values to columns by the
listed order, independent of physical schema order. Omitted columns receive, in
order:

1. their `DEFAULT`, if one exists;
2. otherwise `NULL` if nullable;
3. otherwise the statement fails.

Rejected: unknown column, duplicate column, empty list, mismatched value count,
incompatible value type, and missing `NOT NULL` / no-default column.

`DEFAULT` in a `VALUES` position means "use the declared default for that
destination column". If the column has no default it is an error; `DEFAULT` is
never silently treated as `NULL`. The two forms are equivalent when defaults
exist:

```sql
INSERT INTO T (Id) VALUES (1);
INSERT INTO T VALUES (1, DEFAULT, DEFAULT);
```

---

## 4. Foreign keys

SQL8 implements **single-column** foreign keys only. A foreign key references
exactly one column in exactly one table. The referenced column must be backed by
a `PRIMARY KEY` or `UNIQUE` constraint/index; an ordinary non-unique index is
not acceptable.

```sql
CREATE TABLE Orders (
    Id INT64 PRIMARY KEY,
    UserId INT64 NOT NULL,
    Amount FLOAT64,

    FOREIGN KEY (UserId) REFERENCES Users (Id)
);
```

- Composite keys (`FOREIGN KEY (A, B) ...`) are rejected.
- Named constraints are not required.
- `ON DELETE CASCADE` / `SET NULL` / `ON UPDATE ...` are rejected as
  unsupported; requested referential actions are never silently ignored.

### Type compatibility

Child and referenced parent columns must use the same logical `DbType`. No
implicit promotion is allowed (`Int32 → Int64`, `Float64 → Int64`,
`Text → numeric` all fail). Indexable relationship types are `Boolean`,
`Int32`, `Int64`, `Float64` and `Text` (subject to SQL6 Float64/index rules);
BLOB foreign keys are not supported.

### NULL semantics

A nullable child FK may contain `NULL`, meaning no referenced parent row is
required. Many `NULL` values are allowed and FK checking skips them; SQL NULL
semantics are used, never a lookup of a `NULL` parent key.

### Parent-column requirements

Before committing table creation the engine validates that the parent table and
referenced column exist, that the referenced column has a PK/UNIQUE index, that
the types match, and that child and parent metadata are compatible.

### Self-reference

Self-referential foreign keys are supported:

```sql
CREATE TABLE Employees (
    Id INT64 PRIMARY KEY,
    ManagerId INT64 NULL,
    FOREIGN KEY (ManagerId) REFERENCES Employees (Id)
);
```

A newly created table has no rows, so the relationship is established safely in
the same `CREATE TABLE` transaction. General mutually cyclic table creation is
not required (the referenced table must already exist, and `ALTER ADD FOREIGN
KEY` is out of scope); self-reference is the only supported cycle.

### Metadata

Each FK record stores a stable `foreignKeyId`, child table/column, parent
table/column, referenced unique/PK index id, child-support index id, and
version/flags. Relationships are never stored by fragile object names.
Diagnostics may synthesize `$FK_<id>`-style names, but the stable id is
authoritative.

### Child support index

Every foreign key owns a private, non-unique B+ tree on the child column so
parent DELETE/UPDATE can find child references without a full scan. The support
index is transactional and persistent and is marked **system-owned** with an
`ownerForeignKeyId`, distinct from ordinary, UNIQUE and PRIMARY KEY indexes. A
user cannot drop it directly. SQL8 deliberately creates its own support index
even when an equivalent ordinary user index exists, so `DROP INDEX` can never
remove an FK dependency; a later optimizer/storage phase may deduplicate.

---

## 5. Foreign-key enforcement

FK enforcement lives **below** the SQL language layer. Native `Table` insert /
mutation APIs and SQL `INSERT` / `UPDATE` / `DELETE` all enforce relationships;
SQL syntax is not the only enforcement layer and no supported native mutation
can bypass integrity.

- **Child INSERT**: before committing, a non-NULL child key is encoded with the
  SQL6 canonical index semantics and probed against the referenced PK/UNIQUE
  index through the current transaction view. Exactly one parent must match;
  otherwise a `ForeignKeyViolation` is returned and no heap or index change is
  made durably.
- **Child UPDATE**: a new non-NULL value must reference an existing parent;
  `NULL` is allowed if nullable. The whole SQL5 statement is atomic.
- **Parent DELETE RESTRICT**: a parent row referenced by any visible child row
  is rejected; no parent rows are partially deleted.
- **Parent UPDATE RESTRICT**: changing a referenced parent key while visible
  children reference the old value is rejected atomically. SQL8 does not cascade
  child keys. Changing unrelated parent columns performs no FK rejection.

### Read-your-writes

Explicit transactions use current transaction state. A child inserted in the
same transaction sees the transaction-private parent index entry; a parent
deleted earlier in the transaction is no longer visible, so a later child
insert referencing it is rejected. FKs are not enforced only against committed
state.

### Statement-final-state checking

FK enforcement reasons about the **final logical state of the statement**, not
rows one at a time. Parent-side checks collect all mutations in the batch:
a parent key that is removed (deleted or changed away) is rejected only if a
surviving child row still references it. For a self-referential FK, child rows
that are themselves deleted or moved away in the same statement do not block the
change. Thus:

```
Employees: 1 -> NULL, 2 -> 1, 3 -> 2
DELETE FROM Employees WHERE Id = 1;   -- RESTRICT (row 2 survives)
DELETE FROM Employees;                -- allowed (no child remains in final state)
```

Child-side checks are batch-aware too: a self-referential parent inserted or
moved within the same statement is treated as present, and a parent deleted or
moved away is treated as absent. The outcome never depends on heap iteration
order; SQL5's plan-before-apply / statement-savepoint machinery is reused.

### Multiple keys and child tables

A table may have multiple single-column FKs (each with its own support index),
and one parent may be referenced by many child tables. Parent DELETE is
RESTRICTed if **any** incoming FK has a visible referencing row; dependency
discovery does not stop after the first FK record.

---

## 6. DROP INDEX

```sql
DROP INDEX index_name;
```

Resolved by persistent database-wide name. Unknown indexes are rejected. Only
**user-created ordinary indexes** may be removed. `PRIMARY KEY` backing indexes,
`UNIQUE` constraint indexes, foreign-key support indexes and other system-owned
indexes are rejected with a dependency error. Dropping a user index removes its
catalog visibility, planner availability and schema-discovery visibility; old
B+ tree pages may remain unreachable (SQL8 does not reclaim free pages).

Inside a transaction the planner stops using the dropped index and queries fall
back to another access path or a full scan; `ROLLBACK` restores it and `COMMIT`
persists the removal. Crash recovery exposes either the complete pre-state
(index present and usable) or the post-state (index absent from the catalog and
planner), never a catalog entry referencing partially removed metadata.

---

## 7. DROP TABLE

```sql
DROP TABLE table_name;
```

Unknown tables fail. Dropping a table transactionally removes catalog
visibility for its metadata, columns, PK/UNIQUE indexes, user indexes, outgoing
FK constraints, outgoing FK support indexes and self-referential FK metadata.
Heap/index pages may become unreachable and are not reclaimed.

A table may **not** be dropped while another table has a visible foreign key
referencing it — schema dependency enforcement, not merely row-data enforcement,
and it holds even when the child table currently has zero rows. Within one
transaction the child can be dropped first, removing the incoming dependency in
the transaction-visible catalog, then the parent:

```sql
BEGIN;
DROP TABLE Orders;
DROP TABLE Users;
COMMIT;
```

`ROLLBACK` restores both tables and their relationship. A table containing only
an FK to itself may be dropped: its own self-reference disappears with the same
table and does not block the drop. After `DROP TABLE` inside a transaction, a
query against the table reports an unknown table; `COMMIT` removes it durably.

---

## 8. ALTER TABLE ADD COLUMN

SQL8 supports only:

```sql
ALTER TABLE table_name ADD COLUMN column_definition;
```

The new column is appended at the end of schema ordinal order and may define a
type, `NULL` / `NOT NULL` and a `DEFAULT` literal. `PRIMARY KEY`, `UNIQUE`,
`REFERENCES` / `FOREIGN KEY`, `DROP COLUMN`, `ALTER COLUMN`, `RENAME`,
`ADD PRIMARY KEY`, `ADD UNIQUE` and `ADD FOREIGN KEY` are rejected. Existing
column ordinals never change.

### Backfill

Every existing row is made compatible with the new schema by appending the
declared `DEFAULT` (or `NULL` if nullable). If the table is non-empty and the new
column is `NOT NULL` with no default, the ALTER is rejected before any durable
mutation. For an **empty** table a `NOT NULL` column without a default may be
added; future inserts that omit the column fail.

### Physical strategy

ALTER prefers an eager transactional rewrite: scan the old rows, remove them,
append the column to the catalog, then re-insert every row with the appended
value. Row expansion may rebuild heap pages, change slots, relocate rows and
allocate new heap pages; SQL5 mutation machinery is reused and every SQL6 index
whose row locators change is remapped even though the indexed logical values are
unchanged. After commit, every persisted row uses the new schema — there are no
mixed old/new persistent row formats.

The table schema version is incremented and persisted transactionally; column
count is not used to infer evolution.

### Statement atomicity and read-your-writes

If ALTER fails because of the transaction page limit, an oversized default /
index key interaction, an allocation failure or a row-rewrite failure, the
statement savepoint restores the old schema, rows, indexes and page-allocation
view; prior successful statements in the same transaction remain. Inside an
active transaction a subsequent `SELECT` sees the new column and defaulted
values; after `ROLLBACK` the column no longer exists. After commit and reopen the
schema includes the column, all existing rows contain the default, future
omitted inserts use the same persisted default, and existing indexes remain
structurally valid.

---

## 9. Schema generation and stale handles

Schema-changing statements increment a catalog/schema generation. Cached table
objects, schema descriptions, index descriptors and query bindings are
invalidated; query execution clears its per-statement table cache whenever a
schema statement runs. A previously opened `Table` handle for a dropped table
fails closed (its catalog record no longer exists) rather than mutating a
missing schema object.

---

## 10. Resource limits

| Bound                          | Value                |
| ------------------------------ | -------------------- |
| Foreign keys per table         | 64                   |
| Foreign keys per database      | 2048                 |
| Columns per table              | SQL2 `kMaxColumnsPerTable` |
| Transaction modified pages     | SQL3 `kMaxTransactionPages` (4096) |
| SQL mutation plan targets/bytes| SQL5 limits          |
| Name length / catalog payload  | SQL1/SQL2 policies   |

Catalog dependency traversal is bounded, cycle-aware and safe against invalid
ids; corrupted catalog counts are validated before allocation. SQL8 supports no
general FK cycles beyond self-reference, but corrupted metadata containing a
cycle is still traversed safely.

---

## 11. Crash semantics

Every schema change uses the SQL3 WAL and transaction-private page images; no
`FOREIGN_KEY`, `DROP_TABLE` or `ALTER_TABLE` WAL record types are introduced.
Recovery exposes only complete pre- or post-states:

- **CREATE TABLE + FK**: none of the table/columns/PK/FK/support-index objects,
  or all of them valid. Never a table without its support index, an FK without a
  table, an index without its FK owner, or a dangling parent reference.
- **Parent + child commit**: neither parent nor child, or both with a valid
  child. Never a committed child without its parent.
- **Parent/child delete**: both still present, or both removed. Never a removed
  parent with a surviving child.
- **DROP INDEX**: the complete valid index, or its complete absence from the
  catalog and planner.
- **DROP TABLE (child then parent)**: both tables present with the FK intact, or
  both absent.
- **ALTER ADD COLUMN**: the old schema with old rows and old locator/index
  state, or the new schema with all rewritten rows and valid remapped indexes.
  Never a mixed-schema state.

Row-level FK violations are caught before commit.

---

## 12. Backward compatibility

Databases produced by catalog v1, v2 and v3 open unchanged. A v3 database with
tables and indexes but no FK metadata behaves as **zero foreign keys**; SQL8
never infers relationships from column naming conventions such as `UserId`.
Foreign keys exist only when explicitly declared. After a SQL8 feature runs, the
catalog is v4 and subsequent reopens read v4.

The outer `.gxdb` file format remains `1.0`. Heap row encoding, B+ tree encoding
and WAL encoding are unchanged; DEFAULT and FK metadata live in catalog
structures, and ALTER rewrites rows using the already-supported current row
encoding.

---

## 13. Diagnostics

Read-only schema diagnostics expose per-column default presence/value, per-table
schema version, per-index ownership kind and `ownerForeignKeyId`, and per-FK id,
child/parent table and column, referenced index and support index. A relational
integrity validator enumerates FK metadata, scans child rows, verifies each
non-NULL child key resolves through the referenced PK/UNIQUE index, validates
the child support index and referenced parent index against the heap, and
detects dangling references, missing support indexes, wrong support-index
ownership, wrong referenced indexes, metadata referencing missing tables or
columns, and orphan support-index records. It never silently repairs anything.

---

## 14. Unsupported in SQL8

- Cascading referential actions: `ON DELETE CASCADE`, `ON DELETE SET NULL`,
  `ON DELETE SET DEFAULT`, `ON UPDATE CASCADE`, `ON UPDATE SET NULL`,
  `ON UPDATE SET DEFAULT`.
- `ALTER TABLE ... ADD FOREIGN KEY`.
- `DROP CONSTRAINT`.
- `ALTER TABLE ... DROP COLUMN`, `ALTER COLUMN`, `RENAME TABLE/COLUMN/INDEX`.
- Composite foreign keys.
- Physical page reclamation (free-page list, `VACUUM`, page reuse, truncation):
  DROP and ALTER may leave unreachable pages, so the file can grow. A later
  maintenance phase can reclaim space.
