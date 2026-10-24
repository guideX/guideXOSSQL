# NDB1R4P11R3P — Bounded Portable Leaf-Split Planning

## Result

**PORTABLE_LEAF_SPLIT_PLANNING_VERIFIED**

The transaction-backed SQL9 B+ tree now uses a bounded `GxArray` of fixed-size
leaf descriptors to plan selected real INSERT split paths. Those paths no
longer decode keys into hosted `IndexEntry` vectors, copy left and right entry
vectors, or sort a second owned copy of each key. Planning, separator
preparation, leaf candidate construction, and the accepted R3O parent-plan
integration finish before the first structural split image is staged.

This result qualifies the selected leaf-split planning boundary. It does not
qualify the height-one root-leaf compatibility fallback, arbitrary-depth
cascading splits, every index read or mutation, or global
`DatabasePage::payload` portability.

## Repository gates and accepted R3O baseline

The writable checkout was `D:\dev\guideXOS_SqlDatabase`, branch `main`, at
starting HEAD `fcb1070233c0364b05a547bea0c897a5eaee20ae`. The worktree was clean
before editing. The final local commit is recorded in the delivery summary.
The ordinary marker remained:

```text
project=guideXOSSql
last_completed=SQL9
next_expected=SQL10
```

The native portability marker remained:

```text
project=guideXOSSqlNativePortability
last_completed=NDB1R4P10
next_expected=NDB1R4P11
```

The accepted [R3O report](NDB1R4P11R3O_PORTABLE_SPLIT_PLANNING.md) and current
production code were reviewed. Its accepted baseline is 21/21 CTest targets,
160,567 checks, zero failures; the focused split executable reported 10,276
checks; the freestanding census had 34 production units, 16 passing and 18
blocked. R3O's shared internal-parent plan remains transaction-allocator
backed and is consumed by the R3N fitting-grandparent production route. Its
parent-plan, separator, and grandparent-plan allocation faults still occur
before structural split staging. The statement savepoint, checked transaction
publication, WAL format, and persistent page format were not changed.

The read-only Server checkout was observed at branch `NATIVE_DB_STORAGE`, HEAD
`f29850c93bbac0a0763a31e806d2816310156575`, and clean. Its AMD64 build flags,
qualification script, and kernel allocator were read for the portability
contract. The allocator uses an 8-byte-aligned free list in the configured
512 MiB AMD64 heap. The Server checkout, artifacts, configuration, Git state,
and markers were not modified.

## Production path and selected boundary

The production call path is:

```text
Table::insert / Table::insertInTransaction
  -> IndexTree::insert
     -> findLeafForInsert
     -> readLeaf
     -> insertTransactionPlan
        -> readLeafPlan
        -> appendLeafPlanEntry for existing entries and the new entry
        -> sortPlannedLeafEntries
        -> leafPlanRangeFits / first legal split-point search
        -> stageNonCascadingLeafSplit, when its parent can be handled directly
        -> stageInternalParentSplit for the R3M/R3N parent topologies
           -> preparePhysicalLeafSeparator
           -> prepareLeafPlanPayload for each leaf candidate
           -> prepareInternalPlanPayload for parent candidates
           -> Transaction::allocatePagePayload / writePagePayload
  -> existing INSERT statement-savepoint rollback on error
```

The main implementation is in `database/database_index.cpp`:
`PlannedLeafEntry` and its byte access/comparison helpers begin near line 162;
the page-derived bound is `maximumLeafPlanEntryCount` near line 213;
`readLeafPlan` is near line 1025; shared leaf-byte preparation is
`prepareLeafPayloadBytes` near line 655; `stageNonCascadingLeafSplit` and
`stageInternalParentSplit` begin near lines 1516 and 1660; and the selected
transaction path is `insertTransactionPlan` near line 2378. Declarations are
in `database/database_index.h` near lines 178–244.

`IndexTree::insert` preserves its existing fitting-leaf compatibility path.
When the incoming entry cannot fit, it enters `insertTransactionPlan` only
with the active transaction as its page access. The R3L non-cascading,
R3M full-root-parent, and R3N fitting-grandparent split routes consume the
same descriptor plan. Direct candidate preparation is completed before the
first structural transaction write. If the topology is the height-one
root-leaf fallback, or a deeper cascade cannot be handled by the bounded
direct routes, the existing hosted compatibility code still materializes
`IndexEntry` vectors. No arbitrary-depth conversion is claimed.

## Hosted allocation inventory and ownership

| Operation | Before this change | R3P representation and owner | Bound and failure/publication behavior |
|---|---|---|---|
| Existing leaf entries | `readLeafEntries` created a `std::vector<IndexEntry>`; each entry owned a key vector copied from the page | `PlannedLeafEntry` stores a pointer to encoded logical key bytes and copies locator page ID and slot as scalars. `readLeafPlan` borrows bytes from the live `DatabasePage::payload` | Entry count is checked against the page-derived format limit before descriptors are appended. Every append checks `GxStatus`; failure returns `NoSpace`/SQL `ResourceLimit` or a checked bound error |
| Inserted entry | Hosted entry/vector insertion copied the new key into an owned `IndexEntry` and could grow the vector | One descriptor borrows `logicalKey.data()` and copies the `RowLocator` fields | `logicalKey` remains alive throughout `IndexTree::insert`; the descriptor capacity permits one additional entry. A failing append occurs before split staging |
| Entry ordering | Hosted vector sorting moved entries containing owned key vectors | In-place heap sort swaps fixed descriptor bytes using `memcpy`; comparisons read borrowed key bytes and scalar locators | No sort scratch allocation and no nested vector/string. Logical encoded key bytes are compared lexicographically, then locator page ID and slot using the existing physical-key comparator |
| Left/right distribution | Hosted `left` and `right` vectors copied each key and locator again | The same sorted plan is addressed by `[first,count]` ranges | The first candidate split point for which both encoded ranges fit is retained. No left/right plan allocation is needed |
| Separator | `makePhysicalKey` returned an owned hosted vector | `preparePhysicalLeafSeparator` writes the first right entry's key plus the same 12-byte big-endian locator into transaction-allocator `GxArray` storage | Length is checked against the format-derived physical-key limit; status is propagated before structural staging |
| Leaf candidates | Vector writer built a hosted `DatabasePage::payload` | `prepareLeafPayloadBytes` is the shared byte writer used by both the old vector wrapper and the new descriptor-plan wrapper. Candidate bytes are transaction-allocator `GxArray` buffers | The complete candidate is validated for bounds, sorted entries, locator/page identity and sibling before it is handed to checked transaction staging |
| Search path and source page | `DatabasePage::payload`, `std::vector<uint64_t>` path, and SQL `logicalKey` remain hosted | They remain compatibility inputs to the selected plan | They are explicitly outside the converted entry-plan boundary. Source page and key remain live until candidate bytes are encoded and copied to transaction-owned page images |

The plan record contains a key pointer, `uint64_t` locator page ID,
`uint32_t` key length, and `uint32_t` locator slot. It is 24 bytes on the
qualified AMD64 ABI. `GxArray` owns record storage through a copy of the active
transaction allocator, preserving the allocator callback and context for
release. Records are copied in and out with `memcpy` because `GxArray` stores
raw bytes and does not construct C++ object lifetimes. RAII destroys the array
on every return path.

Existing key pointers refer into the still-live leaf `DatabasePage` image;
the new key pointer refers into the live INSERT argument. Sorting only moves
descriptors. The descriptors are not retained after `insertTransactionPlan`
returns. Both leaf payloads and the separator are fully copied into
transaction-allocator buffers before the source page or INSERT key can leave
scope. Transaction staging copies prepared candidate bytes into its own page
overlay storage. No borrowed pointer is published or retained by the database.

## Format-derived bounds and ordering

The leaf page payload is `pageSize - kPageHeaderSize`; the leaf header is 24
bytes. Each entry needs a 16-byte fixed encoded overhead (4-byte key length
and 12-byte row locator), one or more logical key bytes, and an 8-byte slot.
The plan bound is derived as:

```text
maximum existing entries = floor((payloadCapacity - 24) / (16 + 1 + 8))
maximum planned entries  = maximum existing entries + 1 inserted entry
```

The additional one is checked for size overflow before it is used. `readLeaf`
validates the page header and entry count, and `readLeafPlan` independently
rejects counts above this bound, slot arithmetic outside the page, entries
overlapping the slot directory, invalid key lengths, and encoded lengths that
do not match the key-length field. All capacity arithmetic uses checked
subtraction or widened integer arithmetic before addition.

| Page size | Payload capacity | Existing-entry bound | Planned-entry bound | Maximum plan bytes at 24 bytes/record |
|---:|---:|---:|---:|---:|
| 512 | 480 | 18 | 19 | 456 |
| 4,096 | 4,064 | 161 | 162 | 3,888 |
| 65,536 format maximum | 65,504 | 2,619 | 2,620 | 62,880 |

The implementation initializes `GxArray` with the derived maximum. It appends
only within that maximum and maps every growth failure explicitly. It does
not reserve a fixture-specific count or allocate a maximum-size stack array.
The maximum logical key is the existing page-size-aware `maxIndexKeyBytes`
limit (capped by the SQL9 format at 1,024 bytes); the physical separator adds
the unchanged 12-byte locator. The 512-byte fixture has a 90-byte logical key
maximum and 102-byte physical separator; the 4,096-byte fixture has a
986-byte logical key maximum and 998-byte separator; the format maximum can
require 1,036 physical bytes.

`comparePlannedLeafEntries` uses the established physical-key comparison:
encoded logical-key bytes first, then the locator's big-endian page ID and
slot bytes. The heap sort does not allocate or change the stable ordering
contract. The split-point scan uses the same first-fitting candidate rule;
the first right descriptor supplies the separator. Leaf sibling relationships
and parent lower bounds are built by the existing R3L–R3N checked staging
code.

## Failure atomicity and deterministic allocator evidence

Tests use the transaction allocator's real size-matching failure hook. The
source-plan growth matrix fails actual `GxArray` requests after zero, four,
eight, or 128 descriptors have already been copied:

| Page size | Failed request | Descriptors already prepared | Result |
|---:|---:|---:|---|
| 512 | 24 bytes | 0 | `index leaf planning allocation failed` |
| 512 | 192 bytes | 4 | same checked planner error |
| 512 | 384 bytes | 8 | same checked planner error |
| 4,096 | 24 bytes | 0 | same checked planner error |
| 4,096 | 192 bytes | 4 | same checked planner error |
| 4,096 | 3,888 bytes | 128 | same checked planner error |

A separate 512-byte SQL fixture has eight 31-byte text keys that fit one leaf;
the ninth entry forces a split and makes appending the inserted descriptor
grow the plan from eight to sixteen records. The test fails that actual
384-byte request after all eight existing descriptors have been prepared.
It asserts one matching failed allocation, zero matching successes, the exact
SQL `ResourceLimit` and planner error, unchanged source leaf bytes, unchanged
transaction modified-page and page counts, and no transaction allocation
leak. It also verifies that the failed row is absent and an earlier successful
UPDATE remains visible. Disabling the fault lets the same INSERT succeed; a
full explicit `ROLLBACK` removes the retry and restores the original eight-row
leaf.

Corrupt-source tests pass an out-of-range entry length, a `UINT32_MAX` encoded
key length, and a `UINT32_MAX` entry count through the real SQL INSERT path.
Each is rejected as an execution error before a structural split image is
staged. The transaction retains its pre-existing corrupt source and its prior
UPDATE until explicit rollback; the durable index remains valid afterward.

The planner errors happen before parent/leaf structural page stages. INSERT
can prepare heap work before invoking its index maintenance; the existing R3I
statement savepoint removes that statement's heap/index overlay, page-count
changes, and catalog changes when the planner reports failure. Prior
successful transaction statements remain visible. Existing candidate
preparation faults and R3N partial-stage faults remain in the focused target;
the latter fail after one and four successful structural stages and verify
that savepoint rollback restores all page images, index state, page count,
and allocator ownership while preserving the earlier UPDATE.

The accepted R3O parent-plan, separator, and grandparent-plan faults remain
reached in both page-size cases (96 bytes, 102/998 bytes, and 72 bytes). They
still return before structural split images. R3N candidate allocation faults,
partial-stage failures, and the post-durable-COMMIT page-cache publication
fault are also retained.

## Page compatibility, SQL behavior, and persistence

Both the vector-facing writer and the descriptor-plan writer call
`prepareLeafPayloadBytes`. This preserves the existing leaf header, magic and
version, sibling ID, entry count, index ID, encoded key-length field, key
bytes, locator encoding, slot offsets/lengths, and zero-filled unused region.
The focused tests compare complete candidate payload byte vectors with the
independent expected SQL9 page encoder for 512-byte and 4,096-byte split
fixtures, including slot directory, sibling bytes, locators, and free space.
The 512-byte integer-key entries include valid embedded zero bytes in their
encoded values. The variable-width text tests cover exact fit, multiple key
lengths, maximum legal keys, unique-key rejection, and the 102-byte physical
separator. The 4,096-byte split covers the 998-byte physical separator.

R3L non-cascading splits, R3M root growth, and R3N fitting-grandparent splits
still validate child routing, separator equality with the first physical key
of the right child, leaf-chain links, index lookup, range order, duplicate
rejection, row locators, primary and secondary indexes, commit, close/reopen,
and relational integrity. Existing-leaf replacement, indexed UPDATE/DELETE,
prepared statements, and foreign-key tests remain in the complete regression
suite.

The successful split fixtures commit and reopen without a forced checkpoint.
The accepted R3F-R1 history `4 -> 8 -> 23 -> 48` still recovers, while the
invalid committed history `4 -> 3` remains rejected as `WalCorrupt`. The
512-byte post-durable-COMMIT cache-publication failure still reports
`RecoveryRequired`, retains the durable WAL commit, and recovers the committed
split on reopen; it is not treated as a rolled-back transaction. No page,
catalog, WAL, CRC, locator, header, SQL, or on-disk version format changed.

## Regression and qualification results

The final source build completed with:

```text
cmake --build build --config Debug -j 4
```

One existing unused-function warning appeared while compiling
`database_index_test.cpp` (`findIndexByName`); it is outside the changed
planner and does not fail the build.

The focused executable, `database_heap_page_construction_test`, passed
**10,716 checks, 0 failures**. It includes the R3P source-plan allocation
matrix, inserted-entry growth failure, corrupt leaf input cases, and the
accepted R3J/R3K/R3L/R3M/R3N/R3O coverage.

The complete serial Debug CTest run passed **21/21 targets, 161,007 checks,
0 failures** in 1,139.73 seconds. Counts were summed from the successful
per-test results in `build/Testing/Temporary/LastTest.log`. This retains the
accepted P11 and SQL1–SQL9 targets, including savepoints, heap append,
publication faults, internal split planning, page-cache recovery, WAL
recovery, indexes, foreign keys, and prepared statements.

The required `tools/qualify_sql9_freestanding.ps1` run used the read-only
Server kernel flags and allocator contract. It reported **34 production
translation units: 16 passing, 18 blocked**, unchanged from R3O. No new
production unit was introduced and no previously blocked unit newly passed.
The passing units are `database_format.cpp`, `database_checksum.cpp`, and the
portable allocator, array, page, heap-page-builder, recovery, cache, codec,
WAL, and transaction-page implementation units. The changed index translation
unit still stops at its first include, `database/database_index.h:27`:

```text
fatal error: set: No such file or directory
```

It therefore produces no object or symbol listing in the freestanding census.
Other blocked units and their first diagnostics are:

| Translation unit | First missing hosted header |
|---|---|
| `database_io.cpp` | `database_io.h:13` — `cstdio` |
| `database_native_io.cpp` | `database_native_io.h:5` — `memory` |
| `database_header.cpp` | `database_header.h:7` — `string` |
| `database_page.cpp` | `database_page.h:7` — `vector` |
| `database_file.cpp` | `database_file.h:17` — `memory` |
| `database_engine.cpp` | `database_engine.h:9` — `memory` |
| `database_buffer.cpp` | `database_file.h:17` — `memory` |
| `database_catalog.cpp` | `database_catalog.h:35` — `string` |
| `database_index.cpp` | `database_index.h:27` — `set` |
| `database_heap.cpp` | `database_heap.h:26` — `memory` |
| `database_wal.cpp` | `database_wal.h:30` — `memory` |
| `database_transaction.cpp` | `database_transaction.h:25` — `memory` |
| `database_relational.cpp` | `database_relational.h:21` — `memory` |
| `database_sql_token.cpp` | `database_sql_token.h:11` — `string` |
| `database_sql_tokenizer.cpp` | `database_sql_tokenizer.h:12` — `string` |
| `database_sql_parser.cpp` | `database_sql_parser.h:13` — `vector` |
| `database_sql_expr.cpp` | `database_sql_expr.h:18` — `vector` |
| `database_sql.cpp` | `database_sql.h:16` — `map` |

The changed index unit's first compiler diagnostic did not change. The passing
checksum object still references `__cxa_guard_acquire` and
`__cxa_guard_release`; the hosted allocator object still references `malloc`
and `free`. The portable units retain their existing Gx allocator, array,
page-codec, and CRC API references. No new ABI, global-allocation, exception,
RTTI, or vtable evidence was introduced by R3P. This is a translation-unit
census only; it does not claim a native SQL link or QEMU run. The detailed
qualification output is in the task's temporary `qualification.tsv` artifact.

## Remaining hosted boundaries and recommended next continuation

R3P removes the selected leaf split's hosted entry collection and partition
copies. The following remain hosted and must be handled in their own checked
boundaries:

| Production boundary | Current hosted storage and ownership | Failure / transaction contract | Smallest safe follow-up |
|---|---|---|---|
| Height-one root-leaf and deeper cascading split fallback | `IndexEntry`, `Child`, separator, search path, and recursive partition vectors in `IndexTree::insert` | Existing SQL INSERT savepoint is the rollback boundary; these paths remain the compatibility implementation | Convert one explicit topology after its complete candidate/stage group is bounded |
| Search, range scan, and validation | `findLeaf` paths, `readInternalChildren`, and `std::set<uint64_t>` cycle membership in `lookupAll`, `rangeScan`, and validation | Read-only traversal; cycle detection must continue to reject repeated page IDs and allocation failure must be reported | Replace the shared visited-page membership with an allocator-aware, incrementally bounded portable structure; cap it by actual page count and preserve failure propagation |
| Page materialization and compatibility writers | `DatabasePage::payload` vector in parse/read and leaf/internal compatibility writers | Some allocation errors map to `NoSpace`; transaction/savepoint publication remains the caller boundary | Move one caller-owned page view across a selected reader/writer boundary while retaining CRC, type, identity, and lifetime checks |
| Transaction savepoint/catalog snapshots | Portable page overlay plus hosted catalog copies | Snapshot allocation fails before mutation; rollback uses move/swap without allocation | Audit catalog snapshot ownership after page-view consumers are converted |
| SQL parser and row materialization | Hosted strings, vectors, and maps across SQL token, parser, and relational boundaries | Parsing/value coercion precede DML mutation; failures use existing SQL errors | Keep parser portability separate from index page planning |
| UPDATE/DELETE index compatibility paths | Hosted search paths and entries; no index merge or root shrink is implemented | R3I savepoint protects DML mutation; current delete scope only rewrites supported leaf/index pages | Reuse checked page/entry writers before extending remove behavior |

The existing `<set>` is a cycle detector, not an ordering source. Its uses are
outside the selected split path and R3P leaves them intact. Its maximum
membership is the database page count, up to `2^32`; that is not a safe fixed
stack array or a small hardcoded capacity. Because `<set>` is the index unit's
first current freestanding blocker, the recommended next bounded continuation
is a portable page-membership design shared by lookup, range scan, and
validation, with explicit growth/allocation errors and the existing cycle
checks preserved. Removing `<set>` alone would still not make the index
translation unit freestanding: its vectors, strings, and other hosted headers
remain.

The R3P result does not close parent phase NDB1R4P11 and does not begin P12.

## Closeout

The SQL checkout remained on `main`; both phase markers remain at SQL9 to
SQL10 and NDB1R4P10 to NDB1R4P11. The Server checkout remains clean at
`NATIVE_DB_STORAGE` / `f29850c93bbac0a0763a31e806d2816310156575`. The accepted
changes are committed locally on the existing SQL `main`; the commit SHA is
reported with the final status. No push, branch change, or Git topology change
was made.
