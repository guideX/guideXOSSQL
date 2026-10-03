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

- SQL1 format and design: [`docs/SQL1_DATABASE_STORAGE.md`](docs/SQL1_DATABASE_STORAGE.md)
- SQL2 format and design: [`docs/SQL2_RELATIONAL_CATALOG_HEAP.md`](docs/SQL2_RELATIONAL_CATALOG_HEAP.md)
- SQL3 WAL/transaction design: [`docs/SQL3_WAL_TRANSACTIONS.md`](docs/SQL3_WAL_TRANSACTIONS.md)
- SQL4 language reference: [`docs/SQL4_LANGUAGE.md`](docs/SQL4_LANGUAGE.md)
- SQL5 predicates/mutation/ordering: [`docs/SQL5_PREDICATES_MUTATION_ORDERING.md`](docs/SQL5_PREDICATES_MUTATION_ORDERING.md)
- Test inventory: [`docs/SQL1_TEST_REPORT.md`](docs/SQL1_TEST_REPORT.md)

## Layout

```
database/   storage engine (format, checksum, I/O, header, page, file, engine,
            buffer, catalog, heap, wal, transaction, relational) and the SQL
            language layer (sql_token, sql_tokenizer, sql_parser, sql)
tests/      hosted test suites (SQL1 + SQL2 + SQL3 + SQL4)
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
SQL4: 429 checks, SQL5: 1775 checks). SQL3 proves crash-atomic transactions on
the hosted backend, including a full commit crash matrix and a 250-lifecycle
transaction/recovery stress suite. SQL4 proves a bounded SQL language over that
engine. SQL5 proves predicates with three-valued logic, deterministic ordering
and LIMIT/OFFSET, plus UPDATE/DELETE with statement-level atomicity, variable
width row relocation, multi-page compaction and SQL-driven UPDATE/DELETE crash
matrices. QEMU and bare-metal proof are deferred to the phase that provides a
native `IDatabaseFile` backend over the guideXOS VFS / block device.
