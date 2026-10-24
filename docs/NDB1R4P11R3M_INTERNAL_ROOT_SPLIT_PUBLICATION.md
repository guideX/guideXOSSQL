# NDB1R4P11R3M — Checked Internal Root Split Publication

**Outcome: `CHECKED_INTERNAL_ROOT_SPLIT_VERIFIED` (bounded root-growth case).** This qualifies the one-level case where a leaf split overflows the current internal root and that root becomes the left child of a newly constructed root. It does not qualify arbitrary-depth cascades, other full-parent variants, or global `DatabasePage::payload` portability. Parent phase NDB1R4P11 remains open.

## Repository and phase gates

- Writable checkout: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting SQL HEAD: `fc2e20ed792179cb5bfd6cc87ea39631677ee46f` (the accepted R3L commit).
- Ending SQL HEAD before the authorized closeout commit: same starting commit; source work remained uncommitted during qualification.
- Starting worktree was clean. At closeout, only `database/database_index.cpp`, `database/database_index.h`, this report, and `tests/database_heap_page_construction_test.cpp` are intended for the local commit.
- `.phase` remained `project=guideXOSSql`, `last_completed=SQL9`, `next_expected=SQL10`.
- `.native-sql.phase` remained `project=guideXOSSqlNativePortability`, `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- Read-only Server checkout remained on `NATIVE_DB_STORAGE` at `f29850c93bbac0a0763a31e806d2816310156575`; it had no worktree changes. No Server files, phase markers, branches, or Git topology were changed.

The accepted R3L report and implementation, R3K publication report, and R3I/R3J statement-savepoint and heap-staging contracts were reviewed before editing. R3L remains the baseline for fitting-parent leaf splits. Its accepted baseline was 21/21 CTest targets, 153,828 checks, zero failures; 3,537 focused R3L checks; and a 34-unit freestanding census with 16 passing and 18 blocked.

## Production call path and split variants

A real indexed SQL INSERT follows this path:

1. `SqlEngine::executeInsert` (`database/database_sql.cpp`) starts a `TransactionSavepoint` for an explicit transaction.
2. `Table::insert` and `Table::insertInTransaction` (`database/database_heap.cpp`) validate and encode the row, stage the heap append, then insert each indexed key.
3. `IndexTree::insert` (`database/database_index.cpp`) descends through `findLeaf`, prepares the inserted physical key (logical encoded key plus row locator), and detects an over-capacity leaf. Its attempted `writeLeaf` is a capacity probe: over-capacity returns `NoSpace` before building or staging a replacement page.
4. The accepted `stageNonCascadingLeafSplit` handles a leaf split when the parent fits. If it declines, `IndexTree::insert` calls the new `stageRootInternalSplit` only when the recorded path contains exactly the current root.
5. `stageRootInternalSplit` locates the split leaf in the root child list, inserts the new right-leaf reference, selects a legal internal split point, and prepares both leaf images, both internal images, and the new root image before the first page stage.
6. It stages the new leaf, new right internal page, new root, replacement old leaf, and replacement old root through transaction `allocatePagePayload` / `writePagePayload`. It returns the new root ID only after all five stages succeed.
7. Only after `IndexTree::insert` succeeds does `Table::insertInTransaction` set the catalog index root ID, increment entry counts, and mark the catalog dirty. The root and index metadata are part of the transaction catalog snapshot; height is derived by validation and is not persisted as a separate field.
8. On failure, `executeInsert` restores the explicit statement savepoint. The transaction overlay restore is allocation-free, the catalog snapshot is swapped back, `_nextPageId` is restored, and prior transaction work remains. For an implicit transaction, `Table::insert` rolls the transaction back. `Table::insert` also catches `std::bad_alloc` from existing hosted planning containers and returns `NoSpace`.

| Variant | R3M result |
|---|---|
| Leaf replacement fits | Preserve R3K |
| Leaf split, parent fits | Preserve R3L |
| Leaf split, full parent is current root | Converted and qualified here |
| Internal split with a fitting grandparent | Audited; deferred |
| Repeated cascading internal splits | Deferred |
| Root height growth in selected case | Included and qualified |
| Root shrink on DELETE | Deferred; existing behavior preserved |

The selected split preserves the engine's child-lower-bound representation. The promoted physical separator remains the lower bound of the right internal node's first child and is copied into the new root's right-child lower bound. Search uses the existing inclusive child-bound routing (`chooseChild`), so equality routes into that right subtree. The separator carries the full 12-byte row locator, preserving physical ordering for duplicate logical keys.

## Fixture and resulting tree

The fixture is a production SQL workload using `RootRows(K TEXT PRIMARY KEY, Mirror TEXT NOT NULL)` and a unique secondary index on `Mirror`. It dynamically fills both indexes to a height-two tree whose internal root has four children and whose rightmost leaf is full. It uses maximum encoded text keys for each page size, and continues only when the next SQL INSERT causes the full-root growth. `PriorRows` has a foreign key to `ParentRows`; a prior successful UPDATE supplies transaction work that must survive failed INSERTs.

| Page size | Rows before triggering INSERT | Original primary root | New leaf | New right internal | New root | Page count | Root split |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 512 | 7 | 14 | 22 | 23 | 24 | 22 → 28 | 4 children become 2 / 3 |
| 4,096 | 7 | 13 | 21 | 22 | 23 | 21 → 27 | 4 children become 2 / 3 |

The existing root page ID is retained as the left internal node. The new root has two children: the original root page with an empty lower bound and the new right internal page with the promoted bound. The old and new leaves have the expected next-leaf links. The combined child lists contain every original leaf exactly once plus exactly one new leaf; all leaf page types and each child's lower bound were checked. The indexes validate at height 3 with the expected eight rows and visited pages.

For the fixture's maximal text keys, the encoded physical separator (encoded text key plus 12-byte row locator) is 102 bytes on 512-byte pages and 998 bytes on 4,096-byte pages. These lengths are derived from the actual page/key limits; they are not hardcoded as a universal key limit. Internal records retain the existing 24-byte internal header, 12-byte `{child page ID, key length}` record overhead, and 8-byte slot. Checked fit arithmetic accounts for each variable-width lower bound separately.

After publication and reopen, tests query every key through both primary and secondary indexes, compare row locators, and query the key immediately below, equal to, and immediately above the promoted separator. They also validate the leaf chain, row count, index structure, foreign-key behavior, and database integrity. Both indexes grow from the same inserted row, so both catalog root IDs and entry counts are checked.

## Ownership, preparation, and publication boundary

All five final candidate payloads are prepared before the first stage using the existing `prepareLeafPayload` and `prepareInternalPayload` encoders. Those allocate `GxArray` storage from the active transaction's `pageAllocator()`, validate page capacity, page identity/type fields, entry counts, ordering, child ranges, duplicate child references, variable key lengths, slots, offsets, and full payload bounds, then retain allocation provenance for matching destruction. No raw relocation of owning page objects is used.

Page IDs are planned from the transaction's current page count with checked room for exactly three new pages per growing index: new leaf, new right internal node, and new root. The actual P7 allocations are checked against the planned IDs; `_nextPageId` and page-overlay count advance only through successful transaction page allocation. Existing leaf and root IDs are then replaced with checked P7/P8 writes. The logical root scalar is changed by the table only after the entire index page group returns success; catalog index metadata and page-overlay state remain protected by the INSERT statement savepoint.

The publication path no longer constructs the five final page payloads through `DatabasePage::payload` or the legacy `writeLeaf` / `writeInternal` serialization route. The existing overfull-leaf `writeLeaf` call is only the `NoSpace` capacity probe described above. Existing hosted structures remain in the path: `DatabasePage` reads materialize `std::vector<uint8_t>`, leaf entries and paths use vectors, `Child::lowerBound` and physical-key construction use vectors, and split planning/candidate slices use `std::vector<Child>`. Those allocations are **not** covered by the Gx allocator fault injector. They occur before the current root split's first page stage; `Table::insert` catches `std::bad_alloc`, and the enclosing statement/transaction boundary restores changes. The fault suite does not claim direct hosted-vector injection or a hosted-free/freestanding B+ tree.

There is no page-format, catalog-format, index-format, or version change. Existing codec/header/CRC behavior is retained; successful reopen and recovery validate the existing bytes.

## Fault injection and rollback evidence

Faults use the test's actual `GxAllocator` callbacks and transaction/cache allocator instances. The targeted candidate tests fail the production transaction-allocator allocation for the two leaf candidates and internal candidate construction. They observe the exact allocator attempt/success/failure deltas and the expected `NoSpace` preparation status. On return, the prior overlay image, transaction page count, outstanding owner balance, row count, primary/secondary index lookups, and index validation remain at baseline.

A later candidate-allocation fault is placed in preparation of the second (secondary) index after the first index's complete root-growth group and catalog root value have already been staged. It confirms page-image allocation advanced before the fault, then verifies savepoint restoration of both roots, page count, overlay, allocator balance, heap row absence, and both index entries. The earlier successful `PriorRows` UPDATE remains visible. A retry inside the same explicit transaction succeeds; explicit full `ROLLBACK` then removes the retry while restoring the earlier UPDATE to its pre-transaction value.

The partial-staging fault is keyed to the measured successful page-image allocations before the intended stage: after the heap append and the root group's three new-page stages (new leaf, right internal, new root), it fails the following existing-leaf replacement. The test observes the exact failed P7 allocation attempt, confirms the new root image was already transaction-owned, and then verifies statement rollback restores the old root identity, page count, modified-page count, allocator outstanding bytes, heap rows, and both indexes. The previous UPDATE remains visible; the failed key is absent from both indexes; a retry succeeds; and explicit rollback restores the pre-transaction state.

For the 512-byte scenario, the fixture also stages catalog serialization before arming the cache allocator fault. The catalog requires four cache allocations in this larger fixture, so the next allocation is the first committed page-image cache publication point after the durable WAL COMMIT. The observed result is one cache allocation attempt, zero successes, one allocation failure, `page-cache image allocation failed`, and `recoveryRequired=1`. This is a **post-durable-COMMIT cache-publication failure**, not a rolled-back commit. The handle refuses ordinary reads/close as expected, outstanding cache and transaction owners drain after destruction, and reopening performs WAL recovery and reconstructs the committed root and row state.

The 4,096-byte case follows the normal COMMIT path. Both page sizes close and reopen twice. The 512-byte case reopens after WAL recovery and verifies the same catalog roots, page count, two-index tree shape, all row lookups, foreign-key/integrity state, and promoted-bound routing.

## Test and portability qualification

- Focused executable: `database_heap_page_construction_test`, **5,304 checks, 0 failures**. It contains the R3M cases alongside the pre-existing R3J/R3L/heap cases; the test binary retains its legacy R3J output label, so 5,304 is the combined focused executable total, not a separately isolated R3M-only counter.
- Full serial verbose CTest: **21/21 targets, 155,595 checks, 0 failures**, 1,252.79 seconds. Target #19 (`database_heap_page_construction_test`) passed with 5,304 checks. No tests were disabled or skipped.

| CTest target | Checks | CTest target | Checks |
|---|---:|---|---:|
| `database_storage_tests` | 223 | `database_relational_test` | 10,411 |
| `database_transaction_test` | 2,578 | `database_sql_test` | 1,779 |
| `database_index_test` | 2,752 | `database_sql7_test` | 1,307 |
| `database_sql8_test` | 4,503 | `database_sql9_test` | 15,274 |
| `database_sql9r_test` | 53,898 | `database_native_io_test` | 19 |
| `database_portable_runtime_test` | 20,668 | `database_wal_portable_test` | 305 |
| `database_wal_page_portable_test` | 153 | `database_wal_recovery_portable_test` | 204 |
| `database_ndb1r4p5_test` | 4,594 | `database_ndb1r4p6_test` | 619 |
| `database_ndb1r4p7_test` | 308 | `database_ndb1r4p8_test` | 25,544 |
| `database_heap_page_construction_test` | 5,304 | `database_owned_page_test` | 251 |
| `database_cache_publication_test` | 4,901 | **Total** | **155,595** |

The actual AMD64 census was run with `tools/qualify_sql9_freestanding.ps1`, the read-only Server AMD64 configuration, and `-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti`. Result: **34 production translation units, 16 pass, 18 blocked**, unchanged. All 16 previously passing units remain passing; no new production translation unit was added or newly qualified. The first blockers for SQL code remain missing hosted C++ headers (`<cstdio>`, `<memory>`, `<string>`, `<vector>`, and `<set>`); `database_index.cpp` remains blocked at `database/database_index.h:27` on `<set>`. Therefore there is no object or undefined-symbol report for the modified index translation unit in this run, and no new SQL9 native link/QEMU execution or guest recovery claim is made. Passing portable object symbol reports continue to show their existing cross-unit `gxAllocator*`, `gxArray*`, `gxTransaction*`, and CRC APIs; `gx_allocator_hosted.cpp` references hosted `malloc/free`, while `database_checksum.cpp` references `__cxa_guard_acquire/release`. No new hosted API or runtime ABI dependency was introduced into a newly passing unit because none became newly passing.

## Remaining writer dependencies and next P11 continuation

Global payload conversion is not ready. `DatabasePage::payload` remains a hosted vector compatibility boundary for page reads and legacy writes. `Table::applyMutationsInTransaction` rebuilds/compacts heap pages through hosted mutation maps, sets, encoded-row vectors, and `packHeapPage`; UPDATE/DELETE relocation remains a separate writer. `IndexTree::writeInternal` and its recursive fallback still use hosted child vectors and legacy page construction. Transaction page overlay/savepoint ownership is already transaction-allocator-backed, while catalog snapshots and hosted parser materialization remain separate boundaries. Root shrink/merge remains unchanged.

The next bounded P11 continuation should convert **the internal split with a fitting grandparent** in `IndexTree::insert`: when a leaf split overflows its parent but the next ancestor has capacity, prepare and stage the two internal children plus the fitting grandparent using transaction-owned candidates, preserve the established lower-bound promotion rule, and cover savepoint rollback after partial multi-page staging. This is the next unconverted branch adjacent to R3M's root-only dispatch. Keep repeated cascades and root shrink out of scope until that bounded case is qualified; do not combine this with global payload conversion or start P12.

## Closeout

The root-growth question for the selected bounded case is answered **yes**: a production indexed INSERT can split a full internal root, stage the complete new root group with transaction-owned page images, and either finish the logical root update or restore the full statement savepoint after injected allocation/staging failure. The evidence preserves prior transaction work, heap and index state, row locators, page count, catalog roots, and committed WAL recovery. Hosted planning containers and all other split variants remain explicit limitations rather than freestanding qualifications.

The implementation and this report are authorized for local commit on the existing `main`; the resulting commit SHA is recorded in the final closeout. No push, topology change, Server modification, phase-marker advance, or P12 work was performed.
