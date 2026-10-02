# guideXOS SQL — Database Subsystem

Native relational database subsystem for guideXOS. This repository contains:

- **Phase SQL1: Durable Database Storage Foundation** — a bounded, versioned
  `.gxdb` file format and page-storage engine with integrity checking,
  append-only allocation and explicit flush/durability semantics.
- **Phase SQL2: Relational Catalog and Heap Tables** — durable table schemas,
  typed columns, heap-backed row storage, bounded scanning and a small buffer
  manager. No SQL parser yet; the native relational API is the execution
  target for the future SQL layer.

- SQL1 format and design: [`docs/SQL1_DATABASE_STORAGE.md`](docs/SQL1_DATABASE_STORAGE.md)
- SQL2 format and design: [`docs/SQL2_RELATIONAL_CATALOG_HEAP.md`](docs/SQL2_RELATIONAL_CATALOG_HEAP.md)
- Test inventory: [`docs/SQL1_TEST_REPORT.md`](docs/SQL1_TEST_REPORT.md)

## Layout

```
database/   storage engine (format, checksum, I/O, header, page, file, engine,
            buffer, catalog, heap, relational)
tests/      hosted test suites (SQL1 + SQL2)
tools/      gxdb_cli (create / inspect diagnostics)
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

The test binaries can also be run directly: `build/database_storage_tests` and
`build/database_relational_test`.

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

## Command-line diagnostics

```sh
build/gxdb_cli create sample.gxdb 4096
build/gxdb_cli inspect sample.gxdb
```

## Status

Hosted proof complete (SQL1: 172 checks, SQL2: 10398 checks). QEMU and
bare-metal proof are deferred to the phase that provides a native
`IDatabaseFile` backend over the guideXOS VFS / block device.
