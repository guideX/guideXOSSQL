# NDB1R4P11R3G: Checked File-Backed Page Reader

## Outcome

**Outcome A — `CHECKED_PAGE_READER_VERIFIED`.** The existing
`DatabaseFile::readPage` boundary now reads through an exact-size,
caller-allocator `GxOwnedPage`. File bytes are read directly into that final
allocation, validated before owner publication, and adopted without a second
page-image copy. Open-time page validation uses the same path. The legacy
`DatabasePage` API remains available and publishes its compatibility value by
a nonallocating move after successful parsing.

Parent NDB1R4P11 remains open. `DatabasePage::payload`, cache design,
transaction management, and WAL recovery were not replaced or rewritten.

## 1. Starting SQL state and phase gates

- Writable checkout: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `6325ed89bab4cd013a209f3c5b1ebf81aeb17a63`.
- Starting worktree: clean. The local branch was one commit ahead of
  `origin/main`; the expected R3F-R1 commit was the local HEAD.
- No other agent was active.
- No R3G implementation or report existed before this work.
- Ordinary marker remains `project=guideXOSSql`,
  `last_completed=SQL9`, `next_expected=SQL10`.
- Native marker remains `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- The read-only Server checkout remained clean on `NATIVE_DB_STORAGE` at
  `f29850c93bbac0a0763a31e806d2816310156575`.

The accepted R3F-R1 recovery commit at the starting HEAD was inspected. Its
recovery validator and tests are unchanged by R3G.

## 2. R3F-R1 acceptance verification

The preserved report `docs/NDB1R4P11R3FR1_UNCHECKPOINTED_WAL_RECOVERY.md`
records its accepted baseline:

- Full CTest: 21/21 targets, 150,342 checks, zero failures.
- Focused cache/recovery fixture: 4,884 checks.
- Freestanding census: 34 production units, 16 passing, 18 blocked.
- Valid retained committed header counts include `4 → 8 → 23 → 48`.
- A committed `4 → 3` header-count regression remains `WalCorrupt`.

The complete R3G CTest rerun again passed the cache/recovery fixture with
4,884 checks. It exercised the uncheckpointed and checkpointed controls,
page-count growth, multi-COMMIT replay, durable-COMMIT hosted-view failure,
corrupt history rejection, recovery-required read/flush blocking, and repeated
safe reopen. `Database::readPage`, `Database::readOwnedPage`, transaction
commit, WAL scanning, and recovery implementation were not changed.

The durable-COMMIT behavior remains as accepted: the WAL COMMIT is durable,
the transaction is inactive, cache publication failure requires recovery,
reads and flushes stay blocked, and reopen restores committed rows and index
state. R3G does not reinterpret the SQL transaction error as rollback or retry
COMMIT.

## 3. Current reader/copy inventory before the conversion

The current source audit found these original production boundaries:

| Function and source location | Source and representation | Previous allocating operation | Destination, lifetime, and validation | Pin / visibility / error behavior |
|---|---|---|---|---|
| `DatabaseFile::readPage`, `database/database_file.cpp` | File bytes in `readPageBytes` vector, then parsed hosted `DatabasePage` | `readPageBytes` resized a hosted vector; `DatabasePage::parse` allocated payload; `out = page` deep-copied payload without a local allocation catch | Caller-owned `DatabasePage`; exact page-size bytes inspected for id, supported type, payload bound, and CRC before parse | No cache pin; committed file view; parse errors returned, final copy could throw |
| `DatabaseFile::validateOpenedPages`, `database/database_file.cpp` | Root/all-page file bytes in temporary vectors followed by local hosted pages | Per-page vector allocation and payload parse allocation | Temporary validation values die before open returns | No cache pin; I/O, extent, and codec checks gate open |
| `Database::readPage`, `database/database_relational.cpp` | Pinned committed cache slot's hosted page | `out = slot->page` deep copy; `bad_alloc` maps to `NoSpace` | Caller-owned hosted page may outlive database | Slot pinned during copy and unpinned on both branches; committed-only; recovery-required state blocks the call |
| `Database::readOwnedPage` → `BufferManager::readOwnedPage` | Encoded committed cache image or exact file image | Checked `GxOwnedPage::materialize` into caller allocator; no `DatabasePage` copy on this path | Caller owner retains callback/context provenance and may outlive database | No pin-count change; committed-only; recovery-required state blocks the call |
| `BufferManager::installPage(pageId, const DatabasePage&)`, `database/database_buffer.cpp` | Hosted page input and cache-owned encoded image | When the slot is pinned, `preparedView = page` deep-copies hosted payload; failure maps to `NoSpace` before publication | Cache image uses cache allocator; hosted slot view remains compatibility storage | Existing pins and slot identity are preserved on failure; writer compatibility overload |
| `Transaction::readPage`, `database/database_transaction.cpp` | Transaction overlay's encoded image or base database page | Overlay hit parses into hosted output payload | Caller-owned hosted output | Overlay wins for read-your-writes; base read delegates to committed database; allocation failure maps to `NoSpace` |

R3G selected the first two rows as one bounded file-reader group. It did not
convert the cache `DatabasePage` representation or transaction overlay.

## 4. Selected production operation and new contract

Selected path:

```text
DatabaseFile::open
  → validateOpenedPages
  → DatabaseFile::readOwnedPage
```

The same `DatabaseFile::readOwnedPage` is also the checked source used by the
legacy `DatabaseFile::readPage` adapter.

The new operation accepts a caller-configured `GxOwnedPage` and:

1. Rejects a closed file, unconfigured or wrong-size owner, header page, and
   out-of-range page before allocation.
2. Computes the exact page offset with checked arithmetic and asks the file
   provider for its actual byte extent.
3. Returns `Truncated` when the physical file does not contain the full
   requested page. It does not synthesize zero-filled bytes.
4. Allocates exactly one page through the destination owner's allocator
   callbacks and context.
5. Reads the full configured page size directly into that allocation.
6. Calls `GxOwnedPage::adoptEncodedBytes`, which checks allocator identity,
   exact extent, expected page id, supported page type, payload/header bounds,
   and CRC with the accepted R3A inspector.
7. Replaces the previous owner only after complete read and validation. The
   final adoption does not allocate or copy page bytes.

`adoptEncodedBytes` requires the source allocation's allocator callback pair
and context to match the destination domain. On failure, the caller retains the
temporary allocation; `DatabaseFile` frees it through that same tuple. On
success, the owner retains the tuple and frees both the adopted image and any
previous image through the matching allocator. Equal callback addresses with
different contexts are different domains.

Status mapping is explicit: allocation exhaustion is `NoSpace`; a physically
short file extent is `Truncated`; a failed size query or failed/short provider
read is `IoError`; malformed page bytes, wrong identity, unknown type, or bad
CRC are `CorruptPage`; owner configuration and page-zero errors are
`InvalidArgument`; and page-range/offset errors remain bounds errors. The
`IDatabaseFile::readAt` contract reports short and failed reads with one boolean,
so when the reported source extent is complete but the provider returns false,
the result is `IoError`. A file whose reported extent is short is separately
reported as `Truncated`.

## 5. Existing caller adoption and hosted copy removed

`validateOpenedPages` now reuses one temporary `GxOwnedPage` for the bootstrap
page and, when requested, every page. Its allocator is the existing hosted
adapter with a null context and is destroyed before validation returns. The
bootstrap validator reads payload bytes from the already validated encoded
image and owner metadata; it no longer materializes a `DatabasePage` payload.
The private `readPageBytes` vector helper was removed.

`DatabaseFile::readPage` remains for compatibility. It reads through
`readOwnedPage`, parses into a temporary `DatabasePage`, maps hosted payload
`std::bad_alloc` to `NoSpace`, and then move-assigns the fully parsed value.
`DatabasePage` remains hosted storage, but the former unchecked `out = page`
deep-copy allocation is gone. The method does not claim that
`DatabasePage::payload` itself is portable.

The file-backed selected path has no cache source and acquires no cache pin.
The returned owner is independent of the file and may remain live after
`DatabaseFile::close` and destruction. Database cache paths continue to use
their prior cache-image ownership and pin protocols.

## 6. Allocator, lifetime, cache, and transaction evidence

The focused file-reader test uses allocator contexts A and B:

- A performs the page read and retains the original allocation provenance.
- A-to-B clone allocates independent bytes through B; source remains in A.
- B allocation failure preserves both the prior B destination and A source.
- Adoption from B into an A-owned page is rejected before mutation.
- Repeated A allocation failures preserve a valid prior destination; retry
  succeeds.
- Initial allocation failure leaves an empty destination invalid and leaks no
  allocation.
- An allocated page remains valid after the file closes and its
  `DatabaseFile` object is destroyed; destruction uses the still-live caller
  allocator context.
- Final A/B/initial-failure/wrong-size arena accounting is zero outstanding
  allocations and zero invalid frees.

No cache-owned reference is returned, so cache pin acquisition, release, slot
identity, and eviction do not apply to this file-only source. Existing cache
read and publication tests still verify their pin accounting and failure
behavior. The SQL `Database::readPage` and `Database::readOwnedPage` paths keep
their existing `recoveryRequired` checks. Transaction-aware reads still take
the transaction overlay first; R3G does not replace them with a committed-only
file read, and it does not change read-your-writes, statement savepoints,
rollback, commit ordering, or cache consistency.

## 7. Focused failures and page-byte equivalence

`tests/database_owned_page_test.cpp` reports **230 checks passed, zero
failed**. The R3G addition is **68 checks** over its previously accepted
162-check target. The new cases cover:

- Exact 512-byte page image materialization compared with an independent
  encoded fixture and `DatabasePage::serialize`.
- Direct allocation through the caller owner, exact byte count and capacity,
  and allocator provenance.
- Initial and replacement allocation failures, repeated failure, successful
  retry, and preservation of the previous valid destination.
- Actual short physical extent, size-query I/O failure, failed page read, and
  provider partial read.
- Wrong page id with otherwise valid page encoding, unsupported page type,
  corrupt CRC, page zero, out-of-range page, wrong owner page size, and
  unconfigured owner.
- Cross-context adoption rejection, A-to-B clone, failed B clone, close and
  destroy lifetime, and final allocator accounting.
- The legacy `DatabasePage` reader's successful byte equivalence and
  preservation of its existing destination when file I/O fails.

No `.gxdb` or `.gxwal` version, page header, page type, page size, CRC,
heap/index/catalog encoding, WAL framing, or SQL status changed. File bytes
read through the new path are compared exactly with the established fixture.

## 8. SQL9 workload and recovery state

The existing SQL9 owner workload remains enabled in the focused target. It
creates and opens a database, creates a table and indexes, inserts a row,
performs indexed SELECT, updates in an explicit transaction, checks
read-your-writes, fails a duplicate-key statement while preserving the active
transaction, commits, deletes and rolls back, closes/reopens, and verifies the
indexed committed row. `Database::open` reaches the new open-time validation
path when validation is not deferred; WAL recovery still owns its existing
defer/replay/validate ordering.

The full suite includes prepared SQL9 SELECT reuse, heap and primary/secondary
index reads, INSERT/UPDATE/DELETE, transaction/savepoint failures, foreign-key
integrity, commit, WAL recovery, and close/reopen. The post-durable-COMMIT
allocation-failure case still blocks stale reads until recovery and restores
the committed rows, indexes, and foreign-key relationships on reopen.

## 9. Full hosted regression

The full build relinked every CTest executable against the final library.
`ctest --test-dir build --output-on-failure` completed with **21/21 targets
passed, 150,410 checks passed, zero failures**, in **870.68 seconds**. The
accepted R3F-R1 total was 150,342; the increase of 68 is the new R3G reader
coverage. No existing target or assertion was removed, weakened, or skipped.

Focused final outputs:

- `database_owned_page_test`: 230 checks, zero failures.
- `database_storage_tests`: 223 checks, zero failures.
- `database_cache_publication_test`: 4,884 checks, zero failures.

The full regression includes SQL1–SQL9, prepared-plan reuse, P1–P8,
R1–R3F-R1, transaction/savepoint, index, foreign-key, and cache/recovery
coverage.

## 10. Freestanding AMD64 qualification

Ran `tools/qualify_sql9_freestanding.ps1` under Windows PowerShell 5.1 with the
Server AMD64 flags, including `-ffreestanding`, `-nostdinc++`,
`-fno-exceptions`, and `-fno-rtti`. Result: **34 production translation
units, 16 pass and 18 blocked**.

- All 16 previously passing units remain passing.
- No previously blocked original unit became newly qualified.
- No translation unit was added.
- `database/database_file.cpp` remains blocked at the existing hosted
  `<memory>` include in `database/database_file.h`.
- There is no full SQL native link or execution claim.

The 18 first diagnostics are unchanged hosted-header failures:

| Unit | First diagnostic |
|---|---|
| `database/database_io.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_io.h:13:10: fatal error: cstdio: No such file or directory` |
| `database/database_native_io.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_native_io.h:5:10: fatal error: memory: No such file or directory` |
| `database/database_header.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_header.h:7:10: fatal error: string: No such file or directory` |
| `database/database_page.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_page.h:7:10: fatal error: vector: No such file or directory` |
| `database/database_file.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_file.h:17:10: fatal error: memory: No such file or directory` |
| `database/database_engine.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_engine.h:9:10: fatal error: memory: No such file or directory` |
| `database/database_buffer.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_file.h:17:10: fatal error: memory: No such file or directory` |
| `database/database_catalog.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_catalog.h:35:10: fatal error: string: No such file or directory` |
| `database/database_index.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_index.h:27:10: fatal error: set: No such file or directory` |
| `database/database_heap.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_heap.h:26:10: fatal error: memory: No such file or directory` |
| `database/database_wal.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_wal.h:30:10: fatal error: memory: No such file or directory` |
| `database/database_transaction.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_transaction.h:25:10: fatal error: memory: No such file or directory` |
| `database/database_relational.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_relational.h:21:10: fatal error: memory: No such file or directory` |
| `database/database_sql_token.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_sql_token.h:11:10: fatal error: string: No such file or directory` |
| `database/database_sql_tokenizer.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_sql_tokenizer.h:12:10: fatal error: string: No such file or directory` |
| `database/database_sql_parser.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_sql_parser.h:13:10: fatal error: vector: No such file or directory` |
| `database/database_sql_expr.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_sql_expr.h:18:10: fatal error: vector: No such file or directory` |
| `database/database_sql.cpp` | `D:\dev\guideXOS_SqlDatabase\database\database_sql.h:16:10: fatal error: map: No such file or directory` |

`nm -u -C` reports the same 32 unique undefined symbols in passing objects:

```text
__cxa_guard_acquire
__cxa_guard_release
free
malloc
gxAllocatorAllocate(GxAllocator const*, unsigned long long, unsigned long long, void**)
gxAllocatorFree(GxAllocator const*, void*)
gxArrayClear(GxArray*)
gxArrayDestroy(GxArray*)
gxArrayInit(GxArray*, GxAllocator const*, unsigned long long, unsigned long long)
gxArrayReserve(GxArray*, unsigned long long)
gxArrayResize(GxArray*, unsigned long long)
gxArraySwap(GxArray*, GxArray*)
gxDatabasePageEncode(unsigned char*, unsigned int, unsigned long long, unsigned short, unsigned short, unsigned int, unsigned char const*, unsigned long long)
gxDatabasePageInspect(unsigned char const*, unsigned long long, unsigned int, unsigned long long, GxDatabasePageInfo*)
gxos::db::crc32Final(unsigned int)
gxos::db::crc32Init()
gxos::db::crc32Update(unsigned int, unsigned char const*, unsigned long long)
gxTransactionPageIndexClear(GxTransactionPageIndex*)
gxTransactionPageIndexClone(GxTransactionPageIndex*, GxTransactionPageIndex const*)
gxTransactionPageIndexDestroy(GxTransactionPageIndex*)
gxTransactionPageIndexInit(GxTransactionPageIndex*, GxAllocator const*, unsigned long long)
gxTransactionPageIndexInsert(GxTransactionPageIndex*, unsigned long long, unsigned long long)
gxTransactionPageIndexLookup(GxTransactionPageIndex*, unsigned long long, bool*, unsigned long long*)
gxTransactionPagesAppendPage(GxTransactionPages*, unsigned long long, unsigned short, unsigned short, unsigned int, unsigned char const*, unsigned long long, unsigned long long*)
gxTransactionPagesAt(GxTransactionPages const*, unsigned long long)
gxTransactionPagesClear(GxTransactionPages*)
gxTransactionPagesClone(GxTransactionPages*, GxTransactionPages const*)
gxTransactionPagesCount(GxTransactionPages const*)
gxTransactionPagesDestroy(GxTransactionPages*)
gxTransactionPagesInit(GxTransactionPages*, GxAllocator const*, unsigned long long, unsigned long long)
gxTransactionPagesPopLast(GxTransactionPages*, unsigned long long)
gxTransactionPagesReplacePage(GxTransactionPages*, unsigned long long, unsigned long long, unsigned short, unsigned short, unsigned int, unsigned char const*, unsigned long long)
```

No `__cxa_throw`, exception-personality, RTTI, `operator new`, or
`operator delete` import appeared in passing objects. This is object
compilation and symbol inspection only.

## 11. Remaining reader, writer, and copy inventory

| Area | Current representation and hosted allocation | Explicit allocator / checked operation | Failure and publication behavior | Remaining dependency and next conversion |
|---|---|---|---|---|
| `DatabasePage::payload`, `database/database_page.h` | Hosted `std::vector<uint8_t>`; parse assigns payload and serialize resizes an encoded vector | R3A checks page bytes; no explicit allocator for payload | Callers need local `bad_alloc` conversion; parsed temporary publishes only after validation | Keep compatibility type; future payload-owner work must migrate every producer and copy site together |
| `DatabaseFile::readPageBytes` | Removed; former vector staged the complete page before parsing | Direct `DatabaseFile::readOwnedPage` into caller allocator; open validation uses a scoped hosted adapter | Exact read and validation precede owner replacement | `DatabasePage::readPage` wrapper still materializes hosted payload for compatibility |
| `DatabaseFile::readPage` | Compatibility `DatabasePage` result; checked owner page image followed by hosted payload parse | Uses the new file owner, catches `bad_alloc`, then nothrow move publication | Preserves previous output on I/O, corruption, or payload allocation failure | The result representation remains hosted; callers needing portable lifetime should use `readOwnedPage` |
| `DatabaseFile::readOwnedPage` | Exact page image read directly into a caller-owner allocation | Caller allocator callbacks/context; R3A inspection; checked adopt | On failure frees only the temporary and preserves existing destination | Converted in R3G; low-level file-only API has no cache pin or transaction overlay |
| `Database::readPage` | Pinned committed `DatabasePage` view deep-copied to caller vector | Copy catches `std::bad_alloc`; pin released on both branches | `NoSpace` leaves cache valid; `RecoveryRequired` blocks stale view | Next bounded group: use existing encoded `Database::readOwnedPage` as source, retain transaction overlay behavior in `Transaction::readPage` |
| `Database::readOwnedPage` | Encoded committed cache image copied to caller `GxOwnedPage` | Caller allocator, checked clone, prior owner preserved | Existing recovery-required check and committed-only visibility remain | Existing portable boundary; no SQL caller migration in R3G |
| `BufferManager::installPage(const DatabasePage&)` | Hosted page input plus cache allocator image; pinned view uses `preparedView = page` | Cache image encoding/validation and caught hosted copy | Candidate is freed and slot remains unchanged on allocation failure | Legacy compatibility writer; encoded-image installer remains the transaction commit path |
| Legacy file/cache writers | `DatabaseFile::writePage` serializes to hosted vector; compatibility cache installer accepts `DatabasePage` | Cache image uses explicit allocator; file writer remains hosted | Existing validation and transaction ordering unchanged | Convert after reader ownership contract is established |
| Heap and index writers | Heap page payload assignment/copies; index leaf/internal `payload.assign` | R2 heap scratch is bounded, but final page payloads remain hosted | Transaction/savepoint rollback remains current behavior | Convert heap page production and index leaf/internal construction as a paired bounded writer step |
| Catalog writers | Hosted payload vectors assigned to root/continuation pages for catalog versions | No direct portable page payload owner | Existing transaction staging handles current failures | Convert all catalog versions together after a checked writable page owner exists |
| Transaction reads and savepoints | Overlay stores encoded pages with transaction allocator; `Transaction::readPage` parses into hosted page; statement savepoints clone staged state | Overlay lookup/staging is checked and preserves read-your-writes | Current rollback/savepoint behavior remains covered | Keep overlay precedence; migrate parsed output separately from the committed page reader |
| `SqlEngine::executeInsert` | SQL row and page staging crosses hosted page payload ownership and multi-row statement boundary | Statement savepoint APIs exist; no R3G change | Existing duplicate/failure rollback tests pass | Retain as a separate statement-atomicity conversion after page writers |
| P7/P8 test-only page copies | Tests use `DatabasePage` value copies such as `firstWrite = committed` and `changed = original` | Hosted test vectors only | Test behavior remains unchanged | Test-only compatibility copies; do not count as migrated production paths |

The only explicit original production `DatabasePage` copy removed in this
phase is `DatabaseFile::readPage`'s `out = page`. Database::readPage and the
pinned compatibility writer still have their documented hosted copies.

## 12. Next bounded continuation

Continue with the committed cache reader `Database::readPage`: use the existing
validated encoded-image `Database::readOwnedPage` path for the committed cache
snapshot, then decode into the hosted compatibility result with a checked
allocation status and nonallocating final move. Keep
`Transaction::readPage`'s overlay-first branch intact so read-your-writes and
statement savepoints remain unchanged. The existing cache-owner tests already
cover stable pins and destination failure behavior needed to bound that step.

## 13. Closeout

- Outcome: `CHECKED_PAGE_READER_VERIFIED`.
- Starting SQL HEAD: `6325ed89bab4cd013a209f3c5b1ebf81aeb17a63` on `main`.
- Ending branch remains `main`; local implementation commit is recorded in
  the final task closeout.
- `.phase` and `.native-sql.phase` remain unchanged; parent P11 remains open.
- Server remains read-only and unchanged at
  `f29850c93bbac0a0763a31e806d2816310156575`.
- Full hosted CTest: 21/21 targets, 150,410 checks, zero failures.
- Freestanding census: 16/34 passing, 18 existing hosted-header blockers; no
  native SQL link or execution claim.
- No Server changes, branch changes, history rewrites, worktree creation, or
  pushes occurred.

## Acceptance answer

**Yes.** `DatabaseFile::readPage` and open-time file validation now use a
validated, allocator-provenance-preserving file page reader. The unchecked
hosted deep copy was removed, failed reads preserve the prior destination,
caller-owned pages survive file close, and SQL9, cache, transaction, and
multi-COMMIT WAL recovery behavior remains qualified.
