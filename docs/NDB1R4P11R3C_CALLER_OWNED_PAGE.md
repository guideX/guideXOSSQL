# NDB1R4P11R3C: Caller-Owned Page Materialization

## Outcome

**Outcome A — `CALLER_OWNED_PAGE_MATERIALIZATION_VERIFIED`.** The SQL engine
now offers a checked production read that materializes the exact encoded bytes
of a committed page into a bounded, caller-owned portable owner. The owner
retains the allocation callbacks and context used for its storage, and the
caller must keep that context alive through owner destruction. The new API
does not change `DatabasePage::payload`; the older page-read and writer paths
remain hosted and are inventoried below.

Parent NDB1R4P11 remains open. Neither phase marker advances.

## 1. Starting checkout and preserved evidence

- Writable SQL checkout: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `92d7b566090d8ca631a3175d8f51aa3a2c77492c`.
- The only pre-existing worktree change was the authorized untracked
  `docs/NDB1R4P11R3B_PAGE_ALLOCATOR_LIFETIME.md`; it remains unchanged and is
  included with this continuation's commit.
- `.phase` remains `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase` remains `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- The original P11 ownership audit and the R1, R2, R3, R3A, and R3B reports
  were read. R1, R2, and R3A accepted their bounded results; R3B remains
  `PAGE_ALLOCATOR_LIFETIME_PARTIAL` and is preserved as requested.
- No Server source, build configuration, Git state, or phase marker was
  changed. The read-only Server checkout remained on
  `NATIVE_DB_STORAGE` at `f29850c93bbac0a0763a31e806d2816310156575`.
- No branch switch, branch creation, detached HEAD, history rewrite, remote
  change, fetch, pull, merge, rebase, cherry-pick, reset, clean, stash,
  worktree creation, or push was performed.

The accepted starting regression was 19/19 CTest targets, 145,271 checks, and
zero failures. The freestanding census was 33 production translation units,
15 passing and 18 blocked. R3B made no production-source changes.

## 2. Existing lifetime problem and ownership separation

R3B confirmed that public `DatabasePage` values returned by
`DatabaseFile::readPage`, `Database::readPage`, and `Transaction::readPage`
can outlive their file, cache, database, and transaction. Today those values
remain safe because `DatabasePage::payload` is an owning hosted
`std::vector<uint8_t>`. It does not use any SQL `GxAllocator` context.

The allocation domains remain separate:

```text
Database-owned cache pages
    -> cache allocator lifetime; its context must outlive Database

Transaction/WAL-owned page images
    -> transaction or recovery owner lifetime; their contexts are borrowed

Returned or caller-retained pages
    -> independent caller allocator lifetime; its context must outlive pages
```

An allocator domain is the allocation callback, release callback, and context
together. Equal callbacks with different contexts are different domains. The
cache allocator cannot be retained by a returned page because its borrowed
context may end at Database destruction. The transaction allocator likewise
cannot own a returned page because it can end at transaction destruction.

Cache slot `DatabasePage` views are internal and valid only while pinned and
materialized. Transaction/WAL image owners are internal to their transaction
or recovery scope. The value returned by the existing page-read APIs is
caller-owned and may escape those scopes.

## 3. Caller-owned model and portable owner

Added `GxOwnedPage` in `database/portable/gx_owned_page.h` and
`database/portable/gx_owned_page.cpp`. It owns one exact serialized database
page image, including the established header and padding, and stores the
metadata reported by the existing page inspector. It is not a new page format.

Each configured owner retains a copy of:

- Allocation callback.
- Release callback.
- Allocator context.
- Supported configured page size.
- Owned byte pointer, byte length, and allocated capacity.
- Page id, type, flags, payload length, generation, and valid/empty state.

The callback context is borrowed. It must remain valid until every owner
configured with it has released its page. The owner does not reference-count
the context and does not take the allocator from the originating Database.
There is no hidden global allocator. `init` requires a complete explicit
allocator and rejects invalid page sizes, insufficient maximum allocation,
and unsupported alignment before materialization.

The owner has a fixed one-page capacity, with an allocation size equal to the
configured database page size. It does not grow. The existing supported-size
policy and R3A codec enforce the 32-byte page header, exact byte extent,
supported page type, page identity, payload bound, and CRC. A 512-byte page
remains supported; its payload bound is 480 bytes. The maximum configured
page size remains 65,536 bytes. The fixed allocation avoids page-size
multiplication and growth arithmetic.

Implicit copies are deleted. Operations have explicit status semantics:

| Operation | Contract |
|---|---|
| Initialization | Copies a complete allocator tuple and exact page-size bound; does not allocate. |
| Materialization | Uses `gxDatabasePageInspect` with caller extent and expected id, allocates one full image through the configured allocator, copies all bytes, then replaces the old image. Validation or allocation failure leaves the prior owner unchanged. |
| Deep clone | Validates and allocates through the explicitly selected destination allocator. Source stays unchanged. Destination replacement happens after the clone is complete; any old destination image is released through its original tuple. |
| Move | Carries bytes and the original allocator tuple together. An unconfigured destination or same-domain destination is accepted. A configured destination with different callbacks, context, or page size is rejected before either owner changes. |
| Clear / destroy | Clear releases the image and keeps configuration for reuse. Destroy releases image and configuration; the object may then be initialized again. |

The materialized owner stores the exact encoded page image, so the allocated
capacity and byte count are both exactly the configured page size; payload
length and payload capacity are retained separately as metadata.

## 4. Production adoption boundary

The selected production caller is `Database::readOwnedPage`, which delegates
to `BufferManager::readOwnedPage`. The source is the serialized image in the
existing committed-page cache entry. Its extent is the cache's fixed
`pageSize`. The caller initializes the destination with its own allocator and
the database's configured page size. That allocator context must outlive the
returned owner. The page's lifetime belongs to the caller and is independent
of the Database, cache, active transaction, and WAL.

The production method uses the existing `pinPage` operation and does not
introduce another file-read, cache-fill, or transaction-visibility algorithm.
While the page is pinned, it prepares a temporary `GxOwnedPage` from the
cache image using the R3A inspector. It then unpins and only publishes the
prepared owner after unpin succeeds. Invalid input maps to `CorruptPage`,
allocator exhaustion maps to `NoSpace`, size/resource limits map through the
existing cache status mapping, and an invalid destination configuration maps
to `InvalidArgument`. Failures preserve a previously valid destination and
release the temporary through the allocator that created it.

The existing `pinPage` path still materializes a temporary/internal
`DatabasePage` view using its hosted vector. Its current buffer-manager
allocation catch maps `std::bad_alloc` to `NoSpace` before a pin is published.
This remaining hosted vector behavior is visible in the API boundary and is
not represented as a portable payload conversion. `DatabasePage::payload`
remains `std::vector<uint8_t>`.

## 5. Lifetime, cross-domain, and fault qualification

`tests/database_owned_page_test.cpp` adds **109 focused checks**. The
production lifetime case creates a database with a separately instrumented
cache arena, reads page 1 into an owner using a caller arena, closes and
destroys the Database, validates and accesses the retained image, clones it
into a third allocator, and releases the owners. The test confirms the cache
arena has no outstanding allocations after Database destruction, the caller
arena still owns the returned page, and each arena finishes with zero
outstanding allocations and zero invalid frees.

The cross-allocator cases use the same callback functions with distinct A/B
contexts. They cover allocation through A, deep clone into B, byte
independence, B release and A release, failed clone into B with destination
preservation, failed replacement materialization with source preservation,
retry, successful move into an unconfigured destination preserving A's
provenance, and rejected A-to-B transfer with both prior owners unchanged.
Invalid allocator tuples, unsupported page size, insufficient page capacity,
null source, short extent, unsupported page type, and over-capacity payload
are also checked.

The encoded-byte oracle is built independently in the focused test using
explicit little-endian field writes and an independent CRC-32 routine. Its
512-byte fixture has CRC `0xBDEE9A6C`. The complete `GxOwnedPage` bytes are
compared against that fixture and against the existing hosted
`DatabasePage::serialize` output. No page header, type id, size, endian rule,
payload encoding, CRC, heap layout, B+ tree page, `.gxdb`, or `.gxwal` format
changed.

The SQL9 workload in the focused target creates a table and index, inserts a
row, performs an indexed SELECT, modifies the row in an explicit transaction,
checks read-your-writes, commits, reads the committed page through the new
production API, closes and reopens the database, and verifies the indexed
committed row. The complete existing suite also covers WAL crash recovery and
committed-page isolation; neither transaction staging nor `executeInsert`
savepoint handling was rewritten here.

## 6. Freestanding qualification

Ran `tools/qualify_sql9_freestanding.ps1` through Windows PowerShell 5.1 using
the actual Server AMD64 compiler configuration, including
`-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti`, the kernel include
roots, and the existing no-SIMD/no-red-zone/no-PIC options. The result is
**16/34 production translation units passing, 18 blocked**:

- All 15 previously passing units remain passing.
- The new production object `database/portable/gx_owned_page.cpp` passes.
- No pre-existing blocked unit became qualified.
- The same 18 hosted units remain blocked at their known standard-library
  includes.
- `database/database_page.cpp` still fails first on `<vector>` in
  `database/database_page.h`; it is not fully portable and this result does
  not claim otherwise.

The new owner object has undefined references only to
`gxAllocatorAllocate`, `gxAllocatorFree`, and `gxDatabasePageInspect`. It
introduces no hosted STL, exceptions, RTTI, global allocation, file I/O, or
unbounded growth. This is object compilation and symbol inspection, not a
native link or kernel runtime test. Qualification artifacts are in
`%TEMP%\guideXOSSql-NDB1R4P11R3C-freestanding-20261020`.

## 7. Full hosted regression

The complete CTest suite was run against the final implementation. Results
are recorded after completion below.

| Result | Value |
|---|---:|
| CTest targets | 20/20 passed |
| Checks reported by target output | 145,380 passed |
| Failures | 0 |
| CTest elapsed time | 1,898.24 seconds |

The final count is taken from the per-target outputs in
`build/Testing/Temporary/LastTest.log`. It includes the 109 checks in the new
`database_owned_page_test`, all SQL1–SQL9 and prepared-plan suites, WAL
recovery, and the retained P1–P8/R1/R2/R3A test targets. No test target or
assertion was disabled or weakened.

## 8. Remaining hosted writer and copy inventory

| Function / site | Existing owner and current implicit allocation | Required destination allocator and explicit operation | Failure / rollback / publication invariant | Migration order |
|---|---|---|---|---|
| `DatabasePage::payload`, `DatabasePage::parse`, `DatabasePage::serialize` | Public hosted vector; parse assigns payload and serialize resizes a vector | Future `DatabasePage` payload owner must retain the caller/page domain; parse needs checked materialization, serialize needs a checked destination image | Preserve old output until inspection and allocation succeed; report OOM as status; retain the R3A validator and exact bytes | First group in the next payload-conversion phase; keep this vector until every producer and copy site is migrated |
| `DatabaseFile::readPageBytes` / `DatabaseFile::readPage` | Hosted byte vector, hosted parsed page, then `out = page` deep copy | Caller-selected page domain for escaped output; scoped domain for validation-only pages; explicit clone or direct checked materialization | Propagate I/O, corruption, and allocation failures; publish `out` only after complete read and validation | Reader group, after `DatabasePage` exposes checked owner operations |
| `Database::readPage` | Pinned cache `DatabasePage` vector copied to caller vector | Caller destination's own page allocator; explicit cross-domain clone from internal cache view | Keep cache image and destination unchanged on clone failure; release pin exactly once | Same reader group; `readOwnedPage` provides a bounded adoption boundary meanwhile |
| `BufferManager::materialize` and cache-miss `pinPage` | Cache image uses cache Gx allocator; slot payload uses hosted vector | Manager-scoped slot page domain, distinct from escaped caller page domain | Parse before setting materialized/publishing candidate/incrementing pin; free failed candidate through cache tuple | Reader group, with pin/eviction behavior tested before removing vector catches |
| `BufferManager::installPage` | Cache image uses cache Gx allocator; pinned slot uses `viewCopy = page` vector deep copy | Explicit clone into the manager's slot domain, never relabel source allocation | Prepare clone and encoded image before stable view/image mutation; preserve pins and cache identity on failure | After reader owner APIs; bounded cache-writer conversion |
| Heap `Table::applyMutationsInTransaction` / `packHeapPage` | R2 uses bounded Gx scratch then copies into hosted payload vector | Active page destination domain; checked write/clone into prepared page | Do not stage until page is prepared; statement savepoint restores earlier pages on failure | Heap writer group after explicit page-owner writable access exists |
| Heap `Table::appendEncodedRowToTail` | New-page `payload.assign` and row-planning hosted buffers | Active transaction page domain | Return capacity/allocation errors before write; establish savepoint for multirow `executeInsert` | Pair with heap writer conversion; retain `executeInsert` savepoint work for a later bounded step |
| `IndexTree::writeLeaf` / `IndexTree::writeInternal` | Hosted vector `assign` creates page payloads | Active transaction page domain; checked bounded page construction | Do not publish root, sibling, or parent changes before page preparation succeeds | After heap owner API; convert leaf/internal construction together |
| Catalog v2/v3/v4 save paths | Hosted page payload vectors copied from generated `pagePayloads` | Active transaction page domain; checked clone or direct encoding into prepared owner | Preserve overlay/savepoint state if any root or continuation allocation fails | After generic page clone, migrate all catalog versions together |
| `Transaction::readPage` | Transaction image has transaction allocator; parsed output is hosted vector | Caller destination page domain; explicit checked cross-domain page materialization | Preserve staged image and caller output on failure; retain read-your-writes precedence | Reader group; transaction image allocator is not a returned-page allocator |
| `Transaction::writePage` / staging | Transaction overlay copies encoded bytes using transaction allocator; source payload remains hosted | Transaction overlay domain for serialized staging, caller page domain remains separate | Stage/replace only after complete validation and allocation; retain statement savepoints | Keep image staging separate; do not reuse its context for escaped owners |
| `Transaction::commit` reconstruction | Transaction image owner plus hosted local page, then cache installation | Database/cache domain for committed page; transaction image domain remains WAL-private | Before durable COMMIT use existing rollback; after durable COMMIT preserve recovery-required and `CommitFailed` behavior | After reader and cache writer groups; do not convert durable commit into rollback |
| `executeInsert` savepoint path | Multirow producer can leave earlier staged work if a later row allocation fails | Transaction/page owner plus statement savepoint | Roll back the full statement on any row/page allocation failure; preserve transaction siblings | Explicitly deferred from R3C; keep as a separate next transaction step |
| Remaining implicit production/test `DatabasePage` copies | Hosted vector deep copy at three production sites and five P7/P8 test sites | Explicit checked clone to each destination's domain | No false success on copy failure; old destination survives; migrate test-only copies before deleting copy operations | Migrate with each corresponding reader/writer group, then remove implicit copies |

The smallest coherent next group is the `DatabasePage` reader/copy boundary:
checked `DatabasePage` materialization plus `DatabaseFile::readPage`, cache
slot materialization, `Database::readPage`, `Transaction::readPage`, and
commit-time reconstruction. It must add explicit destination-domain clone
operations and preserve cache pins and WAL's durable-commit rule. The hosted
payload conversion remains its own acceptance gate. `executeInsert`
savepoint conversion and page writers should follow after that read/copy
contract is stable; neither starts automatically in R3C.

## 9. Closeout

- Outcome: `CALLER_OWNED_PAGE_MATERIALIZATION_VERIFIED`.
- Starting SQL HEAD: `92d7b566090d8ca631a3175d8f51aa3a2c77492c`.
- Ending SQL HEAD, branch, full regression, and local commit SHA: to be filled
  after final gate verification and commit.
- Both phase markers remain at SQL9 → SQL10 and NDB1R4P10 → NDB1R4P11.
- The R3B Outcome B report remains preserved and is included with the R3C
  source, tests, and report in the local commit.
- Server remains read-only and unchanged.
- No push or Git topology operation was performed.
