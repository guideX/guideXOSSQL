# guideXOS SQL — Database Subsystem

Native relational database subsystem for guideXOS. This repository contains:

- **Phase SQL1: Durable Database Storage Foundation** — a bounded, versioned
  `.gxdb` file format and page-storage engine with integrity checking,
  append-only allocation and explicit flush/durability semantics.
- **Phase SQL2: Relational Catalog and Heap Tables** — durable table schemas,
  typed columns, heap-backed row storage, bounded scanning and a small buffer
  manager. No SQL parser yet; the native relational API is the execution
  target for the future SQL layer.
- **Phase SQL3: Write-Ahead Log and Atomic Transactions** — a sidecar
  `.gxwal` redo log plus single-writer transactions make bounded multi-page
  relational mutations crash-atomic: after any interruption, recovery exposes
  either the complete pre-transaction state or the complete committed state.
- **Phase SQL4: SQL Language Layer** — a bounded tokenizer, recursive-descent
  parser, typed AST and execution facade (`SqlEngine`) over the existing
  relational engine. Supports `CREATE TABLE`, `INSERT ... VALUES`, `SELECT`,
  `BEGIN`/`COMMIT`/`ROLLBACK`, multi-statement input, typed literals, NULL,
  string escaping and BLOB literals. No `.gxdb`/`.gxwal` format change.
- **Phase SQL5: Predicates and Data Manipulation** — a bounded expression AST
  with SQL three-valued logic, `WHERE`, `ORDER BY`/`ASC`/`DESC`,
  `LIMIT`/`OFFSET`, `UPDATE ... SET ... WHERE`, and `DELETE ... WHERE`. Rows are
  located through internal ephemeral locators and mutated through generic
  transaction-aware relational primitives with statement-level atomicity. Still
  a full-scan, index-free engine; no `.gxdb`/`.gxwal` format change.
- **Phase SQL6: B+ Tree Indexes and Constraints** — persistent single-column
  B+ tree indexes, `CREATE INDEX` / `CREATE UNIQUE INDEX`, `PRIMARY KEY` and
  `UNIQUE`, and a deterministic rule-based access path that uses an index for
  supported equality/range predicates. Indexes live in the transaction-private
  page/WAL path, are maintained by the relational layer on every INSERT /
  UPDATE / DELETE, survive row relocation and page compaction, and are enforced
  below the SQL layer. Catalog upgraded to a backward-compatible v3; outer
  `.gxdb` format and the WAL format are unchanged.
- **Phase SQL7: JOINs, Aliases, Aggregates, GROUP BY and DISTINCT** — table
  aliases and qualified column references, `INNER JOIN` / `JOIN` and
  `LEFT [OUTER] JOIN`, self-joins and multi-relation chains, `COUNT` / `SUM` /
  `AVG` / `MIN` / `MAX`, `GROUP BY` and `SELECT DISTINCT`, with correct SQL NULL
  semantics. A deterministic, statistics-free join planner chooses
  `NestedLoop` or `IndexNestedLoop`; indexed probes reuse the SQL6 B+ tree APIs
  and never change the logical result. Strictly a query-layer phase: no `.gxdb`,
  catalog, B+ tree, heap or WAL format change.
- **Phase SQL8: Foreign Keys, DEFAULT, DROP and Scoped ALTER TABLE** — typed
  column `DEFAULT` literals, `INSERT` column lists and the `DEFAULT` keyword,
  single-column `FOREIGN KEY ... REFERENCES ...` with RESTRICT / NO ACTION
  semantics, system-owned FK support indexes, `DROP INDEX`, `DROP TABLE` and
  `ALTER TABLE ... ADD COLUMN`. Foreign keys are enforced below the SQL layer
  against the statement's final logical state (including self-references) and
  see transaction-private rows. Catalog upgraded to a backward-compatible v4;
  outer `.gxdb`, heap, B+ tree and WAL formats are unchanged.

- SQL1 format and design: [`docs/SQL1_DATABASE_STORAGE.md`](docs/SQL1_DATABASE_STORAGE.md)
- SQL2 format and design: [`docs/SQL2_RELATIONAL_CATALOG_HEAP.md`](docs/SQL2_RELATIONAL_CATALOG_HEAP.md)
- SQL3 WAL/transaction design: [`docs/SQL3_WAL_TRANSACTIONS.md`](docs/SQL3_WAL_TRANSACTIONS.md)
- SQL4 language reference: [`docs/SQL4_LANGUAGE.md`](docs/SQL4_LANGUAGE.md)
- SQL5 predicates/mutation/ordering: [`docs/SQL5_PREDICATES_MUTATION_ORDERING.md`](docs/SQL5_PREDICATES_MUTATION_ORDERING.md)
- SQL6 B+ tree indexes/constraints: [`docs/SQL6_BTREE_INDEXES_CONSTRAINTS.md`](docs/SQL6_BTREE_INDEXES_CONSTRAINTS.md)
- SQL7 joins/aggregates/grouping: [`docs/SQL7_JOINS_AGGREGATES.md`](docs/SQL7_JOINS_AGGREGATES.md)
- SQL8 schema lifecycle/integrity: [`docs/SQL8_SCHEMA_INTEGRITY_LIFECYCLE.md`](docs/SQL8_SCHEMA_INTEGRITY_LIFECYCLE.md)
- Test inventory: [`docs/SQL1_TEST_REPORT.md`](docs/SQL1_TEST_REPORT.md)

## Layout

```
database/   storage engine (format, checksum, I/O, header, page, file, engine,
            buffer, catalog, heap, index, wal, transaction, relational) and the
            SQL language layer (sql_token, sql_tokenizer, sql_parser, sql)
tests/      hosted test suites (SQL1 + SQL2 + SQL3 + SQL4 + SQL5 + SQL6 + SQL7 + SQL8)
tools/      gxdb_cli (create / inspect / sql / run / shell)
docs/       architecture and test reports
```

## Build and test

Requires CMake ≥ 3.16 and a C++11 compiler (verified with MinGW-w64 g++ 15.2
and Ninja on Windows).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The test binaries can also be run directly: `build/database_storage_tests`,
`build/database_relational_test`, `build/database_transaction_test` and
`build/database_sql_test`.

## Quick example

```cpp
#include "database_relational.h"
using namespace gxos::db;

std::unique_ptr<Database> db;
Database::create("app.gxdb", DatabaseCreateOptions(), db);

TableDefinition users;
users.name = "Users";
users.columns.push_back(ColumnDefinition("Id", DbType::Int64, false));
users.columns.push_back(ColumnDefinition("Name", DbType::Text, false));
users.columns.push_back(ColumnDefinition("Enabled", DbType::Boolean, false));
uint32_t id = 0;
db->createTable(users, id);

std::unique_ptr<Table> table;
db->openTable("Users", table);
table->insert({DbValue::int64(1), DbValue::text("Alice"), DbValue::boolean(true)});

db->flush();
db->close();

// Later:
Database::open("app.gxdb", DatabaseOpenOptions(), db);
db->openTable("Users", table);
std::unique_ptr<TableScan> scan;
table->scanStart(scan);
std::vector<DbValue> row;
while (scan->next(row)) { /* ... */ }
db->close();
```

## SQL quick example

The same SQL text works through the library (`SqlEngine`) and the CLI:

```sql
CREATE TABLE Users (
    Id INT64 NOT NULL,
    Name TEXT NOT NULL,
    Enabled BOOLEAN NOT NULL,
    Note TEXT NULL
);

BEGIN;
INSERT INTO Users VALUES (1, 'Alice', TRUE, NULL);
INSERT INTO Users VALUES (2, 'Bob', TRUE, 'temporary');
INSERT INTO Users VALUES (3, 'Carol', FALSE, NULL);
INSERT INTO Users VALUES (4, 'Dave', TRUE, 'keep');
COMMIT;

-- Find enabled users, deterministically ordered and bounded.
SELECT Id, Name
FROM Users
WHERE Enabled = TRUE
ORDER BY Name ASC
LIMIT 10 OFFSET 0;

-- Mutate: disable Bob, then remove every disabled user.
UPDATE Users SET Enabled = FALSE WHERE Name = 'Bob';
DELETE FROM Users WHERE Enabled = FALSE;
```

```sh
build/gxdb_cli sql app.gxdb "CREATE TABLE Users (Id INT64 NOT NULL, Name TEXT NOT NULL, Enabled BOOLEAN NOT NULL, Note TEXT NULL);"
build/gxdb_cli sql app.gxdb "INSERT INTO Users VALUES (1, 'Alice', TRUE, NULL);"
build/gxdb_cli sql app.gxdb "SELECT Id, Name FROM Users WHERE Enabled = TRUE ORDER BY Name ASC LIMIT 10;"
build/gxdb_cli sql app.gxdb "UPDATE Users SET Enabled = FALSE WHERE Name = 'Alice';"
build/gxdb_cli sql app.gxdb "DELETE FROM Users WHERE Enabled = FALSE;"
```

`UPDATE` and `DELETE` print their affected-row count in the CLI. Without a
`WHERE`, every row is targeted; the table schema always survives a `DELETE`.

The `.gxdb` can be closed and reopened; the same `SELECT` returns the same
logical rows.

```cpp
#include "database_sql.h"
using namespace gxos::db;

std::unique_ptr<Database> db;
Database::open("app.gxdb", DatabaseOpenOptions(), db);

SqlEngine engine(*db);
SqlExecutionResult result = engine.execute("SELECT Id, Name FROM Users;");
const SqlResultSet& rows = result.statements[0].resultSet;
// rows.columnCount(), rows.column(i).name/type, rows.row(r), rows.isNull(r, c)
```

## Indexed example (SQL6)

```sql
CREATE TABLE Users (
    Id INT64 PRIMARY KEY,
    Email TEXT UNIQUE,
    Name TEXT NOT NULL,
    Enabled BOOLEAN NOT NULL
);

CREATE INDEX IX_Users_Name
ON Users (Name);

INSERT INTO Users VALUES (1, 'alice@example.com', 'Alice', TRUE);
INSERT INTO Users VALUES (2, 'bob@example.com', 'Bob', FALSE);

-- Uses the PRIMARY KEY index.
SELECT *
FROM Users
WHERE Id = 1;

-- Uses IX_Users_Name as a range access path.
SELECT Name
FROM Users
WHERE Name >= 'A';
```

`PRIMARY KEY` implies `NOT NULL` and is backed by a persistent unique B+ tree.
`UNIQUE` allows any number of `NULL` values but rejects duplicate non-NULL
values. Indexed lookups return exactly the same logical rows as a full scan.
Composite indexes and foreign keys are not supported in SQL6.

```sh
build/gxdb_cli sql app.gxdb "CREATE TABLE Users (Id INT64 PRIMARY KEY, Email TEXT UNIQUE, Name TEXT NOT NULL, Enabled BOOLEAN NOT NULL);"
build/gxdb_cli sql app.gxdb "CREATE INDEX IX_Users_Name ON Users (Name);"
build/gxdb_cli sql app.gxdb "SELECT * FROM Users WHERE Id = 1;"
GXDB_CLI_EXPLAIN=1 build/gxdb_cli sql app.gxdb "SELECT Name FROM Users WHERE Name >= 'A';"
build/gxdb_cli inspect app.gxdb   # lists every index and its root page
```

## JOIN + aggregate example (SQL7)

```sql
CREATE TABLE Users (
    Id INT64 PRIMARY KEY,
    Name TEXT NOT NULL
);

CREATE TABLE Orders (
    Id INT64 PRIMARY KEY,
    UserId INT64 NULL,
    Amount INT64 NOT NULL
);

-- A non-unique index lets the join probe candidate orders.
CREATE INDEX IX_Orders_UserId ON Orders (UserId);

INSERT INTO Users VALUES (1, 'Alice');
INSERT INTO Users VALUES (2, 'Bob');

INSERT INTO Orders VALUES (10, 1, 100);
INSERT INTO Orders VALUES (11, 1, 50);
INSERT INTO Orders VALUES (12, NULL, 999);

-- INNER JOIN: NULL foreign keys never match.
SELECT u.Name, o.Amount
FROM Users AS u
JOIN Orders AS o ON o.UserId = u.Id
ORDER BY o.Id;

-- LEFT JOIN + aggregate: users with no orders still appear.
SELECT u.Name, COUNT(o.Id) AS Orders, SUM(o.Amount) AS Total
FROM Users AS u
LEFT JOIN Orders AS o ON o.UserId = u.Id
GROUP BY u.Id, u.Name
ORDER BY Orders DESC;

-- DISTINCT on projected values.
SELECT DISTINCT Name FROM Users ORDER BY Name;
```

```sh
build/gxdb_cli sql app.gxdb "SELECT u.Name, COUNT(o.Id) AS Orders FROM Users AS u LEFT JOIN Orders AS o ON o.UserId = u.Id GROUP BY u.Id, u.Name ORDER BY Orders DESC;"
GXDB_CLI_EXPLAIN=1 build/gxdb_cli sql app.gxdb "SELECT u.Name, o.Amount FROM Users AS u JOIN Orders AS o ON o.UserId = u.Id;"
```

`JOIN` is an `INNER JOIN`; `LEFT [OUTER] JOIN` null-extends unmatched right
relations. `COUNT(column)` ignores NULLs while `COUNT(*)` counts every joined
row, so an unmatched LEFT JOIN row contributes `0` to `COUNT(o.Id)` and `1` to
`COUNT(*)`. Indexed join probes use the SQL6 B+ tree but always evaluate the
complete `ON` predicate, so they return exactly the same logical rows as a
nested-loop join.

## Parent / child example (SQL8)

```sql
CREATE TABLE Users (
    Id INT64 PRIMARY KEY,
    Name TEXT NOT NULL,
    Enabled BOOLEAN NOT NULL DEFAULT TRUE
);

CREATE TABLE Orders (
    Id INT64 PRIMARY KEY,
    UserId INT64 NOT NULL,
    State TEXT NOT NULL DEFAULT 'new',

    FOREIGN KEY (UserId) REFERENCES Users (Id)
);

-- Column lists let omitted columns take their declared DEFAULT.
INSERT INTO Users (Id, Name) VALUES (1, 'Alice');
INSERT INTO Users (Id) VALUES (2);                 -- Enabled = TRUE

INSERT INTO Orders (Id, UserId) VALUES (100, 1);   -- State = 'new'
INSERT INTO Orders (Id, UserId, State) VALUES (101, 1, DEFAULT);

-- A non-NULL foreign key must reference an existing parent row.
INSERT INTO Orders (Id, UserId) VALUES (102, 999);  -- ForeignKeyViolation

-- The parent key is protected while children reference it.
DELETE FROM Users WHERE Id = 1;                    -- ForeignKeyViolation
UPDATE Users SET Id = 20 WHERE Id = 1;             -- ForeignKeyViolation

-- Remove the children first, then the parent.
DELETE FROM Orders WHERE UserId = 1;
DELETE FROM Users WHERE Id = 1;

-- Evolve and remove schema objects transactionally.
ALTER TABLE Orders ADD COLUMN Note TEXT DEFAULT 'none';
DROP INDEX IX_Orders_State;                        -- only user indexes
DROP TABLE Orders;
DROP TABLE Users;
```

```sh
build/gxdb_cli sql app.gxdb "CREATE TABLE Users (Id INT64 PRIMARY KEY, Name TEXT NOT NULL, Enabled BOOLEAN NOT NULL DEFAULT TRUE);"
build/gxdb_cli sql app.gxdb "CREATE TABLE Orders (Id INT64 PRIMARY KEY, UserId INT64 NOT NULL, State TEXT NOT NULL DEFAULT 'new', FOREIGN KEY (UserId) REFERENCES Users (Id));"
build/gxdb_cli sql app.gxdb "INSERT INTO Users (Id, Name) VALUES (1, 'Alice');"
build/gxdb_cli sql app.gxdb "INSERT INTO Orders (Id, UserId) VALUES (100, 1);"
build/gxdb_cli sql app.gxdb "INSERT INTO Orders (Id, UserId) VALUES (101, 999);"  # rejected
build/gxdb_cli inspect app.gxdb   # shows defaults, index ownership and foreign keys
```

Foreign keys are single-column only and use RESTRICT / NO ACTION semantics:
there are no cascading actions in SQL8. A nullable child column may hold any
number of `NULL`s (no parent is required). Self-referential foreign keys are
supported, and FK checking reasons about the final state of a statement, so
`DELETE FROM Employees;` succeeds even when rows reference one another. Every
foreign key owns a private, system-owned B+ tree support index on the child
column; `DROP INDEX` rejects it along with PRIMARY KEY / UNIQUE indexes. DROP
TABLE refuses to remove a table that another table's foreign key references.
`ALTER TABLE ... ADD COLUMN` eagerly rewrites existing rows (backfilling the
DEFAULT or NULL) and remaps every index locator.

## Command-line diagnostics

```sh
build/gxdb_cli create sample.gxdb 4096
build/gxdb_cli inspect sample.gxdb
build/gxdb_cli sql sample.gxdb "SELECT * FROM Users;"
build/gxdb_cli run sample.gxdb schema.sql
build/gxdb_cli shell sample.gxdb
```

## Status

Hosted proof complete (SQL1: 172 checks, SQL2: 10398 checks, SQL3: 2285 checks,
SQL4: 429 checks, SQL5: 1779 checks, SQL6: 2752 checks, SQL7: 1307 checks,
SQL8: 4503 checks). SQL3 proves
crash-atomic transactions on the hosted backend, including a full commit crash
matrix and a 250-lifecycle transaction/recovery stress suite. SQL4 proves a
bounded SQL language over that engine. SQL5 proves predicates with three-valued
logic, deterministic ordering and LIMIT/OFFSET, plus UPDATE/DELETE with
statement-level atomicity, variable width row relocation, multi-page compaction
and SQL-driven UPDATE/DELETE crash matrices. SQL6 proves persistent single-column
B+ tree indexes: multi-level splits, duplicate-key and range traversal,
PRIMARY KEY / UNIQUE enforcement, transactional CREATE INDEX over existing rows,
locator remapping under page compaction, indexed-vs-full-scan equivalence,
multi-index consistency, index corruption safety, CREATE INDEX / INSERT / UPDATE
/ DELETE crash matrices, and a 3000-row deterministic workload plus a
250-lifecycle indexed transaction stress suite. SQL7 proves multi-relation and
aggregate query semantics: table aliases and qualified references, ambiguity
detection, INNER/LEFT/self/multi-table joins, indexed-vs-forced-NestedLoop
equivalence, COUNT/SUM/AVG/MIN/MAX with NULL and overflow semantics, GROUP BY
(including NULL grouping), DISTINCT, aggregate ordering, transaction visibility
through indexed join probes, join-index corruption safety, Float64 edge policy,
a 1000/3000/8000-row deterministic multi-table workload with an independent
model, and a 250-lifecycle joined/aggregate transaction workload. SQL7 requires
no persistent-format change. SQL8 proves schema lifecycle and relational
integrity: typed column DEFAULT metadata with reopen persistence, INSERT column
lists and the DEFAULT keyword, single-column FOREIGN KEYs with exact type
matching and nullable-NULL semantics, child-INSERT / child-UPDATE enforcement,
parent DELETE / key-UPDATE RESTRICT, self-referential final-state checking,
multiple FKs and multiple child tables, transaction-private FK visibility,
system-owned FK support indexes that DROP INDEX cannot remove, DROP TABLE
dependency enforcement, ALTER ADD COLUMN eager backfill with full index locator
remapping, catalog v1/v2/v3 backward compatibility, corruption-safe catalog
parsing, a deterministic 200/600/1200-row referential workload, a 250-lifecycle
schema/integrity workload, and CREATE TABLE+FK / parent+child commit / DROP
INDEX / DROP TABLE / ALTER crash matrices. SQL8 introduces catalog v4 while the
outer `.gxdb`, heap, B+ tree and WAL formats remain unchanged. QEMU and bare-metal proof are deferred to the phase
that provides a native `IDatabaseFile` backend over the guideXOS VFS / block
device.
