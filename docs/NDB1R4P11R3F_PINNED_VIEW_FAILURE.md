# NDB1R4P11R3F: Pinned Hosted-View Allocation Failure

## Outcome

**Outcome B — `PINNED_VIEW_FAILURE_PARTIAL`.** The test deterministically
fails the `std::vector<uint8_t>` allocation used to create the temporary
hosted page view. The error is reported before publication, leaves the
resident image and pinned slot intact, and permits retry. The SQL9 recovery
case succeeds when its seeded database has been checkpointed. However, the
same post-COMMIT fault with prior page-count-changing transactions still in
the WAL leaves evidence intact but reopen returns `WalCorrupt`; this recovery
case prevents Outcome A.

The allocation-failure and cache-reference boundary is reproduced and
qualified. The accumulated-WAL recovery case remains open. `DatabasePage::payload`
remains `std::vector<uint8_t>` and is not qualified as portable. Parent
NDB1R4P11 remains open; neither phase marker advances.

## 1. Starting checkout and preserved gates

- Writable SQL repository: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `a231629129329ebcb7f86cb1f6efe4a319535893`.
- The worktree was clean before the R3F test changes. The R3E implementation
  commit `ea1b601acf19e4707a31906cd4449557565427de` is the parent of the
  starting report-closeout commit.
- `.phase` remained:

  ```text
  project=guideXOSSql
  last_completed=SQL9
  next_expected=SQL10
  ```

- `.native-sql.phase` remained:

  ```text
  project=guideXOSSqlNativePortability
  last_completed=NDB1R4P10
  next_expected=NDB1R4P11
  ```

- No concurrent agent was active. R3F did not switch branches, alter Git
  topology, contact a remote, or push.
- The read-only Server repository remained on `NATIVE_DB_STORAGE` at
  `f29850c93bbac0a0763a31e806d2816310156575`. Its worktree was clean; no
  Server file, build setting, Git state, or phase marker was changed.

## 2. R3E and prior evidence verified

Read the accepted R3E report plus the R3C and R3D reports. R3E reports 64
focused cache-publication checks, 21/21 CTest targets with 145,522 checks and
zero failures, and 16/34 freestanding production units passing.

Source inspection confirmed the accepted R3E behavior remains in place:

- `Transaction::commit` passes validated transaction-owned encoded images to
  `Database::installCommittedPageImage` after WAL COMMIT sync.
- The encoded image is copied into cache-allocator-owned storage and validated
  before publication. The old image is released only after successful
  replacement.
- A pinned replacement prepares its hosted view before publication. The slot
  object and pin count remain stable; the replacement operation itself does
  not allocate.
- A post-COMMIT install failure marks recovery required, finishes the
  transaction as committed, returns `CommitFailed`, blocks committed reads
  and checkpointing, and retains WAL evidence for reopen.
- R3C caller-owned `GxOwnedPage` reads and R3D checked cache reads remain
  unchanged. The focused test also keeps an independently allocated
  `GxOwnedPage` live across the injected hosted-view failure.

The committed R3E inventory and its persistent-format checks remain intact.
R3F changed no production source, page encoding, database bytes, WAL framing,
or transaction ordering.

## 3. Exact allocation and exception boundary

The tested vector allocation is in `DatabasePage::parse` at
`database/database_page.cpp:78-80`:

```cpp
page.payload.assign(bytes + kPageHeaderSize,
                    bytes + kPageHeaderSize + info.payloadSize);
```

For the R3E commit-publication path, the production route is:

1. `Transaction::commit` writes WAL BEGIN and PAGE_IMAGE records, appends
   COMMIT, and syncs the WAL (`database/database_transaction.cpp:224-273`).
2. It sets the committed page count and calls
   `Database::installCommittedPageImage` for the transaction's encoded page
   images (`database/database_transaction.cpp:275-300`).
3. `Database::installCommittedPageImage` delegates to the encoded-image
   `BufferManager::installPage` overload
   (`database/database_relational.cpp:159-166`).
4. The installer validates and copies the encoded candidate, looks up the
   resident key, and only when that entry is pinned parses the candidate into
   a local `DatabasePage` (`database/database_buffer.cpp:686-749`).
5. The `std::bad_alloc` catch at
   `database/database_buffer.cpp:733-741` frees the candidate through the
   cache allocator and returns `DbStatus::NoSpace`. The call to
   `publishCandidate` at line 748 is reached only after parse succeeds.

The target allocation is the fresh temporary view's payload assignment. It
occurs after the existing cache pin has been acquired, but before cache image
replacement or slot-view publication. On failure, the old slot stays pinned
and the cache-owned candidate is released. The result is not successful page
materialization, and no candidate pointer escapes.

The same parse allocation also occurs outside commit processing:

- `BufferManager::materialize` catches it and returns `NoSpace`
  (`database/database_buffer.cpp:269-285`). On a resident cache hit,
  `pinPage` materializes before incrementing the pin count
  (`database/database_buffer.cpp:331-350`).
- A cache-miss `pinPage` parses a local page and catches `bad_alloc` before
  publishing the candidate or incrementing the pin count
  (`database/database_buffer.cpp:377-385`).

The focused R3F read test exercises the resident materialization case. The
commit installer tested here cannot run before durable COMMIT: its production
caller is the post-sync transaction publication loop. No exception handling
or hosted dependency was added to freestanding production code.

## 4. Deterministic hosted fault injection

`tests/database_cache_publication_test.cpp` contains an executable-local
replacement for hosted `operator new`/`operator new[]`. A thread-local,
scoped one-shot filter throws `std::bad_alloc` only when an allocation request
matches the requested byte count. All ordinary allocations in this isolated
test executable use `malloc`/`free`; the filter is armed only around the
production call and is disarmed by the scope even if the target is missed.
No production library or other test target sees this hook, and no persistent
fault state remains after each scope.

The direct pinned-page fixture uses a committed 127-byte payload. The actual
hosted vector allocation requests that exact size; the hook's trigger count is
asserted to be one on each of two repeated attempts. Both calls return
`DbStatus::NoSpace` through the production catch. The read materialization
fixture independently targets the current page payload length, asserts one
trigger, and receives `NoSpace` with a null output slot and zero acquired
pins. A successful retry follows each failure.

In the SQL integration fixture, the filter is armed for the pinned heap page's
current payload length only around `COMMIT`. The resulting SQL error message
is exactly `pinned page-view allocation failed`, from the encoded installer’s
catch. This, together with the one-shot trigger assertion and
`recoveryRequired`, proves that the real production boundary was reached
after durable COMMIT rather than an earlier allocation or a fabricated status.

## 5. Pin, image, retry, eviction, and close evidence

The direct fixture creates and commits a data page with a nonempty payload,
reads its encoded bytes into a caller-owned `GxOwnedPage`, and pins the page
through the supported `BufferManager::pinPage` API. It retains the stable
`BufferSlot*`, its page identity, payload, generation, slot pin count, and
manager pin count across two failed same-key replacements.

After each failure it verifies:

- The trigger fired once and the API returned `NoSpace`.
- The cache allocator's outstanding allocation count returned to its
  pre-attempt value; the candidate image was released.
- The same slot address, page ID, prior generation, and prior payload remain
  valid, with exactly the original pin accounting.
- `Database::readOwnedPage` still returns the complete original encoded image.
- The separately owned source `GxOwnedPage` remains valid and byte-identical.

With injection disabled, replacement succeeds in the same slot without
changing its pin count. The replacement bytes are readable; the pin is
released exactly once, and a second release is rejected as
`InvalidArgument`. The resident encoded page is then made unmaterialized and
the read-path allocation is failed. It acquires no pin, leaves the encoded
replacement readable, and succeeds on retry.

The fixture uses a one-slot cache. It loads another page to evict the repaired
entry, reads the repaired page back from its encoded file image, closes the
database, and independently reopens it. The replacement bytes survive
eviction and close/reopen. Cache, source, output, and reopened allocator
accounts finish with zero outstanding allocations and zero invalid frees.

## 6. Durable COMMIT and SQL9 recovery workload

The SQL fixture creates parent and child tables, a foreign key, primary-key
indexes, and a secondary index on the child value. It inserts committed rows,
performs an indexed SELECT, and checkpoints this seeded schema as the durable
base state before the faulted transaction. The tested update changes existing
heap and index pages and does not allocate a new page.

It then pins the row's heap page using the locator returned by the primary
index and runs an explicit SQL transaction that:

- Updates `Value` from `oldtag` to `newtag`.
- Reads the updated value through the transaction overlay.
- Attempts a duplicate-key INSERT, verifies statement failure leaves the
  transaction active, and verifies the earlier update remains visible.
- Fails the hosted page-view allocation during SQL `COMMIT`.

The SQL layer reports `TransactionError` with the pinned-view allocation
message. The transaction is no longer active, while the database reports
`recoveryRequired`. The borrowed slot remains valid with its old committed
bytes and balanced pin count. A committed `Database::readPage` returns
`RecoveryRequired`; flush/checkpoint is also blocked. The WAL sidecar remains
nonempty after the failure and after close, and the pin is released exactly
once.

A fresh writable open replays the durable transaction. The test verifies the
`newtag` row through the secondary index, runs `validateIntegrity` for the
heap, catalog, and foreign-key relation, and structurally validates every
index (primary, secondary, and FK support). A subsequent valid SQL update to
`newlog` succeeds; after another close/reopen the row is found through the
secondary index and integrity validation passes again.

### Uncheckpointed WAL history result

An initial run used the same real SQL setup and injected view failure without
checkpointing the earlier schema and row commits. The allocation hook fired,
SQL COMMIT returned the pinned-view failure, and the WAL file remained
nonempty through close. A fresh writable open then returned:

```text
WalCorrupt: recovery header images are incompatible or regress page count
```

`Database::validateRecoveryCoverage` at
`database/database_relational.cpp:362-373` initializes `finalPageCount` from
the current on-disk header and rejects a retained header image whose count is
less than that value. With multiple committed transactions retained in the
WAL, an older valid header image can precede the latest on-disk page count.
This failure is after the durable page-view error: WAL evidence was retained,
but recovery could not apply it. It is a relevant recovery contract gap, not a
failure of the view allocation catch.

The passing SQL recovery test now checkpoints the seeded schema first, then
faults a transaction that changes existing pages without changing page count.
It proves recovery for that bounded history, but does not erase the observed
uncheckpointed-history failure. R3F makes no WAL recovery change because the
phase explicitly prohibits changing recovery implementation. The smallest
safe continuation is a separate, narrowly scoped recovery validation
qualification for retained transactions with increasing page counts. Until
that gate is resolved, the overall outcome remains B.

The fault is therefore classified as post-durable-COMMIT for the production
installer. The read materialization fault is pre-pin and outside commit. No
pre-COMMIT transaction failure was manufactured for the installer, because
the audited production call order cannot reach it before WAL sync.

## 7. Qualification results

The focused `database_cache_publication_test` now reports **180 checks passed,
zero failed**. R3E's original 64 checks are retained; R3F adds 116 checks for
the actual hosted allocation failure, repeated failure/retry, pin and image
preservation, read materialization, eviction, SQL COMMIT status, WAL recovery,
and relational integrity.

The final full hosted regression completed with **21/21 CTest targets passed,
145,638 checks passed, and zero failures** in **2,561.21 seconds**. This
includes SQL1–SQL9, prepared
statement reuse, transaction/savepoint behavior, WAL crash recovery,
P1–P8, allocator and heap preparation, R3A validation, R3C owned-page reads,
R3D checked reads, and R3E cache publication. No target or assertion was
removed or suppressed.

The full build completed, the focused target compiled successfully, and
`git diff --check` passed. The R3F changes are limited to the hosted test
executable; no production correction was needed.

### Freestanding AMD64 census

Ran `tools/qualify_sql9_freestanding.ps1` with the Server AMD64 settings,
including `-ffreestanding`, `-nostdinc++`, `-fno-exceptions`, and `-fno-rtti`.
Result: **16/34 production translation units pass; 18 remain blocked**.
The same 16 units that passed at R3E still pass; none of the original blocked
units newly passes, and no source unit was added. The first errors are the
same hosted-header blockers:

| Translation unit | First compiler diagnostic |
|---|---|
| `database/database_io.cpp` | `fatal error: cstdio: No such file or directory` |
| `database/database_native_io.cpp` | `fatal error: memory: No such file or directory` |
| `database/database_header.cpp` | `fatal error: string: No such file or directory` |
| `database/database_page.cpp` | `fatal error: vector: No such file or directory` |
| `database/database_file.cpp` | `fatal error: memory: No such file or directory` |
| `database/database_engine.cpp` | `fatal error: memory: No such file or directory` |
| `database/database_buffer.cpp` | `fatal error: memory: No such file or directory` through `database_file.h` |
| `database/database_catalog.cpp` | `fatal error: string: No such file or directory` |
| `database/database_index.cpp` | `fatal error: set: No such file or directory` |
| `database/database_heap.cpp` | `fatal error: memory: No such file or directory` |
| `database/database_wal.cpp` | `fatal error: memory: No such file or directory` |
| `database/database_transaction.cpp` | `fatal error: memory: No such file or directory` |
| `database/database_relational.cpp` | `fatal error: memory: No such file or directory` |
| `database/database_sql_token.cpp` | `fatal error: string: No such file or directory` |
| `database/database_sql_tokenizer.cpp` | `fatal error: string: No such file or directory` |
| `database/database_sql_parser.cpp` | `fatal error: vector: No such file or directory` |
| `database/database_sql_expr.cpp` | `fatal error: vector: No such file or directory` |
| `database/database_sql.cpp` | `fatal error: map: No such file or directory` |

The 16 passing units are `database/database_format.cpp`,
`database/database_checksum.cpp`, `database/portable/gx_allocator.cpp`,
`gx_allocator_hosted.cpp`, `gx_array.cpp`, `gx_owned_page.cpp`,
`gx_heap_page_builder.cpp`, `gx_recovered_pages.cpp`, `gx_page_cache.cpp`,
`gx_page_codec.cpp`, `gx_wal_record.cpp`, `gx_wal_scan.cpp`,
`gx_wal_page_image.cpp`, `gx_transaction_pages.cpp`,
`gx_transaction_page_index.cpp`, and `gx_transaction_page_overlay.cpp`.
The 34-unit source census is unchanged; no production source unit was added.

The unique undefined symbols in passing objects are:

```text
__cxa_guard_acquire  __cxa_guard_release  malloc  free
gxAllocatorAllocate  gxAllocatorFree
gxArrayClear  gxArrayDestroy  gxArrayInit  gxArrayReserve  gxArrayResize  gxArraySwap
gxDatabasePageEncode  gxDatabasePageInspect
crc32Final  crc32Init  crc32Update
gxTransactionPageIndexClear  gxTransactionPageIndexClone
gxTransactionPageIndexDestroy  gxTransactionPageIndexInit
gxTransactionPageIndexInsert  gxTransactionPageIndexLookup
gxTransactionPagesAppendPage  gxTransactionPagesAt  gxTransactionPagesClear
gxTransactionPagesClone  gxTransactionPagesCount  gxTransactionPagesDestroy
gxTransactionPagesInit  gxTransactionPagesPopLast  gxTransactionPagesReplacePage
```

`__cxa_guard_*` is the pre-existing checksum static-initialization ABI; the
hosted allocator object imports `malloc`/`free`; portable objects import the
existing allocator, codec, array, checksum, and transaction-page APIs. No new
exception (`__cxa_throw`/personality), RTTI, hosted STL, or global-allocation
symbol was introduced into passing production objects. This remains object
compilation and symbol inspection, not a native link or QEMU execution.

## 8. Remaining hosted page-copy inventory

The following production boundaries still use hosted page storage or copying:

| Source location | Remaining operation |
|---|---|
| `database/database_file.cpp:136-148`, `394-420` | `DatabaseFile::readPageBytes` fills a vector; `DatabaseFile::readPage` parses into a local page and copies it to caller output. |
| `database/database_buffer.cpp:269-285`, `331-385` | `BufferManager::materialize` and cache-miss `pinPage` parse cache images into hosted payload vectors. R3F fault-tests the existing catches; it does not convert the representation. |
| `database/database_relational.cpp:114-135` | `Database::readPage` copies a pinned slot page into caller output and maps `bad_alloc` to `NoSpace`. |
| `database/database_transaction.cpp:81-118` | `Transaction::readPage` parses transaction-owned encoded images into hosted page output. |
| `database/database_buffer.cpp:620-684` | Compatibility `BufferManager::installPage(const DatabasePage&)` encodes a hosted page and copy-constructs the pinned replacement view at line 674. This separate copy boundary was not the R3E encoded-image parse failure injected here. |
| `database/database_heap.cpp:1102-1166`, `1226+` | Heap append and mutation preparation still create or assign hosted page payloads; bounded heap scratch does not yet replace all payload copies. |
| `database/database_index.cpp:298-315`, `438-454` | `IndexTree::writeLeaf` and `writeInternal` assign full hosted payload vectors. |
| `database/database_catalog.cpp:1546+` | Catalog save versions copy generated payload vectors into root and continuation `DatabasePage` values. |
| `database/database_sql.cpp:2867+` | `SqlEngine::executeInsert` still needs its separately bounded statement/savepoint ownership qualification for multi-row failures. |

`DatabasePage::parse`, `serialize`, `payload`, and remaining implicit page
copies remain hosted. The R3E report's full reader/writer inventory remains
applicable; no other conversion started in R3F.

## 9. Remaining recovery gate and recommended R3G boundary

Before accepting R3F, qualify and resolve the uncheckpointed header-image
sequence described above in a separately authorized, focused recovery change.
The test should retain several successful page-count-changing commits in WAL,
then inject the pinned hosted-view failure after durable COMMIT and verify
reopen, page coverage, exact rows, and indexes. Keep WAL framing and COMMIT
ordering unchanged.

After that gate is closed, convert one reader boundary next:
`DatabaseFile::readPageBytes` and `DatabaseFile::readPage`.

Add a checked `DatabaseFile::readOwnedPage(pageId, GxOwnedPage& out)` path that
prepares the exact encoded page bytes with the caller owner's allocator,
reads into prepared storage, validates page extent/identity/type/CRC with the
portable inspector, and publishes to `out` only after the read and validation
both succeed. The allocator tuple is copied into the temporary owner, while
its context remains caller-owned and must outlive the returned `GxOwnedPage`;
the file/database may close first. Keep the existing `DatabasePage` reader as
a compatibility edge until callers have checked owner operations.

R3G tests should cover allocator failure with prior output preserved,
short/failed file reads, corrupt identity/CRC, successful byte-for-byte
materialization, allocator accounting, and owner use after file close. This is
a recommendation only; no R3G work is included here.

## 10. Closeout

- Outcome: `PINNED_VIEW_FAILURE_PARTIAL`. The exact hosted allocation failure,
  cache pin behavior, retry, and checkpointed SQL recovery were verified; the
  uncheckpointed retained-header recovery case remains unresolved.
- Starting SQL HEAD: `a231629129329ebcb7f86cb1f6efe4a319535893`, branch `main`.
- Ending SQL HEAD remains `a231629129329ebcb7f86cb1f6efe4a319535893` on `main`;
  R3F test and report evidence remains uncommitted while Outcome B is open.
- No push or Git topology change occurred.
- Both phase markers remain unchanged at SQL9 → SQL10 and NDB1R4P10 → NDB1R4P11.
- Server remains read-only and unchanged at
  `f29850c93bbac0a0763a31e806d2816310156575`.
- Persistent database and WAL formats are unchanged. `DatabasePage::payload`
  remains hosted and is not claimed portable.
