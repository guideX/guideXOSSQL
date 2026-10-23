# NDB1R4P11R3I: INSERT Savepoint Ownership and Checked Writer Publication

## Result

Outcome: **INSERT_SAVEPOINT_OWNERSHIP_VERIFIED (Outcome A)** for the selected INSERT statement-savepoint and index page-payload boundary.

This qualifies the selected INSERT/savepoint boundary. It does not qualify every hosted writer or migrate DatabasePage::payload.

## Repository gates and ending state

- Authoritative SQL checkout: D:\dev\guideXOS_SqlDatabase.
- Starting branch and HEAD: main at 7286126b2c8f476fa9cb01ffcb7070eb33bc845c.
- Starting worktree: clean; main was three commits ahead of origin/main.
- Ending branch: main, with the R3I local commit and no branch or history rewrite.
- .phase remains SQL9 -> SQL10.
- .native-sql.phase remains NDB1R4P10 -> NDB1R4P11.
- The parent NDB1R4P11 phase remains open; P12 was not started.
- No fetch, pull, merge, rebase, cherry-pick, reset, clean, stash, worktree creation, branch change, or push was performed.
- Read-only Server reference: D:\dev\guideXOSServer_NATIVE_DB_STORAGE, branch NATIVE_DB_STORAGE, HEAD f29850c93bbac0a0763a31e806d2816310156575. Its worktree remained clean and no Server files or Git state were modified.
- The SQL local commit SHA is included in the final handoff. The commit contains this report, so the report records the ending state symbolically as the R3I commit on main.

## Accepted R3H baseline and WAL recovery

Read docs/NDB1R4P11R3H_DATABASE_READER_MIGRATION.md and the accepted R3F-R1 WAL recovery evidence.

R3H's actual Database::readPage implementation calls Database::readOwnedPage, validates into the portable owned-page representation, then publishes through the existing hosted DatabasePage compatibility representation using a non-allocating move. Its remaining hosted compatibility allocation reports NoSpace. Transaction overlay precedence, recovery-required read blocking, page-byte compatibility, and the 512-byte and 4,096-byte paths were preserved.

The accepted R3H qualification was 21/21 CTest targets, 150,448 aggregate checks, zero failures; 251 owned-page checks; 4,901 cache/recovery checks; and 16/34 freestanding units passing with 18 blocked. R3F-R1 retained-WAL recovery and post-durable-COMMIT recovery-required behavior were left unchanged.

## Actual INSERT call graph and mutation order

Source locations are the final R3I source line numbers:

~~~text
SqlEngine::executeStatement (database/database_sql.cpp:2655)
  -> SqlEngine::executeInsert (database/database_sql.cpp:2871)
     -> target table lookup and INSERT ordinal/value construction
        (literal coercion, DEFAULT/NULL handling)
     -> Transaction::beginStatement (database/database_transaction.cpp:308)
        only when SqlEngine::_tx is active
     -> Table::insert (database/database_heap.cpp:987)
        -> Table::insertInTransaction (database/database_heap.cpp:1024)
           -> validateRowValues
           -> encodeRow
           -> tableIndexTargets / index key encoding
           -> uniqueness and PRIMARY KEY lookups
           -> enforceChildForeignKeys
           -> appendEncodedRowToTail (database/database_heap.cpp:1117)
              -> append to the tail page or allocate/write/link a new heap page
              -> returns the RowLocator
           -> for each primary/secondary index:
              IndexTree::createEmpty / IndexTree::insert
                -> IndexTree::writeLeaf (database/database_index.cpp:299)
                -> IndexTree::writeInternal (database/database_index.cpp:444)
              update catalog root and entry count
           -> mark catalog dirty and update heap statistics
     -> release savepoint on success
     -> on error, Transaction::rollbackStatement and clear cached Table facades
  -> later Transaction::commit writes WAL records and durable COMMIT,
     then publishes committed page images to cache

INSERT values are literals or bound values in this SQL9 path, coerced before the savepoint. Table/column/default/null validation and value-vector construction happen before the first database mutation. In Table::insertInTransaction, value validation, row encoding, index-key generation, uniqueness checks, and child foreign-key checks precede heap staging. INSERT appends or allocates/links a heap page; it does not relocate an existing row. Index writes then occur sequentially, so a later index failure can follow an earlier index overlay update.

## Savepoint construction and rollback audit

Transaction::beginStatement first clones the complete transaction page overlay and its page-number index with gxTransactionPageOverlayClone. These allocations use the active transaction page allocator. A failed clone returns its GxStatus as a DbResult and leaves the live overlay untouched; partially allocated snapshot storage is destroyed. It then copies Catalog under a std::bad_alloc catch. If that copy fails, it destroys the cloned page snapshot and returns NoSpace. Only after both parts succeed does it record _nextPageId and _catalogChanged and return a usable savepoint.

The SQL savepoint is created immediately before Table::insert, after INSERT value construction and before any table mutation. If preparation fails, Table::insert is never called. Earlier transaction pages and catalog state are unchanged.

Transaction::rollbackStatement now returns DbResult. It checks that the transaction is active, checks the Gx overlay move result, restores the page overlay and page-number index without allocation, restores page-count/catalog-dirty metadata, swaps the saved Catalog back through Catalog::swap (a noexcept, non-allocating vector/scalar swap), restores _nextPageId, and increments Database::_dataEpoch so heap-tail caches cannot retain a page removed by rollback. SqlEngine clears cached Table facades after rollback. Rollback errors propagate as RollbackFailed rather than being discarded or replaced by the original mutation error. Other SQL statement-savepoint rollback call sites were updated to check the same status.

Full transaction rollback also restores Catalog by swap. Catalog's persisted fields and encoding are unchanged.

## Selected production allocation boundary and inventory

Selected operation: SQL INSERT mutation with an explicit-transaction statement savepoint, including the hosted index leaf payload preparation in IndexTree::writeLeaf.

Original unchecked operation: DatabasePage::payload.assign(capacity, 0) in writeLeaf could throw std::bad_alloc past its DbResult interface. writeInternal had the same issue. IndexTree::insert treated any NoSpace from writeLeaf/writeInternal as an entry-overflow split request, which could hide allocation failure behind split logic.

Production change:
- IndexTree::writeLeaf and writeInternal catch std::bad_alloc from payload assignment and return DbStatus::NoSpace with a specific leaf/internal payload allocation message.
- IndexTree::insert only enters leaf/internal split logic when encoded entries or children actually exceed page capacity. A NoSpace from payload allocation at an otherwise fitting size is returned as NoSpace.
- Table::insert catches std::bad_alloc around insertInTransaction and translates it to NoSpace. This also covers other INSERT-time hosted heap/index/vector allocations within the table mutation. For implicit transactions, it rolls back the whole transaction and reports RollbackFailed if that rollback fails. For explicit transactions, the SQL savepoint restores the pre-statement state.
- SqlEngine::executeInsert creates the explicit savepoint before mutation, checks rollback status on every failure, clears table facades after restore, and releases the snapshot only after success.
- Catalog::swap provides allocation-free catalog restoration. The transaction rollback status is now observable instead of ignored.
- Prepared SELECT execution refreshes its cached column-vector references from stable prepared Table facades each run. This is required because allocation-free Catalog vector swap invalidates references into the old catalog record vector.

Ownership and allocator lifetime:
- The selected leaf/internal payload is still hosted std::vector storage owned by a local DatabasePage during construction. Its allocation failure now has an explicit DbResult path; this does not make the hosted payload a portable owner.
- Transaction/savepoint page images and page-number lookup use the active Transaction's GxAllocator-backed overlay. The allocator context remains owned by the active transaction throughout snapshot, mutation, and restore.
- Catalog snapshots remain hosted vectors, but construction is caught before mutation and restoration is a non-allocating swap.
- Transaction::writePage stages encoded images in transaction-owned overlay storage. Committed cache publication occurs only in commit after durable WAL COMMIT.
- No new transaction page owner or borrowed GxOwnedPage lifetime was introduced.

Other fallible operations inventoried:
- INSERT target/ordinal/value vectors and literal coercion are hosted allocations before the first database mutation. If they fail, no heap/index/catalog state has been changed; these allocations are not migrated in R3I.
- Row encoding and index-target/key vectors in insertInTransaction are hosted allocations. Table::insert catches bad_alloc and the surrounding explicit statement savepoint or implicit transaction rollback handles any subsequent mutation.
- appendEncodedRowToTail uses hosted DatabasePage payload vectors and R2 heap-page preparation/publication. It can write/allocate a heap page before index insertion; failures return status or are caught at Table::insert, then the statement snapshot restores all transaction pages.
- Gx transaction page image creation/replacement and page-index updates return explicit GxStatus through Transaction::writePage; the complete page overlay is covered by the savepoint.
- Catalog root/count changes are in-memory state captured by the Catalog snapshot.
- DatabasePage::payload remains a hosted compatibility representation. Other DML, legacy page-write, parser, and non-INSERT writer allocations remain outside the selected conversion.

## Failure-injection evidence and statement atomicity

Focused workload is the authoritative SQL9 engine on 512-byte pages with:
- ParentRows(Id PRIMARY KEY).
- PriorRows(Id PRIMARY KEY, V TEXT) and IX_PriorRows_V.
- ChildRows(Id PRIMARY KEY, ParentId FK to ParentRows, Code TEXT NOT NULL), its primary index, and IX_ChildRows_Code.

The workload first begins an explicit transaction, updates PriorRows to rollback-probe, and verifies at least two staged pages. A transaction allocator fault is injected on the third snapshot allocation: two snapshot allocations succeed, the later allocation fails. The test observes exactly three attempts, two successes, and one allocation failure. executeInsert reports statement page snapshot allocation failed, does not call Table::insert, leaves transaction page count/final page count and allocator outstanding bytes unchanged, leaves ChildRows empty, and preserves the prior UPDATE as visible read-your-writes. An explicit ROLLBACK then restores PriorRows to before.

A second explicit transaction updates PriorRows to commit-probe and records its staged page count, final page count, and transaction allocator outstanding bytes. A test-executable-local, scoped hosted operator-new fault injector targets the actual 480-byte page-payload allocations for 512-byte pages. It is disabled immediately after each execute call.

Three deterministic IndexTree::writeLeaf allocation failures were reached, each asserted by exact attempt/success/failure counts and the production error text:
- 3 matching attempts: 2 successful matching allocations, then failure at the primary leaf writer after the heap row was staged.
- 8 matching attempts: 7 successful matching allocations, then failure during the primary-index insertion path. The allocation NoSpace was not misread as a leaf-overflow split.
- 13 matching attempts: 12 successful matching allocations, allowing the prior primary-index work to complete before failure in the later secondary-index leaf writer.

Each failure returns index leaf payload allocation failed, keeps the explicit transaction active, restores the transaction's modified-page count and final page count, restores transaction allocator outstanding bytes, and leaves ChildRows with zero rows. The child indexes remain at zero entries and validate successfully. This exercises rollback after heap staging and after a prior index update; it does not claim that every possible multi-level split allocation was separately fault-injected.

After the injected failures, the transaction remains usable. A prepared INSERT with an invalid ParentId is still rejected by the normal foreign-key check before heap mutation. A valid prepared INSERT retry succeeds. The test verifies one child row, full database integrity, each child index contains one entry, and primary/secondary index lookups resolve to the identical page/slot RowLocator. The earlier UPDATE remains readable by its index lookup. The transaction commits. After close/reopen, the row and prior UPDATE persist, both primary and secondary index queries use IndexLookup and return the expected row, integrity/index validation succeeds, and both allocator outstanding counts return to zero.

Prepared plans are also included in the full regression. Refreshing prepared SELECT column metadata after catalog swap avoids stale references across transaction rollback.

## Cache, transaction visibility, WAL, and formats

Uncommitted INSERT and earlier UPDATE pages remain in the active transaction overlay, visible to transaction reads. Failed INSERT restores that overlay to the savepoint snapshot; committed cache images are not published by statement execution. Transaction::commit retains the existing WAL ordering and publishes committed cache images only after durable WAL COMMIT. The post-durable-COMMIT cache-publication failure still sets recovery-required state and is not reported as an ordinary rollback.

The P6 focused target passed 619 checks and explicitly printed its durable WAL recovery after cache-publication allocation failure case. The R3H cache/publication/recovery target passed 4,901 checks; its 4,096-byte fixtures retain the valid committed header histories and post-COMMIT recovery-required behavior. WAL format/recovery code was not changed.

No persistent format or codec source was changed. The R3I SQL workload creates, commits, closes, and reopens a 512-byte database. Existing 4,096-byte reader and WAL recovery fixtures passed. Page headers/types, CRC, heap slots, B+ tree encoding, catalog encoding, .gxdb/.gxwal framing, and transaction commit semantics remain unchanged.

## Focused and full hosted qualification

- R3I target: database_heap_page_construction_test, 300 checks, zero failures. This executable includes the accepted R2 heap-page construction and P11R2 coverage; it previously had 159 checks, so this turn adds 141 checks.
- P7 production transaction/savepoint target: 308 checks, zero failures; its rollbackStatement caller now asserts the returned Ok status.
- P8 production transaction-index/savepoint target: 25,544 checks, zero failures; its rollbackStatement caller now asserts the returned Ok status.
- R3H owned-page target: 251 checks, zero failures.
- P6 cache and durable-recovery target: 619 checks, zero failures.
- Cache publication/recovery target: 4,901 checks, zero failures.
- Full CTest: 21/21 targets passed, zero failures, 150,591 aggregate checks. This is 143 more checks than the accepted R3H total (141 R3I target additions and one new rollback result assertion in each P7 and P8 target).
- SQL1-SQL9 tests, prepared statements, FK/index constraints, DML, DELETE/rollback regression, P7/P8 transaction ownership, R2 heap construction, R3E cache publication, R3F-R1 WAL recovery, and R3H owned reads all remained green.
- The full hosted run completed after the final production and test changes. git diff --check passed before commit.

The R3I target uses 512-byte pages; existing reader/cache/WAL fixtures also exercise 4,096-byte behavior.

## Freestanding census and symbol audit

Ran tools/qualify_sql9_freestanding.ps1 with the actual Server AMD64 target configuration, including -ffreestanding, -nostdinc++, -fno-exceptions, and -fno-rtti. Artifacts are in C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3I-20261022.

- 34 production translation units: 16 pass, 18 blocked.
- All 16 previously passing units remain passing; no previously blocked unit newly passes and no production source was added.
- The same 18 hosted-header blockers remain: database_io (cstdio), database_native_io (memory), database_header (string), database_page (vector), database_file (memory), database_engine (memory), database_buffer (via database_file.h memory), database_catalog (string), database_index (set), database_heap (memory), database_wal (memory), database_transaction (memory), database_relational (memory), database_sql_token (string), database_sql_tokenizer (string), database_sql_parser (vector), database_sql_expr (vector), and database_sql (map).
- Passing object symbol files retain the accepted Gx allocator/array/page/transaction-overlay APIs and hosted allocator malloc/free imports, with crc32 references as before. No exception throw/personality, RTTI type/vtable, or global new/delete symbols were found in the passing object symbol files.
- This is a production object census and symbol inspection only; no native SQL link or execution is claimed.

The changed hosted SQL units remain among the same 18 blocked units because the AMD64 freestanding command intentionally omits the C++ standard library. There is no freestanding regression or new portable source.

## Remaining hosted boundaries and next bounded continuation

R3I makes the selected index writer allocation explicitly status-returning and controls exceptions from the complete Table::insert mutation with a transaction/savepoint rollback. It does not convert the hosted DatabasePage::payload representation or every heap/index writer. Heap-page payload assignment in appendEncodedRowToTail, other index building vectors, SQL value preparation, legacy page-write compatibility APIs, parser allocations, and non-INSERT DML remain hosted.

Recommended next P11 continuation: make the newly allocated heap-page final payload construction/publication in Table::appendEncodedRowToTail an explicitly bounded production operation. Reuse the accepted R2 heap-page builder; construct scratch bytes in a GxArray allocated by the active Transaction's retained GxAllocator (not by borrowing a caller-owned GxOwnedPage), then pass the completed bytes into the existing transaction-owned page overlay through Transaction::writePage. Check every builder, page allocation, page write, and tail-link status before publishing the RowLocator/stat updates. Keep the existing SQL statement savepoint as the rollback owner so failure after a new heap page is written or linked restores page images, page count, locator visibility, catalog state, and earlier statements. Do not start this continuation automatically.

## Acceptance

**Yes.** The authoritative SQL9 INSERT/savepoint path now reports failure at a real production index payload allocation boundary, restores the complete explicit-statement overlay/catalog baseline, preserves prior transaction work, maintains heap/index/FK integrity, supports retry/commit/reopen, and leaves WAL durability semantics unchanged. The qualification applies to this selected boundary; remaining hosted payload and writer allocations are listed above.

Local R3I commit: on existing SQL main; the final SHA is in the commit metadata and final handoff.
