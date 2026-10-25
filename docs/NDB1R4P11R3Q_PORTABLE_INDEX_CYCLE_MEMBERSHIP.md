# NDB1R4P11R3Q — Portable Index Cycle Membership

## Pre-implementation audit and selected design

This design was recorded before changing production code.

### Repository gates and accepted R3P baseline

- Writable checkout: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting SQL HEAD: `4125be5f3699436a243323f33595c1554e9a6060`; worktree clean.
- `.phase`: `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase`: `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- The accepted R3P report and selected transaction-backed leaf planner are
  present. R3P records 512-byte / 4,096-byte / format-maximum leaf plan bounds
  of 18/19, 161/162, and 2,619/2,620; it reports 10,716 focused checks,
  21/21 full Debug CTest targets with 161,007 checks, and 16/34 freestanding
  translation units passing. The report and code retain R3L–R3P planning and
  fault coverage.
- Read-only Server reference: branch `NATIVE_DB_STORAGE`, HEAD
  `f29850c93bbac0a0763a31e806d2816310156575`. Its AMD64 flags include
  `-ffreestanding`, `-nostdinc++`, `-fno-exceptions`, and `-fno-rtti`; the
  kernel allocator has an 8-byte alignment contract and a bounded heap.
  Server files and Git state are not part of this change.

### Original production membership inventory

`database/database_index.h` includes `<set>` for the private
`validateNode(..., std::set<uint64_t>&, ...)` declaration. The implementation
includes `<set>` directly. The actual production instances are all
`std::set<uint64_t>` with default ascending integer ordering; no caller
iterates them, so ordering has no result semantics.

| Production function | Membership meaning and first insertion | Revisit behavior | Lifetime and bound |
|---|---|---|---|
| `IndexTree::lookupAll` | One page ID per leaf encountered in the sibling chain, after page ID range check and before page read | Duplicate returns `CorruptPage` (`index leaf chain cycle`); `size() > pageCount` is a redundant guard | One operation; at most `pageCount - 1` legal non-header page IDs |
| `IndexTree::rangeScan` | Same sibling-chain membership after `findLeaf` or the bounded leftmost descent | Duplicate returns the same leaf-chain cycle error; size guard is redundant | One operation; at most `pageCount - 1` |
| `IndexTree::validateNode` via `IndexTree::validate` | Global DFS membership for every node, inserted after depth and page-range checks and before reading the page | Any repeat anywhere in the child graph returns `CorruptPage` (`index tree cycle`), including an ancestor cycle or a repeated/shared child reference | Entire recursive validation operation; at most `pageCount - 1` |
| `IndexTree::validate` leaf-chain pass | A separate sibling-chain membership set, first inserted after leaf range check and before page read | Duplicate returns `CorruptPage` (`index leaf chain cycle`); size guard is redundant | Leaf-chain pass only; at most `pageCount - 1` |

The tree validator's global repeat rule is stricter than ancestor-only cycle
detection. SQL9 stores a B+ tree, whose child graph has one parent per non-root
page; a repeated child is malformed and the current validator rejects it.
Lookup and range use path descent with `kMaxIndexDepth` plus a separate global
leaf-chain repeat check. Validation has distinct child-tree and leaf-chain
passes. Reusing a tracker is safe only after resetting it between those
validation passes.

Call paths are `Database::indexLookup -> IndexTree::lookupAll`,
`Database::indexRange -> IndexTree::rangeScan`, and
`Database::validateIndex -> IndexTree::validate -> validateNode`; table
uniqueness, foreign-key, and index-maintenance operations also reach
`IndexTree` through transaction page access. `PageAccess::pageCount()` is the
existing logical traversal bound. It is validated by database/header and
transaction rules, includes page zero, and may reflect logical WAL recovery
extent beyond the materialized file. The tracker will not use untrusted page
IDs as allocation sizes or substitute physical file length for this bound.

### Selected portable contract

| Design field | Selected contract |
|---|---|
| Consumers | Lookup leaf chain, range-scan leaf chain, validation child DFS, validation leaf chain |
| Page identifier width | `uint64_t`; valid IDs are `1 <= id < pageCount` |
| Membership semantics | Exact page-ID membership; each consumer keeps its existing operation-wide semantics |
| Lifetime | One stack-scoped tracker per operation; validation resets its tracker between child DFS and leaf chain |
| Maximum legal members | `pageCount - 1`, bounded by validated SQL9 `kMaxPageCount == 2^32` |
| Storage | Incrementally grown open-addressed hash slots in `GxArray`, with zero as the empty marker (page zero is invalid) |
| Capacity policy | Power-of-two slot count, max 75% occupancy, maximum slot capacity derived with checked arithmetic from `pageCount - 1` |
| Allocation bound | No allocation at construction; only visited pages drive growth. At the format maximum, the table's theoretical final cap is `2^33` 64-bit slots (64 GiB); a rehash temporarily holds at most 1.5 times the final slot bytes. Actual allocator limits fail with `NoSpace` before any unbounded growth. |
| Allocator | `Database::scratchAllocator()` selects the database buffer allocator for committed reads or the active transaction's page allocator for transaction reads; the chosen pointer is passed explicitly into `IndexTree` |
| Allocator lifetime | Tracker is destroyed before its `Database` or `Transaction` caller returns; `GxArray` retains callbacks and context and frees through that copied allocator |
| First insert / duplicate | First insert reports `revisited=false`; existing page reports `revisited=true` without allocation |
| Out of range | `GxStatus::CorruptData`, adapted to the existing `DbStatus::CorruptPage` |
| Capacity / arithmetic limit | Explicit `ResourceLimit` or `Overflow`, adapted to `DbStatus::NoSpace` |
| Allocation failure | Explicit `OutOfMemory`, adapted to `DbStatus::NoSpace`; never treated as an unvisited page or corruption |
| Reset / destruction | Reset clears all slots and retains owned capacity; destruction is idempotent and frees through the same allocator |
| Expected performance | Expected amortized O(1) membership and O(n) total visits; each probe is capped by table capacity and occupancy stays at or below 75% |

The helper will byte-encode slot values instead of forming `uint64_t*` over
`GxArray` storage, whose allocator contract requests byte alignment. The
portable helper will be compiled as a distinct production translation unit
under the real Server AMD64 freestanding flags and will be used by the SQL
production traversal paths. The allocator plumbing will leave the existing
`PageAccess` virtual interface unchanged.

## Implementation and qualification

The source conversion, focused fault tests, Debug qualification, and
freestanding census are complete. Results follow.

## Final implementation and qualification

### Portable tracker and bounded storage

Added `database/portable/gx_page_visit_set.h` and `.cpp`. `GxPageVisitSet`
stores nonzero `uint64_t` page IDs in byte-encoded open-addressed slots backed
by `GxArray`. It uses a mixed page-ID hash and linear probing, with a
power-of-two capacity and a 75% maximum load. Initialization validates the
logical page count and `maximumVisits <= pageCount - 1`, derives a checked
maximum capacity, and performs no allocation. Storage grows only when a new
visit is recorded. Reset zeroes the existing slots and retains the allocation;
destruction frees via the allocator copied into `GxArray`.

For `N` maximum visits, the slot bound is the next power of two at or above
`ceil(4N/3)`, computed with checked arithmetic. The legal page-count ceiling
is `kMaxPageCount == 2^32`, so `N <= 2^32 - 1`. At that theoretical maximum,
the final table is `2^33` slots, or 64 GiB of 64-bit slots; a doubling rehash
temporarily retains the old table and can peak at 96 GiB (1.5 times the final
table). This is a format-derived upper bound, not an eager reservation. The
tracker grows with actual visits, and allocator maximums or allocation failure
stop growth explicitly. Probes are capped by capacity; expected membership
cost is amortized O(1), and expected total traversal cost is O(n).

The allocator is operation-scoped. `Database::scratchAllocator()` selects
the active transaction's page allocator while a transaction is active, or the
database buffer's scratch allocator for committed reads. Production
`IndexTree` construction passes this allocator explicitly. Lookup, range,
validation, index building, and heap-side index callers use the appropriate
database allocator. The existing `PageAccess` virtual interface was left
unchanged. `IndexTree` retains its four-argument constructor and adds an
allocator-taking overload; a direct caller using the old constructor has no
traversal allocator, so a visit-tracker operation returns `NoSpace` rather
than silently using hosted/global storage.

### Converted call sites and behavior

All four index-side `std::set<uint64_t>` instances were removed:

- `IndexTree::lookupAll`: sibling-chain membership remains operation-wide;
  a repeated leaf returns `CorruptPage`.
- `IndexTree::rangeScan`: sibling-chain membership and its cycle error remain
  the same.
- `IndexTree::validateNode`: one operation-wide tracker preserves the
  validator's rule that any repeated node reference, including a shared child,
  is malformed.
- `IndexTree::validate` leaf-chain pass: the tracker is reset after DFS and
  reused as a separate chain-membership table, preserving the old two-pass
  semantics.

The former redundant `set.size() > pageCount` guards are unnecessary: valid
visits are range-checked, page zero is rejected, and the tracker's explicit
maximum is `pageCount - 1`. Invalid IDs return `CorruptPage`; revisits retain
the original cycle errors. Allocation failure, capacity exhaustion, and
checked-size overflow return `DbStatus::NoSpace`. Validation preserves
`NoSpace` instead of adapting it to corruption. No error path treats a
failed membership insertion as an absent page or a successful end of scan.

`database_index.h` and `.cpp` no longer include `<set>`. Other production
`std::set` dependencies remain outside this coherent index conversion:

| Source location | Remaining use |
|---|---|
| `database/database_heap.cpp:1276` | `std::set<std::pair<uint64_t,uint32_t>>` rejects duplicate row locators in one mutation plan. |
| `database/database_heap.cpp:1343` | `std::set<uint64_t>` rejects cycles in a heap data-page chain. |
| `database/database_heap.cpp:1379` | `std::set<uint64_t>` confirms every by-page mutation group was processed. |
| `database/database_sql.cpp:3583` | `std::set<std::vector<DbValue>, ResultRowLess>` implements SQL `DISTINCT` deduplication. |

These are separate mutation, heap-chain, and SQL-result semantics and remain
hosted prerequisites for later work.

### Corruption and allocation-failure evidence

`database_index_test` now exercises real `Database::indexLookup`,
`Database::indexRange`, and `Database::validateIndex` paths. The CRC-valid
fixtures cover valid traversal, self/two-/three-leaf sibling cycles,
self/two-/three-page child cycles, out-of-range references, wrong child page
type, a repeated child, and an invalid child-level relationship. A large
fixture provides a multi-level tree and several leaf siblings. The valid
lookup and range cases preserve their expected rows and order.

Allocator-fault tests use a custom `GxAllocator` shared with the database's
cache/transaction allocation domain. Initial tracker allocation failures in
lookup, range, and validation all return `NoSpace`; range and lookup expose no
partial successful result. A range-table growth failure also returns
`NoSpace`. The checks compare outstanding allocations, file bytes, and dirty
page counts before and after failure. An explicit transaction test injects a
tracker failure during a later `INSERT`, verifies the SQL resource-limit
error, unchanged transaction page state, and visibility of the prior row,
then retries successfully, commits, closes/reopens, and validates both rows
and their indexes. The tracker unit tests separately cover first visit,
duplicate, reset, invalid IDs, bounded capacity, initial/growth allocation
failure, retry, allocator cleanup, and idempotent destruction.

The R3Q filtered integration run reported **188 checks passed, 0 failed**.
The complete `database_portable_runtime_test` reported **20,912 passed, 0
failed**. The focused R3P heap/page-construction executable reported
**10,716 checks passed, 0 failures**.

Existing WAL recovery and durable-commit fault coverage was retained and
passed in the complete CTest run; R3Q does not change WAL code or introduce a
new persistent format. In particular, SQL3 WAL recovery, P6 recovery after
cache-publication allocation failure, and the R3M/R3N durable-commit recovery
cases remain in the suite.

### Full Debug CTest results

Configured with `CMAKE_BUILD_TYPE=Debug`, built all targets, and ran
`ctest --test-dir build --output-on-failure`. All **21/21 targets passed** in
1,446.07 seconds. Summing the assertion totals printed by each executable in
`build/Testing/Temporary/LastTest.log` gives **161,439 checks, 0 failures**.

| CTest target | Checks | Result |
|---|---:|---|
| `database_storage_tests` | 223 | pass |
| `database_relational_test` | 10,411 | pass |
| `database_transaction_test` | 2,578 | pass |
| `database_sql_test` | 1,779 | pass |
| `database_index_test` | 2,940 | pass |
| `database_sql7_test` | 1,307 | pass |
| `database_sql8_test` | 4,503 | pass |
| `database_sql9_test` | 15,274 | pass |
| `database_sql9r_test` | 53,898 | pass |
| `database_native_io_test` | 19 | pass |
| `database_portable_runtime_test` | 20,912 | pass |
| `database_wal_portable_test` | 305 | pass |
| `database_wal_page_portable_test` | 153 | pass |
| `database_wal_recovery_portable_test` | 204 | pass |
| `database_ndb1r4p5_test` | 4,594 | pass |
| `database_ndb1r4p6_test` | 619 | pass |
| `database_ndb1r4p7_test` | 308 | pass |
| `database_ndb1r4p8_test` | 25,544 | pass |
| `database_heap_page_construction_test` | 10,716 | pass |
| `database_owned_page_test` | 251 | pass |
| `database_cache_publication_test` | 4,901 | pass |
| **Total** | **161,439** | **21/21, 0 failures** |

The R3P baseline was 161,007 checks. The higher current count includes the
new portable membership and R3Q integration assertions; no target was
skipped or suppressed.

### Freestanding census and `<set>` checkpoint

Ran `tools/qualify_sql9_freestanding.ps1` against the read-only Server AMD64
configuration with the original freestanding flags. The census now contains
**35 production translation units: 17 pass, 18 blocked**. All 16 previously
passing units still pass; no previously blocked original unit newly passes.
The newly added `database/portable/gx_page_visit_set.cpp` compiles as a
standalone unit under the real flags. Its undefined symbols are only the
project-local `gxArrayInit`, `gxArraySwap`, `gxArrayResize`,
`gxArrayDestroy`, and `gxArrayCapacity` functions; it introduces no hosted
runtime import.

The index header and source no longer require `<set>`. The original index
translation unit advances past that dependency and is still blocked at
`database/database_index.h:27` by `<string>`. The 18 current first
diagnostics are:

| Production unit(s) | First hosted blocker |
|---|---|
| `database_io.cpp` | `<cstdio>` |
| `database_native_io.cpp`, `database_file.cpp`, `database_engine.cpp`, `database_buffer.cpp`, `database_heap.cpp`, `database_wal.cpp`, `database_transaction.cpp`, `database_relational.cpp` | `<memory>` |
| `database_header.cpp`, `database_catalog.cpp`, `database_index.cpp`, `database_sql_token.cpp`, `database_sql_tokenizer.cpp` | `<string>` |
| `database_page.cpp`, `database_sql_parser.cpp`, `database_sql_expr.cpp` | `<vector>` |
| `database_sql.cpp` | `<map>` |

The exact include locations and compiler messages are recorded in the fresh
qualification artifacts under
`C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3Q-final-20261025`.
The portable helper is qualified as an object only; the index engine as a
whole is not claimed to compile or link freestanding.

### ABI, persistent compatibility, and remaining blockers

No `PageAccess` virtual method or vtable entry changed. The existing
four-argument `IndexTree` constructor remains, and an allocator-taking
overload plus `Database::scratchAllocator()` were added. A private allocator
pointer changes `sizeof(IndexTree)` and its C++ object layout; consumers that
depend on binary layout must rebuild. The normal Debug build links the old and
new constructor symbols, the new database allocator accessor, and the
tracker methods. No stable external binary ABI is claimed. The freestanding
helper object's dependencies are project-local `GxArray` calls only.

No database or WAL format files were changed. The edit set is limited to
tracker source, index/allocator plumbing, build and qualification source
lists, focused tests, and this report. The new tracker does not convert
`DatabasePage::payload`, hosted leaf-entry or internal-child vectors, path and
split-planning vectors, or the `std::string` diagnostic/output surfaces.
Those still block the index unit after the removed `<set>` dependency.

### Next bounded P11 continuation

The next scope should be a read-only IndexTree page/entry-decoding boundary
for lookup, range scan, and validation. Derive leaf and child descriptor
capacity from the validated per-page format bounds, keep storage
allocator-owned and failure-aware, and adapt to the current result vectors at
the API boundary. First address the now-visible `<string>` and per-page
`std::vector` dependencies with a bounded, testable conversion. Leave split
planning, `DatabasePage::payload`, mutation algorithms, and other hosted
`std::set` users as separately scoped prerequisites; do not combine them into
one index redesign.

## Outcome

**PORTABLE_INDEX_CYCLE_MEMBERSHIP_VERIFIED (Outcome A).** The real SQL9
index-cycle membership dependency was replaced by bounded, allocator-correct
portable storage in lookup, range scan, and structural validation. Valid
traversals, malformed graph rejection, allocation-failure propagation, the
existing planner/split/recovery suite, the full hosted suite, and the
freestanding census all meet the R3Q gates. This verifies the converted
cycle-membership boundary only; it does not qualify the complete index engine
or SQL9 native runtime.

Repository and final commit metadata are recorded at closeout after the
accepted source and report are committed locally.
