# NDB1R4P1: Minimal Portable Runtime Foundation

## Result and scope

**Outcome A — PORTABLE_FOUNDATION_VERIFIED.** The SQL and Server repository gates matched the requested snapshots; the production SQL engine was not mechanically converted. A separate portable module now provides an injected allocator contract, a bounded byte-record array, a length-aware owned byte string, and an explicit single-resource owner. Hosted fault injection, randomized reference models, and a freestanding object build pass.

This qualifies only these initial primitives. SQL9 itself still depends on the hosted C++ library, typed containers, C runtime functions, and the current database error flow. A successful freestanding object build is not a kernel link or native SQL execution claim.

## Repository gates

| Checkout | Branch / HEAD | Worktree | Phase marker |
|---|---|---|---|
| `D:\dev\guideXOS_SqlDatabase` | `main`, starting `47862b78d3d643483798d8874b5bc321d782df51` | Clean before edits | `.phase`: SQL9 → SQL10 |
| `D:\dev\guideXOSServer_NATIVE_DB_STORAGE` | `main`, `028c493df824b4dc8c5f0df41171fb9b773c6543` | Clean; branch reports 10 commits ahead of local `origin/main` | `.native-db.phase`: NDB1R3 → NDB1R4 |

The Server checkout uses `.native-db.phase`; it contains the expected native marker. Its source and marker were read only. The prior `docs/NDB1R4_NATIVE_SQL9_INTEGRATION.md` report documents the first `-nostdinc++` probe failure at `<map>` and the prior hosted SQL9 result. The actual allocator and AMD64 flags were checked in `kernel/core/cxx_runtime.cpp`, `kernel/Makefile`, and `kernel/arch/amd64/Makefile.arch`.

SQL `.phase` remains `last_completed=SQL9`, `next_expected=SQL10`. Server `.native-db.phase` remains `last_completed=NDB1R3`, `next_expected=NDB1R4`.

## SQL production dependency inventory

The authoritative CMake library has 20 production translation units: format, checksum, hosted/native file providers, header, page, file, engine, buffer manager, catalog, index, heap, WAL, transaction, relational layer, SQL token, tokenizer, parser, expression binder, and SQL engine. Counts below are lexical occurrences in the production `database/` C++ sources and headers, not runtime object counts.

| Facility | Occurrences / files | Significant use and current bounds |
|---|---:|---|
| `std::vector` | 578 / 28 | Byte and page buffers, catalog rows, heap/index work lists, WAL scan records, parser tokens/AST, SQL rows, join intermediates, plans, and prepared state. Many values are bounded by page/SQL limits; several storage and scan collections scale with database size. |
| `std::string` | 228 / 36 | SQL input and diagnostics, identifiers, paths, SQL Text values, parser tokens, and names. Known limits include 1 MiB SQL input, 64-byte identifiers, and 65,536-byte Text values; provider paths and some diagnostic construction have no single portable runtime cap. |
| `std::map` | 10 / 5 | Transaction page overlay/savepoint, heap update grouping, SQL GROUP BY key lookup, and query-local table cache. Details are in the map audit below. |
| `std::set` | 10 / 4 | Heap/index cycle and duplicate detection plus SQL DISTINCT. |
| `std::unordered_map` | 0 / 0 | None. |
| `std::unique_ptr` | 73 / 18 | Exclusive ownership of database/file/WAL/transaction/table/scan and SQL row-source objects. |
| `std::shared_ptr`, `std::weak_ptr` | 4 / 2; 1 / 1 | Shared prepared-plan/lifetime state and weak engine-lifetime observation. |
| `std::optional`, `std::function` | 0 / 0 | None. |
| Algorithms / utility | 4 `<algorithm>` includes; `sort` 2, `stable_sort` 1, `find` 4, `move` 53, `pair`/`make_pair` present | Sorting per-page mutations and SQL result rows, vector membership checks, move-based ownership/value transfer, pair keys, numeric limits, and `to_string`. Six production files include `<utility>`. No `<new>` include or `std::bad_alloc` use was found. |
| Direct allocation | 19 `new` expressions in 8 production files | Buffer slots and owned engine, provider, transaction, scan, table, and prepared-plan objects. The portable module itself uses no global `new`. |

SQL has no explicit `throw`/`try`/`catch`, RTTI use, or allocation-failure translation for implicit STL growth. `DbResult` already carries `DbStatus`; common statuses include `NoSpace`, `OutOfBounds`, and `InvalidArgument`. The hosted provider uses stdio; it is not part of the freestanding module.

### Container and subsystem conversion inventory

| Subsystem / source | Container/value shape | Bound and growth | Lookup / ordering / ownership | Current failure behavior and replacement direction | Effort estimate |
|---|---|---|---|---|---:|
| Page and storage bytes (`database_page`, `database_file`, `database_wal`) | `vector<uint8_t>` page payloads, serialized WAL/header records, record/payload staging; `DatabasePage` is copied as a value | Page size ≤ 65,536 bytes; WAL file ≤ 64 MiB; WAL record payload ≤ page size + 64. Records grow by append/resize. | Primarily indexed byte access and sequential serialization; byte order is format-defined. No owning object elements in raw byte buffers. | DbResult handles I/O/format failures but vector allocation is exception-driven. Convert byte buffers first to bounded bytes; return GxStatus through the caller. | 3–5 days |
| Buffer cache (`database_buffer`) | `new BufferSlot[_capacity]`; each slot includes a `DatabasePage` with a vector payload | Default capacity 32; user option is `uint32_t`, with no small global cap. Slot array multiplies capacity by slot size. | Page-id lookup and eviction scan; stable slot addresses while manager lives. Slots own page images. | Direct `new[]` assumes throwing allocation and does not expose a `DbResult` from construction. Needs checked count × element-size and partial initialization cleanup; typed object lifetimes must be handled before conversion. | 3–5 days |
| Catalog (`database_catalog`) | Vectors of tables, indexes, foreign keys, columns, continuation page IDs, and byte payloads; strings for names/default Text | 512 tables, 2,048 indexes, 2,048 foreign keys, 64 columns/table, 64 FK/table; identifiers ≤64 bytes. Catalog continuation pages and serialized payloads grow with content. | Name/id lookup is linear; catalog vectors carry stable logical insertion/serialization order. Strings and values own memory. | Many operations return DbResult, but STL growth can fail before status is returned. Use bounded typed arrays only after copy/destruction semantics and atomic catalog updates are defined. | 4–7 days |
| Transactions (`database_transaction`) | `map<uint64_t, DatabasePage>` overlay plus `vector<uint64_t>` commit order; savepoint copies both and a catalog | ≤4,096 modified pages per transaction; up to 65,536 bytes/page, so page images alone can approach 256 MiB before savepoint copies/metadata. | Map is used for page-id equality lookup/update; `_order` separately owns commit order. DatabasePage values own payload vectors. | DbResult exists around mutations, but map/vector allocation is implicit. Use bounded hash lookup with a separate order list; preserve savepoint atomicity and insertion/replay order. | 4–6 days |
| Heap (`database_heap`) | Row/value vectors, encoded byte vectors, page-to-entry map, duplicate/visited/processed sets, relocation and index-change plans | Row ≤65,536 bytes; SQL mutation plan ≤100,000 targets and 64 MiB; page chain may scale to the format's 2^32 page-id ceiling. | Mutation groups keyed by page; entries sorted by slot. Page chain vector dictates physical traversal. Sets are equality/duplicate/cycle checks, not ordered output. | Status failures exist for logical/I/O errors; container allocation is unchecked. Use hash lookup for page/locator membership and explicit page order; avoid vector-only membership on large plans. | 5–8 days |
| B+ tree indexes (`database_index`) | Encoded byte keys, leaf/internal entry vectors, search paths and validation visited sets | Key cap 1,024 bytes; tree depth ≤32; page fanout constrained by page size. Full validation visited pages may scale with the index/database. | Search path ordered by key; per-node entries are sorted. Visited set is equality-only. | DbResult covers corruption/space conditions but not implicit allocations. Keep SQL-specific B+ tree as the durable index; use bounded scratch buffers and a bounded equality set for validation. | 4–7 days |
| WAL (`database_wal`, `database_relational`) | Record/payload byte vectors; vector of `WalTransaction`, each with page images; unique ownership of file provider | 64 MiB log cap, record payload ≤65,600 bytes, transaction pages ≤4,096. Scan transaction count is bounded by log bytes and record minimum size, but has no dedicated count constant. | Sequential log order and transaction/page order are semantically important. File provider has exclusive ownership. | Recovery/I/O errors propagate through DbResult; memory allocation does not. Convert scratch bytes then scan buffers while retaining exact ordering and crash-recovery checks. | 4–7 days |
| Tokenizer, parser, AST (`database_sql_token*`, `database_sql_parser`, `database_sql_ast`) | Token vectors; strings and blobs; AST node and clause vectors | SQL input ≤1 MiB, tokens ≤200,000, statements ≤4,096, expression nodes ≤4,096, nesting ≤32, text/blob literals ≤65,536 bytes, most clause widths ≤64. | Ordered token/AST traversal is required; AST fields own nested arrays and strings. | Parser reports syntax/resource errors but implicit allocations can bypass them. Convert string and AST ownership only after a typed no-throw construction model exists. | 7–11 days |
| SQL execution, JOIN, GROUP BY, DISTINCT, results (`database_sql`) | Nested vectors of rows/values; result and mutation plans; map of group keys; set of distinct rows; map of table names to unique table owners | Up to 16 join sources, 100,000 joined/result/distinct rows, 64 MiB caps for joined/results/group/distinct/sort/mutation work; up to 100,000 groups and 64 GROUP BY columns. | Join and projection order is meaningful. GROUP BY's `groups` vector determines output order; its map is lookup-only. DISTINCT's set is membership-only. Table map is query-local lookup, max 16 sources. | SQL error types report resource limits, but `std::bad_alloc` cannot become one. Use bounded hash membership for maps/sets and deterministic insertion-order arrays where output order matters. | 10–16 days |
| Prepared statements / lifetime (`database_sql`) | Shared plan, weak engine lifetime, vectors for parameters/bindings/result columns and per-plan table ownership | Parameters ≤256; statement text ≤1 MiB; plan contents bounded by parsed SQL and 64 MiB mutation limit where applicable. | Plan reused across executions; shared lifetime has intentional aliases; tables owned by plan. | Ownership cleanup is RAII today but implicit shared allocations are unbounded. Keep plan lifetime explicit/ref-counted; `GxOwner` alone is not a shared-owner replacement. | 4–7 days |

These are subsystem ranges for a staged conversion with hosted regression after each stage; they are not a P1 implementation promise. Several existing SQL logical limits do not bound total process memory because independent intermediate structures and copies may coexist.

## Kernel runtime constraints confirmed from Server

The Server AMD64 C++ flags are C++14, `-ffreestanding`, `-fno-exceptions`, `-fno-rtti`, `-nostdlib`, `-nostdinc++`, and `-fno-builtin`, plus `-m64 -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2`, no PIC/PIE, no stack probe/check/protector, and the bare-metal define. The configured compiler is MinGW-W64 `g++ 15.2.0`; the kernel link uses `-nostdlib` and libgcc, not libstdc++ or a hosted CRT.

`kernel/core/cxx_runtime.cpp` has a fixed 512 MiB AMD64 free-list arena, coalesces released blocks, rounds allocations to 8 bytes, and exposes `kernel_alloc(size)` returning null on exhaustion. Global `operator new` delegates to that null-returning function. P1 does not adapt or call it. The runtime currently does not provide allocator quotas, requested alignment above 8 bytes, STL container support, or an exception-based failure path. The hosted allocator and test arena therefore use the same explicit callback contract with a maximum alignment of 8 bytes.

## Portable module design

Files are under `database/portable/`:

- `gx_status.h`: `Ok`, `InvalidArgument`, `OutOfMemory`, `ResourceLimit`, `Overflow`, and `UnsupportedAlignment`.
- `gx_allocator.h/.cpp`: callback-based allocation with context, maximum allocation, requested alignment, explicit failure, and a paired free callback.
- `gx_allocator_hosted.h/.cpp`: test-only hosted `malloc/free` adapter. Containers do not make hidden hosted allocations.
- `gx_array.h/.cpp`: fixed-width **byte-record** array. It is intentionally not a template over arbitrary C++ objects.
- `gx_string.h/.cpp`: owned length-aware bytes with explicit maximum length and NUL terminator outside the logical bytes.
- `gx_owner.h/.cpp`: one opaque resource with explicit adopt/reset/move/destroy and a resource-specific cleanup callback.

The core files include only compiler-provided C headers (`stddef.h`, `stdint.h`) and local headers. They do not include Windows, hosted I/O, STL, exception, or memory headers. Arithmetic and byte movement are implemented locally so the freestanding objects do not import libc memory routines.

### Allocator contract

`GxAllocator` carries `context`, `allocate`, `deallocate`, `maxAllocation`, and `maxAlignment`. `gxAllocatorAllocate` clears its output first; rejects a missing callback, non-power-of-two alignment, unsupported alignment, and over-limit size; calls the backend only for nonzero requests; and converts a backend false return or success-with-null into `OutOfMemory`. A failed callback that nevertheless returns a pointer is paired with deallocation. Zero-size allocation is defined as `Ok` with a null output and no backend call. Freeing null is a no-op.

Every container allocation checks its own addition/multiplication and bound before calling the allocator. The interface never depends on global `operator new`. The runtime receives `UnsupportedAlignment` for requests above the configured 8-byte contract.

### Array behavior and object lifetime

`GxArray` is a fixed-width byte-record sequence with init, reserve, append, insert, erase, clear, size/capacity, indexed access, and destroy. Capacity is measured in records; `elementSize * capacity` is checked before allocation. Geometric growth is bounded by configured maximum elements and fails with `Overflow` before wraparound. If allocation fails, the old pointer, capacity, contents, and size remain unchanged. Aliased append preserves its source offset across growth; aliased insert stages source bytes before shifting records.

This API does not construct, copy, move, or destroy C++ values. It must not receive `DbValue`, `DatabasePage`, AST nodes, `unique_ptr`, or any owning/nontrivial object. Callers treat records as bytes and decode them into caller-owned objects. A later typed container must add no-throw construction/destruction callbacks or a rigorously restricted trivially-copyable API before storing those values.

### String behavior

`GxString` has explicit byte length, payload capacity, maximum length, and allocator ownership. It preserves embedded zero bytes, appends and assigns by supplied length, compares unsigned bytes lexicographically, supports self-copy and aliased assignment/append, and keeps a terminating zero at `data[length]` when storage exists. A zero-length string needs no allocation. `gxStringCString` validates the entire logical value and rejects embedded NUL instead of exposing a truncated C string. BLOB data remains a separate byte sequence; this module does not apply Unicode normalization or alter SQL collation.

### Ownership strategy

P1 prototypes explicit single-resource ownership with `GxOwner`. Adoption requires a matching cleanup callback; move clears the source; reset clears owner state before invoking cleanup; destroy is idempotent. The callback owns partial-initialization cleanup and must pair each allocation with its allocator. A three-step test proves two allocated child buffers are released when the third allocation fails.

`GxOwner` does not provide typed destructors or reference counting. For future exclusive SQL ownership, prefer explicit initialization plus cleanup on every failure edge, or a tiny typed wrapper whose deleter is nonthrowing and whose factory returns status. Shared prepared-plan lifetime needs its own explicit reference-counted strategy; it must not be changed to unmanaged pointers.

### Error propagation into SQL

Portable calls return `GxStatus`; no operation reports success after incomplete mutation. A future SQL adapter should translate `OutOfMemory` and `ResourceLimit` to `DbStatus::NoSpace` (and the SQL resource-limit error where appropriate), `Overflow` to `DbStatus::OutOfBounds`, `InvalidArgument` to `DbStatus::InvalidArgument`, and an internal alignment-contract violation to `DbStatus::Internal`. P1 leaves `database_result.h` unchanged and does not add a competing SQL error hierarchy.

## Map and lookup strategy audit

| Current use | Required behavior | Bounded strategy recommendation |
|---|---|---|
| Transaction page overlay `map<uint64_t, DatabasePage>` | Equality lookup/update; commit order already exists independently in `_order`; max 4,096 pages | Bounded open-addressed table keyed by page id, with `_order` retained for replay. Keep a low load factor and explicit allocation status. |
| Heap `byPage` and `seen`/`visited`/`processed` sets | Page and `(pageId,slot)` equality lookup; chain vector controls physical order; mutation target count ≤100,000 | Open-addressed page map/set using a composite locator hash. Preserve the page chain and sorted-per-page slot order separately. |
| B+ tree validation visited sets | Equality-only cycle/duplicate checks; no ordered traversal | Bounded open-addressed set, sized from a validated page/work limit; do not make the durable B+ tree itself a generic STL container. |
| GROUP BY `map<vector<DbValue>,size_t>` | Key equality lookup; output is emitted from insertion-ordered `groups`; max 100,000 groups and 64 MiB | Bounded open-addressing keyed by SQL-compatible row hash/equality. `ResultRowLess` semantics must define both hash and collision equality. Keep `groups` vector for current output order. |
| DISTINCT `set<vector<DbValue>>` | Membership only; set traversal is unused; max 100,000 rows and 64 MiB | Bounded open-addressed set with SQL row equality/hash; output remains in the `dedup` vector's input order. |
| Query table cache `map<string,unique_ptr<Table>>` | Exact normalized-name lookup; at most 16 SELECT sources; ownership transfer | Small bounded sorted table or open addressing; sorted table is reasonable at this limit, with owning table cleanup explicit. |

No scanned use requires ordered map/set iteration for result semantics: separate vectors preserve transaction, heap-chain, group, distinct, and join ordering. A portable balanced tree is not justified for P1. Do not replace these structures with linear lookup over unbounded row/page collections; GROUP BY, DISTINCT, transaction overlay, and heap mutation paths are hot enough to require bounded hash lookup. A deterministic hash implementation and exact SQL text/composite-key equality remain later conversion work.

## Test and qualification evidence

The independent `database_portable_runtime_test` CTest target builds without linking `gxos_database`. It uses `std::vector` and `std::string` only as hosted reference models. A bounded 32 KiB free-list arena models the Server's 8-byte alignment, coalescing/reuse, total exhaustion, request cap, and deterministic failure after N successful allocations.

Hosted run: **20,663 assertions passed, 0 failed**. It includes first-allocation failure, array growth failure preserving contents/size/capacity, string append failure preserving bytes/length/capacity, failure after two successful allocations and partial-owner cleanup, total arena exhaustion, size/alignment rejection, reuse, zero-size behavior, overflow checks, max lengths, aliasing/self-assignment, repeated init/destroy, allocation/free balance, and 2,500 randomized array operations plus 2,200 randomized string operations against independent reference values.

Freestanding command:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/compile_portable_freestanding.ps1
```

The script uses the Server AMD64 C++14 compile flags, including `-ffreestanding -fno-exceptions -fno-rtti -nostdlib -nostdinc++ -fno-builtin`, the AMD64 code/alignment/stack flags, and the same MinGW-W64 `g++ 15.2.0`. All four portable core objects compile. `nm -u` shows only cross-object references to `gxAllocatorAllocate` and `gxAllocatorFree`; there are no STL, exception, RTTI, hosted I/O, or C memory-helper symbols. The script rejects those hosted/runtime symbol classes. `gx_allocator_hosted.cpp` is excluded from this build.

ASan/UBSan support is available as `-DGX_ENABLE_PORTABLE_SANITIZERS=ON`, but this configured MinGW toolchain cannot link the test: its linker reports missing `-lasan` and `-lubsan`. The ordinary hosted target passes. No sanitizer result is claimed for this host.

The complete SQL9 regression suite and final Git closeout are recorded below after final verification.

## Ordered migration plan and P2 recommendation

The safest conversion order keeps SQL as one engine and changes one owning subsystem at a time. Each stage must preserve SQL9 hosted regression equivalence and `.gxdb`/`.gxwal` bytes before proceeding.

1. **Raw byte buffers and simple arrays** — adapt allocator injection and convert selected serialized/WAL scratch bytes where signatures can carry status. Keep format encoders byte-identical. Estimate 3–5 days.
2. **Page/storage utilities** — convert page payload, page encode/decode, file staging, and buffer-manager allocation only after a typed page/slot lifetime plan exists. Preserve 64 KiB page and exact file-provider behavior. Estimate 4–6 days.
3. **WAL and catalog helpers** — propagate allocation statuses through scan/append/recovery and catalog load/save; preserve record order, WAL cut points, and catalog format. Estimate 5–8 days.
4. **Heap and index support** — port row/key buffers, mutation plans, page lookup sets, and B+ tree scratch paths with nonquadratic bounded lookup. Preserve index order and heap relocation behavior. Estimate 8–13 days.
5. **Parser-owned AST structures** — port strings, token arrays, AST nodes, and clause lists with explicit nested cleanup and SQL input bounds. Do not use the byte-record array for AST objects. Estimate 7–11 days.
6. **Query executor containers** — port result rows, JOIN intermediates, aggregate states, GROUP BY and DISTINCT lookup, preserving row order and 64 MiB/100,000-row limits. Estimate 10–16 days.
7. **Prepared plans and result ownership** — preserve shared plan lifetime and engine weak lifetime with a tested explicit refcount or manager, then remove remaining hosted ownership. Estimate 4–7 days.

Recommended next phase is **NDB1R4P2 — SQL Storage/Core Portable Conversion**, limited initially to allocator plumbing, raw storage buffers, page/storage, and WAL/catalog helpers. Do not begin parser/query conversion in P2. After each bounded conversion, run the full hosted suite and verify unchanged SQL persistence bytes; expand freestanding compilation coverage before attempting a kernel link. P1 does not begin P2.

## Final verification and closeout

The complete hosted regression suite was run after the final source changes:

| Check | Result |
|---|---|
| SQL baseline CTest targets | **10/10 passed; 92,655 checks, 0 failures** |
| New portable-runtime target | **1/1 passed; 20,668 checks, 0 failures** |
| Total CTest | **11/11 passed; 113,323 checks, 0 failures; 677.39 seconds** |
| Whole-project CMake build | Exit 0; no compiler warnings reported |
| Freestanding portable core | Four objects compiled; only internal `gxAllocatorAllocate` / `gxAllocatorFree` unresolved references |
| ASan/UBSan attempt | Not linkable with this MinGW install: `-lasan` and `-lubsan` are unavailable |

No SQL engine implementation, persistent format, Server source, Server marker, or SQL `.phase` was changed. SQL started at `47862b78d3d643483798d8874b5bc321d782df51`; the P1 source changes are committed locally on the existing `main` branch. The SQL remote push was not attempted. Build output stays under ignored `build/`.

### Remaining portability blockers

- SQL9 still includes hosted STL headers across all engine layers; no SQL engine translation unit was built freestanding in P1.
- The first array supports fixed-width byte records only. Typed nontrivial values, vectors of `DbValue`/`DatabasePage`, and AST ownership need a safe typed lifetime design.
- No portable hash table is implemented yet; map/set replacements are audited but deferred.
- The Server's real allocator adapter is intentionally deferred, and the portable module has not been linked or executed in the kernel.
- Hosted tests cannot use ASan/UBSan with the currently installed MinGW runtime libraries.

P1 acceptance therefore answers the requested question narrowly: **yes for the tested minimal allocator, byte-array, length-aware string, and explicit owner foundation; no claim that SQL9 itself is yet freestanding-compatible.**
