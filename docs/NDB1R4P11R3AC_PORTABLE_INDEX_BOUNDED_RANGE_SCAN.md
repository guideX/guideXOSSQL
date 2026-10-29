# NDB1R4P11R3AC — Portable Index Bounded Range Scan

## Result

**Outcome A — `PORTABLE_INDEX_BOUNDED_RANGE_SCAN_VERIFIED`**

R3AC adds the freestanding operation `gxIndexRangeScan` to the portable index lookup layer and routes hosted `IndexTree::rangeScan` through it. The scan reuses R3AA root descent, R3Z leaf decoding and physical ordering checks, and R3AB provider/status conventions. Database formats, writes, transactions, recovery, SQL syntax, and the query planner are unchanged.

Starting branch and HEAD: `main`, `859ec8f58328cae975ae0b1d93039ff3ba2bda51`. The starting working tree was clean and the local branch was two commits ahead of local `origin/main`. Phase markers are unchanged: `.phase` SHA-256 `f84ef02a6b82d4c53cdb2bdd0b80c083a49c6fa2e5df46914d9aadd7555295f0`; `.native-sql.phase` SHA-256 `fe722cf1a92a04c9f16e91e556933c682a7e3475bf3aabb06ce994410a28a52`.

Production implementation commit: `eacbffd837109a586d76e1fcf99bfec635e1360b`. This is the source revision under the final Debug CTest aggregate. Documentation commit and final HEAD: pending.

## Existing range-scan audit

Before R3AC, `IndexTree::rangeScan` already existed as a hosted production method. `IndexRangeBound` represented optional typed `DbValue` endpoints and inclusive flags. It supported lower-only, upper-only, two-sided, and unbounded scans. Endpoint checks compared complete encoded logical keys, excluding the physical locator suffix; physical entries remained ordered by the existing 12-byte row locator.

The prior implementation descended with `findLeaf` using the minimum locator `(0, 0)` for a lower endpoint. With no lower endpoint, a separate loop followed child zero to the leftmost leaf. It decoded, filtered, and accumulated entries in hosted code, followed next-leaf links, and stopped at the first logical key outside the upper bound. A hosted `GxPageVisitSet` detected repeated leaves and a page-count bound limited traversal. Provider errors and corruption became `DbResult` errors, but rows were appended directly to `out` while scanning.

The existing production path is `Database::indexRange`: it constructs `IndexTree` with `pages()`, the indexed type and id, `activeTransaction()`, and the scratch allocator. The portable operation is therefore below a real transaction-aware hosted index API, without SQL or planner changes.

## Portable API and semantics

`gxIndexRangeScan` is declared in `database/portable/gx_index_lookup.h` and implemented in `.cpp`. It accepts root/page/owner metadata, optional encoded bounds with explicit lengths and inclusivity, the established page provider, finite descent/leaf limits, caller-owned path/key scratch, a visitor, and an explicit `GxIndexLookupStatus` result.

Supported modes are unbounded, lower-only, upper-only, and bounded. Inclusive lower means `key >= lower`; exclusive lower means `key > lower`. Inclusive upper means `key <= upper`; exclusive upper means `key < upper`. Equal inclusive endpoints return all duplicate locators. Equal endpoints with either exclusive flag, and reversed endpoints, complete as empty ranges without page acquisition after argument and root-id validation. NULL uses the existing one-byte `00` tag; non-NULL bounds begin with `01`. Type-specific encoding remains hosted.

Descent uses `lower logical bytes || twelve zero locator bytes`, or twelve zero bytes without a lower bound. Physical comparisons retain the locator suffix for descent and ordering; endpoint comparisons use the full logical key only. This visits all locators for an inclusive duplicate lower bound and filters all equal keys for an exclusive lower bound. Explicit lengths preserve embedded zero bytes.

The implementation composes `gxIndexTreeDescendToLeaf` with `gxIndexLeafScanRangeChain`; it adds no second page parser. The chain walker checks physical ordering across pages. Before reporting that an upper bound completes a range, it acquires and validates the successor boundary. A missing, malformed, or out-of-order successor is an error, not successful completion. A zero successor is end-of-chain. Exceeding the finite leaf-page budget reports `TraversalLimit`.

The visitor's key span is borrowed and valid only until its callback returns; locator fields are passed by value. `Stop` yields `VisitorStopped`; `Error` yields `VisitorFailure`. Each successful page borrow is released exactly once on success, early stop, and error. Later errors remain authoritative after earlier callbacks, so partial callbacks must be discarded. Hosted code copies results to a staging vector and publishes only after completion; errors leave public output empty.

The portable scan is allocation-free, nonrecursive, read-only, and independent of STL, `DbValue`, exceptions, RTTI, and hosted OS services. Typed conversion, transaction-aware `PageAccess`, the hosted leaf-cycle set, error translation, and owned result materialization remain hosted.

## Production fixture and focused qualification

The freestanding runner uses `tests/fixtures/NDB1R4P11R3AC_production_range_tree.hex`, serialized by the R3AC test target's `--write-fixture` mode after building and validating the tree with production `IndexTree::insert` and `IndexTree::validate`. Header: `512 767 37 615 100 766` (page size, page count, owner id, root id, duplicate count, serialized nonzero pages). The fixture is a height-7 tree with 420 ordered entries and 100 `m-dup` entries crossing leaf pages.

Focused results:

- R3AA: **4,018 passed, 0 failed**.
- R3AB: **1,354 passed, 0 failed**.
- R3AC: **1,008 passed, 0 failed**.

R3AC compares exact hosted and portable key/locator sequences. Coverage includes empty and leaf-only trees; single/multiple entries; one-leaf and multi-leaf ranges; unbounded and one-sided bounds; inclusive, exclusive, equal, reversed, and empty ranges; duplicate order; repeatable scans; NULL, Boolean, Int32/Int64 boundaries, empty and embedded-zero Text; and invalid typed bounds. It also covers visitor stop/failure, provider failure after callbacks, missing successor, release and provider-buffer reuse, short views, budget exhaustion, invalid root, oversized length arithmetic, invalid endpoint tag, invalid internal child, internal cycle, hosted leaf cycle, malformed leaf key, cross-page ordering, and hosted failure atomicity.

## Full Debug regression

The final Debug CTest aggregate passed **33/33 targets** in **1,636.71 seconds** on implementation commit `eacbffd837109a586d76e1fcf99bfec635e1360b`. This includes storage, relational, transaction, SQL, index/range, portable runtime, WAL/recovery, native I/O, heap-page, owned-page, and cache-publication targets. The starting baseline was 32/32 in 4,209.28 seconds. CTest does not provide a grand assertion count; only direct R3AA/R3AB/R3AC counts are stated as assertion totals.

## Freestanding proof and census

The established qualification script ran with MinGW 15.2.0 at `C:\mingw64\bin\c++.exe` and the unchanged AMD64 freestanding flags. The census is **26/44 passing**; all previous 26 passes remain, and the modified portable lookup, descent, and leaf traversal units compile. `database/database_index.cpp` remains first blocked at `database/database_index.h:27:10: fatal error: string: No such file or directory` under `-nostdinc++`. The remaining failures are hosted database/SQL units requiring standard C++ headers. The denominator and settings were unchanged.

The production range and complete-lookup probes compile with those flags, link the actual portable comparator, leaf, traversal, internal-node, descent, and lookup objects using `ld -r --no-undefined`, and execute against the production-written R3AC fixture. `nm -u -C` reports no unresolved symbols. The probes verify all 100 duplicate locators across leaves, inclusive/exclusive and empty endpoints, early stop, provider failure after callbacks, invalid root, corrupt views, and releases. Required symbols include `gxIndexRangeScan` and `gxIndexLeafScanRangeChain`. This proves the range operation is portable; it does not make the hosted SQL engine or `database_index.cpp` freestanding.

`SANITIZER_NOT_RUN — TOOLCHAIN_UNAVAILABLE`. MinGW resolves sanitizer libraries only as bare `libasan` and `libubsan` names; neither `clang++` nor MSVC `cl.exe` is installed. No sanitizer coverage is claimed.

## Native-readiness assessment

1. **Portable reads:** Point lookup and bounded range scan now share the provider and root-descent contracts. Point visitors receive locator values; range visitors also receive the key bytes as a temporary borrow.
2. **Traversal ownership:** Hosted range code delegates descent, leaf parsing, ordering, and filtering. Typed bound encoding, transaction-consistent storage, cycle tracking, error translation, and result staging remain hosted.
3. **Key types:** Portable codecs cover NULL, Boolean, Int32, Int64, and Text. Float64 conversion remains hosted; Blob remains non-indexable.
4. **Server adapter:** A native caller needs catalog root/owner/page-count metadata and a provider that lends identified page bytes from one consistent snapshot and releases every borrow. It must supply bounded scratch and a result visitor, and map storage errors to provider statuses.
5. **Transaction guarantees:** Catalog/root selection, snapshot consistency, transaction visibility, synchronization/locking, and write publication remain in hosted `Database` and transaction code. A changing or mixed-snapshot provider cannot guarantee consistency.
6. **Read-side maturity:** The portable point/range surface is ready for a dedicated native-readiness qualification, but Server integration still needs a real page provider and snapshot tests.
7. **Portable writes:** Create/insert/remove, splits and balancing, transaction staging, WAL/recovery, and atomic publication remain hosted. Portable writes need bounded mutation and commit/publication interfaces and are outside this read-only phase.

Recommended next milestone: qualify a read-only guideXOS Server adapter mapping its transaction-consistent index pages to `GxIndexPageProvider`, then exercise point and range reads with duplicate runs, snapshot consistency, corruption, and provider failures. Keep writes, Float64 migration, SQL syntax, and planner work out of that milestone.

Ending qualified source HEAD: `eacbffd837109a586d76e1fcf99bfec635e1360b`. This is the production revision under the final CTest aggregate. The report is a documentation-only follow-up and does not alter that tested source revision.
