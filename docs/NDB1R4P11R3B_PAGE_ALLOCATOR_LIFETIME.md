# NDB1R4P11R3B: Page Allocator Identity and Lifetime

## Outcome

**Outcome B — `PAGE_ALLOCATOR_LIFETIME_PARTIAL`.** The production ownership
paths and their lifetimes are verified, and the accepted R1 allocator rules
remain intact. R3B did not introduce a page allocator binding or allocation:
with `DatabasePage::payload` still a hosted `std::vector<uint8_t>`, no new
portable allocation can become the page's owned payload without either
replacing that representation or creating and copying a second payload buffer.
The first is expressly outside R3B and the second would not establish page
ownership or provenance. R3B therefore stops before source or test changes.

This is not an ownership-safety regression. The existing page payload remains
hosted and self-contained; the existing cache and transaction image owners
continue to release through their own allocator tuples. Parent P11 remains
open and neither phase marker advances.

## 1. Starting state and repository gates

- Writable SQL checkout: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `92d7b566090d8ca631a3175d8f51aa3a2c77492c`.
- Starting worktree: clean. `main` was one local commit ahead of
  `origin/main`, with no staged, modified, or untracked files.
- Ending HEAD is unchanged. This report is the only new, uncommitted file;
  no SQL source or test file changed.
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

- The P11 ownership audit and R1, R2, R3, and R3A reports were read. R3B
  had no report or implementation at preflight.
- Read-only Server checkout at preflight: branch `NATIVE_DB_STORAGE`, HEAD
  `f29850c93bbac0a0763a31e806d2816310156575`, clean.
- No prohibited Git operation, topology change, push, Server edit, or
  cross-repository synchronization was performed.

## 2. Accepted R3A and R1 foundations

R3A is present at the required SQL starting HEAD. The actual production
`DatabasePage::parse` calls `gxDatabasePageInspect` with `sourceLength` before
copying payload bytes. The portable inspector verifies an exact configured
page extent before reading the header, then checks page identity, supported
serialized type, payload bounds, and CRC. The four supported page types are
Catalog, Data, IndexLeaf, and IndexInternal. The parser builds a temporary
page and moves it to the caller only after validation and payload copying.
The file, cache, transaction, WAL, recovery, and open-validation paths pass
actual vector lengths or known fixed allocation extents as recorded in the
R3A report.

R3A's persisted results are 19/19 CTest targets, 145,271 checks, and zero
failures, with 15/33 freestanding translation units passing. The R3A storage
and relational tests exercise malformed extents and types through actual
production reads and confirm malformed parse attempts preserve a prior page.
The full current-source regression rerun and freestanding census are recorded
in Sections 12 and 13 below.

R1 still defines the cache allocator behavior: an omitted allocator selects
the hosted adapter; a non-null allocator missing either callback returns
`InvalidArgument`; a valid callback/context tuple is copied without
substitution; its context is borrowed and must outlive the Database. The
cache's Gx allocations are freed through that same callback tuple. R2's
`packHeapPage` scratch allocation is also Gx-backed and checked, but it is
destroyed before returning; the final `DatabasePage::payload` handoff remains
a checked hosted-vector copy.

## 3. Allocator and page ownership graph

```text
Caller-supplied pageCacheAllocator (or omitted hosted default)
  └─ Database::openImpl
      └─ BufferManager copies callback tuple; borrows context
          ├─ GxPageCache descriptors and serialized page images
          │    └─ released by GxPageCache with the same tuple
          └─ BufferSlot storage
               └─ BufferSlot::page payload is std::vector<uint8_t>

Caller-supplied transactionPageAllocator (or hosted default)
  └─ Transaction copies callback tuple; borrows context
      └─ Gx transaction overlay owns serialized page images
           └─ destroyed with the overlay through that same tuple

DatabasePage payloads from parse/read/build/copy
  └─ std::vector<uint8_t> and its hosted allocator
       └─ independent of Database, BufferManager, Transaction, cache, and WAL
```

The owner-category audit is:

| Creator / production path | Allocator source and storage | Page owner and lifetime | Escape, copy, move, destruction, and failure behavior |
|---|---|---|---|
| `DatabasePage` default construction | No allocation; empty hosted vector | The value's C++ owner | Public values can outlive every SQL context. Compiler-generated copies deep-copy the vector; moves transfer its hosted storage. Vector destruction releases it through the hosted allocator. |
| `DatabasePage::parse` | No Gx allocator argument. `payload.assign` allocates in the hosted vector | A temporary page is prepared, then moved into caller output | Caller output may escape. Validation errors preserve output. A vector allocation exception occurs before the move and is caught only by some production callers. |
| `DatabaseFile::validateOpenedPages` and bootstrap validation | `readPageBytes` produces hosted vectors; each parsed page is local | Local pages and byte vectors die before validation returns | No page escapes. File/parse errors propagate as `DbResult`; vector allocation remains hosted/exception-based. |
| `DatabaseFile::readPage` | Hosted byte vector and hosted parsed page | Caller owns `out` after return | `out = page` is a deep-copy allocation without a local `bad_alloc` conversion. The returned page can outlive the file or Database. |
| `BufferManager::materialize` on a cache hit | Cache image and descriptors use the manager's copied Gx tuple; `slot.page` payload uses hosted vector | Stable slot page view is valid while materialized and pinned; final unpin clears it | Parse prepares a temporary and moves on success. A parse failure leaves `materialized` false; hosted allocation exceptions map to `NoSpace`. Slot destruction happens before cache owner destruction. |
| `BufferManager::pinPage` cache miss | Candidate serialized image uses the Gx cache allocator; parsed page uses hosted vector | Candidate image is local until parse and victim preparation succeed; then the slot owns a hosted vector view | Parse failure frees candidate through `gxPageCacheFreeImage`; cache publish and pin happen after parse. Successful page view is moved into the slot. |
| `BufferManager::installPage` | Encoded image uses the Gx cache allocator; pinned-slot `viewCopy = page` uses hosted vector | Internal view lasts through pin/manager lifetime; input page remains caller-owned | Pinned replacement copies before touching image state; caught copy failure returns `NoSpace`. A successful prepared view is moved to the stable slot. Gx image cleanup uses the cache tuple. |
| `Database::readPage` | Cache slot is a borrowed hosted-vector view; destination copy uses hosted vector | Caller owns the independent output | The function pins, copies, catches `bad_alloc` as `NoSpace`, and unpins on that error and on success. Output may outlive Database/cache. |
| `Transaction::readPage` | Overlay image uses transaction allocator; parsed output uses hosted vector | Caller owns output; transaction owns only the serialized overlay | An overlay hit parses to output and maps `bad_alloc` to `NoSpace`; a base read delegates to `Database::readPage`. Output may outlive the transaction. |
| Transaction stage and savepoint | Transaction allocator tuple is copied into `Transaction` and portable overlay owners; context is borrowed | Transaction owns staged serialized images until commit/rollback/destruction | Gx clone/stage operations return status and release using the transaction tuple. No `DatabasePage` payload allocation is transferred into the overlay. |
| Commit-time reconstruction | Overlay bytes use transaction allocator; local reconstructed page uses hosted vector; cache image uses Database cache allocator | Local page dies after installation attempt | Parse/install happens after durable WAL COMMIT. Failure marks recovery required, finishes as committed, and returns `CommitFailed`; it is not represented as rollback. Local vector destruction remains hosted. |
| WAL recovery | `GxRecoveredPage` images use WAL/recovery Gx allocator; redo writes validated raw images | Recovery collection is scoped to recovery; reopened validation uses hosted local pages | Redo does not materialize `DatabasePage`. Recovery/image cleanup uses its original Gx tuple. |

`Database::close` rolls back an active transaction, destroys the buffer
manager (which destroys slot page views before cache images), closes WAL, and
closes the file. `Database::~Database` calls `close` when open. Cache and
transaction callback contexts therefore need to outlive the open Database.
Page values returned through `PageAccess`, `DatabaseFile`, `Database`, or
`Transaction` are caller-owned and may remain alive after those contexts end;
their current hosted vector ownership makes that safe.

## 4. Escaped lifetime policy

The cache allocator cannot be used as the owner of a returned page payload:
`Database::readPage` copies a page into caller storage that can outlive the
Database and its borrowed callback context. The transaction allocator cannot
be used either: a page returned from `Transaction::readPage` can outlive the
transaction. WAL/recovery and scratch allocators have the same narrower
scopes. Storing any of those borrowed tuples in a caller page would leave a
callback context that may already be destroyed when the page is released.

R3B therefore keeps every escaping page hosted. A future page owner must
receive an explicit caller-selected allocation domain whose context the page
owner is contractually required to outlive, or keep such escaping outputs
hosted. A callback tuple copied by value does not extend its `context`
lifetime. This phase does not infer permanent lifetime, add a global current
allocator, or introduce reference counting.

## 5. Allocator compatibility policy

For `GxAllocator`, allocation/release provenance is the allocator's
`allocate` callback, `deallocate` callback, and `context` together. Matching
function pointers with different contexts are incompatible. A hosted default
and a caller-provided allocator are separate domains, even if a caller
implements similar behavior. `maxAllocation` and `maxAlignment` are request
limits; they may make an operation invalid or too large, but do not make
different callback/context tuples compatible.

The safe future policy is:

- An ownership transfer is permitted only when the owner carries the original
  release tuple and the API explicitly permits that destination to retain the
  original domain. It must never relabel allocation A as allocation B.
- If the destination requires allocator B, a cross-domain clone allocates
  destination bytes through B, copies the data, then releases any temporary
  or old source allocation through A.
- An unsupported cross-domain transfer returns an explicit invalid/unsupported
  status before ownership changes.
- A missing explicit callback pair is `InvalidArgument`; omitted allocator
  selection preserves the existing hosted default where the API allows it.

No `DatabasePage` allocation or transfer currently uses Gx, so no R3B
allocator identity or transfer test can truthfully exercise this policy.

## 6. Selected production boundary and why it is blocked

The narrowest materialization candidate is the real cache-miss path:

```text
BufferManager::pinPage
  → allocate candidate serialized image
  → DatabaseFile::readPageImage validates actual image extent/type/CRC
  → DatabasePage::parse prepares a local page
  → flush dirty victim if needed
  → publish cache image and move page into stable slot
  → return pinned slot
```

| Required boundary fact | Current evidence |
|---|---|
| Selected production path | `BufferManager::pinPage` cache miss into a stable `BufferSlot` |
| Allocator creator | Caller supplying `pageCacheAllocator`, or the hosted adapter when omitted |
| Allocator owner | `BufferManager` and `GxPageCache` copy callback fields and context by value |
| Borrowed context lifetime | R1 requires the context to outlive the Database; it may not outlive a caller page |
| Page/materialization lifetime | Internal slot view ends on final unpin/clear or manager destruction. `Database::readPage` returns a separate caller-owned page. |
| Existing allocation | Serialized candidate image uses the Gx cache allocator. Parsed `payload.assign` uses hosted `std::vector`. |
| Matching release | Candidate image failures use `gxPageCacheFreeImage`; cache destruction frees published images through the same cache tuple. Slot vectors use hosted vector destruction. |
| Failure status | Gx cache allocation failure maps to `NoSpace`/existing cache status. Parse `bad_alloc` maps to `NoSpace`; malformed images map to `CorruptPage`. |
| Publication boundary | Candidate image and page are prepared before cache publication and pin increment. Parse failure frees candidate and does not publish the page. |
| Existing hosted allocations | File read vectors, page payload vectors, page copies, and serialization vectors remain hosted. |

There is no new R3B allocation in this path. The existing candidate image
allocation belongs to R1's cache owner and does not allocate a page payload.
Allocating an additional Gx payload buffer and then copying it into the still
hosted vector would add a second full payload buffer, discard the Gx owner
before page return, and leave no allocator identity attached to the page.
That would add cost without satisfying the ownership contract. An inert
allocator field would have the same problem. Replacing the vector payload
would cross the explicit R3B boundary and require migrating the writers and
copy sites listed in Section 14.

The selected path therefore remains unchanged. Its preexisting candidate,
parse, victim-flush, publication, and pin order was audited, but no R3B
fault-injection result is claimed. In particular, no new allocation failure
was injected into an R3B seam, no new cache pin test was added, and no
R3B-specific real-SQL materialization boundary is qualified.

## 7. Production SQL, cache, transaction, and WAL behavior

Existing SQL9 operations reach `BufferManager::pinPage` through
`Database::readPage`; catalog, heap, and index access use the same
`PageAccess` boundary. R1 has production cache allocator tests for open,
cache miss, replacement failure, cleanup, and retry. R2 tests heap update and
delete repacking through SQL and savepoints. R3A tests malformed page types
through the real cached read path, repair, close, reopen, and read again.
Those are evidence for the accepted existing behavior; they are not tests of
a new R3B allocator-aware payload seam.

The audit confirms that transaction overlay reads still prefer staged images
(read-your-writes), while base reads use the committed Database path. Staging
uses its separate portable serialized-image owner. Commit reconstruction is
after the durable WAL COMMIT point, and a later install error retains the
existing `recoveryRequired`/`CommitFailed` behavior. WAL redo continues to
write validated serialized images without a `DatabasePage` owner. No R3B
source change altered transaction savepoints, cache identities or pins,
commit ordering, recovery, or persistent format.

## 8. Focused R3B tests and failure evidence

- New focused R3B tests: **0**.
- New R3B portable production allocation fault injections: **0**.
- New R3B allocator A/B transfer or lifetime tests: **0**.
- New R3B cache pin/failure tests: **0**.

These counts are zero because no allocation seam was added; existing vector
allocation was not relabeled as Gx allocation. Existing R1, R2, and R3A
focused tests remain in the hosted suite. This report makes no claim of
R3B-specific allocation/release counts, lifetime instrumentation, or retry
success.

## 9. Persistent representation and hosted boundaries

No page header, page type, payload encoding, CRC, heap slot directory, B+ tree
representation, transaction framing, `.gxdb`, or `.gxwal` format changed.
`DatabasePage::payload` remains a public `std::vector<uint8_t>`. Its implicit
copy operations, vector allocation exceptions, serialization output growth,
and multiple hosted producers remain outside freestanding qualification.
No allocation through `GxAllocator` is claimed for those operations.

## 10. Remaining writer and implicit-copy inventory

| Production function/site | Current page allocation or copy | Classification after R3B |
|---|---|---|
| `DatabasePage::parse` (`database/database_page.cpp`) | `payload.assign` in a prepared temporary; final move to output | Hosted/unconverted payload; checked validation, hosted allocation exception |
| `DatabasePage::serialize` | `out.resize(pageSize)` | Hosted/unconverted serialized output; allocation exception is not a `DbStatus` |
| `DatabaseFile::readPageBytes` / `readPage` | Byte-vector growth, parse, then `out = page` deep copy | Hosted/unconverted; file copy lacks a local allocation-status conversion |
| `DatabaseFile::create` bootstrap root | `root.payload = makeCatalogPayload()` and root serialization | Hosted/unconverted producer/output |
| `BufferManager::materialize` / cache-miss `pinPage` | Parse allocates hosted vector payload; cache image itself is Gx-owned | Allocator-aware cache image plus hosted page payload |
| `BufferManager::installPage` pinned entry | `viewCopy = page` deep copy before cache image publication | Hosted/unconverted copy; existing `bad_alloc` maps to `NoSpace` |
| `Database::readPage` | `out = slot->page` deep copy while pinned | Hosted/unconverted copy; existing `bad_alloc` maps to `NoSpace` and unpins |
| `Transaction::readPage` and commit reconstruction | Parse serialized transaction image into hosted page vector | Portable transaction image plus hosted/unconverted page payload |
| `Table::applyMutationsInTransaction` / `packHeapPage` | R2 `gxHeapPagePrepare` bounded scratch, then caught copy into hosted vector and swap | Allocator-aware preparation, hosted payload handoff with explicit status |
| `Table::appendEncodedRowToTail` | Existing-page writes mutate vector in place; new tail page uses `payload.assign(capacity, 0)` | New-page allocation hosted/unconverted; direct existing-page mutation does not allocate payload |
| `IndexTree::writeLeaf` | `payload.assign(cap, 0)` | Hosted/unconverted page construction |
| `IndexTree::writeInternal` | `payload.assign(cap, 0)` | Hosted/unconverted page construction |
| Catalog v2 save | Root and continuation `payload = pagePayloads[...]` | Hosted/unconverted payload copies |
| Catalog v3 save | Root and continuation `payload = pagePayloads[...]` | Hosted/unconverted payload copies |
| Catalog v4 save | Root and continuation `payload = pagePayloads[...]` | Hosted/unconverted payload copies |
| Production `DatabaseFile::readPage` copy | Parsed local page → caller `out` | Implicit allocating deep copy, no local `bad_alloc` conversion |
| Production `Database::readPage` copy | Pinned slot page → caller `out` | Implicit allocating deep copy, caught; pin is released on failure |
| Production `BufferManager::installPage` copy | Incoming page → prepared pinned-slot view | Implicit allocating deep copy, caught before cache image publication |
| Test-only P7/P8 copies | Five explicit `DatabasePage` deep copies in the retained tests | Hosted copies; migrate to status-returning clone with the owner |

No production API returns a `DatabasePage` by value. Parse and cache-miss
publication use moves; slot storage is placement-constructed and individually
destroyed rather than raw-relocated. Heap/catalog decoders borrow vector
references synchronously and do not retain payload pointers. The inventory
does not imply that page payload or vector allocation is portable.

## 11. Freestanding qualification and symbol census

Ran the unchanged `tools/qualify_sql9_freestanding.ps1` with its configured
Server AMD64 compiler settings, including C++14,
`-ffreestanding -nostdlib -nostdinc++ -fno-exceptions -fno-rtti`,
`-fno-builtin`, `-DGXOS_BARE_METAL`, the Server kernel include roots, and the
AMD64 large-code-model/no-red-zone/no-SIMD/no-PIC options.

| Census item | Result |
|---|---:|
| Production translation units | 33 |
| Passing | 15 |
| Blocked | 18 |
| Newly passing / new portable R3B objects | 0 / 0 |

All 15 previously passing units remain passing:
`database/database_format.cpp`, `database/database_checksum.cpp`,
`database/portable/gx_allocator.cpp`,
`database/portable/gx_allocator_hosted.cpp`,
`database/portable/gx_array.cpp`,
`database/portable/gx_heap_page_builder.cpp`,
`database/portable/gx_recovered_pages.cpp`,
`database/portable/gx_page_cache.cpp`,
`database/portable/gx_page_codec.cpp`,
`database/portable/gx_wal_record.cpp`,
`database/portable/gx_wal_scan.cpp`,
`database/portable/gx_wal_page_image.cpp`,
`database/portable/gx_transaction_pages.cpp`,
`database/portable/gx_transaction_page_index.cpp`, and
`database/portable/gx_transaction_page_overlay.cpp`. There is no R3B object
to add to that set.

The 18 unchanged first diagnostics are:

| Unit | First diagnostic |
|---|---|
| `database/database_io.cpp` | `database_io.h:13`: missing `<cstdio>` |
| `database/database_native_io.cpp` | `database_native_io.h:5`: missing `<memory>` |
| `database/database_header.cpp` | `database_header.h:7`: missing `<string>` |
| `database/database_page.cpp` | `database_page.h:7`: missing `<vector>` |
| `database/database_file.cpp` | `database_file.h:17`: missing `<memory>` |
| `database/database_engine.cpp` | `database_engine.h:9`: missing `<memory>` |
| `database/database_buffer.cpp` | `database_file.h:17`: missing `<memory>` |
| `database/database_catalog.cpp` | `database_catalog.h:35`: missing `<string>` |
| `database/database_index.cpp` | `database_index.h:27`: missing `<set>` |
| `database/database_heap.cpp` | `database_heap.h:26`: missing `<memory>` |
| `database/database_wal.cpp` | `database_wal.h:30`: missing `<memory>` |
| `database/database_transaction.cpp` | `database_transaction.h:25`: missing `<memory>` |
| `database/database_relational.cpp` | `database_relational.h:21`: missing `<memory>` |
| `database/database_sql_token.cpp` | `database_sql_token.h:11`: missing `<string>` |
| `database/database_sql_tokenizer.cpp` | `database_sql_token.h:12`: missing `<string>` |
| `database/database_sql_parser.cpp` | `database_sql_parser.h:13`: missing `<vector>` |
| `database/database_sql_expr.cpp` | `database_sql_expr.h:18`: missing `<vector>` |
| `database/database_sql.cpp` | `database_sql.h:16`: missing `<map>` |

The passing portable objects retain their prior symbol profile. The page
codec's CRC functions, allocator/array/cache/WAL/transaction owner APIs, and
the checksum's known `__cxa_guard_acquire` / `__cxa_guard_release` references
remain. The hosted adapter still references `malloc` and `free`. No new
freestanding object, ABI dependency, full native link, or native SQL runtime
is claimed. In particular, `database_page.cpp` is still blocked at
`<vector>`. The next hosted dependency visible from its include chain is
`database_result.h`'s `<string>` through `DbResult`; removing the vector include
alone will not qualify the translation unit.

Server reference (read-only): its kernel `operator new` and `operator new[]`
delegate to `kernel_alloc`, which returns null when the fixed heap has no
fitting block; matching delete operators delegate to `kernel_free`. This
global interface is not a checked page allocator substitute. The separate
Server `allocator.h` is hosted C++ code using vectors, mutexes, and maps, not a
freestanding page-allocation contract. The native database bridge is a C ABI
over VFS callbacks and has no SQL `GxAllocator` integration. The inspected
Server AMD64 flags include `-m64 -mcmodel=large -mno-red-zone -mno-mmx
-mno-sse -mno-sse2 -fno-pic -fno-pie`, alongside its freestanding,
`-nostdinc++`, no-exceptions, and no-RTTI options. No Server file or state was
changed.

## 12. Full hosted regression

The full current-source CTest run was started with:

```text
ctest --test-dir build --output-on-failure
```

The run completed with **19/19 CTest targets passed, 145,271 checks passed,
zero failures, and 3,244.31 seconds total**. The check count is the sum of the
per-target result lines in `build/Testing/Temporary/LastTest.log`. No test
target was disabled or had its assertions changed. It includes SQL9, WAL
recovery, savepoints, cache allocator behavior, portable heap construction,
and R3A page validation. The check total matches the accepted R3A source
baseline; R3B added no assertions.

## 13. Exact R3C handoff

R3C should begin by implementing a caller-owned page allocation binding whose
context lifetime is explicit and independent of the Database/cache/transaction
lifetimes for any page that may escape. Keep null/default hosted behavior and
reject incomplete explicit tuples. The binding must preserve the complete
callback/context identity. Do not pass the cache allocator to caller outputs
or the transaction allocator to page reads. Then migrate the smallest
coherent writer/materialization set below; each row is a prerequisite
specification, not work started here.

| Source function | Allocator binding | Potential allocation | Current implicit copy | Replacement operation | Status propagation | Rollback/publication rule | Required tests |
|---|---|---|---|---|---|---|---|
| `DatabasePage::parse` | Destination's caller-selected page owner; internal cache callers may use their manager-scoped owner | Bounded payload allocation after R3A inspection | Current `payload.assign`; final temp move | Status-returning prepare/clone into a temporary owner, then nonallocating publish | Invalid tuple → `InvalidArgument`; OOM → `NoSpace`; malformed bytes → `CorruptPage` | Preserve destination and its allocator identity on all failures | Invalid/omitted/valid allocator; A/B contexts; parse failure; allocator fault; retry; exact codec bytes |
| `DatabaseFile::readPage` and file validation locals | Caller domain for returned output; scoped hosted/default domain for validation-only pages | Byte image plus payload owner | `out = page` | Parse/prepare directly into temporary output owner, then publish | Return I/O, parse, and owner statuses without exception leakage | Preserve caller output; validation temporaries die before return | Read success/failure; page survives file close; injected page-owner failure; truncated/corrupt source |
| `BufferManager::materialize` / cache-miss `pinPage` | Manager's valid cache-scoped page owner for slot views | Slot payload; candidate image already uses cache allocator | Parse-to-slot and local-to-slot move | Prepare page owner before setting `materialized`, publishing candidate, or incrementing pin | OOM/invalid allocator reaches caller as status | Free candidate with cache tuple; preserve old cache identity, pins, and victim until preparation succeeds | Hit/miss; A and B; failed preparation; existing entry pin; replacement; clear/close; retry |
| `Database::readPage` | Allocator already owned by caller's destination page | Destination deep clone | `out = slot->page` | Cross-domain status-returning clone from cache view into caller destination | Propagate clone status and unpin exactly once | Keep output/cache unchanged on failure; do not let page borrow cache context | A cache page copied to B; page used after DB close; injected clone failure and retry; pin counts |
| `BufferManager::installPage` | Source owner's binding plus manager-scoped slot binding | Clone only for a pinned slot view; encoded candidate image already Gx-backed | `viewCopy = page` | Clone bytes into slot domain; never relabel A storage as B | Propagate clone/encode/allocation status | Prepare clone and image before changing stable view/image; preserve cache identity/pins on failure | Same-domain and A→B clone; rejected transfer; failed clone/encode; pinned replacement; cache eviction |
| `Transaction::readPage` | Caller destination binding; transaction image allocator remains separate | Parsed destination payload | Overlay-image parse into output | Parse/copy into caller page owner | Preserve existing status mapping including `NoSpace` | Leave caller output and staged overlay unchanged on failure | Read-your-writes; page outlives transaction; A/B; failure/retry; rollback |
| `Transaction::commit` reconstruction | Database internal/cache destination owner, not transaction overlay owner | Temporary reconstructed payload | Parse local, then `Database::writePage` and optional cache-view clone | Prepare into committed-owner page, then cache installation | Before durable COMMIT, existing rollback; after durable COMMIT, existing `CommitFailed` + recovery-required rule | Do not call a durable commit rollback; WAL remains recovery evidence | Fault after durable COMMIT; reopen/recovery; committed isolation and WAL byte invariants |
| `Table::applyMutationsInTransaction` / `packHeapPage` | Page domain from active `PageAccess`; R2 currently borrows cache allocator only for scoped scratch | R2 GxArray scratch plus hosted handoff | Current `hostedPayload` full payload copy and swap | Write prepared bytes directly into the page owner's bounded storage once that owner exists | Preserve R2 status mappings | Do not stage a page until preparation succeeds; statement savepoint restores earlier staged pages | R2 fault-after-first-stage scenario; byte oracle; UPDATE/DELETE; retry/rollback; index consistency |
| `Table::appendEncodedRowToTail` | Active transaction/page owner | New-page payload and current row-planning buffers | New page `payload.assign`; no deep page copy at that operation | Checked page-owner initialization and bounded writes | Return OOM/limit before `writePage` | First establish statement savepoint for multirow `executeInsert`; rollback partial statement | Insert second-row allocation failure; transaction remains valid; retry/rollback; heap/index consistency |
| `IndexTree::writeLeaf` and `IndexTree::writeInternal` | Active `PageAccess` page owner | Zeroed leaf/internal payload | No whole-page copy at construction; `payload.assign` allocates | Status-returning bounded owner preparation followed by existing encoding | Return status before page staging | Do not publish root/sibling/parent changes until page preparation succeeds; transaction rollback remains authoritative | Leaf/internal split failure; root change; A/B allocator; retry; reopen/index validation |
| Catalog v2/v3/v4 save paths | Active `PageAccess` page owner | Root/continuation page payloads | Six `payload = pagePayloads[...]` copies across three versions | Checked clone/replace or encode directly into prepared owners | Propagate owner status from each save result | Keep transaction overlay/savepoint unchanged when any continuation fails | All catalog versions; fail each root/continuation allocation; retry; close/reopen |
| `DatabaseFile::create` bootstrap page and `DatabasePage::serialize` | Create caller/page domain; serialized-image domain kept explicit | Bootstrap payload plus output page image | `root.payload = makeCatalogPayload()`; `out.resize(pageSize)` | Checked page initialization and checked output-image owner; no vector exception path | Return `NoSpace`/invalid configuration distinctly | Do not publish output page/file header until bootstrap image is complete | Create failure/retry; supported page sizes; exact bootstrap bytes and CRC |

After those operations have explicit failure-aware semantics, migrate the
five P7/P8 test-only copies and remove implicit `DatabasePage` copy operations
only after every production copy caller has changed. Keep public caller-owned
pages independent of Database close, and verify callback context outlives
each retained owner through controlled lifetime instrumentation. Preserve
the R3A parser boundary and do not alter persistent formats. R3C is not
started by this report.

## 14. Closeout

- Outcome: `PAGE_ALLOCATOR_LIFETIME_PARTIAL`.
- SQL branch and HEAD: `main`,
  `92d7b566090d8ca631a3175d8f51aa3a2c77492c` (unchanged).
- Both phase markers: unchanged as recorded in Section 1.
- Server: read-only; no files, branch, build, or markers changed.
- SQL source/test changes: none. No local commit or push was made for this
  incomplete outcome. This report is preserved in the worktree as evidence.
- Persistent format, transaction/WAL semantics, cache identity and pin rules:
  unchanged.

### Acceptance question

Does the authoritative SQL9 production page-materialization path now carry
and enforce a valid allocator identity and lifetime contract, with matching
allocation/release behavior, checked failures, and no unsafe cross-allocator
transfers, while preserving existing page, cache, transaction, and WAL
semantics?

**No.** The existing cache and transaction image owners have valid allocator
contracts, but the materialized and returned `DatabasePage::payload` remains
hosted and has no Gx allocator binding. R3B did not claim portable page-payload
ownership or a new production allocation seam.
