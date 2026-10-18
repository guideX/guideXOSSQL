# NDB1R4P11R1: Allocator Propagation and Page Lifetime

## Outcome

**Outcome A — PAGE_ALLOCATOR_CONTRACT_VERIFIED.** The allocator-propagation
foundation is accepted for this bounded cache seam. This continuation does
not complete parent P11 or advance either phase marker.

R1 preserves `DatabasePage::payload` as `std::vector<uint8_t>`. Its bounded
production seam hardens the existing page-cache allocator path: a caller that
supplies an incomplete allocator now receives `InvalidArgument`; it can no
longer be silently replaced with the hosted allocator. A null allocator still
selects the documented hosted default.

## 1. Starting state and repository gates

- SQL checkout: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `37a948000d4503adf528a517843b125e975fd14d`.
- Starting worktree: only the authorized untracked
  `docs/NDB1R4P11_DATABASE_PAGE_OWNERSHIP.md` report was present. No source
  changes or other unexpected files were present.
- The existing P11 audit was read in full and preserved without edits. Its
  original Outcome B evidence remains intact.
- `.phase` remains `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase` remains `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- No prior R1 report or completion was present at preflight.
- Read-only Server checkout: branch `NATIVE_DB_STORAGE`, HEAD
  `f29850c93bbac0a0763a31e806d2816310156575`, clean before and after
  inspection. No Server files or Git state were modified.

The Server AMD64 make configuration confirms C++14 and freestanding flags,
with exceptions, RTTI, hosted standard C++ headers/libraries, SIMD, red zone,
and PIC disabled. Its kernel heap backs global `operator new`, which can
return null and is not used as this page-cache allocator. The native SQL file
bridge is a C ABI over VFS operations and does not provide SQL's `GxAllocator`.

## 2. Original P11 blockers

The preserved P11 audit identifies these concrete prerequisites:

- `database/database_page.h` owns payload bytes through public
  `std::vector<uint8_t> payload`; the type has implicit allocating copy
  operations and compiler-generated moves.
- `DatabasePage::parse` has no allocator argument. It parses into a temporary
  and moves to output after validation, but vector allocation failure is an
  exception, not a returned status.
- `DatabaseFile::readPage` copies a parsed local page into output without a
  local `bad_alloc` conversion.
- The committed page cache owns serialized images through `GxAllocator`, but
  materialized `DatabasePage` views use hosted vectors.
- `PageAccess` exposes page size, page count, read, write, and allocate; it has
  no page-payload allocator or allocator-lifetime contract.
- Transaction staging already uses a separate portable allocator for owned
  serialized page images. Its allocator must not be borrowed for a page that
  could outlive the transaction.
- Heap and index builders allocate payload capacity with vector `assign` in
  `database/database_heap.cpp` and `database/database_index.cpp`; these
  allocation failures do not return through their `DbResult` APIs.
- Catalog persistence copies generated vector payloads into page payloads.
  These are payload-buffer copies, even though they are not whole-page copies.
- Removing page copy operations requires converting the real page copy sites
  and five explicit P7/P8 test copies together. Changing assignment alone
  cannot report allocation failure.

## 3. Allocator ownership and lifetime graph

```text
Caller creates GxAllocator and context
    │  context must outlive the open Database (documented in options)
    ▼
DatabaseOpenOptions / DatabaseCreateOptions::pageCacheAllocator
    │  Database::openImpl passes pointer to BufferManager
    ▼
BufferManager copies GxAllocator callbacks and context by value
    ├── allocates stable BufferSlot storage
    └── GxPageCache copies the same allocator tuple
          ├── allocates fixed cache descriptors at init
          └── allocates serialized page images on cache misses
    ▼
BufferManager destruction destroys page views, then releases slots,
cache images, and descriptors through their corresponding allocator tuple

Separate current payload owner:
DatabasePage::payload ── std::vector / hosted C++ allocator
```

For the selected cache seam, the allocator creator is the caller (for example,
the `GxArena` in `database_ndb1r4p6_test.cpp`) or the stateless hosted adapter
selected when the option is null. `BufferManager` owns a copy of the callback
tuple; it borrows `GxAllocator::context`. The options in
`database/database_file.h` require that context to outlive the `Database`.
Cache page views are internal to the manager and are invalid after unpin,
replacement, or manager destruction. The test arena is declared before and
destroyed after the manager, and all outstanding allocations are released.

No current `DatabasePage::payload` allocation uses this Gx context, so current
page values may outlive the cache allocator. A future payload owner must not
reuse the cache allocator for a caller-owned page without an explicit lifetime
contract: `Database::readPage` returns a page value to its caller, which can
retain it after the cache or database closes. R2 must provide an allocator
domain whose context outlives every page using it, or make that lifetime
restriction part of the page API. No allocator may be inferred from a
transaction, cache slot, global mutable state, or unrelated default.

## 4. Page copy and move inventory

| Caller / source → destination | Current operation and allocation | Failure/status behavior | Expected domain, lifetime, proposed operation |
|---|---|---|---|
| `DatabasePage::parse` in `database/database_page.cpp`: borrowed serialized bytes → temporary page → `out` | Vector `assign` may allocate; final temporary-to-output operation is a move | `DbResult` covers validation; allocation exception escapes the function and is caught only by some callers | Use caller-selected page allocator; create a temporary, then publish with nonallocating transfer only after full success. R2 |
| `DatabaseFile::readPage` in `database/database_file.cpp`: local parsed page → caller `out` | `out = page` deep-copies vector and may allocate | Function returns `DbResult` but has no local `bad_alloc` handling | Clone into the destination's explicit page domain; preserve `out` on failure. R2 |
| `Database::readPage` in `database/database_relational.cpp`: pinned borrowed cache view → caller `out` | `out = slot->page` may allocate | Catches `bad_alloc` as `NoSpace` and unpins; source slot remains valid | Explicit clone into caller page domain; propagate clone status and always balance pin. R2 |
| `BufferManager::installPage` in `database/database_buffer.cpp`: borrowed committed page → temporary view → pinned stable slot | Copies only when replacing a currently pinned view; then moves temporary into slot | Catches `bad_alloc` before encoding/publication; move is nonallocating | Clone explicitly into slot's intended domain; ensure allocation/encoding finish before publishing the view. R2 |
| `BufferManager::pinPage` and `materialize`: borrowed cache image → local/slot page | Parse may allocate vector; successful miss moves local page into stable slot | Catches `bad_alloc`; candidate image is freed and cache entry is not published on failure | Explicit allocator-aware parse; prepare before cache publication. Slot page lifetime is bounded by cache manager. R2 |
| `Transaction::readPage` in `database/database_transaction.cpp`: transaction-owned serialized image → caller page | Parse allocates vector | Catches `bad_alloc` as `NoSpace` | Use the page domain, not the transaction image allocator; clone/parse status reaches caller. R2 |
| Transaction commit in `database/database_transaction.cpp`: transaction-owned image → local page → committed cache | Parse allocates vector; the page is then passed by reference to `Database::writePage` | Catches allocation failure; after durable WAL commit, publication failure returns `CommitFailed` and marks recovery required | Explicit parse into a temporary and status propagation before cache publication. R2 |
| P7/P8 tests: `database_ndb1r4p7_test.cpp` lines 249, 251, 319; `database_ndb1r4p8_test.cpp` lines 608, 634 | Five explicit deep copies; test helper returns use NRVO/move | Test-only; no failure status | Replace with explicit clone and add failed-clone preservation tests when copy API changes. R2 |

No production `std::vector<DatabasePage>` or associative container of pages was
found. Transaction savepoints clone the portable serialized-page overlay with
status-returning Gx APIs; they do not copy `DatabasePage` objects. Heap,
index, and catalog code also has vector payload assignment/copy sites; these
must be migrated as payload-owner operations before `std::vector` is removed.

Current move assumptions are explicit in the code: `database_buffer.cpp`
statically requires `DatabasePage` to be nothrow move-assignable;
`DatabasePage::parse` moves its fully prepared temporary to output;
`BufferManager::pinPage` moves a parsed local into a stable slot; and
`discardMaterialized` move-assigns an empty page to release the old vector.
P7/P8 page-building test helpers return by value and rely on NRVO or the
compiler-generated vector move. Slot objects are placement-constructed in a
fixed allocation and individually destroyed, not byte-relocated.

## 5. Explicit page operation contracts for R2

The existing public status convention is `DbResult` / `DbStatus`; no new error
hierarchy is needed. A page creation operation should accept a valid
`GxAllocator`, supported configured page size, and metadata, and establish a
bounded payload capacity of `pageSize - kPageHeaderSize` (32-byte serialized
header; supported page sizes are powers of two from 512 through 65,536 bytes).
It returns:

- `Ok` after complete initialization.
- `InvalidArgument` for unsupported page size, invalid metadata/input, or an
  incomplete/incompatible allocator.
- `TransactionTooLarge` for configured bounds or `GxStatus::ResourceLimit` /
  overflow.
- `NoSpace` for `GxStatus::OutOfMemory`.
- `CorruptPage` when source bytes fail page identity, bounds, or CRC checks.

`clonePage(destination, source, allocator)` must validate both pages, the
allocator, and payload capacity; allocate all destination storage first; copy
metadata and bytes; then swap/publish. On any error the prior destination
remains valid, temporary memory is released through the allocator that
created it, and the error reaches the `DbResult` caller. It must not publish
an empty replacement after failed allocation.

An explicit transfer operation must allocate nothing, move the allocation and
its allocator provenance exactly once, leave the source empty and safely
destructible, and prevent double-free. A transfer carries the source's
allocator identity with the payload; it must not silently relabel the payload
as owned by the destination's different allocator. A failed clone never
changes allocator identity or validity of the destination.

Once the portable owner is introduced, `DatabasePage` cannot retain
allocation-dependent implicit copy construction or assignment. Keep move
nonallocating. Do not make all existing page users noncopyable during R1.

## 6. R1 production seam and changes

The selected seam is the existing committed page-cache allocator path, which
already has a real `DbResult` boundary and is exercised on cache misses:

```text
Database::open/create options
  → Database::openImpl
  → BufferManager allocator copy
  → GxPageCache descriptor/image allocation
  → BufferManager::pinPage cache miss
  → DbResult::NoSpace / successful image publication
```

`database/database_buffer.cpp` previously treated either missing callback as
permission to replace the supplied allocator with the hosted allocator. R1
records whether the constructor received a null pointer. Null continues to
select `gxHostedAllocatorInit`; a non-null allocator with either callback
missing returns `DbStatus::InvalidArgument` before allocation. This makes
allocator selection explicit while preserving hosted callers that omit the
option.

Changed production files:

- `database/database_buffer.h`: documents hosted default and borrowed context
  lifetime; stores whether the default was explicitly selected by null.
- `database/database_buffer.cpp`: distinguishes null/default from malformed
  explicit allocator and rejects the latter.
- `tests/database_ndb1r4p6_test.cpp`: tests malformed allocator rejection,
  resource limit propagation, repeated init rejection, and allocation balance.

The selected production path remains allocator-aware only for cache metadata,
stable slots, and serialized cache images. `DatabasePage::payload`, page
serialization output vectors, and heap/index/catalog vector operations still
allocate through their hosted C++ allocator. R1 does not qualify those
allocations for freestanding OOM behavior and adds no duplicate full-page
buffer.

## 7. Failure tests and production-path evidence

The real `BufferManager` implementation is exercised by
`database_ndb1r4p6_test`:

- Custom arena allocation failures during init clean up partial portable
  allocations.
- The real `pinPage` miss path returns `NoSpace`, leaves the output slot null,
  and does not publish a resident entry when cache-image allocation fails.
- Repeated failure followed by success proves reuse after failure.
- A failed replacement preserves existing resident pages and their bytes.
- Multiple distinct page images are allocated, evicted, and freed through the
  same arena; destruction returns outstanding allocation count to zero.
- An explicit allocator with a missing callback returns `InvalidArgument`;
  no hosted fallback allocation or page publication occurs.
- A valid allocator whose max allocation is below the descriptor requirement
  returns `TransactionTooLarge` with no outstanding allocation.
- A second initialization returns `InvalidArgument` without changing
  outstanding allocation count.
- The database-level open allocation-failure test verifies failed opens leave
  the `.gxdb` and `.gxwal` bytes unchanged.

The arena outlives each `BufferManager`/`Database` that uses its callback
context. Tests do not inject failure into `std::vector` allocation for page
payloads, and they do not claim page-payload allocation-failure qualification.
The portable cache image and `DatabasePage` view have distinct owners.

## 8. Qualification results

- Focused `database_ndb1r4p6_test`: passed after source changes.
- Focused `database_ndb1r4p6_test.exe`: 619 checks, 0 failures.
- Full hosted CTest: 18/18 targets passed, 145,014 checks, 0 failures;
  1,428.91 seconds. The check total is the accepted 145,007-check baseline
  plus the seven new assertions in the focused P6 target.
- Freestanding qualification used Windows PowerShell 5.1 and the Server
  AMD64 flags in `tools/qualify_sql9_freestanding.ps1`. Result: 14/32 pass,
  18/32 blocked, matching P10. The modified
  `database/database_buffer.cpp` remains blocked at the preexisting
  `database/database_file.h:17` `<memory>` dependency. The page translation
  unit remains blocked at `database/database_page.h:7` `<vector>`. No new
  portable component was added; no additional qualifying original production
  unit is claimed.
- Passing units remain the 14 portable leaves. Their undefined references
  remain portable allocator/array/cache/page-codec/WAL/transaction-owner
  interfaces plus the known checksum `__cxa_guard_acquire` and
  `__cxa_guard_release`; the hosted allocator adapter references `malloc` and
  `free`. The modified `database_buffer.cpp` emitted no object or undefined
  symbol list because compilation stopped at `<memory>`; there are no new
  unresolved symbols to report. No freestanding link or runtime is claimed.
- Qualification logs and per-unit diagnostics:
  `C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R1-20261018`.

## 9. Remaining implicit operations and R2 migration order

The vector representation remains allocation-dependent. The copy operations
and payload mutations in Section 4 remain in production. R2 should proceed in
this order:

1. Define a stable page-payload allocator domain whose context outlives every
   page using it. Keep it distinct from the transaction overlay allocator and
   the committed cache-image allocator. State how caller-owned pages may
   outlive a database; do not silently impose cache lifetime on public page
   outputs. Add a dedicated page-payload allocator option, copy its callback
   tuple into `Database`, expose that allocator through `PageAccess`, and have
   `Transaction` forward the database page domain. A page owner stores its
   allocator tuple, while the caller-owned callback context must outlive all
   pages created with it. The hosted null default uses stateless hosted
   callbacks.
2. Add a move-only RAII payload owner and status-returning page initialization,
   bounded payload replacement, explicit clone, and nonallocating transfer.
   Store allocation provenance with each owner. Clone through a temporary so
   failure leaves the previous destination intact.
3. Convert page creation/mutation callers first, because their APIs already
   return `DbResult`: start with `packHeapPage` in `database_heap.cpp`, passing
   the page allocator from its `PageAccess` owner and returning its
   `NoSpace` / `TransactionTooLarge` status before page write; then convert
   heap split/new-page construction, index leaf/internal construction in
   `database_index.cpp`, and catalog page payload replacement in
   `database_catalog.cpp`. Propagate allocation status before `writePage` or
   metadata publication.
4. Convert the reader and publication path as one group:
   `DatabasePage::parse`, `DatabaseFile::readPage`,
   `BufferManager::materialize` / `pinPage` / `installPage`,
   `Database::readPage`, and `Transaction::readPage` / commit. Add allocator
   context at the narrow owner boundary and return clone/parse errors through
   each existing `DbResult`; keep pins balanced and cache/WAL publication
   after successful preparation only.
5. Delete page copy construction/assignment and migrate the five P7/P8 test
   copies to explicit clone. Audit catalog savepoints and transaction rollback
   again to confirm they continue using their separate serialized Gx owners.
6. Inject clone and create failures at each newly converted caller. Verify
   output destinations, cache entries, transaction overlay, WAL recovery
   behavior, and page bytes remain unchanged on failure; then run page-byte
   fixtures, storage/cache/transaction/WAL regressions, full CTest, and the
   freestanding census.

This sequence follows real page creation and publication APIs; it is not a
mechanical replacement of vector calls. Catalog has three parallel page-save
paths that all need status propagation. File and transaction page values can
escape cache/transaction lifetimes, so their allocator source must be selected
explicitly rather than copied from those temporary owners.

## 10. Closeout

Both phase markers remain unchanged. No Server files, branches, build files,
markers, or Git topology were changed. No push was performed. The accepted
R1 source, test, and documentation changes are committed locally; the ending
SQL HEAD and commit SHA are recorded in the task closeout. The checkout is
expected to be clean after commit.
