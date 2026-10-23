# NDB1R4P11R3J: Checked Heap Append Publication

## Result

Outcome: **CHECKED_HEAP_APPEND_PUBLICATION_VERIFIED (Outcome A)** for the selected heap append and transaction staging boundary. This continuation qualifies `Table::appendEncodedRowToTail`; parent NDB1R4P11 remains open. It does not qualify every heap or index writer, migrate `DatabasePage::payload`, or authorize P12.

The append path now builds complete heap payload candidates through the existing R2 portable heap encoder using the active transaction's page allocator. Existing-tail replacement and new-page staging use checked transaction-owner APIs. The row locator and Table tail cache are published only after the corresponding staged image succeeds. Failed INSERT operations continue through the accepted R3I statement savepoint.

## Repository and accepted baseline

- SQL checkout: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `a5e52a1631974b09c784337dc0957f467a928d07`; worktree was clean before R3J changes.
- `.phase` remains `project=guideXOSSql`, `last_completed=SQL9`, `next_expected=SQL10`.
- `.native-sql.phase` remains `project=guideXOSSqlNativePortability`, `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- The R3I acceptance report and source were inspected. Its accepted baseline is 21/21 CTest targets, 150,591 checks, 0 failures; focused R3I 300; P6 recovery 619; R3H owned-page 251; cache/recovery 4,901; freestanding 34 units with 16 passing and 18 blocked.
- R3I's INSERT savepoint clones transaction pages and catalog before mutation, restores the savepoint on selected heap/index errors, preserves successful earlier statements, and clears cached Table facades after rollback. The R3I failure behavior and savepoint implementation were retained. Its test allocator targets were recalibrated only where the R3J removal of a hosted heap payload allocation changed the production allocation sequence; the index and transaction-read failures remain asserted at their actual fault sites.
- R2's `gxHeapPagePrepare` remains the authoritative heap encoding implementation. P7/P8 transaction ownership and lookup contracts, R3C–R3H owned-page/cache/recovery work, and R3F-R1 WAL recovery were inspected and left intact.
- Read-only Server reference remains at `D:\dev\guideXOSServer_NATIVE_DB_STORAGE`, branch `NATIVE_DB_STORAGE`, HEAD `f29850c93bbac0a0763a31e806d2816310156575`. Its source, build output, phase state, and Git state were not changed.

## Actual append call graph and publication boundary

For SQL INSERT, the current call sequence is:

```text
SqlEngine::executeInsert (database/database_sql.cpp)
  -> construct/coerce INSERT values
  -> Transaction::beginStatement when an explicit transaction is active
  -> Table::insert (database/database_heap.cpp)
     -> Table::insertInTransaction
        -> validate values and encode row bytes
        -> build/check primary and secondary index keys and uniqueness
        -> enforce child-side foreign keys
        -> Table::appendEncodedRowToTail
           -> ensureLastHeapPage / read the current tail through PageAccess
           -> prepare complete existing-tail candidate with
              gxHeapPageAppendPrepare, or a complete one-row page with
              gxHeapPagePrepare
           -> Transaction::writePagePayload for a fitting tail, or
              Transaction::allocatePagePayload for a new page
           -> when creating a page after a tail, stage the tail-link image
           -> publish the RowLocator and local tail/page outputs
        -> update index trees and catalog roots/counts
        -> update row/page statistics in the transaction catalog
  -> release statement savepoint on success; restore it on failure
  -> Transaction::commit writes WAL and publishes at the existing commit boundary
```

`appendEncodedRowToTail` is also called by the row-relocation path in `Table::applyMutationsInTransaction`; that path remains covered by its established transaction/statement behavior. INSERT performs heap staging before its index writes, so a later index failure can follow successful heap staging. R3I's savepoint restores the overlay, page count, and catalog for that case.

The pre-R3J append implementation read a `DatabasePage`, mutated `payload` in place for a fitting row, then called `writePage`. For a full tail it allocated a placeholder page, read it back into another hosted `DatabasePage`, assigned a zeroed payload vector, manually wrote the heap header/row/slot, staged it, then mutated and staged the old tail's next-page link. Hosted vector growth and page staging could therefore fail at separate points after local mutation, and the payload copy path did not directly express the transaction allocator domain.

## R3J implementation

### Existing-tail append

- Checks the encoded row length and minimum empty-page fit with widened arithmetic before page work.
- Reads and validates the tail page using the existing `PageAccess` checked-read result. `DatabasePage::payload` remains the hosted compatibility representation.
- Calls `gxHeapPageAppendPrepare` with the allocator copied from the active `Transaction`'s page owner. The portable builder validates page size, payload length, heap magic/version, slot bounds, existing row offsets/lengths and row-length headers, row fit, and slot capacity.
- The builder constructs a full candidate in temporary `GxArray` storage and swaps it into the output only on success. It preserves the page bytes other than the appended row, new slot entry, slot count, and row-area end.
- Stages the completed bytes with `Transaction::writePagePayload`. That path calls the existing transaction overlay owner/index staging routine. A failed owner or lookup allocation returns a `DbResult`; the previous overlay entry remains intact.
- Sets the locator to the prior slot count only after staging succeeds. A preparation or staging failure leaves the committed/staged tail bytes and old locators unchanged.

### New-page append

- Uses the same `gxHeapPagePrepare` encoder as R2, with a one-row source and the transaction page allocator. It does not allocate a placeholder page, materialize it with `readPage`, or assign a new hosted payload vector.
- `Transaction::allocatePagePayload` stages the complete image at `_nextPageId` first. It advances `_nextPageId` and returns the id only after the transaction page owner accepts the page. A failed candidate or stage does not reserve a visible page id.
- If the prior tail exists, its next-page field is changed only in the local read copy; the updated tail is then checked through `Transaction::writePage`. If that link stage fails after new-page staging, R3I statement rollback removes the staged page and restores `_nextPageId` and prior tail overlay.
- Updates `newPagesOut`, `firstNewPageOut`, `_lastHeapPageId`, and the new-page locator only after the new image and required tail link have staged successfully.
- `updateStats` changes table row count, first-page id, and heap page count later in the same transaction catalog, after heap and index operations succeed. The INSERT savepoint restores catalog and page-owner state together.

### Allocator and ownership contract

`Transaction` retains its copied `_pageAllocator` (callbacks plus context) from transaction initialization. `pageAllocator()` exposes that exact domain to the append builder. Candidate `GxArray` buffers are destroyed through the same allocator. Completed payload bytes are copied by P7 transaction page ownership through `gxTransactionPageOverlayStage`; P8 lookup growth remains within that established staging routine. No cache allocator or caller-owned `GxOwnedPage` allocator is substituted.

`writePagePayload` stages a complete existing-page replacement without making another `DatabasePage`. `allocatePagePayload` stages a complete new page and delays transaction page-count advancement until success. Both preserve the existing transaction overlay and lookup implementation.

### Hosted allocation boundary

The append path still reads the current tail into a `DatabasePage`, whose `payload` is `std::vector<uint8_t>`. This is the established compatibility read boundary; its status-returning read reports allocation failure. The new append candidate and final transaction-owned image no longer depend on hosted payload-vector assignment or on a placeholder-page read. `ensureLastHeapPage` still grows a hosted `visited` vector while checking the chain; R3J now catches `std::bad_alloc`, returns `NoSpace`, and leaves `_lastResolved` false so a retry re-walks the chain. This catch remains in hosted-bound `database_heap.cpp`, which is already blocked from freestanding compilation by hosted standard-library headers.

## Failure and SQL9 correctness evidence

The focused heap construction target now reports **697 checks, 0 failures**. It covers the R2 construction checks, accepted R3I savepoint/index checks, and R3J cases below.

| Fault or boundary | Observed result |
|---|---|
| Portable candidate at 512 and 4,096 bytes | Exact-fit append bytes match an encoding oracle; oversized append returns `ResourceLimit`; output remains byte-for-byte unchanged. |
| Corrupt heap header, row header, or prior slot | Rejected before publication; output preserved. |
| Candidate allocator failure and limited allocator | Correct failure status; old output and allocator accounting preserved; retry succeeds. |
| Hosted tail-chain `visited` allocation | Deterministic size-8 hosted allocation fault occurs once; `NoSpace` is returned, active transaction has no modified pages, and same `Table` object retries successfully. |
| Checked read of an existing tail | Deterministic 4,064-byte hosted payload allocation fault occurs during the 4,096-byte-page tail read; the checked `NoSpace` result is returned before staging, then the same `Table` object retries successfully. Failed chain walks and reads leave the tail cache unresolved. |
| Existing-tail candidate allocation | Fault targets the transaction allocator's payload-sized candidate in the production INSERT. Prior UPDATE, exact staged tail payload, row count, and primary locator remain intact. |
| New-page candidate allocation | Fault targets the transaction allocator's payload-sized candidate after a successful UPDATE. No page, row, or index entry is published; earlier UPDATE remains readable. |
| New-page transaction owner staging | Fault targets the page-sized transaction owner allocation; page count, overlay count, allocator accounting, and row/index visibility remain unchanged. |
| Tail-link staging after new-page stage | New page stages successfully, then tail replacement is faulted. Statement rollback restores modified-page count, transaction page count, allocator bytes, row count, tail metadata, and index state; retry succeeds. |
| Explicit transaction rollback | After a successful UPDATE and a failed INSERT, reads see the UPDATE until ROLLBACK; rollback restores the committed prior value and no failed row/index entry remains. |
| Primary/secondary index and reopen | Committed multi-page rows reopen with exact expected row count; primary and secondary indexed lookup return the same page/slot locator; integrity and index checks pass. |

The SQL workload uses variable-width text rows and successfully fills a page to its calculated capacity (6 rows on 512-byte pages and 58 rows on 4,096-byte pages for the tested 61-byte encoded row plus slot), then crosses to a new page. Portable construction checks also fill pages with minimum-length rows to the largest valid row/slot count (28 at 512 bytes and 252 at 4,096 bytes) and reject the next row. It checks empty/one-row insertion, existing-tail append, full-tail append, retry, explicit rollback, commit, close/reopen, uniqueness/index lookup, and integrity. Existing committed row locators are compared across a failed append.

R3I's foreign-key, INSERT savepoint, partial snapshot, index failure, UPDATE/DELETE mutation, and prior-statement regressions remain in the focused target and full suite. P6 durable-WAL/cache-publication behavior, R3H owned-page reads, and cache/recovery tests are included in the final full CTest run. R3J changes no WAL code, page header, CRC, slot format, row encoding, B+ tree encoding, catalog encoding, or transaction framing. The heap builder byte oracle compares the old SQL9 heap encoding, and committed pages are read after close/reopen.

## Full hosted suite and freestanding census

- Focused R3J/heap-construction target: **697 checks, 0 failures**.
- Full hosted CTest: **21/21 targets, 150,988 checks, 0 failures** (the final full run completed after the last production/test edit).
- Freestanding qualification used `tools/qualify_sql9_freestanding.ps1` with the read-only Server AMD64 kernel flags, `-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti`. Result: **34 production translation units; 16 pass, 18 blocked**, unchanged from the accepted baseline.
- `database/portable/gx_heap_page_builder.cpp` compiles freestanding. Its undefined references are only the existing `gxArrayInit`, `gxArraySwap`, `gxArrayResize`, and `gxArrayDestroy` API functions; it adds no hosted STL, exception, RTTI, global `new`, or runtime ABI dependency.
- The same 18 existing units remain blocked by unavailable C++ standard-library headers under `-nostdinc++` (`cstdio`, `memory`, `string`, `vector`, `set`, `map`, etc.). No formerly blocked unit became qualified and no new translation unit was added. `database/database_heap.cpp` remains one of those hosted-header blockers; this is not full native SQL9 linkage or execution.
- Qualification artifacts: `C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3J-final2-20261022`.

## Remaining writer/copy boundaries and next continuation

| Boundary | Current representation and failure domain | Status and rollback invariant | Minimum safe next step |
|---|---|---|---|
| `Database::readPage` / `DatabasePage::payload` | Hosted vector materialization after checked owned-page read; hosted allocation maps to `NoSpace`. | R3H checked-read and recovery rules remain. This compatibility layer is not transaction allocator-controlled. | Keep as a separate migration; define destination ownership before removing the facade. |
| Legacy `PageAccess::writePage` and database compatibility writes | Accept hosted `DatabasePage`; transaction writes now forward its bytes to checked page-owner staging. Other callers can still construct/mutate hosted payloads first. | Result propagation comes from the caller; callers with SQL savepoints restore staged state. | Migrate one bounded writer at a time to complete-payload staging after auditing local mutation and savepoint behavior. |
| `Table::applyMutationsInTransaction` / `packHeapPage` | R2 `gxHeapPagePrepare` scratch path and hosted compatibility representation remain for compacted/rebuilt pages; mutation planning and locator maps use hosted containers. | SQL UPDATE/DELETE statement savepoints cover transaction publication; exception-to-status handling remains hosted. | Next recommend migrating this page-rebuild/staging boundary to the transaction allocator and direct payload staging, with UPDATE/DELETE relocation tests. |
| `IndexTree::writeLeaf` and `writeInternal` | Node construction uses hosted vectors; R3I checks/catches selected leaf payload allocation and stages through transaction `writePage`. | Leaf failure status is covered by INSERT savepoint; broader internal-node allocations remain hosted. | Continue with checked internal-node candidate and transaction staging, preserving split/root publication ordering. |
| Transaction page staging and savepoints | P7 page owner and P8 fixed lookup use transaction allocator; savepoint clone uses same allocator; `Catalog` snapshot remains hosted. | Checked status plus allocation-free restore; commit/cache publication retains R3H/P6 durable-WAL behavior. | Preserve current ownership and durability boundary; only change with targeted fault injection. |
| Parser and SQL statement preparation | Hosted strings/vectors before mutation/savepoint; not part of the portable page owner. | INSERT constructs/coerces values before its savepoint; selected statement failures restore the saved overlay/catalog. | Keep separate from page-owner migration; qualify preparation failure independently if portability scope expands. |
| P7/P8 test-only copies | Hosted copies in tests for assertions and fault injection. | Test-only; no production publication path. | No production change needed. |

The next bounded P11 continuation should target `Table::applyMutationsInTransaction` page rebuild and staging in the UPDATE/DELETE relocation path: use the active transaction allocator for the complete candidate, stage through the payload API, and test index-locator updates and statement rollback. Do not begin that continuation as part of R3J.

## Git closeout

- R3J source, tests, and this report are committed locally on the pre-existing `main` branch. No push or topology-changing operation was performed.
- The report records the R3J implementation commit SHA after the initial local commit; the final documentation-only closeout commit remains the ending SQL HEAD.
- Ending phase markers remain SQL9 → SQL10 and NDB1R4P10 → NDB1R4P11.
- Server remains unchanged at `f29850c93bbac0a0763a31e806d2816310156575`.
