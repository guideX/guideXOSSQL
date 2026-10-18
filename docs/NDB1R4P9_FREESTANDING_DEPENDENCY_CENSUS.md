# NDB1R4P9: Freestanding SQL9 Dependency Census

## Qualification status

**Outcome A — FREESTANDING_DEPENDENCIES_QUALIFIED, CENSUS_ONLY.** This phase
records a reproducible census of every source in the production
`gxos_database` target. The real Server AMD64 compile passes 14 of 32
translation units and fails 18. The audit found no production conversion that
can safely remove a meaningful compile blocker without first changing shared
object ownership, allocation/error contracts, platform providers, or query
algorithms. No production SQL source or test was changed. P8 remains the last
production portability conversion; this report does not claim the SQL9 engine
or its native link is freestanding-ready.

| Census result | Count / state |
|---|---:|
| Production translation units examined | 32 |
| Freestanding compile passes | 14 |
| Freestanding compile failures | 18 |
| Units blocked by hosted C++ headers/types | 18 |
| Units blocked by allocator/lifetime requirements | All 18 compile failures have owning/hidden allocation dependencies; one additional hosted allocator object references `malloc/free` |
| Units blocked by platform APIs | 2 direct SQL units (`database_io.cpp`, `database_header.cpp`); native file-provider boundary has no Server implementation |
| Outstanding link/runtime requirements | hosted `malloc/free`; missing SQL allocator and native file-provider adapters; no whole-engine link |

These are source-file counts, not a measure of the percentage of the engine
that is portable.

## Starting state and gate results

| Gate | Observed state | Result |
|---|---|---|
| SQL repository | `D:\dev\guideXOS_SqlDatabase` | Passed |
| SQL branch / starting HEAD | `main`, `089156e85fb027bbb9ba981abae3978a1837b850` | Passed; required state |
| SQL worktree before edits | clean; branch reported one local commit ahead of `origin/main` | Passed |
| `.phase` | `project=guideXOSSql`, SQL9 → SQL10 | Read; unchanged |
| `.native-sql.phase` | `project=guideXOSSqlNativePortability`, NDB1R4P8 → NDB1R4P9 | Read; required state |
| Prior P9 evidence | no P9 report or implementation present | Passed |
| Server reference | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575` | Inspected read-only |

The P8 commit at the required starting HEAD is `NDB1R4P8: add portable
transaction page index`. Its change summary includes the production overlay and
index, the SQL regression test, the qualification script update, and the P8
report. The accepted report is
[`NDB1R4P8_TRANSACTION_PAGE_INDEX.md`](NDB1R4P8_TRANSACTION_PAGE_INDEX.md).

## Accepted P8 verification

P8 replaced the production transaction overlay's page-number `std::map` with
`GxTransactionPageIndex`, paired with the P7
`GxTransactionPages` page-image owner. For 4,096 maximum staged pages, the
index has 8,192 slots. On the current AMD64 ABI each slot is 16 bytes, so one
index allocation is exactly 131,072 bytes (128 KiB). It is allocated during
overlay initialization; lookup, insertion, clear, and restore do not grow it.

The P8 implementation preserves page-ID lookup and duplicate replacement.
The owner descriptor array remains the source of first-insertion order, so WAL
PAGE_IMAGE enumeration does not depend on hash-table iteration. Savepoint
cloning copies both owner and index, and statement rollback moves the saved
overlay back without allocation. Transaction rollback clears page images and
index entries. The P8 allocation-failure tests exercise partial initialization,
clone, insertion/replacement, rollback, retry, and cleanup. Recovery after a
simulated partial WAL write is covered. The accepted test records 25,543
checks, zero failures; the accepted full suite records 18/18 CTest targets,
145,007 checks, zero failures.

The P8 freestanding evidence applies only to the portable lookup/overlay
boundary and the previously qualified portable objects. It does not include
`database_transaction.cpp`, the database/WAL adapters, relational SQL, or the
hosted file provider.

## Production inventory and dependency search

The authoritative production source set is the 32-file `gxos_database STATIC`
target in the repository `CMakeLists.txt`. The hosted `gxdb_cli` front end is a
separate executable. CTest programs are excluded from this engine census. In
particular, `gx_owner.cpp`, `gx_string.cpp`, `gx_test_allocator.cpp`, and the
portable runtime test source are used by
`database_portable_runtime_test`, not by `gxos_database`.

The requested STL search covered the complete local include closure of each
production source, not only the `.cpp` text. Present production dependencies
include `std::string`, `std::vector`, `std::map`, `std::set`, `std::unique_ptr`,
`std::shared_ptr`, `std::move`, `std::pair`, `std::sort`, `std::find`,
`std::to_string`, `std::numeric_limits`, `std::isfinite`, `std::bad_alloc`,
`<new>`, and `<type_traits>`. There are no production uses of
`std::unordered_map`, `std::optional`, `std::function`, or `std::list` in the
target's source/include closure. `std::map` occurs in `database_heap.cpp` and
`database_sql.cpp`; ordered `std::set` use occurs in the index, heap,
relational, and SQL executor paths. `std::shared_ptr` appears in prepared SQL
execution. Many public headers propagate `std::string` and `std::vector` into
translation units that do not include those headers directly.

### Dependency classes

- **A — Simple mechanical conversions:** P1 `GxArray` is already used by the
  portable WAL encoder/scanner and page-image code. The low-level portable
  objects already pass. Some hosted page/file scratch vectors look mechanical
  in isolation, but their public signatures and failure behavior are shared
  with owning `DatabasePage`, `DbResult`, and catalog types; swapping one local
  vector would not remove the translation unit's blocker.
- **B — Ownership-sensitive conversions:** `DatabasePage::payload`, nested
  catalog/schema/value objects, `std::unique_ptr` file/database/table
  ownership, `std::vector<WalTransaction>`, statement catalog snapshots, and
  SQL token/AST values all rely on nontrivial destruction, deep copy, move, or
  ownership transfer. Raw `memcpy` or container substitution would be unsafe.
- **C — Algorithm-sensitive conversions:** ordered maps and sets in heap,
  index, relational, and SQL execution; JOIN/GROUP BY structures; and
  prepared-plan ownership. These need algorithm and semantic tests, not a
  container-name substitution.
- **D — Runtime/ABI dependencies:** implicit STL allocation and `new`, five
  source files with exception handlers under a `-fno-exceptions` kernel
  configuration, checksum local-static guard calls, and hosted `malloc/free`.
  RTTI is disabled and no production `typeid`/`dynamic_cast` use was found.
- **E — Platform/provider dependencies:** `database_io.cpp` uses hosted file
  APIs selected for Windows/POSIX; `database_header.cpp` uses
  `std::random_device` and `std::chrono`; `database_native_io.cpp` is an
  adapter interface but still uses hosted owning strings and smart pointers.
  The Server checkout has no `NativeDatabaseFileApi` implementation or
  `GxAllocator` bridge at the inspected reference state.

## Freestanding compile and symbol qualification

The reproducible script is
[`qualify_sql9_freestanding.ps1`](../tools/qualify_sql9_freestanding.ps1). It
requires an empty output directory outside the SQL checkout, compiles each of
the 32 CMake production sources independently, saves a compiler log per source,
and audits successful objects with `nm -u -C`. Generated objects and logs for
this run were written under
`%TEMP%\guideXOSSql-NDB1R4P9-census-20261017-03`; the pre-existing ignored
`build/portable-freestanding` directory was not overwritten.

The Server AMD64 target uses MinGW `g++`, C++14, `-ffreestanding`,
`-nostdlib`, `-nostdinc++`, `-fno-exceptions`, and `-fno-rtti`; its architecture
flags add 64-bit large code model, no red zone or SIMD, no PIC/PIE, no stack
probe/check/protector, and `-D__x86_64__`. The qualification script uses those
flags and the current Server kernel/include paths, plus SQL's actual
`database` and `database/portable` include roots. Compiler version:
`g++ (MinGW-W64 x86_64-msvcrt-posix-seh) 15.2.0`. Symbol tool:
`GNU nm 2.46.0.20260210`.

Command shape used for every source (the script supplies the absolute source
and object paths):

```text
g++ -std=c++14 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti -nostdlib -nostdinc++ -fno-builtin -DGXOS_BARE_METAL -m64 -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-pic -fno-pie -fno-PIC -fno-PIE -mno-stack-arg-probe -fno-stack-check -fno-stack-protector -D__x86_64__ -I<server-kernel> -I<server-root> -I<server-kernel>/core/include -I<server-kernel>/arch/amd64/include -Idatabase/portable -Idatabase -fdiagnostics-color=never -H -c <source.cpp> -o <external-output>.o
```

### Per-translation-unit dependency matrix

“Direct headers” are angle-bracket headers included by the source file.
“Transitive blockers” are the hosted types, ownership, or APIs brought in by
the local header closure. A pass is object compilation only; see the symbol
column and link notes below.

| Source / subsystem | Direct hosted headers and dependencies | Transitive blockers; allocation and lifetime risk | AMD64 compile / first diagnostic | Further blocker; conversion approach and priority |
|---|---|---|---|---|
| `database/database_format.cpp` — format constants | None | `database_format.h`; fixed-width C types; no allocation; low risk | Pass | No blocker in this unit; no conversion needed |
| `database/database_checksum.cpp` — checksum | None | Fixed-width C types; function-local static CRC table; no heap; low risk | Pass | Emits supported `__cxa_guard_acquire/release`; keep as qualified leaf |
| `database/database_io.cpp` — hosted file provider | `cerrno`, `cstring`, `sys/stat.h`, `io.h`, `unistd.h` | `database_io.h`/`database_result.h` expose `std::string` and `std::unique_ptr`; `FILE*`, stdio and path APIs; hidden string/new allocation; high risk | Fail — `database/database_io.h:11`, missing `<cstddef>` | OS API and smart-pointer/string ownership remain. Provider split/injection required; Category E |
| `database/database_native_io.cpp` — native file adapter | `utility` | `database_native_io.h`/`database_io.h`/`database_result.h`; string paths/messages and unique ownership; explicit `new` plus hidden strings; high risk | Fail — `database/database_native_io.h:3`, missing `<cstddef>` | Needs native allocator and provider wiring; Server has no implementation; Category E/B |
| `database/database_header.cpp` — header codec/identity | `chrono`, `cstring`, `random` | Header APIs return `std::string`; random-device/MT engine/time source; hidden allocation and platform entropy; medium/high risk | Fail — `database/database_header.h:5`, missing `<cstddef>` | Replace identity/time providers only with an explicit portable contract; Category E/B |
| `database/database_page.cpp` — decoded page value | `cstring`, `utility` | `DatabasePage` owns `std::vector<uint8_t>` payload and `DbResult` text; deep-copy/move allocation; high risk | Fail — `database/database_page.h:5`, missing `<cstddef>` | Needs an owned byte-buffer type with failure-safe copy/move and call-site migration; P10 candidate |
| `database/database_file.cpp` — database file/page storage | `cstring`, `new`, `utility` | `unique_ptr<IDatabaseFile/DatabaseFile>`, strings, vectors, page values, `try/catch`; hidden and explicit allocation; high risk | Fail — `database/database_file.h:16`, missing `<cstdint>` | Supplementary hosted-header probe reaches `try/catch` at line 381, incompatible with `-fno-exceptions`; page/file/result ownership phase |
| `database/database_engine.cpp` — engine factory | `utility` | Smart pointers and strings through engine/file/diagnostic headers; object transfer; high risk | Fail — `database/database_engine.h:9`, missing `<memory>` | Depends on file/database ownership conversion; Category B |
| `database/database_buffer.cpp` — buffer/page cache adapter | `new`, `type_traits`, `utility` | Placement-constructed `BufferSlot` embeds hosted `DatabasePage`; page vector/string/unique pointer closure; catches allocation failures; high risk | Fail — `database/database_buffer.h:23`, missing `<cstdint>` | Hosted-header probe reaches `try/catch` at line 195; requires no-exception status conversion and page-view ownership work |
| `database/database_catalog.cpp` — catalog/schema | `cstring` | Nested vectors/strings, `vector<bool>`, `DbValue` defaults, serialization payloads; implicit allocation and deep-copy risk high | Fail — `database/database_catalog.h:34`, missing `<cstdint>` | Catalog copy/serialization lifetime and rollback need dedicated ownership phase; Category B |
| `database/database_index.cpp` — B+ tree/index/constraints | `algorithm`, `cmath`, `cstring`, `set` | Vectors and strings in keys/values plus `std::set`; sorting, uniqueness and ordering semantics; high risk | Fail — `database/database_index.h:26`, missing `<cstdint>` | Container algorithm and allocation conversion must preserve B+ tree/constraint behavior; Category B/C |
| `database/database_heap.cpp` — heap rows | `algorithm`, `cstring`, `map`, `set`, `utility` | Map/set/vector/string/unique_ptr across row values and index calls; serialization and lookup semantics; high risk | Fail — `database/database_heap.h:25`, missing `<cstdint>` | Ordered lookup plus nested `DbValue` ownership; Category B/C |
| `database/database_wal.cpp` — WAL write/recovery adapter | `cstring`, `new`, `utility` | `unique_ptr<IDatabaseFile>`, strings, `vector<WalTransaction>`, retained pages, `try/catch`; high risk | Fail — `database/database_wal.h:29`, missing `<cstdint>` | Hosted-header probe reaches `try/catch` at line 201. Portable scanner/writer pass, but adapter ownership and transaction vector remain; Category B/D |
| `database/portable/gx_allocator.cpp` — allocator contract | `stdint.h` | Callback allocator only; no hidden allocation; low risk | Pass | Requires a platform callback at integration; no code conversion |
| `database/portable/gx_allocator_hosted.cpp` — hosted allocator adapter | `stdlib.h` | Explicit callback wrapper calls hosted `malloc/free`; allocation bounded by `maxAllocation`; low object-lifetime risk, high native runtime incompatibility | Pass | Object has unresolved `malloc` and `free`; keep out of native link and supply native adapter; Category D/E |
| `database/portable/gx_array.cpp` — byte/record array | None | Explicit `GxAllocator`, checked arithmetic, byte-only relocation; low risk | Pass | Project-local allocator references only; existing portable facility |
| `database/portable/gx_recovered_pages.cpp` — recovery page ownership | `stddef.h` | Explicit allocator; plain-data nodes own serialized page images; moderate lifetime risk, bounded by 4,096 pages/transaction and WAL bytes | Pass | Project-local codec/allocator references; existing portable facility |
| `database/portable/gx_page_cache.cpp` — cache metadata/images | None | Explicit allocator and stable plain-data descriptors; moderate lifetime risk; images allocated on misses | Pass | Project-local allocator references; existing portable facility |
| `database/portable/gx_page_codec.cpp` — page codec | None | Fixed-width page codec and checksum; no allocation; low risk | Pass | Project-local checksum reference only |
| `database/portable/gx_wal_record.cpp` — WAL record encoder | None | Uses `GxArray` and explicit allocator; bounded byte buffer; low risk | Pass | Project-local array/checksum references only |
| `database/portable/gx_wal_scan.cpp` — WAL scanner | None | One borrowed payload scratch array; explicit allocator and checked cursor math; low risk | Pass | Project-local array/checksum references only |
| `database/portable/gx_wal_page_image.cpp` — page-image encoder | None | Caller-owned page data and explicit output array; no owning C++ types; low risk | Pass | Project-local codec/record/checksum references only |
| `database/portable/gx_transaction_pages.cpp` — transaction page owner | None | Explicit allocator; fixed descriptor array plus separately owned page images; moderate/high lifetime risk managed by init/clone/move/destroy API | Pass | Project-local codec/allocator references only |
| `database/portable/gx_transaction_page_index.cpp` — transaction lookup | `stdint.h` | Fixed-size open-addressed index with explicit allocator; 128 KiB at configured limit; low risk | Pass | Project-local allocator references only |
| `database/portable/gx_transaction_page_overlay.cpp` — owner/index pair | None | Explicit paired ownership and rollback/clone/move rules; moderate risk | Pass | Project-local owner/index references only |
| `database/database_transaction.cpp` — transaction manager | `new`, `utility` | Catalog snapshot, strings, `unique_ptr<Table>`, page overlay; `try/catch`; high risk | Fail — `database/database_transaction.h:24`, missing `<cstdint>` | Hosted-header probe reaches `try/catch` at line 109; exception-free allocation status and catalog snapshot ownership required; P10/P11 |
| `database/database_relational.cpp` — JOIN/aggregate/constraints | `algorithm`, `cstdio`, `cstring`, `new` | Vector/set/string/unique_ptr, joins/aggregates, plans and transaction orchestration; `try/catch`; high risk | Fail — `database/database_relational.h:20`, missing `<cstdint>` | Hosted-header probe reaches `try/catch` at line 129; algorithmic and exception conversion required; Category B/C |
| `database/database_sql_token.cpp` — token/error values | None | `SqlToken` text and byte payload use string/vector; error descriptions allocate; medium/high risk | Fail — `database/database_sql_token.h:10`, missing `<cstdint>` | Token value ownership precedes parser conversion; Category B |
| `database/database_sql_tokenizer.cpp` — lexer | `cmath`, `cstdlib`, `limits` | String/vector token construction, numeric limits, floating checks; repeated hidden allocation; medium/high risk | Fail — `database/database_sql_tokenizer.h:12`, missing `<string>` | Requires length-aware token/value storage and explicit failure propagation; Category A/B |
| `database/database_sql_parser.cpp` — parser/AST creation | None | Vector token stream and AST vectors; string errors; nested AST ownership; high risk | Fail — `database/database_sql_parser.h:13`, missing `<vector>` | Parser lifetime and allocation failure need a dedicated AST phase; Category B |
| `database/database_sql_expr.cpp` — expression binding/evaluation | `cstring` | String/vector/DbValue plus AST references; hidden value copies; high risk | Fail — `database/database_sql_expr.h:17`, missing `<cstddef>` | Depends on value and AST owner strategy; Category B |
| `database/database_sql.cpp` — SQL execution/prepared plans | `algorithm`, `cmath`, `limits`, `map`, `set`, `utility` | `map`, `set`, `shared_ptr`, `unique_ptr`, vectors and strings; prepared-plan reuse and query algorithms; high risk | Fail — `database/database_sql.h:16`, missing `<map>` | Plan ownership and relational lookup semantics require a separate algorithm/lifetime phase; Category B/C |

### First failures and deeper probes

The primary compile used the exact `-nostdinc++` freestanding policy. All 18
failures stop in the first local header at a C++ standard header, as listed in
the matrix. They are not reported as “only a missing include”: the complete
local include closure was scanned for the other standard headers and types,
allocation, exception syntax, and platform calls. The table records the
additional blockers found.

A separate diagnostic-only probe enabled the installed MinGW C++ headers and
removed `-ffreestanding`/`-nostdinc++` so their own `requires_hosted.h` guard
would not stop preprocessing. It kept the Server architecture/code-generation
flags and `-fno-exceptions`. Thirteen of the 18 source files then compiled in
that hosted-header probe; the other five exposed source-level exception
handlers:

| Source | Further diagnostic |
|---|---|
| `database/database_file.cpp` | `database_file.cpp:381`: exception handling disabled |
| `database/database_buffer.cpp` | `database_buffer.cpp:195`: exception handling disabled |
| `database/database_wal.cpp` | `database_wal.cpp:201`: exception handling disabled |
| `database/database_transaction.cpp` | `database_transaction.cpp:109`: exception handling disabled |
| `database/database_relational.cpp` | `database_relational.cpp:129`: exception handling disabled |

Those 13 hosted-header successes are diagnostics only. They use MinGW's hosted
standard library and do not establish freestanding compilation, symbol
compatibility, or native runtime safety. No fake headers or interfaces were
used.

## Symbol, runtime, and link requirements

`nm -u -C` was run for each of the 14 successfully compiled objects. SQL
portable object-to-object references are the allocator, array, codec,
checksum, recovery, cache, WAL, and transaction-page functions expected from
the same source target. The non-project references found were:

| Object | Undefined symbols | Server status |
|---|---|---|
| `database_checksum.o` | `__cxa_guard_acquire`, `__cxa_guard_release` | Implemented in Server `kernel/core/cxx_runtime.cpp`; its guard is a simple flag and assumes single-threaded initialization |
| `gx_allocator_hosted.o` | `malloc`, `free` | No corresponding C runtime implementation was found in the Server kernel runtime; this hosted adapter must not be linked natively |

The successful set had no unresolved `operator new/delete`, exception
personality, RTTI, `memcpy`, `memmove`, or `memset` references. The Server
runtime source does implement `__cxa_atexit`, C++ `operator new/delete`, the
guard entry points, and `memcpy`/`memmove`/`memset`. Its allocation operators
use the internal fixed kernel heap; that does not provide the explicit
`GxAllocator` callback/status/alignment contract. The separate root
`allocator.cpp/.h` is a hosted allocator using `std::vector`, mutexes,
`std::unordered_map`, and C `malloc`; it is not the kernel allocator and is
not a valid SQL bridge.

The Server AMD64 build links with its architecture linker script, MinGW `ld`
flags (`-no-pie --image-base=0x100000`), and the compiler-reported `libgcc`
archive, then converts the PE image to ELF64. This phase did not link the whole
SQL production target: 18 of its source objects fail compilation. Native link
readiness is therefore **not qualified**. The inspected Server checkout also
contains no `NativeDatabaseFileApi` implementation and no `GxAllocator`
adapter. A provider/allocator integration and whole-target link remain open.

## Portable memory and ownership model

The figures below are checked theoretical bounds from current constants and
the AMD64 MinGW object layouts, not measurements of a Server SQL workload.
Allocator metadata and hosted decoded/value objects are excluded unless
called out. The layouts were measured with a temporary ABI probe: transaction
page entry 16 bytes, index slot 16 bytes, recovered-page node 16 bytes,
`GxPageCacheEntry` 40 bytes, `BufferSlot` 80 bytes, `WalTransaction` 184
bytes.

| Structure / workload | Bound or observation | Lifetime and overlap |
|---|---:|---|
| WAL encoder record | One record scratch at a time; at most `20 + 65,600 = 65,620` bytes. A max-page PAGE_IMAGE record is `20 + 12 + pageSize` (65,568 bytes at 64 KiB pages). | Local `GxArray`; allocation grows only when encoding a record and is freed before the next record. |
| WAL scanner scratch | At most 65,600 payload bytes. | Lazy: `GxArrayInit` records the bound; reserve occurs only for a nonempty record. Visitor payload is borrowed only until the callback returns. |
| Recovered page collection | At most 4,096 PAGE_IMAGE records per transaction. Total retained page-image bytes across the scanned WAL are bounded by the 64 MiB WAL file cap; node overhead is 16 bytes per distinct retained page. | Allocated lazily; duplicate replacement allocates a candidate node before freeing the old node. Retained images coexist with scanner scratch until replay completes. |
| WAL transaction descriptor vector | WAL cap permits up to 1,677,720 empty BEGIN/COMMIT pairs. At 184 bytes per `WalTransaction`, that count alone requires at least 308,700,480 bytes (about 294.4 MiB) of vector elements, before spare capacity or allocator overhead. | Hosted `std::vector`; a major nonportable recovery allocation and ownership boundary. A large vector capacity may exceed the live element count. |
| Page cache, default 32 slots | Fixed descriptor/view arrays: `32*40 + 32*80 = 3,840` bytes. Serialized image peak is `(32+1)*pageSize`; total at 4 KiB pages is 139,008 bytes, at 64 KiB pages 2,166,528 bytes. | Descriptor/view allocations are eager at `BufferManager::init`; images are lazy on misses. The extra page is the candidate-first replacement peak. Hosted `DatabasePage` payload vectors are additional and are excluded from the P6 memory statistic. |
| Empty transaction overlay | Eager owner descriptors `4,096*16 = 65,536` bytes plus eager index `8,192*16 = 131,072` bytes; 196,608 bytes per transaction before page images. | Per transaction, not shared. `clear` frees page images and clears slots but retains these tables until the `Transaction` is destroyed. New transactions initialize new overlays; there is no cross-transaction reuse. |
| Small transaction (one 4 KiB page) | Overlay metadata plus one image: 200,704 bytes. | Page image is lazy. If a one-page savepoint is live while a replacement is staged, overlay + clone + replacement candidate is at most 405,504 bytes, before page cache/catalog/hosted objects. |
| Moderate transaction (256 4 KiB pages) | Overlay metadata plus images: 1,245,184 bytes. With a cloned statement savepoint and one replacement candidate: 2,494,464 bytes. | Estimate from fixed bounds, not a runtime measurement. |
| Maximum transaction, 4 KiB pages | One overlay: 16,973,824 bytes (16 MiB images + 192 KiB metadata). With a full savepoint clone and one candidate replacement: 33,951,744 bytes. | Clone duplicates all page images and both metadata arrays. |
| Maximum transaction, 64 KiB pages | One overlay: 268,632,064 bytes. With a full savepoint clone and one replacement candidate: 537,329,664 bytes (about 512.44 MiB). | The database page-size limit permits this theoretical overlay; the 64 MiB WAL cap makes recovery of a full 4,096-image transaction at this page size impossible. |
| Commit overlap | Live transaction overlay plus one WAL record buffer (≤65,620 bytes). After durable COMMIT, one decoded `DatabasePage` payload and candidate committed-cache image can coexist with the overlay; cache peak is `(capacity+1)*pageSize`. | WAL scratch is destroyed per append. The transaction overlay remains until its caller destroys the `Transaction`; `commit()` does not clear/destroy it. |
| Large transaction recovery | Scanner scratch ≤65,600 bytes; retained page images ≤64 MiB minus WAL framing, plus up to 16-byte nodes per image; one transaction is limited to 4,096 page records. Add page-cache replay storage and the hosted transaction vector. | Recovery owns copied page images; it never retains pointers into scanner scratch. Default hosted recovery allocator limits each request to one maximum record-sized allocation. Explicit allocators may set a larger bound. |

For reference, an empty committed cache has only the 3,840-byte fixed cache
tables before resident images. The accepted P6 workload measured 1,776 bytes
of portable cache ownership for its small fixture; that observed fixture is
not a maximum-capacity measurement. P8's 131,072-byte index is eager per live
transaction and is cloned for a statement savepoint. `rollback()` clears its
slots but keeps the allocation while the transaction object lives; successful
`commit()` also leaves the overlay owned by that object. Its caller's
`unique_ptr<Transaction>` destruction releases it. Savepoint destruction
releases its cloned index and owner. There is no index sharing or reuse between
transactions.

The inspected Server AMD64 runtime reserves a 512 MiB kernel heap
(`KERNEL_HEAP_SIZE` in `kernel/core/cxx_runtime.cpp`; the 128 MiB setting is
only for its separate Navigator HTTP smoke profile). One P8 index is 128 KiB;
an empty transaction overlay is 192 KiB before cache and SQL/catalog objects.
The theoretical 64 KiB-page transaction plus one full savepoint is about
512.44 MiB, already larger than that raw heap before cache, allocator, and
other kernel allocations. The SQL repository has no native `GxAllocator`
adapter, and no runtime SQL allocation measurement was made. These numbers
identify a required integration budget; they do not imply the current Server
heap can safely host SQL9 at every configured page/transaction bound.

## Bounded-conversion decision

**Choice record**

```text
Chosen subsystem: none; census-only closeout
Current dependency: every remaining failing engine unit depends on hosted
  public string/vector/object ownership, exception syntax, algorithmic
  containers, or a platform/provider boundary
Why it blocks freestanding compilation: 18 of 32 real production objects
  fail under -nostdinc++; replacing the first missing include would expose the
  same local and transitive hosted types, and five units also reject
  try/catch under -fno-exceptions
Existing portable replacement: GxArray covers byte scratch, WAL record/page
  encoding, scanning, recovery page images, page cache metadata/images, and
  transaction page ownership/index; those actual production leaves pass
Allocation/lifetime risks: shared values and snapshots use deep-copy strings,
  vectors, unique/shared pointers, and exception-based failure reporting;
  Server has no callback allocator or native file-provider bridge
Affected production call paths: DatabasePage decode/cache, database files,
  catalog/heap/index, WAL/recovery, transaction/savepoint, relational SQL,
  parser/tokenizer/AST, prepared-plan execution
Required tests: new owner/clone/move/failure tests through storage, WAL,
  transaction and SQL call paths; full SQL1–SQL9 and P1–P8 suite
Excluded dependencies: no STL substitution, parser/plan rewrite, index/join
  algorithm rewrite, file-provider integration, or persistent-format change
```

The most promising next ownership boundary is the shared
`DatabasePage::payload` value in `database/database_page.h/.cpp`. It is a
`std::vector<uint8_t>` copied through the page cache, file, catalog, heap,
index, WAL, transaction, and relational paths. Converting it safely requires
an explicit allocator and deep-copy/move/failure contract; the current
`GxArray` is byte storage and cannot be copied as an owning C++ object by raw
assignment. A one-header change to `<cstdint>` or a local scratch-vector
substitution would only move the first diagnostic and would not remove the
ownership blocker. For that reason P9 made no production edit.

## Tests and regression results

P9 added no test target because no production conversion was selected. The
existing CTest suite still contains all 18 SQL1–SQL9 and NDB1R4P1–P8 targets;
none was removed or altered. The full regression qualification was run with:

```text
ctest --test-dir build --output-on-failure
```

The completed run passed all 18/18 CTest targets and recorded 145,007 checks
with zero failures. The accepted P8 focused result remains 25,543 checks, zero
failures. There is no new P9 focused-test count and no new P9 SQL workload;
P8's accepted real SQL9 workload covers database open, prepared DML, indexed
lookup, commit/rollback, close/reopen, and integrity/recovery behavior through
the production transaction path.

## Remaining blockers and P10 order

1. **P10 — shared page-value ownership:** define bounded allocation, copy,
   move, destruction, and `NoSpace` behavior for `DatabasePage::payload` in
   `database/database_page.h/.cpp`, then adapt `database/database_buffer.h/.cpp`
   and the actual payload call sites in `database/database_file.cpp`,
   `database/database_catalog.cpp`, `database/database_heap.cpp`,
   `database/database_index.cpp`, `database/database_wal.cpp`,
   `database/database_transaction.cpp`, and `database/database_relational.cpp`.
   Requalify one source boundary at a time; retain `DbResult` text as a
   separately tracked blocker until its own ownership/error contract is
   converted.
2. **Allocator/runtime composition:** after the page owner contract, remove
   implicit `gxHostedAllocatorInit` assumptions from native call paths in
   `database/database_buffer.cpp`, `database/database_transaction.cpp`, and
   `database/database_wal.cpp`; supply an explicit Server-compatible
   `GxAllocator` adapter without changing the Server tree in this SQL phase.
   Keep `gx_allocator_hosted.cpp` for hosted development only.
3. **Storage/WAL value ownership:** address remaining page/file byte vectors,
   `DbResult`/path strings, `unique_ptr` provider ownership, and the recovered
   `std::vector<WalTransaction>` in `database/database_file.cpp`,
   `database/database_wal.h/.cpp`, and `database/database_native_io.h/.cpp`.
   Keep WAL bytes and database formats unchanged.
4. **Relational/catalog ownership and algorithms:** convert catalog/schema/
   `DbValue` containers, B+ tree lookup structures, JOIN/GROUP BY containers,
   and prepared-plan ownership in `database/database_catalog.*`,
   `database/database_heap.*`, `database/database_index.*`,
   `database/database_relational.*`, and `database/database_sql.*` with
   SQL-semantic and allocation-failure coverage.
5. **Parser and AST ownership:** convert `SqlToken`, tokenizer buffers,
   parser vectors, AST nodes and expression values in
   `database/database_sql_token.*`, `database/database_sql_tokenizer.*`,
   `database/database_sql_parser.*`, `database/database_sql_ast.h`, and
   `database/database_sql_expr.*` after the common value/error contract is
   established.
6. **Provider and identity services:** define the server native file-provider
   implementation and portable database identity/time source for
   `database/database_io.cpp`, `database/database_native_io.*`, and
   `database/database_header.*`.

These are dependency groups, not estimates of percentage completion. The
first P10 owner phase should measure its bounded peak and preserve SQL
copy/move, cache pinning, WAL replay, savepoint, and recovery behavior before
other public value owners are changed.

## Closeout

No production C++ source, test, CMake target, database/WAL format, or Server
file was changed. The new qualification script writes only to the caller's
fresh external output directory. The SQL ordinary phase remains SQL9 → SQL10.
After successful full regression closeout, `.native-sql.phase` advances from
NDB1R4P8 → NDB1R4P9 to NDB1R4P9 → NDB1R4P10. The Server integration milestone
remains open.
