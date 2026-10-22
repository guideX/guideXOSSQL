# NDB1R4P11R3H: Database-Level Checked Page Reader

## Pre-edit selection record

### Repository gates

- SQL repository: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`, `8b36b6f7cc3013264629a9baca8e3c7251a8c886`.
- Starting worktree: clean; no R3H report or implementation existed.
- `.phase`: `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase`: `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- Read-only Server reference: `NATIVE_DB_STORAGE`,
  `f29850c93bbac0a0763a31e806d2816310156575`, clean.

### Selected function/caller contract (recorded before source edits)

| Field | Finding |
|---|---|
| Selected function/caller | `Database::readPage(uint64_t, DatabasePage&)`, the committed-page compatibility reader; `Transaction::readPage` delegates to it only when no transaction overlay entry exists. |
| Actual source location | `database/database_relational.cpp:114`; owned reader at `:138`; `DatabaseFile` readers at `database/database_file.cpp:407` and `:454`. |
| Existing allocating copy | `out = slot->page` copied the hosted `std::vector<uint8_t>` payload while a cache slot was pinned. `std::bad_alloc` was caught and returned as `NoSpace`, but the boundary still depended on hosted page copying. |
| Source page representation | Validated encoded cache image (`GxPageCacheEntry::image`), including a live dirty slot encoded before read when needed; on a miss the file image is read and validated before cache publication. |
| Page identity and extent | `pageId` must be nonzero and below the committed file page count; source extent is exactly the configured page size; the existing inspector checks identity, supported type, payload bound, and CRC. |
| Visibility | Committed database view only. `Transaction::readPage` checks the transaction-owned encoded overlay first and delegates here only for a base committed page. |
| Destination representation | Existing hosted `DatabasePage` compatibility output, which may outlive the database. Its payload allocation remains hosted and is not claimed as freestanding. |
| Destination allocator | Temporary encoded `GxOwnedPage` uses `BufferManager::scratchAllocator()`; decoded compatibility payload uses the existing hosted `std::vector` allocator. |
| Allocator lifetime | Scratch allocator context is borrowed from the live `BufferManager`; the temporary owner is destroyed on every return path before `Database::readPage` returns. The published `DatabasePage` owns its vector independently and can outlive the database. |
| Cache pin/reference ownership | Existing implementation pins a `BufferSlot`, copies its hosted page, then unpins. The selected replacement uses the existing synchronous encoded-image reader, acquires no cache pin, and returns no cache reference. |
| Existing failure status | `NotOpen`, `RecoveryRequired`, bounds/identity errors, and `NoSpace` for hosted copy allocation; pin release was attempted after copy failure. |
| Publication boundary | Existing output changes only after its copy succeeds. Proposed path validates and materializes a temporary encoded owner, parses into a local hosted page, then move-assigns output only after parse succeeds. |
| Proposed replacement | Call the checked database-owned encoded reader with a temporary owner configured from the manager scratch allocator; decode with `DatabasePage::parse` under `bad_alloc` handling; publish by nothrow move. |
| Remaining hosted operations | The compatibility decode still allocates/copies payload bytes into `DatabasePage::payload`. It will be explicitly identified and excluded from any claim that the payload itself is portable. |

### Evidence reviewed before source edits

- R3G recommends this exact next step: convert committed `Database::readPage`
  through `Database::readOwnedPage`, leaving the overlay-first transaction
  branch intact.
- R3G confirms `Database::readOwnedPage` uses the validated encoded cache
  image path and checks recovery-required state.
- R3C–R3F-R1 reports preserve the caller allocator lifetime contract, cache
  publication and pin rules, the original hosted page-view fault behavior,
  and the multi-COMMIT WAL recovery repair. No WAL or writer change is in
  scope.
- `BufferManager::scratchAllocator()` explicitly permits temporary page
  preparation whose owner is destroyed before return.

Implementation and qualification results follow.
## Production trace and copy-site inventory

### Selected reader and checked ownership path

Database::readPage is a committed-only compatibility reader. Its previous route pinned a BufferSlot, performed out = slot->page (a deep copy of the hosted payload vector), then unpinned. The copy could return NoSpace through its bad_alloc catch, but it still materialized a page through hosted ownership while holding a cache pin.

The replacement configures a temporary GxOwnedPage with BufferManager::scratchAllocator() and the configured page size, calls the existing Database::readOwnedPage, parses the validated encoded bytes into a local DatabasePage, and publishes with a nothrow move assignment. A compile-time is_nothrow_move_assignable<DatabasePage> assertion protects the publication step. Setup and parse allocation errors map to explicit statuses. The temporary owner leaves scope before the call returns, so no returned object retains the borrowed scratch allocator context. The output remains a hosted compatibility value whose vector independently owns its bytes.

This removes the cache-slot hosted DatabasePage deep-copy dependency from the selected production boundary and routes the read through checked encoded ownership. It does not remove the hosted vector allocation in the compatibility decode, and it does not qualify DatabasePage::payload as portable.

Database::readOwnedPage retains its existing open/recovery checks, committed page bounds, cache-image cloning or file-miss staging, and encoded-page validation (identity, supported type, length bound, CRC) before publishing the owner. It does not return a cache pin. A read from a resident page copies the encoded image while the synchronous call owns the source context; a miss stages a complete validated file image before cache publication. The destination owner is independent of cache lifetime.

### Call graph and visibility

Transaction::readPage first checks its transaction-owned encoded page overlay. It delegates to Database::readPage only for a committed base page absent from that overlay. The overlay-first branch was not changed. Thus the migrated boundary remains committed-only and is not substituted for a transaction-aware read; read-your-writes continues to use the transaction overlay.

### Remaining production ownership inventory

| Function or boundary | Remaining hosted operation and owner lifetime | Failure/publication and transaction contract | Minimum safe next conversion |
|---|---|---|---|
| DatabasePage::payload, parse, serialize | std::vector<uint8_t> owns compatibility payload. Parse assignment can allocate; serialization resize can allocate. | Parse validates encoded input before assignment, but generic serialization does not itself turn every allocation exception into a status. | Keep as compatibility boundary until all callers consume encoded owners and serialization has a checked output contract. |
| DatabaseFile::readPage | Compatibility decode into DatabasePage; the accepted R3G path reads into caller-owned exact encoded storage then parses and moves the hosted result. | Source extent is known; parse allocation is caught; result publishes after successful parse. | Preserve as compatibility wrapper; no duplicate R3G rewrite. |
| BufferManager::materialize / cache miss pinPage | Decodes encoded image into the legacy hosted slot page; hosted vector allocation is caught and mapped. | Cache slot publication is staged; a failed materialization does not publish a partial slot. | Migrate only with a checked slot representation or a controlled compatibility adapter. |
| BufferManager::installPage(const DatabasePage&) | Encodes through cache-owned storage; pinned compatibility views may copy DatabasePage under a bad_alloc boundary. | Encoded-image overload is used by transaction commit; legacy view path preserves pinned-view publication behavior. | Remove only after compatibility writers and pinned-view callers use encoded images. |
| DatabaseFile::writePage | Serializes the hosted page to a std::vector; serialization growth is hosted. | File write follows serialization; generic vector growth is not a portable allocator operation. | Add a checked encoded-page write boundary and migrate callers before removing the compatibility method. |
| Transaction::readPage overlay | Committed base delegates to Database::readPage; overlay bytes are parsed into a hosted page under a bad_alloc catch. | Overlay is checked first, preserving read-your-writes. | Continue toward a caller-owned decoded or encoded API only with explicit overlay/base visibility. |
| Transaction::writePage and commit publication | Stages encoded page images through the transaction-owned Gx page overlay; commit installs encoded images into file/cache. | Statement/full rollback and durable commit/recovery-required behavior are preserved by existing code. | This is an available portable staging boundary for future writer work. |
| Heap appendEncodedRowToTail | Existing tail payload is mutated in hosted storage; new-page payload uses vector assignment. packHeapPage uses bounded GxArray scratch then copies into a hosted vector under a caught allocation boundary. | Staged through transaction overlay; a failure after some staging is safe only when the enclosing statement is rolled back. | First establish executeInsert statement savepoint coverage; then consume a prepared encoded heap image directly into the transaction overlay. |
| Index leaf/internal writers | Page construction uses hosted payload assignment before pages.writePage. | Individual allocation failure may escape from those assignments; enclosing mutation rollback must cover prior staged heap/index work. | Add checked page preparation and propagate status through encoded transaction staging, under a complete statement savepoint. |
| executeInsert / Table::insert | Active explicit transaction path calls insertInTransaction; no insert-level beginStatement/rollback/release wrapper was found in executeInsert. | Uniqueness and FK checks precede mutation, but later allocation/index failure can happen after heap staging. Outside explicit transactions, the implicit transaction wrapper rolls back on operation failure. | Smallest next prerequisite is insert statement savepoint ownership and fault tests for failure after heap staging and during index staging. |
| P7/P8 test helpers | A few test-only page copies remain (P7 firstWrite/secondWrite/thirdWrite; P8 changed/replacement). | Not production ownership paths. | Leave until production migrations no longer require the compatibility test helpers. |

### Failure atomicity, cache safety, and lifetime evidence

Database::readPage now builds into a local destination and changes the caller's output only after the owned read and hosted parse both succeed. The final move is compile-time constrained to be nothrow. Owned-reader setup failures are mapped (OutOfMemory to NoSpace; ResourceLimit/Overflow to TransactionTooLarge; other setup failures to Internal); hosted parse bad_alloc maps to NoSpace. The temporary page is destroyed on every exit.

The focused checks exercise a real scratch allocator failure during this production path, verify one allocator attempt and no leaked allocation, preserve the previous destination bytes, preserve a live cache pin and source pointer, and verify a successful retry. A separate hosted allocation injector fails the compatibility vector decode after the checked owner has been materialized; it confirms the failure is reported, the destination remains unchanged, the temporary owner is released, and retry succeeds. The tests also compare the published compatibility bytes exactly with the encoded page image and retain/use a returned DatabasePage after database close and destruction.

The reader no longer acquires or releases a cache pin. Tests keep an existing pin live across both failure modes and the successful retry; the pinned source pointer and pin count remain stable. GxOwnedPage is a copied owner, not a cache reference.

### SQL and persistent compatibility

The SQL9 workload test exercises the real engine path with table/index operations, indexed SELECT, INSERT, UPDATE, explicit BEGIN, read-your-writes, duplicate insert failure, COMMIT, DELETE, ROLLBACK, and close/reopen indexed reads. The test instruments GxArena::allocationAttempts around an indexed SQL SELECT; the count increases, demonstrating that the SQL workload enters the checked owned-page path. The source call trace shows a base-page Transaction::readPage delegates to Database::readPage; transaction-overlay reads remain on their overlay path.

For 4,096-byte pages, the test compares the compatibility page serialization byte-for-byte against the encoded fixture. For 512-byte pages, it compares serialization from Database::readPage against the image returned by Database::readOwnedPage; the compatibility result also remains usable after close. No persistent format, WAL framing, page header/type, CRC, page-size setting, slot layout, index encoding, catalog format, or transaction commit semantics were changed.

The recovery-required checks remain in Database::readPage and the delegated owned reader. Existing durable-COMMIT fault coverage continues to require reads and flushes to return RecoveryRequired after post-durable cache publication failure, then reopen recovers the committed state from WAL. The R3F-R1 WAL history coverage remains in the regression suite: 4 → 8 → 23 → 48 is accepted and 4 → 3 is rejected as WalCorrupt. No WAL source or commit-ordering code was changed.
## R3G acceptance and final qualification

### Accepted baseline and unchanged state

R3G acceptance was reviewed from its report and source before this change. Its accepted baseline was 21/21 CTest targets, 150,410 checks, zero failures; the owned-page target had 230 checks and the cache/recovery target had 4,884. The freestanding census was 16 passing and 18 blocked of 34 production translation units. The R3F-R1 WAL history checks still accept committed header counts 4 → 8 → 23 → 48 and reject 4 → 3 as WalCorrupt.

The SQL branch remained main at starting HEAD 8b36b6f7cc3013264629a9baca8e3c7251a8c886 through the final pre-commit review. Both markers remain unchanged: .phase is SQL9 / SQL10, and .native-sql.phase is NDB1R4P10 / NDB1R4P11. Server remained on NATIVE_DB_STORAGE at f29850c93bbac0a0763a31e806d2816310156575, with no edits.

### Focused and full hosted regression

- database_owned_page_test: 251 passed, 0 failed (R3H adds 21 checks over the accepted 230).
- database_cache_publication_test: 4,901 passed, 0 failed (R3H adds 17 checks over the accepted 4,884).
- database_ndb1r4p6_test: 619 checks, 0 failed, including durable WAL recovery after the allocator fault.
- Full CTest after the final production and test changes: 21/21 targets passed, 0 failures, 1,149.48 seconds. The baseline total of 150,410 plus 21 owned-page checks and 17 cache/recovery checks yields 150,448 checks.

The first full run revealed that the P6 fault injector's allowance of one Gx allocation now failed during the checked committed read, before WAL durability: a cache miss stages both the cache image and the requested owned image. The P6 regression was calibrated to allow those two page-read allocations and fail the following committed-page cache publication. The focused P6 target and the final complete run confirm the durable commit still transitions to recovery-required state, reads and flushes stay blocked, and reopen restores committed data. The first full run also recorded one cache publication test failure at a small-page database-create assertion; it did not reproduce in the focused target or the final full CTest run.

The accepted owner-contract cases remain enabled in database_owned_page_test: invalid explicit allocator configuration, page-size/capacity rejection, cross-domain clone failure, prior-destination preservation, retry after failure, and repeated construction/destruction. The new database-reader cases additionally prove that the actual Database::readPage scratch allocation and the separate hosted payload allocation both fail at their real allocation points.

### Freestanding census and ABI

The qualification ran with tools/qualify_sql9_freestanding.ps1, the Server AMD64 flags, and Windows PowerShell 5.1. Its artifacts are in C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3H-qual-20261022. The census remained 16/34 passing and 18/34 blocked. All 16 previously passing units remain passing; there are no newly passing units and no translation unit additions. The changed database/database_relational.cpp remains blocked at its existing hosted <memory> include in database/database_relational.h. This is object-level compile and symbol inspection only; no full SQL native link or execution is claimed.

The 18 exact first diagnostics remained the hosted-header blockers. Paths below are relative to the SQL checkout; the compiler emitted the prefix D:\dev\guideXOS_SqlDatabase\:

| Unit | First compiler diagnostic |
|---|---|
| database/database_io.cpp | database/database_io.h:13:10: fatal error: cstdio: No such file or directory |
| database/database_native_io.cpp | database/database_native_io.h:5:10: fatal error: memory: No such file or directory |
| database/database_header.cpp | database/database_header.h:7:10: fatal error: string: No such file or directory |
| database/database_page.cpp | database/database_page.h:7:10: fatal error: vector: No such file or directory |
| database/database_file.cpp | database/database_file.h:17:10: fatal error: memory: No such file or directory |
| database/database_engine.cpp | database/database_engine.h:9:10: fatal error: memory: No such file or directory |
| database/database_buffer.cpp | database/database_file.h:17:10: fatal error: memory: No such file or directory |
| database/database_catalog.cpp | database/database_catalog.h:35:10: fatal error: string: No such file or directory |
| database/database_index.cpp | database/database_index.h:27:10: fatal error: set: No such file or directory |
| database/database_heap.cpp | database/database_heap.h:26:10: fatal error: memory: No such file or directory |
| database/database_wal.cpp | database/database_wal.h:30:10: fatal error: memory: No such file or directory |
| database/database_transaction.cpp | database/database_transaction.h:25:10: fatal error: memory: No such file or directory |
| database/database_relational.cpp | database/database_relational.h:21:10: fatal error: memory: No such file or directory |
| database/database_sql_token.cpp | database/database_sql_token.h:11:10: fatal error: string: No such file or directory |
| database/database_sql_tokenizer.cpp | database/database_sql_tokenizer.h:12:10: fatal error: string: No such file or directory |
| database/database_sql_parser.cpp | database/database_sql_parser.h:13:10: fatal error: vector: No such file or directory |
| database/database_sql_expr.cpp | database/database_sql_expr.h:18:10: fatal error: vector: No such file or directory |
| database/database_sql.cpp | database/database_sql.h:16:10: fatal error: map: No such file or directory |

The unique undefined symbols in passing objects were unchanged from R3G:

- C++ ABI guards: __cxa_guard_acquire, __cxa_guard_release.
- Hosted allocator adapter: malloc, free.
- Gx allocator/page/cache primitives: gxAllocatorAllocate, gxAllocatorFree, gxDatabasePageEncode, gxDatabasePageInspect.
- Gx array primitives: gxArrayInit, gxArrayClear, gxArrayDestroy, gxArrayReserve, gxArrayResize, gxArraySwap.
- CRC functions: gxos::db::crc32Init, gxos::db::crc32Update, gxos::db::crc32Final.
- Transaction page/index primitives: gxTransactionPageIndexInit, gxTransactionPageIndexClear, gxTransactionPageIndexClone, gxTransactionPageIndexDestroy, gxTransactionPageIndexInsert, gxTransactionPageIndexLookup, gxTransactionPagesInit, gxTransactionPagesAt, gxTransactionPagesAppendPage, gxTransactionPagesClear, gxTransactionPagesClone, gxTransactionPagesCount, gxTransactionPagesDestroy, gxTransactionPagesPopLast, gxTransactionPagesReplacePage.

No __cxa_throw, exception-personality, RTTI, operator new, or operator delete imports appeared in passing objects.

### Final outcome and continuation

Outcome: DATABASE_READER_OWNERSHIP_VERIFIED. The selected database-facing reader now obtains a checked encoded page owner, validates before compatibility decode, and publishes only by nothrow move. The hosted payload allocation remains explicit and caught; this outcome qualifies only this checked reader path.

The remaining writer/savepoint prerequisite and bounded next step are recorded in the ownership inventory above: add statement-level rollback coverage to executeInsert while an explicit transaction is active, inject failure after heap staging and during index staging, then migrate prepared heap/index page images into transaction-owned encoded staging. Do not remove DatabasePage::payload until the compatibility reader/writer, BufferManager materialization, heap/index/catalog construction, and remaining test helpers no longer depend on hosted vectors.

The ending SQL branch is main and the ending HEAD is this local R3H commit, created from the gated starting HEAD; the commit SHA is included in the task closeout. The post-commit worktree was verified clean. Parent NDB1R4P11 remains open; no P12 work is started.
