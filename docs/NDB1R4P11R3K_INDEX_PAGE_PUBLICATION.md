# NDB1R4P11R3K: Checked Existing-Leaf Publication

## Result

**Outcome A — `CHECKED_INDEX_PAGE_PUBLICATION_VERIFIED`** for the fitting,
existing B+ tree leaf path in `IndexTree::insert`. A SQL9 INSERT now prepares
the complete leaf payload with the active transaction's allocator, validates
the candidate, and publishes it through P7/P8 transaction-owned staging. The
existing INSERT statement savepoint restores the heap, earlier index work,
and catalog state when preparation or staging fails. Parent NDB1R4P11 remains
open. This does not qualify the complete B+ tree, full-leaf splits,
`DatabasePage::payload`, or P12.

## 1. Repository gates and accepted R3J baseline

- Writable checkout: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `96f32beca2db2650744c946a9f11ace3e8a3e16f` (expected abbreviation
  `96f32be` resolved by `git rev-parse HEAD`). The branch was two local commits
  ahead of `origin/main`, as expected for the accepted R3J implementation and
  report. The worktree was clean before R3K edits.
- No R3K report or R3K implementation was present at the starting HEAD. No
  concurrent worktree change was present.
- `.phase` remains `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase` remains `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- R3J implementation commit: `b2674075c3843db559fec6c95fee8e171e3a0d8b`.
  Its accepted report is
  `docs/NDB1R4P11R3J_HEAP_APPEND_PUBLICATION.md`.
- R3J acceptance was verified: `Table::appendEncodedRowToTail` constructs
  existing-tail and new-page heap payloads with the active transaction's
  allocator, stages complete payloads through checked P7/P8 ownership, and
  publishes locators and tail metadata only after staging succeeds. R3I
  statement rollback restores the overlay, page count, catalog, and earlier
  transaction work. R3J recorded 697 focused checks and 21/21 CTest targets,
  150,988 checks, zero failures. Its freestanding census was 34 production
  units, 16 passing and 18 blocked.
- R3I savepoint/error propagation, R2 heap construction, P7/P8 ownership,
  R3A–R3H readers/cache publication, and the R3F-R1 WAL recovery requirements
  were reviewed. The R3K changes do not modify those foundations.
- Read-only Server reference: `D:\dev\guideXOSServer_NATIVE_DB_STORAGE`,
  branch `NATIVE_DB_STORAGE`, HEAD
  `f29850c93bbac0a0763a31e806d2816310156575`. It remained clean and was used
  only for AMD64 compiler flags. No Server file, artifact, phase marker,
  configuration, or Git state was changed.
- SQL stayed on `main`. No branch, remote, upstream, worktree, or history
  topology operation and no push was performed.
- At final read-only review, `origin/main` pointed at the same R3J HEAD as
  local `main`. Its reflog records an `update by push` at 2026-10-23
  15:43 -0700, after the starting observation where `main` was two commits
  ahead. This external tracking-ref movement contains no R3K work and creates
  no divergence. This task issued no fetch or push and did not alter the
  tracking ref.

## 2. Actual SQL9 INSERT call graph

The source path is:

```text
SqlEngine::executeStatement (database/database_sql.cpp:2655)
  -> SqlEngine::executeInsert (database/database_sql.cpp:2871)
     -> construct/coerce INSERT values
     -> Transaction::beginStatement for an explicit transaction
     -> Table::insert (database/database_heap.cpp:1000)
        -> Table::insertInTransaction (database/database_heap.cpp:1037)
           -> validate values and encode the heap row
           -> tableIndexTargets and encode each index key
           -> unique/primary-key lookups and child-FK check
           -> Table::appendEncodedRowToTail (database/database_heap.cpp:1131)
              -> R3J checked heap-page preparation and transaction staging
           -> for each primary/secondary/support index:
              -> IndexTree::insert (database/database_index.cpp:872)
                 -> make physical key and find/read the target leaf
                 -> decode existing entries, reject duplicates/uniqueness
                 -> add and order the candidate entry
                 -> stagePreparedLeaf for a fitting existing leaf (R3K)
                 -> existing split path when entries exceed leaf capacity
           -> update catalog roots/counts and table statistics
     -> release the statement savepoint on success
     -> Transaction::rollbackStatement and clear cached Table facades on error
  -> a later Transaction::commit writes WAL, reaches durable COMMIT, then
     publishes committed page images to cache
```

INSERT values and key vectors are prepared before heap mutation. The heap row
is staged before the index loop, and index targets are processed in catalog
order. Therefore a later leaf failure can follow heap staging and one or more
successful index mutations. `Table::insert` translates hosted
`std::bad_alloc` to `NoSpace`; `executeInsert` rolls back the explicit
statement savepoint and propagates rollback failure distinctly.

## 3. Selected boundary and allocation/ownership inventory

**Selected function:** the fitting, existing-leaf branch of
`IndexTree::insert`, which calls the new `IndexTree::stagePreparedLeaf`
(`database/database_index.cpp:475`). The SQL9 test exercises a primary leaf
and later secondary/support leaves. The production `IndexTree` is given the
matching active `Transaction`; the writer verifies that this is the same
`PageAccess` object.

`Table::insertInTransaction` supplies the active transaction for indexed
INSERTs. `maintainIndexes` supplies it for UPDATE relocation/index insertion,
and `Database::buildIndex` supplies it while building an index inside its
transaction. These callers share the same fitting-leaf builder; R3K's
deterministic allocation-failure acceptance uses SQL INSERT, while the other
callers are covered by their existing success/rollback regressions rather
than separate R3K fault cases.

| Operation | R3K behavior and boundary |
|---|---|
| Key encoding | The existing `encodeIndexKey` codec is unchanged. SQL key vectors are hosted and bounded by `maxIndexKeyBytes(pageSize)`; Table/SQL error handling remains the accepted R3I path. |
| Row locator | Search order uses `logicalKey || pageId || slot`, with the locator appended big-endian for bytewise physical ordering. The leaf payload persists page ID and slot little-endian, preserving SQL9's existing encoding. |
| Existing leaf read | `readLeaf` uses the active transaction page view and validates page type, capacity, signature, version, index owner, and entry count. `readLeafEntries` still decodes into hosted vectors; checked read failures propagate and hosted allocation exceptions are caught by `Table::insert`. |
| Hosted planning allocations | `IndexTree::insert` still has hosted `std::vector` key/entry/path work, including physical-key temporaries in its sort comparator. These remain inside the INSERT `bad_alloc` to `NoSpace` boundary and statement savepoint. Portable allocator injection does not target them. |
| Leaf candidate | `stagePreparedLeaf` calculates entry/slot bounds with capacity-limited arithmetic, allocates one exact payload-sized `GxArray` from `Transaction::pageAllocator()`, zeroes it, and writes the existing 24-byte header, forward entries, and reverse slot directory. The candidate builder introduces no `std::vector` payload copy. |
| Candidate checks | Before staging it verifies header/version/owner/next-leaf/count, contiguous entry offsets, slot and data extents, key lengths and bytes, locator bytes, page-number range, and strict physical-key order. This preserves duplicate-key tie ordering and prevents overlapping or partial entries. |
| Existing-page copy | The source `DatabasePage` is read without mutation. The replacement uses its flags and generation; the source remains alive through candidate validation and synchronous staging. No committed cache page is modified in place or pinned by this path. |
| Transaction staging | The complete payload goes to `Transaction::writePagePayload` (`database/database_transaction.cpp:153`), then `stagePagePayload` and `gxTransactionPageOverlayStage`. P7 owns the full encoded page image; P8 maintains page-number lookup. The transaction allocator context remains valid while the candidate and staged page are created. |
| Status handling | Candidate `OutOfMemory` returns `NoSpace`; bounded resource/overflow status returns `TransactionTooLarge`; invalid allocator state returns `Internal`. Staging status is returned unchanged. A fitting-leaf `NoSpace` returns directly and cannot be misread as a split request. |
| Metadata | `outInserted`, catalog entry counts, catalog root changes, and table statistics occur only after successful index insertion. The savepoint covers catalog state, transaction page images, and `_nextPageId`. |

The candidate is a temporary payload buffer, not a new page owner. P7 remains
the transaction page owner and P8 remains its lookup index. The staged image
is encoded by the existing transaction page codec, including the established
page header and CRC path.

## 4. Preparation, publication, and split dependencies

For the selected ordinary leaf replacement the sequence is:

```text
read and validate the existing transaction-view leaf
  -> decode entries and check exact/unique duplicates
  -> add and sort the new (key, locator)
  -> verify it fits the existing leaf
  -> allocate a complete transaction-allocator candidate
  -> encode and validate the candidate bytes
  -> call checked transaction payload staging
  -> report inserted and allow catalog/stat publication
```

The existing leaf's `nextLeaf` value is copied unchanged. The candidate keeps
the page ID, index ID, header version, flags, generation, entry count, key
ordering, and little-endian stored locator encoding. The old overlay image
remains owned until P7 successfully replaces it. If a P7/P8 staging allocation
fails, the prior image remains valid; the enclosing INSERT savepoint removes
the already staged heap row and any earlier index image from that statement.

The selected fitting-leaf branch changes no sibling link, parent separator,
root pointer, tree depth, or page count. Full leaves continue through the
existing code: find a split point, allocate a right leaf, stage the old leaf
with its new sibling link, stage the right leaf, compute a separator, then
update a parent or create/split internal nodes and possibly publish a new
root. `IndexTree::writeLeaf` remains the hosted compatibility writer for
empty-leaf creation and split images; `IndexTree::writeInternal` remains
hosted. Their existing `bad_alloc` status handling and transaction/savepoint
behavior are unchanged. The full SQL index suite requalified normal split,
deep-tree, parent/root, range, and duplicate-key behavior, but R3K does not
claim allocator-fault qualification or portable construction for the split or
parent path.

## 5. Savepoint and failure evidence

The test uses production `SqlEngine`, `Table`, `Transaction`, and B+ tree
operations on 512-byte pages. `ChildRows` has a primary key, a foreign key
support index, and a unique secondary text index. It starts with one committed
row so each injected selected leaf update targets an existing leaf. An earlier
successful `UPDATE PriorRows` remains staged in the explicit transaction.

- **Candidate allocation:** the transaction allocator is allowed one
  480-byte payload candidate for the R3J heap append, then fails the next
  480-byte allocation in the primary leaf candidate. The injector observes
  two matching attempts, one success, and one failure. Production returns
  `index leaf preparation allocation failed`; `outInserted` is not published,
  the heap and index state return to the statement baseline, page count and
  outstanding allocator bytes are restored, and the original row remains
  queryable through both indexes.
- **Transaction owner staging:** candidate preparation and validation
  complete, then the allocator fails a 512-byte transaction page-image
  allocation after the statement snapshot and heap stage. Exact attempt,
  success, and failure counts prove the owner allocation was reached. INSERT
  returns `transaction page staging failed`; the prior leaf remains valid and
  savepoint rollback restores page count, overlay, and allocator bytes.
- **Failure after earlier index work:** the injector allows the heap candidate
  and primary-leaf candidate, then fails the next 480-byte leaf candidate.
  The transaction page statistics advance by two image operations before
  rollback (heap image plus the successfully staged primary leaf). The later
  index candidate returns `index leaf preparation allocation failed`. The
  savepoint restores all `ChildRows` indexes to one existing entry, no failed
  row or locator remains, and the earlier `PriorRows` UPDATE is still visible
  via `IndexLookup`.
- **Checked page read:** a scoped hosted allocation fault during the existing
  index-page read returns `hosted compatibility page payload allocation
  failed`. It is a checked R3H page-view failure and is rolled back by the
  same savepoint; the prior UPDATE and original row remain intact.
- **Savepoint clone failure:** the retained R3I partial-clone fault fails the
  third transaction allocation after two successful snapshot allocations.
  `Table::insert` is not reached, the failed row is absent, and the prior
  statement remains visible.
- **Retry and constraints:** after each mutation failure the explicit
  transaction remains usable. An invalid foreign key is rejected separately;
  duplicate primary and unique-secondary values return the existing unique
  constraint error. A valid prepared INSERT retry succeeds, both indexes
  resolve to the same heap `RowLocator`, every child index validates, and
  `validateIntegrity` succeeds.
- **Explicit rollback:** a later successful indexed INSERT is visible through
  the secondary index inside an explicit transaction. `ROLLBACK` removes its
  heap row and index entry while preserving the earlier committed rows.
- **Commit/reopen and ownership cleanup:** the successful retry commits, then
  close/reopen finds the expected rows through primary and secondary
  `IndexLookup`. Index and FK validation pass and cache/transaction allocator
  outstanding counts return to zero.

The focused page-construction target includes the R2/R3I/R3J coverage and now
reports **754 checks, zero failures**. It exercises the R3J SQL index append
workloads at both 512-byte and 4,096-byte page sizes; R3K deterministic
allocation and staging faults use 512-byte pages. The focused byte oracle
compares complete primary and unique-secondary leaf payloads after the retry
against the unchanged SQL9 byte layout, including the slot directory and
persisted row locators.

## 6. WAL, cache, and persistent compatibility

R3K changes only `database/database_index.cpp`, its constructor wiring, and
the focused production SQL test. It changes no `.gxdb`/`.gxwal` format, page
header, page type, page size, CRC routine, heap slot format, B+ tree encoding,
catalog encoding, WAL PAGE_IMAGE framing, transaction framing, or durability
point. Existing page encoding still supplies the full image and CRC when P7
stages the payload.

The full suite retained R3F-R1's valid committed WAL header history
`4 -> 8 -> 23 -> 48`, rejection of the invalid committed regression `4 -> 3`,
recovery when the logical page count exceeds physical file extent, and P6's
post-durable-COMMIT cache publication failure semantics. The cache publication
target passed **4,901 checks**, including WAL replay after a durable commit
whose cache publication fails. A pre-COMMIT INSERT/index failure remains an
ordinary statement-savepoint rollback and is distinct from that recovery
required state. No cache pins are involved in the selected writer; the source
is an ordinary transaction-view `DatabasePage` and the committed cache is not
mutated before COMMIT.

## 7. Full hosted regression

The complete serial CTest run passed **21/21 targets, 151,045 checks, zero
failures** in 1,548.23 seconds. The accepted R3J baseline was 150,988 checks;
the focused target increased from 697 to 754 checks. The full run retained
SQL1–SQL9, prepared statements, B+ tree split and constraint behavior,
foreign keys, UPDATE/DELETE relocation, R3I savepoint failures, R3J heap
append, P6 cache/WAL recovery, R3F-R1 uncheckpointed recovery, R3H checked
database reads, P7 ownership, and P8 lookup tests.

The full build succeeded. It emitted one existing unused-function warning in
`tests/database_index_test.cpp` for `findIndexByName`; no new production
warning was reported. `git diff --check` passed.

## 8. Freestanding census and ABI audit

Ran `tools/qualify_sql9_freestanding.ps1` using Windows PowerShell
**5.1.26100.9711**, MinGW-w64 `g++` **15.2.0**, GNU `nm`
**2.46.0.20260210**, and the read-only Server AMD64 kernel flags including
`-ffreestanding`, `-fno-exceptions`, `-fno-rtti`, `-nostdlib`, and
`-nostdinc++`. Result: **34 production translation units; 16 pass, 18
blocked**, unchanged from R3J. No production unit was added and no previously
blocked unit newly passes.

The changed `database/database_index.cpp` remains blocked at the existing
hosted `<set>` dependency in `database_index.h`; its freestanding command
fails before compiling the new writer. Other unchanged diagnostics include
`<memory>` in `database_heap.cpp` / `database_transaction.cpp`, `<vector>` in
`database_page.cpp` / `database_sql_parser.cpp`, and `<map>` in
`database_sql.cpp`. The 16 passing objects retain the established allocator,
array, page codec, transaction-page, checksum guard, and `malloc`/`free`
references. The `nm -u -C` scan found no `__cxa_throw`, exception-personality,
RTTI, global `operator new`, or global `operator delete` imports in those
passing objects. This is object compilation and symbol inspection only; no
native SQL link or guest execution is claimed.

PowerShell 5.1 artifacts:
`%TEMP%\guideXOSSql-NDB1R4P11R3K-PS51-20261023`.

## 9. Remaining hosted boundaries and payload readiness

| Boundary | Remaining allocation/owner | Failure and transaction behavior | Smallest safe next step |
|---|---|---|---|
| `DatabasePage::payload`, `DatabasePage::parse`, `Database::readPage` | Hosted `std::vector<uint8_t>` page view, independent of the transaction allocator. | R3H catches compatibility materialization failure and returns `NoSpace`; read/cache recovery rules remain. | Keep the facade migration separate and establish destination ownership before replacing it. |
| `IndexTree::readLeafEntries`, key/entry sorting, and SQL key construction | Hosted vectors and temporary physical-key copies remain before the selected candidate. | SQL `Table::insert` catches `std::bad_alloc`; the statement savepoint restores any earlier page/catalog mutation. | Convert only after selecting a portable entry representation and bounding the key work. |
| `IndexTree::writeLeaf` and `writeInternal` | Hosted payload vectors remain for new empty leaves, split leaves, sibling-link images, and internal/parent/root images. | `writeLeaf`/`writeInternal` retain local `bad_alloc -> NoSpace`; outer SQL savepoints roll back multi-page INSERT/DDL changes. | Qualify the complete split/parent publication sequence as its own bounded continuation. |
| `Table::applyMutationsInTransaction` / `packHeapPage` | UPDATE/DELETE repacking still materializes hosted `DatabasePage` payloads and uses hosted mutation/locator vectors; R2 builder support exists. | Existing UPDATE/DELETE savepoints restore transaction pages and catalog; R3J append path is reused for relocated rows. | Use the active transaction allocator for the complete repacked heap candidate, stage via payload API, and fault-test locator/index remapping plus rollback. |
| Legacy `PageAccess::writePage` callers | `DatabasePage` compatibility values can still be built or mutated before staging. | Transaction write forwards bytes into checked P7/P8 owner staging; failures propagate to statement/DDL owners. | Migrate one producer at a time after auditing local mutation and savepoint scope. |
| Transaction savepoint catalog snapshot | Page owner/index clone use the transaction allocator; `Catalog` snapshot remains hosted. | Catalog copy is caught before mutation; rollback uses a non-allocating swap and restores `_nextPageId`. | Preserve the accepted savepoint owner; no conversion is needed for R3K. |
| Parser and SQL value preparation | Hosted strings/vectors are built before the INSERT savepoint and before the first database mutation. | Failure occurs before heap/index publication. | Keep outside the page-owner migration until a separate parser scope is selected. |
| P7/P8 test-only copies | Hosted copies remain in assertions and fault harnesses only. | No production publication dependency. | No production conversion required. |

The engine is **not ready for final `DatabasePage::payload` conversion**. The
next bounded P11 continuation should be exactly the UPDATE/DELETE heap-page
rebuild and staging boundary in `Table::applyMutationsInTransaction` and
`packHeapPage`, using the active transaction allocator and checked payload
staging while testing index locator updates, statement rollback, and earlier
transaction work. That continuation has not been started.

## 10. Acceptance

**Yes.** SQL9 now constructs and stages the selected real existing B+ tree
leaf mutation through allocator-correct, failure-aware transaction ownership.
Production INSERT faults at candidate allocation, page-owner staging, and a
later index operation restore the statement savepoint, preserve an earlier
successful UPDATE, and leave heap rows, primary/secondary/FK indexes, key
ordering, and row locators consistent. Retry, uniqueness/FK rejection,
explicit transaction rollback, commit/reopen, full hosted regression, and
WAL/cache recovery pass. Persistent encodings remain compatible. Qualification
is limited to fitting existing-leaf publication; splits and other hosted
writers remain separate boundaries.
