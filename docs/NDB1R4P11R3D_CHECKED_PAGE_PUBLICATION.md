# NDB1R4P11R3D: Checked Page Publication

## Outcome

**Outcome A — `CHECKED_PAGE_PUBLICATION_VERIFIED` for the committed
`Database::readOwnedPage` path.** The production read now copies the validated
serialized cache image directly into a staged `GxOwnedPage`. It no longer
materializes a hosted `DatabasePage` vector or takes a temporary cache pin.
Cache misses publish only after cache storage, caller storage, and page
validation have succeeded. Failures preserve a prior caller owner and release
temporary storage through its originating allocator.

This qualifies only the selected owned-read path. The three other production
`DatabasePage` copy assignments identified by R3C remain hosted and are listed
below. Parent NDB1R4P11 remains open, and neither phase marker advances.

## 1. Starting repository and phase state

- Writable SQL repository: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `d16e818930fa98bc66bbc2e3d015716fd6459d06` (R3C closeout).
- The worktree was clean before edits. No R3D implementation or report existed.
- `.phase` was and remains:

  ```text
  project=guideXOSSql
  last_completed=SQL9
  next_expected=SQL10
  ```

- `.native-sql.phase` was and remains:

  ```text
  project=guideXOSSqlNativePortability
  last_completed=NDB1R4P10
  next_expected=NDB1R4P11
  ```

- SQL ends on the existing `main` branch. The ending commit is recorded in the
  final closeout below.
- Server was inspected read-only at branch `NATIVE_DB_STORAGE`, HEAD
  `f29850c93bbac0a0763a31e806d2816310156575`; its worktree remained clean.
  No Server files, build settings, Git state, or markers were changed.
- No branch, remote, upstream, or worktree topology operation was performed.
  No push was attempted.

## 2. R3C foundation verification

Read the original P11 ownership audit and the R1 allocator contract, R2 heap
page construction, R3 materialization audit, R3A validation, R3B allocator
lifetime, and R3C caller-owned page reports. Their relevant findings were
confirmed against the current source.

R3C remains accepted at its closeout HEAD. Its final baseline was 20/20 CTest
targets, 145,380 checks, zero failures, and a freestanding census of 34
production translation units with 16 passing and 18 blocked. Its focused
`GxOwnedPage` target had 109 checks.

The owner in `database/portable/gx_owned_page.h/.cpp` was not semantically
changed. It owns one exact encoded page image with the existing 32-byte header,
uses the actual supported page-size policy up to 65,536 bytes, and retains the
allocation callback, release callback, and borrowed allocator context. It is
move-only. Initialization validates the allocator and page-size bound;
materialization and cross-domain cloning validate through the R3A
length/type/identity/payload/CRC inspector before publishing new storage.
Materialization and clone failures preserve the old owner. A caller may keep
the page after Database destruction while keeping the allocator context alive
until owner destruction; the owner does not extend allocator-context lifetime.

The Server AMD64 contracts were checked directly. The runtime's AMD64 heap is
a fixed 512 MiB free-list arena with 8-byte alignment; allocation returns null
on exhaustion, and the global `operator new` delegates to that allocator. The
Server common flags include C++14, `-ffreestanding`, `-nostdlib`,
`-nostdinc++`, `-fno-exceptions`, `-fno-rtti`, and `-fno-builtin`; AMD64 adds
the large code model, no red zone/SIMD, no PIC/PIE, and stack-probe/check
guardrails. R3D adds no fake STL, exception shim, global allocation, or native
link claim.

## 3. Audited production paths

R3C and the current code identify three explicit production
`DatabasePage`-copy assignments:

| Site | Existing copy and allocation | Ownership and current behavior |
|---|---|---|
| `DatabaseFile::readPage`, `database/database_file.cpp:419` | Local parsed page to caller output using vector copy assignment | Hosted caller-owned vector; no local `bad_alloc` conversion at this assignment; no cache pin |
| `Database::readPage`, `database/database_relational.cpp:128` | Pinned cache view to caller output using vector copy assignment | Hosted vector allocation caught and mapped to `NoSpace`; source pin is released on either branch |
| `BufferManager::installPage`, `database/database_buffer.cpp:573` | Incoming page to temporary pinned-slot view using vector copy assignment | Hosted vector allocation caught before image encoding/publication; destination slot may have live pins |

The selected R3C production `Database::readOwnedPage` (`database/database_relational.cpp:138`)
delegated to `BufferManager::readOwnedPage`. Before R3D, that method called
`pinPage`; a cache hit or miss could parse the serialized image into a hosted
`DatabasePage` vector before the method cloned the encoded bytes into the caller
owner. The buffer manager caught `std::bad_alloc` and returned `NoSpace`, so
this was a checked status path, but the page-view allocation still depended on
an implicit hosted vector allocator and sat outside portable fault injection.
The R3C report recorded this exact remaining limitation.

## 4. Selected path audit

```text
Selected production path: Database::readOwnedPage → BufferManager::readOwnedPage
Current implicit copy: pinPage materialized DatabasePage::payload with vector assignment
Source page ownership: committed GxPageCacheEntry image, or a staged cache image read from DatabaseFile
Destination page ownership: caller-configured GxOwnedPage
Allocator domain: cache image uses database cache allocator; returned image uses caller allocator tuple
Page-byte source extent: exactly the configured database page size
Existing failure handling: pinPage mapped hosted bad_alloc to NoSpace; GxOwnedPage preserved its prior value
Publication boundary: caller owner and, on a miss, cache entry publish after complete validation and allocation
Cache-pin involvement: old path pinned temporarily; new path does not change any pin count
Durable COMMIT involvement: none; this is a committed-page read only
Proposed checked replacement: clone encoded image directly into a prepared GxOwnedPage, then publish
Remaining hosted copies: DatabaseFile::readPage, Database::readPage, BufferManager::installPage
```

The byte source is always the committed cache image. A cache miss stages a
cache-allocator image, reads it through `DatabaseFile::readPageImage`, and
validates it there and again through `GxOwnedPage::materialize`. An active
transaction's overlay is not consulted by this API.

## 5. Checked materialization and publication

`BufferManager::readOwnedPage` now uses the R3A inspector and R3C owner without
constructing a `DatabasePage` view:

- It rejects an uninitialized manager, wrong-size or unconfigured destination,
  header page, and out-of-range page before publication.
- It initializes a temporary owner from the destination's exact allocator
  tuple and page-size bound. This setup does not allocate.
- On a cache hit, a pinned, dirty materialized slot is encoded into its stable
  cache image first so the clone reflects the live committed view. The method
  materializes into the temporary owner, touches the LRU entry, and moves the
  owner into the caller destination. The final move is nonallocating and uses
  the same callbacks, context, and page size as the destination.
- On a cache miss, it selects an eligible unpinned cache target, allocates a
  candidate image through the cache allocator, reads and validates the exact
  page extent, then allocates and validates the caller-owned copy. Only after
  these steps does it flush a dirty victim if required and publish the cache
  candidate as unpinned. It then performs the nonallocating owner move.
- A cache-image allocation, I/O, page validation, caller allocation, victim
  flush, or cache publication failure returns status and frees any still-local
  candidate through the cache allocator. The caller's old owner stays valid.
  The miss does not publish a partial cache entry. A cache-hit failure does not
  change the cache pin count or the caller owner.

The selected path no longer has duplicate mutable `DatabasePage` payload and
`GxOwnedPage` copies. The committed cache image remains the cache's source of
truth; the returned encoded page is an independent caller-owned snapshot.

## 6. Cache pins and caller lifetime

Cache-hit reads borrow only the stable encoded image during the synchronous
copy. They neither acquire nor release a pin, replace the slot view, or change
the slot address. A dirty materialized view is encoded in place before cloning;
the live pin remains held even if caller allocation fails. The focused test
holds a page pin across both a failed and successful owned read, checks the
slot and manager pin counts, then unpins exactly once.

Cache-miss reads use only an unpinned selected victim and publish an unpinned
image. Candidate allocation or caller allocation failure leaves resident
ownership counts unchanged. A retry succeeds. The test allocator reports no
invalid frees, and the cache arena's outstanding allocation count is unchanged
after both injected miss failures.

The database-close lifetime proof now runs through this direct production
consumer: three caller owners remain valid after Database close/destruction;
the cache arena has zero outstanding allocations; the caller arena still
owns the three images; an owner is cloned into a third allocator after close;
all owners release through their own contexts, with zero outstanding
allocations and zero invalid frees.

## 7. Transaction visibility and durable COMMIT

`Database::readOwnedPage` still rejects reads when `recoveryRequired` is set.
Its call continues to route through the committed `BufferManager` cache, never
the active transaction overlay. `Transaction::readPage` still checks its
overlay first and falls back to the committed database read. A focused test
stages a page-generation change: `Transaction::readPage` sees it while
`Database::readOwnedPage` returns the old committed generation; rollback then
discards the staged page.

R3D does not change `Transaction::commit`, WAL record ordering, or the durable
COMMIT point. Before durability, existing transaction failure and rollback
rules remain in force. After WAL COMMIT is durable, existing post-commit cache
installation failures set `recoveryRequired`, finish the transaction as
committed, and report `CommitFailed`; reads are then blocked with
`RecoveryRequired` rather than returning a stale cache entry as the new
committed state. The existing WAL recovery tests passed. No new WAL records or
persistence protocol changes were introduced.

## 8. Focused checks and SQL9 workload

`tests/database_owned_page_test.cpp` now reports **162 checks passed, zero
failed**, 53 more than R3C. The added checks cover:

- Initial caller allocation failure and failed replacement with a valid owner.
- Cache allocator failure on a cache miss; destination and cache allocation
  counts remain unchanged.
- Caller allocator failure after cache candidate allocation; the candidate is
  released, the destination remains valid, and retry succeeds.
- Failure with several live owners, a wrong-size owner, and an unconfigured
  destination.
- Failure and retry with a live cache pin, with unchanged pin counts and one
  final unpin.
- Committed read versus transaction overlay visibility and rollback.
- Independent encoded bytes: the production owner matches the test's
  little-endian page builder and independent CRC reference; the existing
  hosted serializer matches that reference too.

The production SQL workload creates and opens the database, creates a table
and index, inserts data, exercises an indexed SELECT, performs an UPDATE and
read-your-writes in an explicit transaction, verifies a failed duplicate
INSERT leaves the transaction's earlier update visible, commits, deletes in a
second transaction and rolls it back, reads the committed page through the
converted API, closes and reopens, and verifies the indexed committed row.
The complete suite additionally exercises WAL crash recovery, partial-tail
recovery, transaction savepoints, primary/secondary indexes, and foreign-key
regressions.

The page header, page types, supported sizes, CRC, heap/index encoding,
transaction framing, `.gxdb`, and `.gxwal` formats are unchanged. No new parser
or serializer was added.

## 9. Freestanding qualification

Ran `tools/qualify_sql9_freestanding.ps1` under Windows PowerShell 5.1 using
the actual Server AMD64 compiler flags. The census remained **16/34 production
translation units passing, 18 blocked**:

- All 15 units that passed before the new `GxOwnedPage` object remain passing.
- `database/portable/gx_owned_page.cpp` remains the 16th passing production
  unit; it has only allocator and R3A inspector references.
- No newly listed production translation unit was added by R3D.
- `database/database_buffer.cpp` remains blocked at the existing hosted
  `<memory>` include through `database_file.h`; it was already blocked before
  R3D. The new glue is in this hosted translation unit, while the portable
  owner it calls remains freestanding-qualified.
- The same 18 units remain blocked at their existing hosted standard-library
  headers. The first errors include `<cstdio>`, `<memory>`, `<string>`,
  `<vector>`, `<set>`, and `<map>`.
- Existing unresolved symbols remain: the checksum object has
  `__cxa_guard_acquire`/`__cxa_guard_release`; the hosted allocator adapter has
  `malloc`/`free`. The owner object imports only `gxAllocatorAllocate`,
  `gxAllocatorFree`, and `gxDatabasePageInspect`.

This is an object census, not a complete native SQL9 link or execution claim.
Artifacts are under
`%TEMP%\guideXOSSql-NDB1R4P11R3D-freestanding-20261020`.

## 10. Hosted regression

The full project build succeeded. It emitted unrelated unused-helper warnings
in the unchanged `database_relational_test.cpp` and `database_index_test.cpp`;
the modified production and focused-test files emitted no warnings.

The complete CTest suite ran after the final production and test changes:

| Result | Value |
|---|---:|
| CTest targets | **20/20 passed** |
| Checks | **145,433 passed** |
| Failures | **0** |
| Elapsed time | **979.44 seconds** |

These totals are taken from `build/Testing/Temporary/LastTest.log`. The R3C
baseline was 145,380 checks; all prior targets remain enabled and the new
focused cases add 53 checks.

## 11. Remaining page ownership inventory

| Operation | R3D status | Boundary still open |
|---|---|---|
| `DatabasePage::payload`, `parse`, `serialize` | Hosted with explicit remaining allocation | Public `std::vector`; parse and encode destinations allocate through the hosted allocator. No global payload conversion was attempted. |
| `DatabaseFile::readPage` | Hosted with unchecked implicit copy | `out = page` remains at `database/database_file.cpp:419`; no allocator-aware clone or publication staging was added here. |
| `Database::readPage` | Hosted with explicit failure mapping | `out = slot->page` remains at `database/database_relational.cpp:128`; `bad_alloc` maps to `NoSpace`, and the pin is released. |
| `Database::readOwnedPage` | Fully checked portable ownership for this API | Direct validated image clone to caller allocator; committed cache only; caller allocator context must outlive the owner. |
| `BufferManager::installPage` | Hosted with explicit failure mapping | `viewCopy = page` remains at `database/database_buffer.cpp:573`; catch maps allocation failure before image/view publication. |
| Heap-page writers | Partially converted | R2 uses bounded Gx scratch in `packHeapPage`, but final page payload and append paths still use hosted vectors (`database/database_heap.cpp:1133,1161`). |
| Index-page writers | Hosted with explicit remaining allocation | Leaf/internal page payloads still use `std::vector::assign` (`database/database_index.cpp:315,454`) before transaction staging. |
| Transaction reads and staging | Partially converted | Serialized overlay storage is Gx-owned and bounded; overlay reads parse into hosted `DatabasePage` vectors (`database/database_transaction.cpp:81-118`). |
| Commit-time reconstruction | Partially converted | WAL images remain portable; commit reparses each image into hosted `DatabasePage` and installs it through the hosted cache-view path (`database/database_transaction.cpp:286-310`). Existing post-COMMIT recovery behavior is preserved. |
| `executeInsert` and statement savepoints | Hosted and unresolved | `executeInsert` at `database/database_sql.cpp:2867` still uses hosted containers; multirow failure/savepoint behavior remains a separate boundary identified by R3C. |
| P7/P8 implicit page-copy tests | Hosted test-only copies remain | Five deep copies remain in `database_ndb1r4p7_test.cpp` and `database_ndb1r4p8_test.cpp`; they are not production owners. |

The three production `DatabasePage` copy assignments remain. R3D does not
claim that all `DatabasePage` callers, all vector allocation, or SQL9 native
linkage have been qualified.

## 12. Recommended next bounded continuation

The next ownership boundary should be an allocator-aware `DatabasePage`
materialization and reader contract, followed by the coherent read/copy group
identified by R3C: `DatabaseFile::readPage`, `BufferManager` slot
materialization, `Database::readPage`, `Transaction::readPage`, and commit-time
reconstruction. It must keep transaction overlay precedence and the existing
post-durable-COMMIT recovery-required behavior. After that reader contract is
stable, migrate the cache writer `BufferManager::installPage` and page writers
in separate bounded steps. Do not begin those steps automatically.

## 13. Closeout

- Outcome: `CHECKED_PAGE_PUBLICATION_VERIFIED` for `Database::readOwnedPage`.
- Starting SQL HEAD: `d16e818930fa98bc66bbc2e3d015716fd6459d06`.
- SQL branch remains `main`; implementation and report closeout commit SHAs
  are recorded by the final closeout follow-up.
- Final SQL phase markers remain SQL9 → SQL10 and NDB1R4P10 → NDB1R4P11.
- Server remains read-only, clean, on `NATIVE_DB_STORAGE` at
  `f29850c93bbac0a0763a31e806d2816310156575`.
- Full hosted suite: 20/20 targets, 145,433 checks, zero failures.
- Freestanding census: 16/34 passing, 18 blocked; no new translation units.
- No push or Git topology change was performed.
