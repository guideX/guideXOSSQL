# NDB1R4P11R3N — Checked Internal Grandparent Split

**Outcome: CHECKED_INTERNAL_GRANDPARENT_SPLIT_VERIFIED.** The selected case is one full nonroot
internal parent with a fitting existing root/grandparent. Parent P11 remains
open; this does not qualify arbitrary-depth cascades or global
`DatabasePage::payload` portability.

## Repository and phase gates

- Writable SQL checkout: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `6c2062c7b28296c542fc90c9841a56f46e6bf34c`; worktree and index
  were clean before this continuation. This is the accepted R3M commit.
- `.phase`: `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase`: `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- The R3M report is present and records 21/21 CTest targets, 155,595 checks,
  zero failures; 5,304 focused executable checks; and 34 freestanding
  production units with 16 passing and 18 blocked. The focused baseline
  `database_heap_page_construction_test` passed again before R3N edits.
- The Server checkout was inspected read-only: branch `NATIVE_DB_STORAGE`,
  HEAD `f29850c93bbac0a0763a31e806d2816310156575`, clean worktree.
- R3L fitting-parent leaf splits and R3I INSERT savepoint restoration were
  reviewed with their production code and reports.

## Production boundary and selected topology

The SQL9 path is `SqlEngine::executeInsert` → `Table::insert` /
`Table::insertInTransaction` → `IndexTree::insert` →
`stageNonCascadingLeafSplit` → the bounded internal split helper. Before R3N,
the R3M helper was root-only (`path.size() == 1`); a depth-two path fell
through to the hosted recursive writer. R3N generalizes the checked helper so
the same internal partitioning and candidate encoders serve both root growth
and the selected fitting-grandparent case.

The production fixture uses `RootRows(K TEXT PRIMARY KEY, Mirror TEXT NOT
NULL)` with a unique secondary index, maximum-width encoded text keys, and
the existing `ParentRows` / `PriorRows` transaction-preservation fixture. An
SQL probe against the accepted R3M implementation established the pre-split
state for the next insertion (row 9):

| Page size | Rows already present | Root/grandparent | Root children | Full nonroot parent | Splitting leaf | Parent children | Leaf entries | Transaction page count |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 512 | 8 | 24 | 2 | 23 | 22 | 3 | 4 | 28 |
| 4,096 | 8 | 23 | 2 | 22 | 21 | 3 | 4 | 27 |

For both formats, the parent cannot accept the new maximum-width physical
separator, while the existing root can accept the promoted separator and a
third child. The physical separator is 102 bytes at 512-byte pages and 998
bytes at 4,096-byte pages. The index already has height 3 and must retain the
same root page and height after R3N.

The selected mutation group contains five complete page images:

1. Existing splitting leaf, with its new right-sibling link and left entries.
2. New right leaf.
3. Existing full internal parent, replaced by its left child partition.
4. New right internal parent partition.
5. Existing fitting grandparent/root, with the new internal child and promoted
   physical separator.

There is no separate neighboring-leaf page: SQL9 leaf links are forward-only,
and the split leaf and new right leaf carry the required link values. The root
page identifier is unchanged and its level remains internal over internal
pages. No catalog root scalar changes for this case.

## Ownership, planning, and staging boundary

The active `Transaction::pageAllocator()` owns checked `GxArray` candidates
from `prepareLeafPayload` and `prepareInternalPayload`. `Transaction` remains
the P7 page-image owner and P8 page lookup. Each candidate is validated in the
existing SQL9 format before any structural page stage. New page IDs are
planned from the transaction's current page count after the heap append; the
page ID reservations advance only when `allocatePagePayload` succeeds.

Hosted planning still uses `std::vector` for decoded leaves, child paths,
parent/grandparent child arrays, partition candidates, and promoted-key bytes.
These allocations are not covered by the Gx allocator injector. In the SQL
INSERT path, `Table::insert` maps `std::bad_alloc` to `NoSpace`, and
`executeInsert` restores the active statement savepoint, including preceding
heap and index stages. The bounded helper performs all hosted planning before
the first structural stage. R3N does not claim hosted planning is portable or
covered by deterministic Gx fault injection.

The intended checked stage order is: allocate the new right leaf; allocate the
new right internal page; replace the original leaf; replace the original
parent with its left partition; replace the existing grandparent with the
promoted child reference. Each operation returns and checks its status. These
stages are not individually atomic; R3I's INSERT savepoint restores the full
statement after any partial-stage failure.

## R3M preservation and R3N structural result

R3M's accepted root-growth implementation prepares the two leaf images, two
internal images, and new-root image before its first stage. `Transaction` owns
the staged images and lookup entries through P7/P8, and the R3I savepoint
restores those pages, `_nextPageId`, catalog root/count fields, and earlier
transaction work after a failed INSERT. The generalized helper retains that
root-growth behavior for a one-page path. Its existing 102-byte and 998-byte
maximum separator checks, root ID growth, height growth, and post-durable
cache-fault recovery still pass in the focused executable.

The SQL9 call path is:

```text
SqlEngine::executeInsert                         database/database_sql.cpp:2871
  -> Transaction::beginStatement                database/database_transaction.cpp:350
  -> Table::insert                               database/database_heap.cpp:1000
     -> Table::insertInTransaction               database/database_heap.cpp:1037
        -> appendEncodedRowToTail                 database/database_heap.cpp:1131
        -> IndexTree::insert                      database/database_index.cpp:1445
           -> findLeaf                            database/database_index.cpp:1302
           -> stageNonCascadingLeafSplit          database/database_index.cpp:916
           -> stageInternalParentSplit           database/database_index.cpp:1023
              -> prepareLeafPayload               database/database_index.cpp:545
              -> prepareInternalPayload           database/database_index.cpp:797
              -> Transaction::allocatePagePayload database/database_transaction.cpp:161
              -> Transaction::writePagePayload    database/database_transaction.cpp:153
        -> update index count/catalog only after each successful index insert
     -> rollbackStatement on failure              database/database_transaction.cpp:375
```

`stageNonCascadingLeafSplit` first checks whether the direct parent can absorb
the new child. When it cannot, `stageInternalParentSplit` builds the enlarged
parent child list once and selects the legal internal split point. For a
one-page path it publishes the R3M new root. For a two-page path it reads the
existing grandparent, inserts the new right-parent child after the old parent,
and handles the checked case only when that grandparent candidate fits. A full
grandparent or a deeper path returns to the existing recursive writer; those
variants remain outside the R3N qualification.

| Page size | Before: rows / root / full parent / split leaf | After: root / left parent / new right parent / new leaf | Root children | Parent children | Height | Page count | Promoted separator |
|---:|---|---|---:|---:|---:|---:|---:|
| 512 | 8 / 24 / 23 / 22 | 24 / 23 / 30 / 29 | 2 → 3 | 3 → 2 + 2 | 3 → 3 | 28 → 33 | 102 bytes |
| 4,096 | 8 / 23 / 22 / 21 | 23 / 22 / 29 / 28 | 2 → 3 | 3 → 2 + 2 | 3 → 3 | 27 → 32 | 998 bytes |

The new page count is one heap page plus two index pages for each of the
primary and secondary indexes. The root page ID remains unchanged. The
original parent page becomes the left partition; the newly allocated internal
page holds the right partition. The promoted bound is the first child bound of
the right partition and is inserted into the root immediately after the old
parent's child reference. Tests verify every old leaf remains reachable once,
the new leaf is reachable once, child bounds match their lower leaves, and
the forward leaf chain has no gaps or duplicate pages.

## Failure, retry, and transaction evidence

For each test INSERT, the heap append precedes the index loop. The
transaction allocator's `GxArray` preparation calls have size 480 bytes on the
512-byte configuration; staged full page images have size 512 bytes. The
candidate injection reached these actual outcomes:

| Matching candidate successes allowed | Reached failure | Observed status/message |
|---:|---|---|
| 0 | Heap append candidate | `heap page preparation allocation failed` |
| 1 | Left leaf candidate | `index leaf preparation allocation failed` |
| 2 | Right leaf candidate | `index leaf preparation allocation failed` |
| 3 | Left internal candidate | `index internal preparation allocation failed` |
| 4 | Right internal candidate | `index internal preparation allocation failed` |
| 5 | Grandparent candidate | `index internal preparation allocation failed` |
| 6 | First secondary-index leaf candidate, after the primary five-image group staged | `index leaf preparation allocation failed` |

Every case runs after a successful UPDATE to `PriorRows`. Assertions check
the exact allocator attempt/success/failure deltas, unchanged root IDs and
entry counts, original root/parent/leaf payload bytes, transaction page count,
modified-page count, transaction allocator ownership, absent failed row and
index locators, retained UPDATE value, and valid indexes/FK integrity. The
allowed-six fault observes seven page-image allocations before rollback
(heap append stages plus the completed primary-index group), proving the
secondary fault follows earlier structural work.

Separate 512-byte page-image faults target two partial-staging points:

- After the new right leaf has staged, the new internal page allocation fails.
- After the new leaf, new internal page, old leaf, and old parent stages
  succeed, the existing grandparent replacement fails.

Both return `transaction page staging failed`. The savepoint restores all
affected images, index lookup/order entries, page count, catalog index counts,
and allocator ownership. The prior UPDATE remains visible. The same INSERT
then succeeds in the same explicit transaction; a full transaction `ROLLBACK`
removes that retry and restores the earlier UPDATE to its original value.

The committed success path queries all nine rows through both primary and
secondary indexes, compares their row locators, runs physical `rangeScan`,
checks keys immediately below/equal/above the promoted separator, rejects a
duplicate unique key, and validates both indexes and relational integrity.
After close/reopen, the tree is checked twice. A prepared UPDATE is reused to
change and restore a secondary key; a prepared INSERT adds a row in the
already-split tree; prepared DELETE removes that row and an existing row; full
rollback restores the committed nine-row tree and height.

On 512-byte pages, the committed R3N split also exercises the accepted
post-durable-COMMIT failure boundary. Catalog serialization used four cache
allocations; the injected next allocation failed after WAL COMMIT durability.
The observed error is `page-cache image allocation failed`, with one attempt,
zero successes, one failure, and `recoveryRequired=1`. Close reports
`RecoveryRequired`, the WAL remains present, and reopening replays the committed
split and metadata. The 4,096-byte case completes a regular COMMIT and reopen.
No checkpoint is required to reconstruct the 512-byte committed split.

## Formats, tests, and freestanding census

No page, catalog, WAL, key, locator, header, CRC, or version encoder changed.
The new images use the existing leaf/internal serializers and existing
transaction page-image encoder. Both supported page sizes retain the accepted
maximum encoded separator behavior. Reopen and WAL recovery validate the same
persistent SQL9 format.

- Focused executable before R3N edits: **5,304 checks, 0 failures**; this
  re-ran the accepted R3M suite on the unmodified starting source.
- Focused executable after R3N: **8,423 checks, 0 failures**. The new
  R3N-specific additions account for **3,119 checks** over that baseline; the
  executable includes the existing R2/R3J/R3K/R3L/R3M cases and retains its
  legacy R3J summary label.
- Full hosted CTest after a complete rebuild: **21/21 targets passed,
  158,714 checks, 0 failures**. This is the accepted 155,595-check R3M
  baseline plus the 3,119 R3N assertions. The serial run completed in
  1,561.98 seconds; the focused split target reported 8,423 checks.
- Freestanding script: 34 production translation units, 16 pass, 18 blocked.
  The passing source set is unchanged from R3M; no new existing or production
  unit passes. The 18 failures remain hosted-library-header blockers. The
  changed `database_index.cpp` still stops at `database/database_index.h:27`
  on `<set>`, so this run has no object or undefined-symbol listing for that
  changed translation unit. Existing passing objects retain their Gx and CRC
  API references, `malloc/free` in the hosted allocator, and
  `__cxa_guard_acquire/release` in the checksum object. No new global
  allocation, exception, RTTI, or vtable symbol appears in passing objects.
  This is an object census only; no native SQL link or QEMU run is claimed.
  Final artifact directory:
  `C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3N-final-20261024`.

## Remaining writer boundaries and next P11 work

| Boundary | Actual function and hosted dependency | Allocator, status, and rollback/durability boundary | Freestanding blocker | Smallest next step |
|---|---|---|---|---|
| Split planning | `IndexTree::insert`, `stageNonCascadingLeafSplit`, and `stageInternalParentSplit`; `std::vector<IndexEntry>`, `std::vector<Child>`, vector lower bounds, and partition copies use the hosted C++ allocator. | `Table::insert` catches `std::bad_alloc` and returns `NoSpace`; the SQL statement savepoint restores earlier heap/index stages. R3N completes all hosted planning before its first structural stage. These hosted allocations are not under Gx fault injection. | `database_index.h:27` includes hosted `<set>`; vectors and strings also block freestanding compilation. | Convert bounded split planning into a checked transaction-allocator-owned plan, with direct status and allocation-failure tests. |
| Deeper cascades / full grandparent | Legacy propagation loop in `IndexTree::insert` and `writeInternal`; hosted child vectors and `DatabasePage::payload`. | `std::bad_alloc` is caught by `Table::insert`; INSERT savepoint is the rollback boundary. Grandparent-full and `path.size() > 2` remain on this path. | Same `database_index.cpp` `<set>` blocker. | Extend one more explicit depth only after a bounded plan and a fixture prove its complete mutation group. |
| Compatibility page readers/writers | `DatabaseFile::readPage` / `DatabasePage::payload`; `IndexTree::writeLeaf` and `writeInternal`; legacy page writes. | Hosted vector allocation; some index writer `assign` failures return `NoSpace`. Transaction `writePage` owns final staged images; callers rely on statement/transaction rollback. | Hosted `vector`/`set` and other C++ headers. | Migrate one compatibility writer only after its callers, page owner, and allocator lifetime are mapped. |
| SQL parser materialization | `SqlTokenizer`, `SqlParser`, AST and prepared-plan vectors/maps/strings. | Hosted allocations occur before DML mutation; INSERT parsing and value coercion precede the statement savepoint. No page rollback is needed before mutation. | Missing `<string>`, `<vector>`, and `<map>` headers in the freestanding environment. | Keep parser portability separate from the selected page writer work. |
| Heap append | `Table::appendEncodedRowToTail`; encoded rows and `DatabasePage` reads remain hosted, while page payload candidates use `gxHeapPageAppendPrepare` / `gxHeapPagePrepare` and the active transaction allocator. | Builder and P7/P8 stage statuses are checked; explicit INSERT savepoint restores after heap allocation/link stages. `UPDATE`/`DELETE` relocations also call this append path. | `database_heap.h` currently stops at `<memory>`. | Audit the remaining hosted row/planning containers around the checked page builder. |
| UPDATE/DELETE heap rewrites | `Table::applyMutationsInTransaction`; maps, sets, vectors, row copies, and relocation planning are hosted; `packHeapPage` uses the buffer scratch allocator. | SQL `applyMutationPlan` establishes the R3I statement savepoint; transaction page writes are checked. Relocation and index maintenance are restored on statement failure. | `database_heap.h` stops at `<memory>` and STL headers. | Convert one bounded rewrite plan while retaining the current savepoint boundary. |
| Savepoint compatibility snapshot | `Transaction::beginStatement` clones P7/P8 overlay through the transaction allocator, then copies `Catalog` with hosted vectors. | Clone and catalog copy failures are checked before mutation; `rollbackStatement` moves the overlay and swaps the catalog without allocating, and restores `_nextPageId`. | `database_transaction.h` stops at `<memory>`. | Assess whether the catalog snapshot copy is the next constrained ownership boundary. |
| DELETE merge / root shrink | `IndexTree::remove` rewrites leaf pages; no merge or root shrink is implemented. | Existing DML savepoint/transaction handling covers leaf and index writes. No merge publication is claimed. | Same hosted index headers and containers. | Keep merge/root shrink deferred until separately scoped. |
| Test compatibility copies | Test fixtures materialize `DatabasePage` and `std::vector` snapshots. | Test-only hosted allocation; not production page ownership. | Not part of the production translation-unit census. | No migration in R3N. |

Global `DatabasePage::payload` conversion is **not ready**: the compatibility
vector still crosses the public page read/write path, legacy index writers,
heap mutation planning, and tests. R3N qualifies only the bounded SQL9
nonroot-parent split with a fitting grandparent. Arbitrary-depth recursive
splitting, DELETE merge/root shrink, and every hosted planning allocation
remain open.

## Closeout

Final checks reconfirmed the SQL branch remains `main`, both phase markers are
unchanged, and the Server checkout remains clean at
`NATIVE_DB_STORAGE` / `f29850c93bbac0a0763a31e806d2816310156575`. The
implementation, tests, and this report are committed locally on `main`; the
commit SHA is included in the delivery summary. The SQL worktree is clean after
commit. No push, Server modification, or Git topology change was made.
