# NDB1R4P11R3: Page Materialization Checkpoint

## Outcome

**Outcome B — `PAGE_MATERIALIZATION_PARTIAL`.** The R2 baseline, current
page-reader/cache/transaction paths, page-copy sites, allocator boundaries,
recovery behavior, and current production compiler blockers were verified.
No production payload conversion was started because the allocator and
caller gates are not satisfied. `DatabasePage::payload` remains
`std::vector<uint8_t>`. This report is a safe pre-conversion checkpoint, not
completion of the R3 materialization seam or parent P11.

## 1. Repository and phase gates

- Writable checkout: `D:\dev\guideXOS_SqlDatabase`.
- Branch: `main`.
- Starting and ending SQL HEAD: `0ca7e2e173214e1a261c0496810f60fbbccfc4ac`
  (`NDB1R4P11R2: add portable heap page preparation`).
- Starting worktree: clean; no R3 report or implementation existed.
- `.phase`: `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase`: `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- Ending worktree contains this new report only. No production or test source
  changed. No local commit was made for this Outcome B checkpoint.

The R1 and R2 reports were read and checked against their accepted claims.
The read-only Server repository remains on `NATIVE_DB_STORAGE` at
`f29850c93bbac0a0763a31e806d2816310156575`, clean and unmodified. Its AMD64
Makefile confirms the target options used by the SQL qualification script.
No Server file, branch, marker, or Git state was changed.

## 2. Accepted R1/R2 evidence and reproduced compiler baseline

R1 records `PAGE_ALLOCATOR_CONTRACT_VERIFIED`, with 18/18 CTest targets,
145,014 checks, zero failures, and 619 focused P6 allocator checks. R2 records
`HEAP_PAGE_CONSTRUCTION_SEAM_VERIFIED`, with 19/19 targets, 145,173 checks,
zero failures, and 159 focused R2 checks. R2's complete hosted suite is the
accepted unchanged-source regression baseline for this checkpoint; the suite
was not rerun because no production or test source changed.

The R2 freestanding count is 15/33: the 14 previously passing production
objects remain passing, and the newly added
`database/portable/gx_heap_page_builder.cpp` is the fifteenth. Eighteen
original units remain blocked. The actual R2 implementation was inspected:
`Table::applyMutationsInTransaction` calls `packHeapPage`, which prepares the
existing heap payload in a bounded `GxArray` through `gxHeapPagePrepare`,
checks its `GxStatus`, catches failure while transferring into a temporary
hosted vector, and only then swaps the completed vector into the local page.
This checks that heap repacking seam; it does not convert the page owner or
other page writers.

The unchanged `tools/qualify_sql9_freestanding.ps1` script was run before any
source change with the Server AMD64 flags. Its artifacts are in
`%TEMP%\guideXOSSql-NDB1R4P11R3-before-20261019`. It reproduced 15/33 passes
and these first blockers:

| Production unit | First current diagnostic |
| --- | --- |
| `database/database_io.cpp` | `database/database_io.h`: missing `<cstdio>` |
| `database/database_native_io.cpp` | `database/database_native_io.h`: missing `<memory>` |
| `database/database_header.cpp` | `database/database_header.h`: missing `<string>` |
| `database/database_page.cpp` | `database/database_page.h`: missing `<vector>` |
| `database/database_file.cpp` | `database/database_file.h`: missing `<memory>` |
| `database/database_engine.cpp` | `database/database_engine.h`: missing `<memory>` |
| `database/database_buffer.cpp` | `database/database_file.h`: missing `<memory>` |
| `database/database_catalog.cpp` | `database/database_catalog.h`: missing `<string>` |
| `database/database_index.cpp` | `database/database_index.h`: missing `<set>` |
| `database/database_heap.cpp` | `database/database_heap.h`: missing `<memory>` |
| `database/database_wal.cpp` | `database/database_wal.h`: missing `<memory>` |
| `database/database_transaction.cpp` | `database/database_transaction.h`: missing `<memory>` |
| `database/database_relational.cpp` | `database/database_relational.h`: missing `<memory>` |
| `database/database_sql_token.cpp` | `database/database_sql_token.h`: missing `<string>` |
| `database/database_sql_tokenizer.cpp` | `database/database_sql_token.h`: missing `<string>` |
| `database/database_sql_parser.cpp` | `database/database_sql_parser.h`: missing `<vector>` |
| `database/database_sql_expr.cpp` | `database/database_sql_expr.h`: missing `<vector>` |
| `database/database_sql.cpp` | `database/database_sql.h`: missing `<map>` |

The passing objects are `database_format.cpp`, `database_checksum.cpp`,
`gx_allocator.cpp`, `gx_allocator_hosted.cpp`, `gx_array.cpp`,
`gx_heap_page_builder.cpp`, `gx_recovered_pages.cpp`, `gx_page_cache.cpp`,
`gx_page_codec.cpp`, `gx_wal_record.cpp`, `gx_wal_scan.cpp`,
`gx_wal_page_image.cpp`, `gx_transaction_pages.cpp`,
`gx_transaction_page_index.cpp`, and `gx_transaction_page_overlay.cpp`.
The hosted allocator adapter's `malloc/free` references remain part of the
existing census and are not a claim that it belongs in the kernel path.

No source object changed during R3, so the passing set, unresolved symbols,
and ABI are unchanged from R2. No new object or runtime symbol was introduced.
`database_page.cpp` is still blocked first by `<vector>`; after removing that
include, its current header dependency on `DbResult` also reaches
`database_result.h`, which includes `<string>`. That second blocker was
inspected in source but was not compiled as a changed state.

## 3. Current payload and allocator ownership

`DatabasePage` in `database/database_page.h` publicly owns
`std::vector<uint8_t> payload`. It has no custom copy or destruction contract.
The vector performs hidden allocation on growth and implicit deep-copy
construction/assignment. `payloadSize` is separate metadata and must stay
synchronized with `payload.size()`.

R1's `pageCacheAllocator` owns bounded cache descriptors and serialized cache
images. R2 also borrows it for temporary heap packing storage, which is
released before the pack operation returns. R1's transaction-page allocator
owns serialized page images in the transaction overlay. Neither domain is a
page-payload owner. A `DatabasePage` returned by `Database::readPage` or
`Transaction::readPage` is caller-owned and can outlive the Database or the
Transaction, so retaining either of those temporary/database-scoped contexts
in a page would be unsafe.

There is currently no `pagePayloadAllocator` option, no page allocator
accessor on `PageAccess`, and no allocator provenance in `DatabasePage`.
`DatabasePage::parse(bytes, pageSize, expectedPageId, out)` accepts no
allocator or explicit source length. It validates page size, identity, payload
bounds, and CRC through `gxDatabasePageInspect`, fills a temporary page using
`vector::assign`, then move-assigns the temporary to `out`. The codec reads the
16-bit page type but does not validate it against the currently declared
`PageType` values; page-type acceptance policy must be explicit before a new
materializer can claim it validates types. The current pointer-only parse
signature also cannot independently detect a truncated source buffer.
Malformed identity/bounds/CRC returns a status before publication, but
`bad_alloc` from payload assignment is an exception rather than a
`DbResult::NoSpace` result at this API boundary.

A safe future page owner needs a separate page-payload allocator option for
create/open, copied callback tuples in the database/file context, and a clear
borrowed-context rule: the callback context must outlive every page allocated
through it, including pages returned to callers. Each page owner must retain
the matching allocator tuple for destruction. The hosted null default and
R1's rejection of incomplete explicit allocators must remain intact.

## 4. Production materialization and publication paths

### File read and validation

`DatabaseFile::readPage` (`database/database_file.cpp:392-418`) checks file
state and page id, reads a full serialized page into a temporary hosted
vector, parses it into a local `DatabasePage`, and then executes `out = page`.
That last operation is an implicit allocating copy with no local
`bad_alloc`-to-status conversion. The destination is caller-owned; there is
no live cache pin on this direct file path.

`DatabaseFile::validateOpenedPages` parses the bootstrap/root page and, when
requested, every page (`database/database_file.cpp:330-370`). Database open
has an outer allocation catch, but this is also a page-materialization caller
that must receive the allocator when `parse` changes. `DatabaseFile::create`
builds its bootstrap page by assigning a returned hosted catalog vector at
`database/database_file.cpp:223`.

`DatabaseFile::readPageImage` (`database/database_file.cpp:420-459`) reads into
caller-provided serialized-image storage and validates it with the portable
codec. It does not create a `DatabasePage`. This is the route used by the
committed cache on a miss.

### Cache materialization, pinning, and installation

`BufferManager::materialize` (`database/database_buffer.cpp:189-208`) parses a
resident serialized cache image into the stable `BufferSlot::page` view. It
catches `bad_alloc`, sets `materialized` only after parse succeeds, then syncs
slot metadata. A cache-hit `pinPage` materializes before incrementing either
pin count; if pinning fails after a newly materialized view, it discards that
view. Thus parse failure does not acquire a pin.

On a cache miss, `pinPage` (`database/database_buffer.cpp:284-331`) selects an
unpinned target, allocates a candidate image through the cache allocator,
reads and validates it, parses it into a local page, flushes a dirty victim if
needed, then publishes the image and moves the page into the stable slot.
Failures before publication free the candidate and do not increment a pin.
The slots themselves are placement-constructed in a fixed array and are not
raw-relocated.

`installPage` (`database/database_buffer.cpp:423-509`) has separate existing
entry and cache-miss paths. For an existing entry with a nonzero pin count it
first copies the incoming page into a temporary `viewCopy`; a copy allocation
failure returns `NoSpace` before touching the cache image or replacing the
slot page. After successful preparation it touches/encodes/marks the existing
entry, then move-assigns `viewCopy` into the stable slot. For a new entry it
allocates and encodes a candidate image before flushing/publishing the victim.
Cache descriptor pin counts and slot object addresses remain stable on the
existing-entry path. A successful replacement changes the page's payload
storage, so callers must not retain a `payload.data()` pointer across
installation even if they still hold the slot address. The allocation-failure
preservation behavior has not been fault-injected for a portable page owner.

`unpin` (`database/database_buffer.cpp:534-557`) validates the slot identity,
encodes dirty views before decrementing pin counts, and discards the
materialized view after the final pin. `Database::readPage`
(`database/database_relational.cpp:114-136`) pins, copies the page into the
caller output, maps `bad_alloc` to `NoSpace`, and unpins on both success and
copy failure. Its returned page is independent of cache-view lifetime.

### Transaction reads, commit, and recovery

`Transaction::readPage` (`database/database_transaction.cpp:81-118`) checks
the transaction overlay first. An overlay hit parses the transaction-owned
serialized image into the output and catches allocation failure; a miss below
the base page count delegates to `Database::readPage`. This preserves
read-your-writes and does not expose staged bytes through the committed cache.
`Transaction::writePage` encodes/copies bytes into the separate portable
transaction overlay; it does not retain a `DatabasePage` pointer or copy its
owner.

`Transaction::commit` appends overlay images and COMMIT to the WAL, then syncs
the WAL at the durability point. After that it publishes the page count,
parses each committed overlay image, and calls `_db.writePage` to install it
into the committed cache (`database/database_transaction.cpp:209-310`). A
post-sync parse, page-count, or cache-install failure sets
`recoveryRequired`, finishes the transaction as committed, returns
`CommitFailed`, and leaves the durable WAL as recovery evidence. It does not
roll back a durable commit. The existing behavior should be preserved by any
new owner.

Recovery redo (`database/database_relational.cpp:301-330`) iterates portable
`GxRecoveredPage` images and calls `DatabaseFile::writePageImage`, which
validates the encoded bytes before writing them. It does not materialize a
`DatabasePage` during redo. After flush, `validateOpenedPages(true)` parses
the resulting pages. This distinction matters: committed cache installation
uses parsed page values, while WAL redo applies validated serialized images.

## 5. Copy, move, borrow, and owner call-site inventory

The explicit production copies of `DatabasePage` are these three assignments.
The five P7/P8 test copies identified by the R1 audit were revisited here.
There is no production container of `DatabasePage` objects. `DatabasePage`
test helpers that return newly constructed pages use return-by-value and
compiler elision/vector move; they are not implicit deep-copy sites. The
helpers are `makePage` in `tests/database_ndb1r4p5_test.cpp:162`,
`tests/database_ndb1r4p6_test.cpp:171`, and
`tests/database_wal_recovery_portable_test.cpp:112`; callers initialize
locals from those returned values at `database_ndb1r4p5_test.cpp:294,331,336`,
`database_ndb1r4p6_test.cpp:188`, and
`database_wal_recovery_portable_test.cpp:146,151,159`. No production function
returns a `DatabasePage` by value. No page-object container insertion was
found. Parse, cache miss, and reset paths use page moves. `BufferManager` has
a compile-time nonthrowing page-move-assignment check.

| Function/site | Source → destination ownership | Copy/move; possible allocation | Error path, live pin, and replacement safety | Proposed migration |
| --- | --- | --- | --- | --- |
| `DatabaseFile::readPage`, `database/database_file.cpp:416` | Local parsed page → caller-owned output; both use hosted default vector allocator | Copy assignment; yes | No local allocation status catch; no cache pin; no explicit status-aware preservation guarantee for caller output | `clonePage` to a temporary using the page-payload allocator, then nonallocating transfer |
| `Database::readPage`, `database/database_relational.cpp:128` | Borrowed cache slot page → caller-owned output | Copy assignment; yes | `bad_alloc` maps to `NoSpace` and the pin is unpinned on failure; live cache pin is held during copy; cache entry itself is not replaced | Explicit checked clone to caller's intended page allocator, preserving output and unpinning exactly once |
| `BufferManager::installPage`, `database/database_buffer.cpp:445` | Borrowed incoming page → temporary cache slot view using hosted `std::vector` default allocator, not the portable cache allocator | Copy assignment; yes | Caught as `NoSpace` before cache image publication; target may be pinned; clone failure preserves old view/image/pin; successful install replaces view payload storage | Explicit checked clone through cache-view page allocator before image publication; transfer after encoding succeeds |
| P7 `testProductionTransactionFailuresAndSavepoint`, `tests/database_ndb1r4p7_test.cpp:249` | Caller-owned `committed` → caller-owned `firstWrite` | Copy construction; yes | No live pin; no status channel | Explicit test clone and assert status |
| P7 same function, `database_ndb1r4p7_test.cpp:251` | Caller-owned `firstWrite` → caller-owned `secondWrite` | Copy construction; yes | No live pin; no status channel | Explicit test clone and assert status |
| P7 same function, `database_ndb1r4p7_test.cpp:319` | Caller-owned `secondWrite` → caller-owned `thirdWrite` | Copy construction; yes | No live pin; no status channel | Explicit test clone and assert status |
| P8 transaction-page-index test, `tests/database_ndb1r4p8_test.cpp:608` | Caller-owned page parsed from transaction overlay → caller-owned `changed` | Copy construction; yes | No live pin; no status channel | Explicit test clone and assert status |
| P8 same test, `tests/database_ndb1r4p8_test.cpp:634` | Caller-owned `changed` → caller-owned `replacement` | Copy construction; yes | No live pin; no status channel | Explicit test clone and assert status |

For the five P7/P8 copies, the source and destination payloads currently use
`std::vector`'s hosted allocator; those tests configure the transaction-image
allocator, not a page-payload allocator. The tests also construct two new page
payloads through direct `payload.assign` calls. These cases need to use and
fault-inject the explicit page owner after that owner exists.

Other ownership edges found:

- `DatabasePage::parse` borrows encoded bytes, allocates into a temporary
  owned page, then move-publishes to its output. Its move is nonallocating
  under the current vector implementation; payload allocation failure throws.
- `BufferManager::pinPage` parses into a local owned page then moves it into a
  stable slot after cache-image preparation. `materialize` parses directly to
  the slot after prior view discard; it sets the materialized flag only on
  success.
- `BufferManager::installPage` borrows the caller page only synchronously. The
  cache's portable encoded image is a distinct owner.
- `Database::readPage` returns an independent owned page and releases its
  cache pin before returning.
- Transaction and WAL page-image owners copy serialized bytes; they do not
  share or retain `DatabasePage::payload` pointers.
- No page payload pointer was found stored past a synchronous operation in
  the audited production call sites. The public vector and public cache slot
  still permit external borrowers to retain pointers incorrectly.
- `database/database_heap.cpp:100` and four catalog decode sites
  (`database/database_catalog.cpp:1231,1317,1449,1923`) bind the payload
  directly as `const std::vector<uint8_t>&`. These are borrowed views with no
  allocation, but their declared type must migrate with any non-vector owner.

## 6. Payload construction sites outside the reader group

Replacing the public vector without first converting these checked production
writers would leave them allocating without a status channel:

| Producer | Current source location | Current operation and status boundary |
| --- | --- | --- |
| Database bootstrap page | `database/database_file.cpp:223` | Assigns `makeCatalogPayload()` to the page vector; the surrounding create path has a `DbResult`, but vector allocation is exception-based |
| Heap mutation repack | `database/database_heap.cpp:272-302` | R2 uses bounded portable scratch and catches the hosted vector transfer before swapping; the owner is still hosted |
| Heap tail/new page | `database/database_heap.cpp:1161` | `payload.assign(capacity, 0)` before writing row/slot bytes; no payload allocator status is returned |
| Index leaf construction | `database/database_index.cpp:315` | `payload.assign(cap, 0)` before encoding entries; no payload allocator status is returned |
| Index internal construction | `database/database_index.cpp:454` | `payload.assign(cap, 0)` before encoding children; no payload allocator status is returned |
| Catalog v2 save | `database/database_catalog.cpp:1663,1679` | Root/continuation payload vectors copied from `pagePayloads`; save returns `DbResult`, but allocation itself has no explicit status |
| Catalog v3 save | `database/database_catalog.cpp:1809,1825` | Root/continuation payload vectors copied from `pagePayloads`; allocation is exception-based |
| Catalog v4 save | `database/database_catalog.cpp:2158,2170` | Root/continuation payload vectors copied from `pagePayloads`; allocation is exception-based |

The R2 report also identified that `SqlEngine::executeInsert` does not take
the statement savepoint used by UPDATE/DELETE. Before changing insert-time
heap construction to return page-allocation errors, its explicit-transaction
rollback boundary must be verified or added; a failure after earlier rows in
the same INSERT have been staged must not leave a partial statement.

## 7. Migration decision and missing pre-conversion requirements

The smallest coherent reader/publication group is:

```text
DatabaseFile::readPage / readPageImage
  → DatabasePage::parse
  → BufferManager::materialize / pinPage / installPage
  → Database::readPage
  → Transaction::readPage and post-WAL commit installation
```

The related open-validation parse paths and WAL recovery validation must also
be updated. This group cannot yet be converted safely because:

1. No stable page-payload allocator reaches `DatabaseFile`, `Database`,
   `PageAccess`, or page-producing heap/index/catalog code. The cache and
   transaction allocators have different owners and lifetimes.
2. A returned page can outlive its Database, but no page currently stores an
   allocator tuple that would release its payload through the same allocator.
3. Page construction still directly allocates in the file bootstrap, heap
   tail, both index writers, and three catalog save implementations. The R2
   heap repack still hands off through a hosted vector.
4. The three production copies and five P7/P8 test copies have no explicit
   clone API. Removing implicit copies before migrating them would break the
   hosted build; leaving them would preserve hidden allocation without an
   error channel.
5. The required allocation-failure tests do not exist for page payloads,
   cache-view clones, `Database::readPage` output, transaction page reads, or
   post-durable cache installation.
6. `DatabasePage`'s status API depends on `DbResult`, which currently owns a
   `std::string`. Removing `<vector>` alone would not make
   `database_page.cpp` compile under `-nostdinc++`.
7. The current codec does not validate page-type values, and `parse` does not
   accept an encoded-source length. Page-type policy and a length-aware input
   contract must be settled before malformed or truncated data can be
   rejected at the materialization boundary.

Therefore the Gate I pre-conversion checkpoint is **not satisfied**. No
payload representation, copy operation, production caller, or test was
changed. There is no explicit R3 `materializePage`, `clonePage`, or owner
transfer API yet. Existing compiler-generated vector moves are used in parse,
cache materialization, and reset paths, but they do not establish Gx allocator
provenance.

## 8. Qualification status

| Requirement | Checkpoint result |
| --- | --- |
| Starting repository and phase gates | Pass; exact `main` / R2 HEAD and both required markers |
| R1/R2 acceptance evidence | Verified from preserved reports; accepted baselines retained |
| Actual R2 implementation inspected | Pass; bounded `GxArray` heap preparation plus checked hosted transfer |
| Current freestanding diagnostics | Reproduced; 15/33 pass, 18 blocked |
| Full page call/copy audit | Complete for the audited production paths and required P7/P8 copies; inventory above |
| Allocator lifetime/provenance | Current domains distinguished; page-payload owner absent |
| Explicit checked materialization / clone / transfer | Not implemented |
| Cache pin and reference qualification under page-owner failure | Not run; no converted owner or fault boundary |
| Transaction and post-commit failure regression under new owner | Not run; source path unchanged |
| WAL redo behavior | Source audit confirms raw validated page-image replay and recovery-required handling; no behavior changed |
| Golden byte equivalence under a new owner | Not run; no new owner. Existing codec and on-disk format are unchanged |
| R3 focused tests | 0 added / not run |
| Full hosted CTest | Not rerun for this docs-only checkpoint; accepted R2 result remains 19/19, 145,173 checks, 0 failures |
| SQL9 workload under converted page ownership | Not run; no converted production path |
| Persistent format | No source or format change |
| Server and Git topology | Preserved; no prohibited Git operation, branch change, or push |
| Parent P11 markers | Unchanged; P11 remains open |

## 9. Smallest safe next continuation

Continue R3 only after defining and implementing one checked page-payload
owner based on the existing portable runtime:

1. Add a dedicated page-payload allocator option to create/open and propagate
   its copied callback tuple through `DatabaseFile`, `Database`, and
   `PageAccess`. Document that its borrowed context outlives every page made
   with it, including pages that outlive the Database.
2. Add a move-only RAII payload owner around the accepted portable allocator
   primitives. It must retain allocator provenance, bound capacity by the
   configured page size, preserve exact logical length, provide strong
   failure behavior for replacement/clone, and transfer ownership without
   allocation. Do not byte-copy an owning `GxArray`.
3. Convert all listed page producers first, propagating allocation status
   before page writes or metadata publication. Remove R2's hosted-vector
   handoff only when the page owner can receive the prepared bytes safely.
   Verify INSERT statement rollback before converting the insert-time path.
4. Convert parse/file/cache/database/transaction/commit as the reader group.
   Clone to temporary owners before cache or caller publication, preserve
   existing cache image and pin on failure, and keep post-WAL failures in the
   existing `RecoveryRequired` path.
5. Add allocator-failure/retry, source-independence, cache pin, transaction
   visibility, commit-install failure, reopen/recovery, and exact-byte tests.
   Then migrate the five P7/P8 copies, delete implicit `DatabasePage` copy
   operations, run the full hosted suite, and compile converted portable
   production components with unchanged Server flags.

Do not advance the native marker or begin P12 from this checkpoint. The exact
remaining parent P11 blockers are the missing allocator-aware page owner,
failure-aware materialization/deep clone/transfer, complete caller migration,
pin and transaction failure qualification, page-byte equivalence under the
new owner, and full post-conversion regression/freestanding evidence.
