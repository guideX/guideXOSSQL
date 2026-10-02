# guideXOS SQL — Database Subsystem

Native relational database subsystem for guideXOS. This repository currently
contains **Phase SQL1: Durable Database Storage Foundation** — a bounded,
versioned `.gxdb` file format and page-storage engine with integrity checking,
append-only allocation and explicit flush/durability semantics.

SQL1 intentionally contains **no SQL parser, no tables and no transactions**.
It is the trustworthy storage base that later phases (schema, SQL, indexes,
transactions, server, Developer Studio, REXX, `system.*` tables) will build on.

- Format and design: [`docs/SQL1_DATABASE_STORAGE.md`](docs/SQL1_DATABASE_STORAGE.md)
- Test inventory: [`docs/SQL1_TEST_REPORT.md`](docs/SQL1_TEST_REPORT.md)

## Layout

```
database/   storage engine (format, checksum, I/O, header, page, file, engine)
tests/      hosted test suite
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

The test binary can also be run directly: `build/database_storage_tests`.

## Quick example

```cpp
#include "database_engine.h"
using namespace gxos::db;

std::unique_ptr<DatabaseFile> db;
DbResult r = DatabaseEngine::createDatabase("app.gxdb", DatabaseCreateOptions(), db);
if (!r.isOk()) { /* r.describe() */ }

uint64_t pageId = 0;
db->allocatePage(PageType::Data, pageId);

DatabasePage page;
page.pageId = pageId;
page.type = PageType::Data;
page.payload = {1, 2, 3, 4};
page.payloadSize = 4;
db->writePage(page);

db->flush();
db->close();

// Later:
DatabaseOpenOptions opts;
DatabaseEngine::openDatabase("app.gxdb", opts, db);
DatabasePage read;
db->readPage(pageId, read); // read.payload == {1,2,3,4}
```

## Command-line diagnostics

```sh
build/gxdb_cli create sample.gxdb 4096
build/gxdb_cli inspect sample.gxdb
```

## Status

Hosted proof complete. QEMU and bare-metal proof are deferred to the phase that
provides a native `IDatabaseFile` backend over the guideXOS VFS / block device.
