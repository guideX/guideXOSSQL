# NDB1R4P11R3AD — Portable Index Native Snapshot Provider

## Result

**Outcome C — `SNAPSHOT_PROVIDER_PREREQUISITE_BLOCKED`.** The audit found no existing SQL or guideXOS Server facility that can bind the index root and all page reads to one stable database view. The SQL transaction captures a base page count, not a page-image snapshot; the Server interface available in the inspected checkout is positional file I/O. No production snapshot adapter or native Server adapter was activated.

R3AD makes the snapshot precondition explicit on the portable page-provider contract and adds a focused hosted fixture qualification. That harness proves that the existing point and range operations behave correctly when supplied an immutable versioned page set, and that provider errors leave hosted outputs empty. It is not evidence that SQL storage or Server currently supplies such a snapshot.

## Repositories and phase markers

The SQL checkout started on branch `main` at the expected clean HEAD `a68322e61421ee351597b3f1f4496f667db63939`. The `.phase` SHA-256 remained `f84ef02a6b82d4c53cdb2bdd0b80c083a49c6fa2e5df46914d9aadd7555295f0`; `.native-sql.phase` remained `fe722cf1a92a04c9f16e91e556933c682a7e3475bf3aabb06ce994410a28a52`. Their contents remain SQL9 → SQL10 and NDB1R4P10 → NDB1R4P11. Neither marker was changed.

The separately inspected Server checkout was `D:\dev\guideXOSServer_NATIVE_DB_STORAGE`, branch `NATIVE_DB`, HEAD `f29850c93bbac0a0763a31e806d2816310156575`, clean at inspection. Its `.native-db.phase` remains NDB1R3 → NDB1R4. That repository was read only and was not modified.

The R3AD provider contract and focused target are in commit `73fdb1afc45951ffa5d642a72046e1c70fa0d22b`; explicit snapshot-byte immutability assertions were added in `ebcd59a0ecfe66f9da8d3d184333d1e63b286cdb`. This report is a documentation-only follow-up; the ending qualified source HEAD is `ebcd59a0ecfe66f9da8d3d184333d1e63b286cdb`.

## SQL storage contract audit

### Page and transaction interfaces

`PageAccess` in `database/database_pagestore.h:27-38` exposes `pageSize`, `pageCount`, `isReadOnly`, `readPage`, `writePage`, and `allocatePage`. It has no snapshot acquisition, root handoff, page-version pin, borrowed-page release, or consistency-validation operation. `Database` selects the active transaction as its page view in `database/database_relational.cpp:112-118`.

`Database::beginTransaction` records the current file page count as `basePageCount` (`database/database_relational.cpp:732-790`). That count bounds the transaction view but does not preserve page images. `Transaction::readPage` (`database/database_transaction.cpp:81-118`) returns an overlay page when present and otherwise calls the database's current committed `readPage` for pages below that base count. It does not read from a page-version set captured at transaction start.

The committed `Database::readPage` path (`database/database_relational.cpp:124-168`) reads an independently owned `GxOwnedPage`, parses it into a hosted `DatabasePage`, and moves that value to the caller. `BufferManager::readOwnedPage` (`database/database_buffer.cpp:416-515`) copies the current cache image or a file image into caller-owned storage. The buffer manager also has `pinPage`/`unpin` (`database/database_buffer.cpp:331-414`, `774-800`), but `PageAccess::readPage` does not return or retain a pinned cache slot. The current index adapter therefore borrows bytes from its own reusable `DatabasePage` value, not from a pinned buffer slot.

Page identity, page type, payload extent, page size, and page-count bounds are checked at the current hosted and portable boundaries. The serialized page has a per-page generation field, but there is no snapshot-wide version or validation operation. Comparing individual generation values would not establish that multiple pages belong to one consistent view.

### Root selection, writes, and concurrent access

`Database::indexLookup` and `Database::indexRange` read the index root from the current catalog record and pass it with `pages()` into `IndexTree` (`database/database_relational.cpp:1249-1309`). The root, owner, page count, and subsequent page reads are not acquired atomically as one snapshot object.

`Transaction::commit` first makes the WAL commit durable, then updates the file page count and installs transaction page images into the committed cache one by one before finishing the transaction (`database/database_transaction.cpp:252-341`). No database-wide reader/writer lock or immutable committed-page snapshot is present in the inspected SQL database/page interfaces. The existing implementation is single-writer and does not establish concurrent reader isolation. A concurrently executing index read could observe a mixture during publication; this SQL checkout does not define concurrent access as safe.

The current hosted calls remain usable under serialized access. Their existing behavior is not a guarantee for a concurrently changing database. R3AD does not add synchronization, MVCC, copy-on-write, or a new transaction model.

## Portable provider and hosted adapters

The portable `GxIndexPageProvider` callback contract is declared in `database/portable/gx_index_leaf_traversal.h`. R3AD now states that `rootPageId`, `pageCount`, and every acquired page must refer to one logically consistent read-only view for the entire operation; the provider context and snapshot must remain alive through completion; and a token or matching generation number alone does not prove consistency.

The hosted adapter in `database/database_index.cpp:173-294` owns one `DatabasePage` in its operation context. It validates the requested page identity, type, and payload length, exposes the payload until release, and checks that the active view/token still matches on release. `IndexTree::lookupAll` and `IndexTree::rangeScan` pass that provider to `gxIndexLookupAll` and `gxIndexRangeScan` (`database/database_index.cpp:2483-2567`, `3210-3308`). Portable traversal retains bounded descent, page parsing, ordering, range comparisons, duplicate handling, and corruption checks. The adapter translates storage errors explicitly; an out-of-bounds read becomes `MissingPage`, while other storage errors remain provider failures.

Both hosted operations stage visitor results and publish them only after successful completion. Typed key conversion and owned result materialization remain hosted. The portable traversal itself remains allocation-free and does not mutate page bytes.

This provider contract defines what a future integration must supply; it does not supply a snapshot implementation. The current SQL adapter does not independently acquire a snapshot and does not prove consistency if another caller can modify or publish pages during the operation.

## guideXOS Server interface feasibility

The local Server source inspected at HEAD `f29850c93bbac0a0763a31e806d2816310156575` contains a real kernel VFS and block/filesystem path, plus the NDB1R3 native database file bridge. `kernel/core/include/kernel/native_database_file.h:47-80` defines `GxDbNativeFileOps` for open, close, read, write, seek, tell, file size, flush, truncate, stat, and unlink. `kernel/core/native_database_file.cpp:319-363` implements positional reads by saving the file position, seeking, reading into a caller buffer, and restoring the position.

Those declarations do not expose database page identifiers, page sizes, a catalog-root/snapshot handoff, page borrows, page release, or a database transaction view. The bridge returns file status codes and bytes for one file operation. The inspected bridge/VFS declarations provide no database-level read-view or page-lifetime synchronization contract. The Server repository's `docs/NDB1R4_NATIVE_SQL9_INTEGRATION.md` also records that SQL9 engine compilation is blocked under the configured freestanding target by unavailable C++ standard headers/runtime. No native adapter can be compiled against the present Server declarations without inventing a missing API.

Therefore actual guideXOS Server interface compatibility is unavailable, native storage integration remains pending, and native kernel execution was not tested. No Server source, build configuration, or phase marker was changed.

## R3AD focused qualification

The new CTest target `database_ndb1r4p11r3ad_test` reads the production-written R3AC multilevel fixture at `tests/fixtures/NDB1R4P11R3AC_production_range_tree.hex`. Its test-only `SnapshotPageAccess` deep-copies the page set to create deterministic versions A and B. Version B is changed through production `IndexTree::insert`; both versions are then exposed read-only to the existing hosted index adapter.

R3AD passed **339 checks, 0 failures**. It verified exact ordered results for the fixture's 420 rows, the 100 duplicate `m-dup` locators spanning leaves, repeated reads of version A, one-sided and unbounded scans, and distinct valid contents in versions A and B. It also injected expiry, a read failure on a later leaf acquisition, leaf disappearance, and root-page identity corruption. Each failed hosted operation returned an explicit error and left its result vector empty. The test compares all page images before and after lookup/scan operations and verifies both frozen versions remain byte-identical; the test view also rejects writes.

The test-only page-set boundary is intentionally not called a production snapshot adapter. It demonstrates operation behavior under the provider's documented precondition. It does not verify a storage engine's ability to supply that precondition or a real concurrent writer.

Existing R3AC qualification covers stable and error behavior for empty, leaf-only, and multilevel trees; bounded, one-sided, and unbounded scans; NULL, Boolean, Int32, Int64, and Text including embedded-zero Text; provider buffer reuse; callback failure/stop; successor and page corruption; and hosted failure atomicity. Existing R3AB qualification covers complete point lookup, including its hosted Float64 compatibility route. The R3AD error harness adds no fault hook to production code.

### Borrow, error, and concurrency evidence

R3AD tests expiry, page acquisition failure after a successfully read matching leaf, missing pages, and detectable page identity mismatch. The hosted staging path publishes no partial results on these errors. Existing R3AB/R3AC portable tests verify one release for each successful acquire, release on provider and visitor failures and early stop, buffer reuse only after release, and no retained page pointers.

No real concurrent-writer regression was run: SQL database, transaction, catalog, and cache access have no established concurrent-reader synchronization contract. Deterministic immutable page sets and injected invalidation errors are used as boundary evidence only. No simulated race is presented as concurrency proof.

## Regression qualification

Focused executables:

- R3AA: **4,018 passed, 0 failed**.
- R3AB: **1,354 passed, 0 failed**.
- R3AC: **1,008 passed, 0 failed**.
- R3AD: **339 passed, 0 failed**.
- R3Z through R3AD CTest subset: **5/5 passed**.

The final Debug aggregate passed **34/34 CTest targets**, with 0 failures and 0 skips, in **1,316.64 seconds**. This includes the existing storage, transaction, SQL, R3R–R3AD index, portable runtime, WAL/recovery, page/cache, and R3P5–R3P8 regression targets. CTest does not provide a combined assertion total, so none is inferred.

## Freestanding census and sanitizer status

The established `tools/qualify_sql9_freestanding.ps1` ran with the unchanged MinGW 15.2.0 compiler and AMD64 flags, using the checked Server kernel tree. The census remains **26/44 passing**; all previous passing units remain passing. The first `database/database_index.cpp` blocker remains `database/database_index.h:27:10: fatal error: string: No such file or directory` under `-nostdinc++`.

The existing production key, leaf, descent, complete-lookup, and range-scan probes compiled and executed. The complete point/range probes linked the real production objects with `ld -r --no-undefined`; both probe summaries report **PASS** for compile, relocatable link, and hosted execution, with no unresolved symbols. The R3AD hosted test and provider comment introduce no freestanding production operation or hosted dependency into portable traversal.

`SANITIZER_NOT_RUN — TOOLCHAIN_UNAVAILABLE`. `clang++` and MSVC `cl.exe` remain unavailable, and MinGW reports only bare `libasan` and `libubsan` library names. No toolchain was installed and no sanitizer coverage is claimed.

## Integration readiness

| Capability | Assessment |
|---|---|
| Portable point lookup | Verified; `gxIndexLookupAll` and hosted `IndexTree::lookupAll` regressions pass. |
| Portable range scan | Verified; `gxIndexRangeScan` and hosted `IndexTree::rangeScan` regressions pass. |
| Snapshot-consistent page reads | **Blocked** for concurrent changes; no page-image snapshot or synchronization guarantee exists. Conditional on externally serialized access only. |
| Page borrow/release | Verified at the portable provider boundary and hosted operation context; committed cache pages are copied by `PageAccess::readPage`. |
| Hosted transaction adapter | Existing path verified for serialized hosted use; not a snapshot adapter and not qualified for concurrent publication. |
| Actual guideXOS Server interface compatibility | Unavailable; current native bridge is positional file I/O without a page/snapshot contract. |
| Native kernel execution | Not tested. |
| Native storage integration | Pending; production activation is prohibited until the snapshot/page prerequisite exists. |
| Index writes | Still hosted; unchanged. |
| WAL and recovery | Existing behavior preserved; storage, transaction, WAL, and recovery regressions pass. |
| Float64 indexing portability | Deferred; the hosted compatibility route remains, with no portability migration. |

## Missing prerequisite and next milestone

The smallest missing SQL contract is an operation-lifetime read view that atomically binds the catalog's index root/owner and page-size/page-count metadata to a stable read-only page set. It must prevent page replacement while borrowed pages are active or provide immutable page versions, keep the view alive for the complete lookup/scan, and return explicit errors for expiry or unavailable historical pages. A mere snapshot ID or generation comparison is not enough. The Server bridge additionally needs a real database/page interface that can create or receive that view; the current file bridge cannot provide it.

Recommended next bounded milestone: qualify a SQL `DatabaseReadView` against the actual storage publication path, including how it excludes or safely versions commits while root-to-leaf and successor reads run. Keep the view disabled for concurrent access until this is proven. Only after that contract exists should a Server adapter be written against real Server declarations and compiled.

## Commits and limits

- Starting HEAD: `a68322e61421ee351597b3f1f4496f667db63939`.
- Implementation/test commits: `73fdb1afc45951ffa5d642a72046e1c70fa0d22b`, `ebcd59a0ecfe66f9da8d3d184333d1e63b286cdb`.
- Ending qualified source HEAD: `ebcd59a0ecfe66f9da8d3d184333d1e63b286cdb` (this report is a documentation-only follow-up).
- No push, history rewrite, Server modification, QEMU run, QAC run, or native guest acceptance was performed.
