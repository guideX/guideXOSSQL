# NDB1R4P11R3E: Checked Cache-Page Installation and Atomic Publication

## Outcome

**Outcome A — `CACHE_PUBLICATION_OWNERSHIP_VERIFIED` for the encoded-image
transaction commit path.** SQL9 now installs each committed transaction image
through a bounded cache-allocator copy of the validated encoded bytes. The
commit path no longer decodes each overlay image into a hosted `DatabasePage`
before publishing it. Existing cache entries are replaced only after the
candidate image and any required pinned view are prepared. Cache key, slot
address, pin count, and dirty accounting are preserved.

This qualifies the selected transaction cache-publication path. The
`DatabasePage` compatibility writer, public page readers, and hosted payload
representation remain open boundaries. Parent NDB1R4P11 remains open, and
neither phase marker advances.

## 1. Starting repository and preserved state

- Writable SQL checkout: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `360edfdf18469b87bd67fb4bfae73d06a3dfd066` (R3D report closeout).
- The worktree was clean before edits. The branch was two commits ahead of
  `origin/main`; no fetch or remote operation was performed.
- `.phase` remains:

  ```text
  project=guideXOSSql
  last_completed=SQL9
  next_expected=SQL10
  ```

- `.native-sql.phase` remains:

  ```text
  project=guideXOSSqlNativePortability
  last_completed=NDB1R4P10
  next_expected=NDB1R4P11
  ```

- Read-only Server reference remains on `NATIVE_DB_STORAGE` at
  `f29850c93bbac0a0763a31e806d2816310156575`, with a clean worktree.
  No Server source, settings, Git state, or markers were changed.
- No branch, remote, upstream, worktree, or other Git topology operation was
  performed. No push was attempted.

## 2. R3D acceptance verification

Read `NDB1R4P11R3D_CHECKED_PAGE_PUBLICATION.md` and the R3C, R3B, R3A, R2, and
R1 reports. The current source retains the accepted `GxOwnedPage` contracts:

- It is move-only; `cloneTo` allocates through the selected destination
  allocator.
- A configured cross-domain move is rejected before either owner changes.
- A caller-owned page can outlive its database, but its allocator context must
  remain alive until the page is released.
- `Database::readOwnedPage` clones a validated encoded cache image directly;
  it does not materialize an intermediate hosted `DatabasePage` or alter the
  read pin count.

The accepted R3D qualification was 20/20 CTest targets, 145,433 checks, zero
failures, 162 focused owned-page checks, and 16/34 freestanding production
units passing. The owner implementation and its tests were not weakened or
replaced.

## 3. Remaining production page-copy inventory before R3E

The three R3D production copy sites and their boundaries were confirmed in
source:

| Caller and source | Destination and copy | Allocation, identity, and lifetime | Pin/publication/failure/WAL |
|---|---|---|---|
| `DatabaseFile::readPage`; validated file bytes are parsed into a local `DatabasePage` | Caller output; `out = page` | Hosted vector copy; `DatabasePage::parse` checks expected page ID and full encoded extent; output is caller-owned | No cache pin. Output assignment follows successful parsing but can allocate without a local `bad_alloc` mapping. This is a committed-file read, outside WAL commit publication. |
| `Database::readPage`; pinned committed cache slot | Caller output; `out = slot->page` | Hosted vector copy; `pinPage(pageId)` validates the encoded page and the cache key; output is caller-owned | The slot remains pinned for the copy and is unpinned on both success and allocation failure. `bad_alloc` maps to `NoSpace`. This reads only the committed view. |
| `BufferManager::installPage(const DatabasePage&)`; hosted page value | Pinned slot view; `preparedView = page` when the matching slot is pinned, followed by a no-throw move into the stable slot object | Hosted vector copy for the view; a separate candidate image uses the cache allocator. The page value ID must equal the requested key and its encoded candidate passes R3A inspection | Cache image and view are published after preparation. The slot pin count and key are retained. Allocation failure returns `NoSpace` without changing the prior entry. This compatibility overload remains for already-materialized page writers. |

### Selected production boundary

```text
Selected production function:
  Transaction::commit -> Database::installCommittedPageImage
  -> BufferManager::installPage(pageId, encodedImage, byteCount)
Current allocating copy removed from this path:
  Parse transaction image into a temporary hosted DatabasePage, then pass it
  to installPage; a pinned replacement could deep-copy that value again.
Encoded page source:
  GxTransactionPageEntry::image, owned by the transaction overlay
Validated byte extent:
  Exactly the configured page size, at most the established 65,536-byte limit
Source allocator:
  Transaction page-overlay allocator (or its established hosted default)
Destination allocator:
  BufferManager cache allocator, through gxPageCacheAllocateImage
Required page identity:
  Transaction entry ID == encoded header ID == requested cache key == existing
  entry key, when replacing
Pin/reference obligations:
  Keep the BufferSlot object and pin count stable; prepare a replacement view
  before publication when a slot is pinned; never evict a pinned victim
Publication point:
  Nonallocating same-key image replacement, or existing eligible-victim publish
Pre/post-COMMIT classification:
  This route is called only after WAL COMMIT sync succeeds; earlier failures
  still use the existing rollback path
Expected remaining hosted operations:
  Pinned-slot view materialization; the DatabasePage compatibility writer;
  DatabaseFile::readPage and Database::readPage copies; other hosted payloads
```

`Transaction::commit` now hands the still-live transaction-owned image to the
synchronous installer. The installer validates and copies it into storage
owned by the cache allocator; it never takes ownership of the transaction
buffer. The source remains valid until installation returns.

## 4. Encoded validation and allocator ownership

Both encoded source and prepared cache candidate pass the established R3A
`gxDatabasePageInspect` validator. It checks the exact byte extent, configured
page size, expected page ID, supported page type, header and payload bounds,
and CRC before cache publication. The cache key is also checked against an
existing entry before replacement.

The cache allocates candidate storage through its own copied `GxAllocator`
tuple. The bounded byte copy is complete before the candidate is published;
the previous cache image is released through that same cache allocator only
after replacement succeeds. Transaction/source storage is not relabeled or
freed by the cache.

For a pinned resident page, the installer parses the already-copied candidate
into a temporary `DatabasePage` view before publication. A hosted allocation
failure is caught, the candidate is freed through the cache allocator, and the
old image and slot view remain intact. A successful view move is statically
required to be nonthrowing. Unpinned commits do not construct a hosted page
view.

No serializer, decoder, page format, or cache implementation was added. The
new `gxPageCacheReplaceImage` operation accepts only a same-key resident
replacement, preflights metadata overflow, preserves the pin count, updates
dirty accounting and LRU time, swaps the image, then frees the superseded
image. It performs no allocation.

## 5. Atomicity, references, and eviction

The installation sequence is:

1. Validate source extent, type, identity, payload bounds, and CRC.
2. Allocate candidate bytes through the cache allocator and copy the exact
   encoded image.
3. Validate the prepared candidate.
4. If the matching slot is pinned, prepare its replacement view and verify
   slot/key/pin consistency.
5. Replace the existing same-key image, or choose the existing empty/LRU
   unpinned target and flush a dirty victim if required.
6. Publish the candidate and synchronize the stable slot metadata.

Before the nonallocating publication step, failures free temporary storage and
leave the previous page image and slot view available. Same-key replacement
retains page identity and pin count. A new-key install uses the existing
capacity, LRU victim selection, and dirty-victim flush policy; pinned victims
remain ineligible. Cache misses still publish only after complete image
preparation and required victim I/O.

Focused tests retain a `BufferSlot*` across failed and successful pinned
replacements, verify slot address and pin counts, check the old page after
allocation/validation failure, and unpin exactly once. The supported borrow is
the stable slot/page object; as before, callers must not retain a raw pointer
into `std::vector` payload storage across a successful page replacement.

The existing P6 tests continue to cover cache capacity, LRU eviction, dirty
victim I/O failure, retry, and clean/dirty accounting. R3E additionally covers
repeated same-page image installation and the SQL commit path with a one-slot
cache that must evict a dirty page.

## 6. Allocation and source-lifetime fault matrix

| Fault or condition | Result and evidence |
|---|---|
| Corrupt CRC or mismatched encoded identity | Rejected as `CorruptPage` before publication; resident page and pin count remain unchanged. |
| Cache candidate allocation failure on a pinned existing page | Returns `NoSpace`; cache allocation count and outstanding ownership return to their prior values; old slot and encoded cache bytes remain readable. Retry succeeds. |
| Different source and cache allocator contexts | Source `GxOwnedPage` is copied into cache-owned bytes. Destroying the source owner does not change the cached result. Both arenas report zero invalid frees and no leaked allocation after cleanup. |
| Repeated same-key replacement | The source bytes remain exact, outstanding cache ownership stays constant, and key/pin metadata is retained. |
| Multi-page transaction cache allocation failure after durable COMMIT | Returns `CommitFailed`, finishes the transaction as committed, sets `recoveryRequired`, blocks reads and checkpointing, and leaves WAL evidence. Reopen replays both page images, including the allocated page. |
| Dirty-victim database write failure after durable COMMIT | Injected by the transaction/WAL regression. The old dirty cache entry remains owned, reads report `RecoveryRequired`, and retained WAL replays both committed updates after reopen. |
| Pinned hosted view allocation failure | The code catches `std::bad_alloc` before publication and releases the candidate through the cache allocator. The standard hosted vector allocator has no isolated R3E fault hook, so this specific allocator failure was not independently induced. |

The existing P1/P6/R3C suites retain coverage for initial cache allocation,
allocator contract rejection, capacity exhaustion, cache miss failure, and
cross-domain owner behavior. R3E adds no synthetic allocation hook to the
nonallocating publication step.

## 7. Durable COMMIT and transaction visibility

The order remains unchanged through WAL `BEGIN`, page images, optional header
image, `COMMIT`, and successful WAL sync. Only after that durability point does
the commit call the new encoded-image installer. Before that point, existing
transaction rollback and statement-savepoint behavior is unchanged.

If candidate allocation, pinned-view preparation, dirty-victim I/O, or cache
publication fails after durable COMMIT, `Transaction::commit` follows the
existing status contract: it returns `CommitFailed`, records that the
transaction committed, and sets `recoveryRequired`. `Database::readPage`,
`readOwnedPage`, and checkpoint/flush refuse to expose stale cache contents or
truncate the committed WAL. Close releases resources while retaining recovery
evidence. The new allocation and I/O fault tests reopen the database and
observe the committed page images after WAL replay.

Transaction overlay precedence is unchanged: `Transaction::readPage` reads
staged images first; committed database reads never consult that overlay.
Existing SQL tests retain statement savepoint failure/reuse, rollback,
read-your-writes, commit, and recovery coverage.

## 8. SQL9 workload and persistent bytes

The accepted production SQL9 workload and full SQL regression now exercise
`Transaction::commit` through the encoded-image installation route. Coverage
includes database create/open, multi-page inserts, index creation and indexed
reads, update/row relocation, delete and rollback, explicit transactions,
statement failure and reuse, close/reopen, primary/secondary indexes, and
foreign-key constraints. The additional R3E transaction tests prove that
actual committed overlay images call the new route and that post-COMMIT
allocation and database-page I/O failures recover correctly.

R3E does not change `.gxdb` or `.gxwal` bytes, page headers/types/sizes,
payload encoding, CRC, heap slots, B+ tree encoding, WAL framing, or transaction
ordering. The byte-copy path preserves the already-encoded overlay image
exactly. The focused publication test compares the installed bytes before and
after close/reopen with the source image.

## 9. Qualification

### Freestanding AMD64 census

Ran `tools/qualify_sql9_freestanding.ps1` with the Server AMD64 flags, including
`-std=c++14`, `-ffreestanding`, `-O2`, `-fno-exceptions`, `-fno-rtti`,
`-nostdlib`, `-nostdinc++`, `-fno-builtin`, `-m64`, `-mcmodel=large`,
`-mno-red-zone`, and the Server's no-SIMD/no-PIE/stack-probe flags. Result:

- 34 existing production translation units; **16 pass, 18 remain blocked**.
- All 16 previously passing units remain passing; no new production unit was
  added and no existing unit newly passed.
- `database/portable/gx_page_cache.cpp` passes with the new same-key image
  replacement operation. `database/portable/gx_owned_page.cpp` remains
  passing.
- `database/database_buffer.cpp` remains blocked at the existing hosted
  `<memory>` include through `database_file.h`; it was blocked before R3E.
- The same 18 units remain blocked at hosted headers:

  | Translation unit | First compiler diagnostic |
  |---|---|
  | `database/database_io.cpp` | `database/database_io.h:13:10: fatal error: cstdio: No such file or directory` |
  | `database/database_native_io.cpp` | `database/database_native_io.h:5:10: fatal error: memory: No such file or directory` |
  | `database/database_header.cpp` | `database/database_header.h:7:10: fatal error: string: No such file or directory` |
  | `database/database_page.cpp` | `database/database_page.h:7:10: fatal error: vector: No such file or directory` |
  | `database/database_file.cpp` | `database/database_file.h:17:10: fatal error: memory: No such file or directory` |
  | `database/database_engine.cpp` | `database/database_engine.h:9:10: fatal error: memory: No such file or directory` |
  | `database/database_buffer.cpp` | `database/database_file.h:17:10: fatal error: memory: No such file or directory` |
  | `database/database_catalog.cpp` | `database/database_catalog.h:35:10: fatal error: string: No such file or directory` |
  | `database/database_index.cpp` | `database/database_index.h:27:10: fatal error: set: No such file or directory` |
  | `database/database_heap.cpp` | `database/database_heap.h:26:10: fatal error: memory: No such file or directory` |
  | `database/database_wal.cpp` | `database/database_wal.h:30:10: fatal error: memory: No such file or directory` |
  | `database/database_transaction.cpp` | `database/database_transaction.h:25:10: fatal error: memory: No such file or directory` |
  | `database/database_relational.cpp` | `database/database_relational.h:21:10: fatal error: memory: No such file or directory` |
  | `database/database_sql_token.cpp` | `database/database_sql_token.h:11:10: fatal error: string: No such file or directory` |
  | `database/database_sql_tokenizer.cpp` | `database/database_sql_tokenizer.h:12:10: fatal error: string: No such file or directory` |
  | `database/database_sql_parser.cpp` | `database/database_sql_parser.h:13:10: fatal error: vector: No such file or directory` |
  | `database/database_sql_expr.cpp` | `database/database_sql_expr.h:18:10: fatal error: vector: No such file or directory` |
  | `database/database_sql.cpp` | `database/database_sql.h:16:10: fatal error: map: No such file or directory` |

- Existing unresolved symbols include checksum guard symbols
  `__cxa_guard_acquire`/`__cxa_guard_release` and hosted allocator
  `malloc`/`free`. The new cache object imports the established allocator
  callbacks `gxAllocatorAllocate` and `gxAllocatorFree`. It adds no runtime
  allocation, exception, RTTI, or C++ library dependency. The page-cache API
  uses the existing `GxAllocator` callback ABI. No native SQL link or execution
  readiness is claimed.

Artifacts are under
`%TEMP%\guideXOSSql-NDB1R4P11R3E-freestanding-20261020`.

### Hosted build and CTest

`cmake --build build` completed. It emitted only the existing unused-helper
warnings in unchanged `database_relational_test.cpp` and
`database_index_test.cpp`; modified production code and focused publication
test emitted no warnings.

Final full CTest results: **21/21 targets passed, 145,522 checks passed, zero
failures** in **823.00 seconds**. The additional target
`database_cache_publication_test` reports **64 checks passed, zero failed**.
The transaction target also includes the post-COMMIT dirty-victim I/O fault
and recovery test. All SQL1–SQL9, P5–P8, R1–R3D, and portability targets remain
enabled.

## 10. Remaining hosted boundaries and next continuation

The compatibility overload `BufferManager::installPage(uint64_t,
const DatabasePage&)` remains for already-materialized page writers. It stages
a validated cache-allocator image and, only when the matching slot is pinned,
copies the hosted page value into a temporary view before publication. The SQL
transaction commit path bypasses that overload and installs the transaction's
encoded image directly. `DatabasePage::payload` remains hosted.

The remaining reader copies are still `DatabaseFile::readPage` and
`Database::readPage`; transaction reads and cache slot materialization also
parse into hosted `DatabasePage` values. Heap and index page writers retain
their previously reported hosted payload operations. No broader conversion was
attempted.

Recommended next bounded continuation: establish the checked reader and
materialization contract for `DatabaseFile::readPage`, `BufferManager::materialize`,
`Database::readPage`, and `Transaction::readPage`. Preserve overlay precedence,
pin release on failure, prior caller output on failure, and the accepted
`GxOwnedPage` allocator/context lifetime. Then separately review whether the
`DatabasePage` compatibility writer still needs pinned-view cloning.

## 11. Closeout

- Outcome: `CACHE_PUBLICATION_OWNERSHIP_VERIFIED` for the encoded transaction
  commit path only.
- Starting SQL HEAD: `360edfdf18469b87bd67fb4bfae73d06a3dfd066` on `main`.
- Implementation acceptance commit:
  `ea1b601acf19e4707a31906cd4449557565427de`.
- This report is the report-only closeout commit that follows the implementation
  commit; its resulting HEAD is recorded in the task closeout.
- Both phase markers remain unchanged: SQL9 → SQL10 and NDB1R4P10 → NDB1R4P11.
- Server remains read-only and unchanged at
  `f29850c93bbac0a0763a31e806d2816310156575`.
- Full hosted CTest: 21/21 targets, 145,522 checks, zero failures.
- Freestanding census: 16/34 passing, 18 hosted-header blockers; no native
  link claim.
- No push or Git topology change was performed. Local implementation and
  report-closeout commits are recorded in the task closeout.
