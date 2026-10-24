# NDB1R4P11R3L: Checked Non-Cascading Index Leaf Split Publication

## Result

**Outcome A — `CHECKED_INDEX_LEAF_SPLIT_VERIFIED` for the selected
non-cascading leaf split path.** SQL9 INSERT now prepares both leaf payloads
and the fitting parent payload with the active transaction allocator, validates
them, and stages the complete page set through the P7/P8 transaction owner.
The existing INSERT savepoint restores the heap row, index pages, page count,
catalog snapshot, and earlier transaction overlay if a later stage fails.
Parent P11 remains open. This result does not qualify recursive parent/root
publication, every B+ tree split variant, or `DatabasePage::payload`.

## 1. Repository gates and accepted R3K baseline

- Writable checkout: `D:\dev\guideXOS_SqlDatabase`.
- Starting branch and HEAD: `main`,
  `3e586c70cbcb81018faa60a8caf8a496a0ea63da`. The worktree was clean before
  edits, and no R3L source or report existed.
- `.phase`: `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase`: `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- Accepted R3K baseline: full hosted CTest 21/21, 151,045 checks, zero
  failures; focused R3K target 754 checks; freestanding census 34 production
  units, 16 passing and 18 blocked.
- R3K source review confirmed that fitting existing-leaf replacements use
  `Transaction::pageAllocator()`, validate a candidate before staging, retain
  P7/P8 ownership and lookup, and use the R3I statement savepoint for INSERT
  failures. Page encoding was unchanged.
- R3J heap append and R3I savepoint reports and implementation were reviewed.
  The SQL index split implementation was traced in `database_index.cpp` before
  changes. No R3K code was duplicated.
- Read-only Server reference: `D:\dev\guideXOSServer_NATIVE_DB_STORAGE`,
  branch `NATIVE_DB_STORAGE`, HEAD
  `f29850c93bbac0a0763a31e806d2816310156575`. No Server source, build output,
  phase marker, or Git state was changed.
- SQL remains on the existing `main` branch. No fetch, branch switch, push,
  history rewrite, remote change, or Git topology operation was performed.

## 2. SQL9 INSERT and split call graph

The production path is:

```text
SqlEngine::executeInsert                         database/database_sql.cpp:2871
  -> Transaction::beginStatement                database/database_transaction.cpp:350
  -> Table::insert                               database/database_heap.cpp:1000
     -> Table::insertInTransaction               database/database_heap.cpp:1037
        -> validate row, encode index keys, check uniqueness and child FKs
        -> Table::appendEncodedRowToTail          database/database_heap.cpp:1131
           -> prepare and stage the heap row through P7/P8
        -> for each primary/secondary/support index:
           -> IndexTree::insert                   database/database_index.cpp:1193
              -> make physical key (logical key + page id + slot)
              -> findLeaf and collect parent path database/database_index.cpp:1050
              -> readLeaf / readLeafEntries       database/database_index.cpp:389, 424
              -> reject duplicate/unique conflicts
              -> if the leaf fits: stagePreparedLeaf (R3K)
              -> if full: choose left/right split point
                 -> stageNonCascadingLeafSplit     database/database_index.cpp:908
                    -> prepare left leaf, right leaf, and parent
                    -> allocatePagePayload for the new right leaf
                    -> writePagePayload for old leaf and parent
           -> update catalog/index statistics only after successful insertion
  -> releaseStatement on success
  -> rollbackStatement and clear cached Table facades on failure
     (database/database_transaction.cpp:375)
```

`IndexTree::findLeaf` uses the active transaction page view and returns the
parent path. `readLeaf` and `readInternalChildren` validate existing pages;
`readLeafEntries` decodes entries into hosted vectors. The split algorithm
tries candidate boundaries in key order and selects the first point where both
leaf halves fit. For a nonempty path, the checked path reads the direct parent,
inserts a separator after the old child, and handles the split only when the
complete parent candidate fits. Root creation and recursive internal splits
continue through the existing path.

`SqlEngine::executeInsert` begins its savepoint before `Table::insert`. The heap
row is staged before the index loop, and indexes are processed in catalog
order. Consequently a split failure can follow a heap append and prior index
stages. `Table::insert` maps hosted `std::bad_alloc` to `NoSpace`; the SQL
savepoint then restores page ownership, lookup, page count, catalog state, and
cached table facades.

## 3. Split variant inventory

| Variant | R3L treatment |
|---|---|
| Existing leaf replacement fits | Preserved from R3K; focused and full regressions pass. |
| Leaf split, direct parent has space | Converted to complete checked leaf/parent preparation and P7/P8 staging. Exercised through SQL INSERT. |
| Leaf split, parent must split | Audited; remains on hosted `writeInternal` recursion and is deferred. Existing deep-tree and split tests remain in the full suite. |
| Leaf split changes the root | Audited; legacy `splitRootLeaf`/`writeInternal` path remains. A maximum-key root split is now covered for read/search compatibility, not for checked publication. |
| Sibling update | The format is forward-only (`nextLeaf`). The old leaf points to the new right page; the new page points to the previous successor. There is no previous-link or separate neighbor page to update. |
| Duplicate/unique rejection | Existing primary/unique-key checks remain before heap mutation; duplicate SQL attempts are explicitly checked. Foreign-key regressions remain in the full suite. |

The bounded handler is selected only for an active transaction, a nonempty
parent path, and a parent with room for the inserted child. A parent that does
not fit returns `outHandled=false` before any structural stage, allowing the
unchanged recursive path to proceed. Root and cascading paths were not claimed
as portable.

## 4. Selected mutation and fallible-operation inventory

The selected mutation is one leaf split with a direct parent that has spare
capacity. Its page group is the existing left leaf, a newly allocated right
leaf, and the direct parent. There is no neighboring-sibling page or root
metadata update for this case.

| Function / page | Source and destination | Allocator and possible failure | First visible mutation / owner / rollback |
|---|---|---|---|
| `readLeaf`, `readLeafEntries` / leaf | Transaction-view `DatabasePage::payload` to hosted `IndexEntry` vectors | Page-view and vector materialization remain hosted; read statuses propagate and `Table::insert` catches `std::bad_alloc`. | No page mutation. Source remains live through preparation. |
| `IndexTree::insert` planning / leaf entries | Hosted entry vector plus hosted `left` and `right` vectors, sorted by physical key | Hosted allocation remains; `std::bad_alloc` maps to `NoSpace` before split staging. | No split page is visible yet; SQL savepoint also covers earlier heap/index work. |
| `prepareLeafPayload` / left and right leaves | Existing SQL9 leaf layout into an exact-capacity `GxArray` | `Transaction::pageAllocator()`. `OutOfMemory` → `NoSpace`; resource/overflow → `TransactionTooLarge`; candidate validation errors → `CorruptPage`. | Temporary candidate only. No transaction page changes. |
| `readInternalChildren` and child insertion / parent | Existing parent page to hosted `Child` vector with the new physical separator | Hosted vector growth can throw before staging; SQL INSERT catches it. | No transaction page changes. Candidate children are checked for ordered bounds, unique child IDs, valid page IDs, and capacity. |
| `prepareInternalPayload` / parent | Existing internal-node format into an exact-capacity `GxArray` | `Transaction::pageAllocator()` with checked status mapping. | Temporary candidate only. No transaction page changes. |
| `Transaction::allocatePagePayload` / new right leaf | Complete right payload to P7 transaction page owner and P8 lookup | P7/P8 transaction owner uses the active transaction allocator; status propagates unchanged. | First structural stage. `_nextPageId` advances only after staging succeeds. |
| `Transaction::writePagePayload` / existing left leaf, then parent | Complete candidates to the existing transaction overlay | P7/P8 staging may fail at either call; each result is checked. | Stages after the new page can be partial. `executeInsert`'s already-active statement savepoint restores the full prior overlay and page count. |
| Catalog/root metadata | None for this direct-parent case; root ID and height stay the same | No extra fallible metadata write in the selected helper. SQL catalog/stat publication follows successful index insertion. | Catalog and `_nextPageId` are covered by the same savepoint. |

The candidate `GxArray`s are temporary buffers, not page owners. P7 remains
the transaction owner of each final encoded page image and P8 remains its page
lookup. A caller-owned `GxOwnedPage` is not used as a substitute. The active
transaction allocator is used for candidate arrays and P7/P8 staging.

## 5. Leaf images and parent separator

`prepareLeafPayload` now factors the R3K builder so it can prepare without
staging. It uses checked 64-bit used-byte arithmetic, validates entry and slot
counts against the payload capacity, checks logical key bounds and the
candidate page-number range, writes the unchanged SQL9 leaf header/entry/slot
layout, and validates the finished image before returning it.

For a selected split:

- The left image replaces the original leaf page ID and preserves that page's
  flags and generation.
- The right image is allocated as `PageType::IndexLeaf` with flags and
  generation zero, matching existing `Transaction::allocatePage` behavior.
- The left `nextLeaf` becomes the planned new page ID.
- The new right `nextLeaf` is the original leaf's successor.
- Both leaf entry arrays retain strict physical-key ordering and the original
  row locators. Record count, logical key bytes, slot directory, and page type
  are validated.
- The parent keeps its page ID, flags, generation, index owner, and header
  format. The new child lower bound is `makePhysicalKey(right[0].key,
  right[0].locator)`, matching the exact first physical key in the new leaf.
- Parent children are checked for ordered lower bounds, duplicate child IDs,
  page-number range, child count, offset arithmetic, and full serialized
  candidate consistency.

The internal separator stores a physical key: encoded logical key plus the
12-byte page/slot locator. During the boundary audit, a maximum valid logical
key exposed that `readInternalChildren` incorrectly limited this physical
separator to the logical-key limit. R3L corrected the bound to
`maxIndexKeyBytes(pageSize) + 12` with checked addition, and uses the same
physical-key bound in checked parent candidate validation. This does not change
the encoding or page format. The exact-fit regression now uses the maximum
512-byte-page logical key and verifies a 102-byte physical separator through
root read, lookup, parent update, traversal, close, and reopen.

## 6. Page number, sibling, and staging order

The helper plans the new leaf ID as the active transaction's `pageCount()` and
the candidate page count as that ID plus one. It validates both new sibling
references and the parent child reference against that planned count. It then
prepares all three payload candidates before staging anything.

The checked staging order is:

```text
1. allocatePagePayload(IndexLeaf, right image)  -> new right leaf and page count
2. writePagePayload(old leaf, left image)       -> left.nextLeaf = new leaf
3. writePagePayload(parent, parent image)       -> separator and child link
4. report outInserted=true to IndexTree::insert
```

Each stage result is checked. These calls are **not** individually atomic as a
group. The group is operation-atomic because `executeInsert` established the
R3I statement snapshot first, and any subsequent error restores the saved P7
owner, P8 lookup, `_nextPageId`, catalog, and transaction-visible page view.
No root ID, tree height, index page count, or catalog index root changes in
this non-cascading case. New page ID assignment is delayed until P7 accepts the
new image; a later failure restores the transaction page count through the
savepoint.

## 7. Failure injection and rollback evidence

The SQL test uses real `SqlEngine`, `Table`, `Transaction`, and `IndexTree`
objects. On 512-byte pages, the transaction allocator injects failures at
480-byte payload candidates; the transaction owner staging fault targets
512-byte page images. Before each failure, the test performs a successful
`UPDATE PriorRows`, captures page count, modification count, allocator
ownership, page images, image-allocation count, and the statement savepoint.

| Fault boundary | Reached production work | Expected failure and evidence |
|---|---|---|
| First candidate (left leaf) | Heap append candidate succeeds; checked left leaf candidate is the next matching 480-byte allocation. | One matching success then one failure; `NoSpace` / `index leaf preparation allocation failed`. No split page staged. Heap/index state, page count, and outstanding transaction allocation return to the statement baseline. |
| Second candidate (right leaf) | Heap append and left candidate succeed; right leaf candidate is the next matching allocation. | Two matching successes then one failure; same leaf preparation status. Parent and structural pages remain unchanged. |
| Third candidate (parent) | Heap append and both leaf candidates succeed; parent candidate is the next matching allocation. | Three matching successes then one failure; `NoSpace` / `index internal preparation allocation failed`. No structural page has staged. |
| Partial P7/P8 staging | The injector allows the statement snapshot and existing page-image allocations, then fails the old-leaf stage after the new right leaf has staged successfully. | `transaction page staging failed`; image-allocation counter records two successful split page images before rollback. Savepoint restores modified-page count, final page count, allocator ownership, original parent and right leaf images, index validation, absent heap row, and absent primary-index result. |
| Malformed parent separator | In an explicit transaction, a separator is changed so the target still routes to the right leaf but the candidate bounds are no longer strictly increasing. | Checked parent preparation returns `CorruptPage` with `index internal candidate bounds are not increasing` before structural staging. Statement rollback retains the preexisting transaction corruption and earlier UPDATE; explicit transaction rollback removes the corruption. |
| Duplicate primary key | Insert an existing primary key after split preparation/retry work. | Existing unique-index error is returned; it is not reported as an allocator failure and no extra row appears. |

For the three candidate failures, the test asserts exactly `allowed + 1`
matching attempts, exactly `allowed` successful allocations, and one injected
failure. For partial staging it asserts the configured successful image
allocations and one failure at the targeted page-image size. Each failed
statement leaves the earlier `PriorRows` UPDATE visible. The test retries the
split successfully; the 512-byte case commits that retry in the same explicit
transaction as the earlier UPDATE. A separate full transaction rollback is
also exercised after successful retries in the preparation-failure cases.

## 8. SQL visibility, locators, and constraints

The 512-byte and 4,096-byte split tests use a primary INT32 index and a
secondary BOOLEAN index. They validate primary and secondary lookup paths,
verify that secondary locators match primary locators, traverse the primary
index in order, compare expected leaf page images, inspect parent separators
against each leaf's first physical key, and run `Database::validateIntegrity`.
The 512-byte variable-width case uses a TEXT primary key, begins with a
small-key leaf and a right leaf filled exactly to payload capacity, then
inserts a maximum-length valid key that causes the checked split. It verifies
the ordered range scan and every SQL primary-index lookup before and after
reopen.

Primary-key duplicate rejection is explicit in the split fixture. Existing
unique-secondary, child-foreign-key, UPDATE/DELETE relocation, and index
constraint regressions remain in the full suite. The selected test table has
no foreign key of its own; the existing SQL FK coverage still runs unchanged.

The 512-byte workload performs one checked non-cascading split. The 4,096-byte
workload performs two successive checked splits under the same parent. It
checks sequential new page IDs, all parent children, separators, leaf links,
range/lookup results, and the final leaf image group.

## 9. Persistent format, COMMIT, cache, and WAL

R3L changes no `.gxdb` or `.gxwal` schema, page header, page ID/type encoding,
CRC routine, leaf/internal layout, key or locator encoding, catalog encoding,
WAL PAGE_IMAGE framing, or COMMIT framing. Candidate payloads use the
existing SQL9 representation; the P7 transaction page codec remains
responsible for the complete page image and CRC. `checkSplitIndexShape`
compares persisted primary leaf payloads against the existing accepted leaf
serializer, including the sibling pointer and slot directory.

The successful 4,096-byte case commits normally and validates the closed and
reopened database. In the 512-byte case, the successful retry and earlier
UPDATE reach durable COMMIT; the established P6 cache-allocation fault is then
injected during committed-page cache publication. The test verifies
`recoveryRequired`, blocked read/flush, `close()` returning
`RecoveryRequired`, a nonempty committed WAL, successful reopen/replay, exact
page-count growth, the split structure, both indexes, and the committed prior
UPDATE. This correctly exercises post-durable-COMMIT recovery; the durable
transaction is not described as rolled back.

The full suite also retains the R3F-R1 valid WAL page-count history
`4 -> 8 -> 23 -> 48`, invalid committed regression `4 -> 3` returning
`WalCorrupt`, and P6 cache pin/publication checks.

## 10. Focused and full hosted qualification

The focused target is `database_heap_page_construction_test`. Its final output
is **3,537 checks, zero failures**, including R2, R3I, R3J, R3K, and the new
R3L cases. The R3L cases cover 512-byte and 4,096-byte splits, two consecutive
splits under one 4,096-byte parent, variable-width exact fit at the maximum
valid key length, deterministic preparation/staging failures, savepoint
rollback, retry, duplicate rejection, durable COMMIT recovery, and reopen.

The final serial `ctest --test-dir build --output-on-failure` run passed
**21/21 targets, 153,828 checks, zero failures** in 1,000.73 seconds. This is
2,783 checks above the accepted R3K total, matching the focused target's
increase from 754 to 3,537 checks. The run retained SQL1–SQL9, P1–P8,
R1–R3K, prepared statements, primary/secondary indexes, FK constraints,
UPDATE/DELETE relocation, cache ownership, and the valid/invalid WAL history
regressions.

## 11. Freestanding census and ABI audit

Ran `tools/qualify_sql9_freestanding.ps1` with the read-only Server AMD64
flags, including `-ffreestanding`, `-nostdinc++`, `-fno-exceptions`, and
`-fno-rtti`. The R3L run retained **34 production translation units; 16
passing and 18 blocked**. No new production unit was added and no previously
passing unit regressed. The changed `database/database_index.cpp` still stops
at its existing hosted `<set>` include; other unchanged first blockers include
hosted `<memory>`, `<string>`, `<vector>`, and `<map>` in the SQL unit census.

The passing object symbol audit retains the existing allocator/runtime imports
such as `malloc`/`free` and required gx APIs. The checksum guard still imports
`__cxa_guard_acquire` and `__cxa_guard_release`; the scan found no
`__cxa_throw`, exception-personality, RTTI, or global `operator new` /
`operator delete` references. This is production object compilation and symbol
inspection only. No native SQL link or QEMU execution is claimed. Census and
symbol artifacts are in
`%TEMP%\guideXOSSql-NDB1R4P11R3L-final-20261023`.

## 12. Remaining hosted boundaries and payload readiness

| Function or area | Hosted allocation/copy and owner | Current failure/rollback contract | Minimum next step |
|---|---|---|---|
| `DatabasePage::payload`, parse/read compatibility | `std::vector<uint8_t>` payload materialization; page facade owns the copy. | Read failures return status; transaction/cache rules remain. | Establish a destination owner for page views before global conversion. |
| `IndexTree::readLeafEntries`, key construction, sort, `left`/`right` and parent child vectors | Hosted vectors and temporary physical-key vectors remain before the checked candidate builders. | `Table::insert` catches `std::bad_alloc`; explicit INSERT savepoint restores earlier heap/index stages. | Select bounded portable entry/path representations and qualify allocation failures separately. |
| `IndexTree::writeLeaf` | Hosted payload vector remains for empty leaves and legacy/root/cascading split construction. | Local `bad_alloc -> NoSpace`; SQL/DDL transaction rollback remains responsible for multi-page failures. | Convert the complete root/cascading mutation group after the direct-parent path is established. |
| `IndexTree::writeInternal` / `splitRootLeaf` | Hosted internal page image and recursive child vectors remain. | Existing checked statuses and statement/transaction rollback remain; no allocator-fault qualification is claimed for these paths. | Qualify internal parent split plus root publication as one bounded continuation. |
| `Table::applyMutationsInTransaction` and `packHeapPage` | UPDATE/DELETE repacking retains hosted page payload and locator/mutation vectors. | Existing statement savepoints restore transaction page images and catalog state. | Prepare a complete heap candidate in the active transaction allocator and fault-test locator/index remapping. |
| Legacy `PageAccess::writePage` callers | Some producers still construct/mutate `DatabasePage` before forwarding bytes to P7/P8. | The transaction owner stages complete images and propagates failure. | Migrate one producer at a time with its own mutation and savepoint audit. |
| Transaction savepoint catalog copy | Page overlay snapshot uses P7/P8 ownership; catalog copy remains hosted. | Snapshot allocation is caught before mutation; rollback swaps catalog and restores `_nextPageId` without allocating. | Keep the accepted R3I savepoint owner contract. |
| SQL parser/value materialization | Hosted AST, string, and value vectors are built before the statement savepoint and first page mutation. | Failure precedes page publication. | Keep outside the page payload migration scope. |
| P7/P8 test-only snapshots | Hosted comparison/fault harness allocations only. | No production publication dependency. | No production conversion required. |

The engine is **not ready for global `DatabasePage::payload` conversion**.
R3L adds checked ownership for one complete split group and repairs the
physical-separator read bound; it leaves the legacy writer family in place for
root and recursive parent changes.

## 13. Next bounded P11 continuation

Qualify the **internal parent split and root publication** boundary. Build a
real SQL workload that forces a leaf split whose direct parent cannot accept
the new child, then prepare and stage the recursive internal split/root change
with transaction-owned payloads, deterministic failure injection after each
staged page, and the same INSERT savepoint proof. Include maximum physical
separator keys and preserve the corrected logical-key-plus-locator bound.
Do not start that continuation or P12 as part of R3L.

## 14. Git closeout

The source, tests, and this report are committed locally on the existing SQL
`main` branch after qualification. The commit SHA is included in the task
closeout response. Both phase markers remain at SQL9 / SQL10 and
NDB1R4P10 / NDB1R4P11. Server HEAD and Git topology are unchanged, and no
push was performed.
