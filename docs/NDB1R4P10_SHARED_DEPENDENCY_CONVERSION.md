# NDB1R4P10: Shared Freestanding Dependency Conversion

## Choice record

```text
Chosen P10 dependency: C++ wrapper headers for C ABI size and fixed-width
  integer types (`<cstddef>` and `<cstdint>`), replaced by `<stddef.h>` and
  `<stdint.h>`.
Actual source files: database/database_io.h, database/database_native_io.h,
  database/database_header.h, database/database_page.h, database/database_file.h,
  database/database_buffer.h, database/database_catalog.h,
  database/database_index.h, database/database_heap.h, database/database_wal.h,
  database/database_transaction.h, database/database_relational.h,
  database/database_sql_token.h, database/database_sql_expr.h, plus the shared
  production headers database/database_diagnostics.h,
  database/database_schema.h, database/database_pagestore.h,
  database/database_sql_ast.h, database/database_utf8.h, and
  database/database_types.h.
Current hosted type or header: `<cstddef>` / `<cstdint>` are unavailable under
  the accepted `-nostdinc++` configuration. The affected declarations use the
  global C ABI `size_t` and `uint*_t` aliases, not `std::size_t` or
  `std::uint*_t`.
Production units blocked: 14 fail first through these headers: database_io.cpp,
  database_native_io.cpp, database_header.cpp, database_page.cpp,
  database_file.cpp, database_buffer.cpp, database_catalog.cpp,
  database_index.cpp, database_heap.cpp, database_wal.cpp,
  database_transaction.cpp, database_relational.cpp, database_sql_token.cpp,
  and database_sql_expr.cpp.
Shared dependency chain: public production headers for database I/O, pages,
  catalog, heap/index, WAL, transactions, relational SQL, tokens, and
  expressions include the same C++ type wrappers and transitively block these
  translation units before their deeper hosted dependencies are diagnosed.
Existing portable replacement: freestanding `<stddef.h>` and `<stdint.h>`
  headers supply the exact global C ABI types required by these declarations.
Required semantic guarantees: identical integer widths, signedness, size type,
  layouts, function signatures, and persistent byte representations.
Allocation/lifetime risks: none introduced; this conversion changes includes
  only and does not alter ownership, allocation, object layout, or runtime
  behavior.
Expected freestanding improvement: remove the first missing-header blocker in
  all 14 affected production translation units. The census shows later hosted
  containers, strings, smart pointers, and platform APIs remain, so no new
  complete object compilation is assumed.
Prioritization: the P9 `DatabasePage::payload` vector has broader ownership
  impact, but changing it requires allocator propagation, explicit clone/move
  results, and migration of cache, WAL, and SQL call sites. The scalar wrapper
  headers are the highest-coverage bounded blocker in the current compiler
  matrix with no object or failure-contract changes. This phase removes that
  common blocker first and keeps the page owner as the next bounded phase.
Excluded work: DatabasePage payload ownership, DbResult strings, parser/AST,
  catalog and index containers, file/provider adapters, allocator integration,
  persistent formats, and compiler flags.
```

## Starting state and P9 baseline

The writable SQL checkout was `D:\dev\guideXOS_SqlDatabase`, branch `main`,
at `85b4d8a930c0f6b35d25a537cf509712c8e0fd1a`
(`NDB1R4P9: census freestanding SQL dependencies`). The worktree was clean
before P10 edits. The required `.phase` was SQL9 → SQL10 and
`.native-sql.phase` was NDB1R4P9 → NDB1R4P10. No P10 report or P10 marker was
present at preflight.

The P9 census and qualification script were present. The census report's
starting-state paragraph refers to an older `089156e...` revision and P8
marker, which conflicts with the explicit P10 preflight revision and current
markers. P10 used the required `85b4d8a...` checkout as its baseline and
independently reran the P9 qualification. That run reproduced 32 production
translation units, 14 passes, and 18 failures. The accepted P9 hosted baseline
was 18/18 CTest targets, 145,007 checks, and zero failures.

The freestanding runs used Server's checked-out AMD64 configuration without
editing that repository: MinGW-W64 `g++` 15.2.0, GNU `nm` 2.46.0.20260210,
C++14, `-ffreestanding -nostdlib -nostdinc++ -fno-exceptions -fno-rtti`, and
the unchanged AMD64 architecture flags and include roots in the qualification
script. Baseline output was isolated at
`%TEMP%\guideXOSSql-NDB1R4P10-before-pwsh-20261018-43d2cc1a`.

## Conversion and compatibility

The production headers used `<cstddef>` and `<cstdint>` only to obtain global
`size_t` and `uint*_t` aliases. No `std::size_t` or `std::uint*_t` use was
found. The 20 affected shared headers now include `<stddef.h>` and/or
`<stdint.h>` instead:

- `database_buffer.h`, `database_catalog.h`, `database_diagnostics.h`,
  `database_file.h`, `database_header.h`, `database_heap.h`,
  `database_index.h`, `database_io.h`, `database_native_io.h`,
  `database_page.h`, `database_pagestore.h`, `database_relational.h`,
  `database_schema.h`, `database_sql_ast.h`, `database_sql_expr.h`,
  `database_sql_token.h`, `database_transaction.h`, `database_types.h`,
  `database_utf8.h`, and `database_wal.h`.

This removes the unavailable C++ wrapper headers from all affected production
include paths while preserving the C ABI type widths, signedness, function
signatures, and layouts. It changes no algorithms, values, allocation,
ownership, SQL behavior, serialized bytes, or database formats. There are no
new allocation or lifetime boundaries. A source scan confirms no
`<cstddef>`/`<cstdint>` includes remain under `database/`.

The qualification script was also made reliable under Windows PowerShell 5.1.
The script temporarily captures native compiler and `nm` stderr with
`ErrorActionPreference=Continue`, then restores its normal `Stop` behavior.
Without this narrow change, the compiler's normal `-H` header trace was
treated as a terminating PowerShell error before the exit status and log were
recorded. The final full qualification ran successfully with Windows
PowerShell 5.1 and the same compiler flags. The script's flags and source
inventory were not changed.

## Tests and freestanding evidence

The isolated Release build used Ninja and MinGW `g++` 15.2.0, with all 82
build steps completing. It emitted six existing `-Wunused-function`
warnings in test helpers: five in `database_relational_test.cpp` and one in
`database_index_test.cpp`; no production source warning was reported.

The full hosted suite passed all 18 targets with 145,007 checks and zero
failures. The run was isolated at
`%TEMP%\guideXOSSql-NDB1R4P10-hosted-build-20261018-912a6c77`. Per-target
check totals were:

| CTest target | Checks |
|---|---:|
| `database_storage_tests` | 172 |
| `database_relational_test` | 10,411 |
| `database_transaction_test` | 2,553 |
| `database_sql_test` | 1,779 |
| `database_index_test` | 2,752 |
| `database_sql7_test` | 1,307 |
| `database_sql8_test` | 4,503 |
| `database_sql9_test` | 15,274 |
| `database_sql9r_test` | 53,898 |
| `database_native_io_test` | 19 |
| `database_portable_runtime_test` | 20,668 |
| `database_wal_portable_test` | 305 |
| `database_wal_page_portable_test` | 106 |
| `database_wal_recovery_portable_test` | 204 |
| `database_ndb1r4p5_test` | 4,594 |
| `database_ndb1r4p6_test` | 612 |
| `database_ndb1r4p7_test` | 307 |
| `database_ndb1r4p8_test` | 25,543 |
| **Total** | **145,007** |

The real SQL9 targets passed: `database_sql9_test` covered prepared bindings,
reuse, indexed/range/join/aggregate queries, DML, and schema invalidation;
`database_sql9r_test` passed its 53,898-check prepared-plan reuse and mixed
workload. P8's 25,543-check production SQL9 workload also passed its prepared
DML, indexes, foreign keys, rollback, commit, and reopen coverage. WAL crash
recovery, transactions and savepoints, portable ownership, allocation-failure
recovery retry, and page-index collision tests all remained enabled and
passed.

No new runtime-focused test was added because this phase changes only type
header selection and introduces no runtime behavior. The focused portability
measurement is the full 32-unit compiler matrix: 14 production units advanced
past the converted headers, and the prior 14 passing objects remained
passing. No new translation unit completed compilation.

The post-change matrix was run twice with identical results: first under
`pwsh` at `%TEMP%\guideXOSSql-NDB1R4P10-after-20261018-0c33a95e`, then with the
fixed qualification script under Windows PowerShell 5.1 at
`%TEMP%\guideXOSSql-NDB1R4P10-final-ps51-20261018-6113ca54`.

### Production translation-unit matrix

| Production unit | Result | Current first diagnostic / remaining blocker |
|---|---|---|
| `database_format.cpp` | PASS retained | No hosted blocker |
| `database_checksum.cpp` | PASS retained | Known checksum local-static guard symbols |
| `database_io.cpp` | FAIL; advanced | `database_io.h:13`, missing `<cstdio>`; then hosted file APIs and owning types |
| `database_native_io.cpp` | FAIL; advanced | `database_native_io.h:5`, missing `<memory>`; then strings, ownership, and native provider |
| `database_header.cpp` | FAIL; advanced | `database_header.h:7`, missing `<string>`; then random/time provider and string ownership |
| `database_page.cpp` | FAIL; advanced | `database_page.h:7`, missing `<vector>`; then page payload and result ownership |
| `database_file.cpp` | FAIL; advanced | `database_file.h:17`, missing `<memory>`; then file, page, and exception dependencies |
| `database_engine.cpp` | FAIL; unchanged | `database_engine.h:9`, missing `<memory>` |
| `database_buffer.cpp` | FAIL; advanced | `database_file.h:17`, missing `<memory>`; then page ownership and exception dependencies |
| `database_catalog.cpp` | FAIL; advanced | `database_catalog.h:35`, missing `<string>`; then nested catalog ownership |
| `database_index.cpp` | FAIL; advanced | `database_index.h:27`, missing `<set>`; then ordered containers and index semantics |
| `database_heap.cpp` | FAIL; advanced | `database_heap.h:26`, missing `<memory>`; then map/set/vector and row ownership |
| `database_wal.cpp` | FAIL; advanced | `database_wal.h:30`, missing `<memory>`; then WAL transaction vectors and exceptions |
| `gx_allocator.cpp` | PASS retained | Explicit allocator contract only |
| `gx_allocator_hosted.cpp` | PASS retained | Hosted adapter references `malloc/free`; remains excluded from native use |
| `gx_array.cpp` | PASS retained | Project allocator callbacks only |
| `gx_recovered_pages.cpp` | PASS retained | Project allocator and page codec only |
| `gx_page_cache.cpp` | PASS retained | Project allocator callbacks only |
| `gx_page_codec.cpp` | PASS retained | Project checksum functions only |
| `gx_wal_record.cpp` | PASS retained | Project array and checksum functions only |
| `gx_wal_scan.cpp` | PASS retained | Project array and checksum functions only |
| `gx_wal_page_image.cpp` | PASS retained | Project array, codec, and checksum functions only |
| `gx_transaction_pages.cpp` | PASS retained | Project allocator and page codec only |
| `gx_transaction_page_index.cpp` | PASS retained | Project allocator callbacks only |
| `gx_transaction_page_overlay.cpp` | PASS retained | Project transaction owner/index functions only |
| `database_transaction.cpp` | FAIL; advanced | `database_transaction.h:25`, missing `<memory>`; then snapshots and exception-based allocation |
| `database_relational.cpp` | FAIL; advanced | `database_relational.h:21`, missing `<memory>`; then relational containers and exceptions |
| `database_sql_token.cpp` | FAIL; advanced | `database_sql_token.h:11`, missing `<string>`; then token byte ownership |
| `database_sql_tokenizer.cpp` | FAIL; unchanged | `database_sql_tokenizer.h:12`, missing `<string>`; then vector and lexer allocation |
| `database_sql_parser.cpp` | FAIL; unchanged | `database_sql_parser.h:13`, missing `<vector>`; then AST ownership |
| `database_sql_expr.cpp` | FAIL; advanced | `database_sql_expr.h:18`, missing `<vector>`; then AST/value ownership |
| `database_sql.cpp` | FAIL; unchanged | `database_sql.h:16`, missing `<map>`; then prepared-plan/container ownership |

Result: 14/32 objects pass before and after; all 14 prior passes were retained;
zero units newly pass; all 18 former failures remain blocked. Four first
diagnostics are unchanged (`database_engine.cpp`, `database_sql_tokenizer.cpp`,
`database_sql_parser.cpp`, and `database_sql.cpp`). The other 14 advanced
past their original missing `<cstddef>`/`<cstdint>` header.

The successful objects' `nm -u -C` symbol sets were identical before and after
P10. The known `__cxa_guard_acquire` / `__cxa_guard_release` references remain
in the checksum object. The hosted allocator adapter still references
`malloc/free`. No new exception/unwind, RTTI, global allocation, hosted file
I/O, or unsupported runtime helper dependency appeared. Object compilation is
not a whole-engine link or native runtime qualification; SQL9 is not claimed
to be natively executable.

## Remaining dependency inventory and P11 scope

The 14 passing units remain the portable leaves listed above. The 18 failing
units and their first remaining compiler errors are listed in the production
matrix. Deeper dependencies are unchanged from the P9 census: owning
`std::string` and `std::vector` values, smart pointers, ordered maps/sets,
exception handlers under `-fno-exceptions`, hosted file APIs, identity/time
providers, and the missing native allocator/file-provider adapters. P10 did
not conceal these dependencies or alter qualification flags.

Recommend NDB1R4P11 address the shared `DatabasePage::payload` ownership
boundary named by P9. Define a `GxAllocator`-backed page byte owner with
explicit initialization, clone/copy status, allocation-free move, destruction,
and strong failure behavior. Adapt the real cache, file, catalog, heap, index,
WAL, transaction, and relational page paths; fault-inject allocation failures;
exercise SQL commit, rollback/savepoint, close/reopen, and recovery through
those paths; then rerun the unchanged 32-unit matrix and all CTests. Keep the
Server allocator/provider integration separately scoped until an explicit
Server-side adapter exists.

## Closeout

Outcome: **A — `SHARED_DEPENDENCY_PORTABILITY_VERIFIED`**, progress
classification **`SHARED_BLOCKER_REMOVED_FURTHER_BLOCKERS_REMAIN`**. The
shared C++ scalar-type header blocker was removed from 14 real production
translation-unit paths, and each now reaches a subsequent real blocker. No
additional production unit is fully freestanding-compiled, so this result does
not imply SQL9 native link or runtime readiness.

The SQL ordinary phase remains `last_completed=SQL9` and `next_expected=SQL10`.
After complete hosted regression and reproducible freestanding qualification,
`.native-sql.phase` advances from NDB1R4P9 to NDB1R4P10. The Server checkout
remained on `NATIVE_DB_STORAGE` at
`f29850c93bbac0a0763a31e806d2816310156575`, clean and unmodified. No branch
switch, branch rename, history rewrite, or push occurred; the requested local
commit is on the existing SQL `main` branch.

## Conversion and compatibility

Pending implementation details.

## Tests and freestanding evidence

Pending qualification.

## Remaining dependency inventory and P11 scope

Pending qualification.

## Closeout

Pending qualification.
