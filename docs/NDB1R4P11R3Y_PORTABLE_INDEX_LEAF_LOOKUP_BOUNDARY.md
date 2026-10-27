# NDB1R4P11R3Y — Portable Index Leaf Lookup Boundary

## Outcome

**Outcome A — `PORTABLE_INDEX_LEAF_LOOKUP_VERIFIED`.** The production leaf
reader and physical-key comparator now provide a freestanding-compatible,
read-only physical lower-bound lookup over caller-owned leaf bytes. Hosted
`IndexTree::lookupAll` reuses that operation and continues through the existing
leaf chain to preserve duplicate logical-key results.

## Repository baseline and closeout

- Repository: `D:\dev\guideXOS_SqlDatabase`.
- Branch: `main`.
- Starting HEAD: `9f761c0fd19b8e15209e037cd3cda71973c7806d`.
- Starting worktree: clean.
- Actual local ref comparison: `HEAD` equals the locally recorded
  `origin/main` (`git rev-list --left-right --count origin/main...HEAD` was
  `0 0`). The supplied task description expected one commit ahead; no fetch,
  reset, rebase, stash, or overwrite was used to reconcile that difference.
- `.phase` remained SQL9 → SQL10. `.native-sql.phase` remained NDB1R4P10 →
  NDB1R4P11.
- The R3X report baseline was 28/28 CTest targets, 162,749 assertions, and a
  22/40 freestanding census.
- Ending implementation HEAD: `7a903edbba3e9516d0c475a3315b7a9c305015a3`.
- This qualification report is committed in a documentation-only closeout
  commit directly above the implementation HEAD.
- No push was performed.

## Existing hosted lookup audit

`IndexTree::lookupAll` in `database/database_index.cpp` first copies the
requested logical key and appends the existing minimum row locator `(0, 0)` in
big-endian physical-key order. `findLeaf` descends the internal-node lower
bounds to the candidate leaf. In each leaf, the old implementation decoded
entries in slot order, compared logical keys linearly, appended equal entries,
and stopped when a greater logical key appeared. It follows `nextLeaf` so a
duplicate logical-key run split across leaves remains visible. Missing keys
return success with an empty output vector.

The leaf scan is linear, not binary. Internal child selection is also a linear
scan of the established lower-bound array. The new operation preserves the
leaf scan's first-candidate decision; it does not replace the tree descent or
implement a whole-tree search.

Logical keys are compared as their existing encoded byte spans. A physical key
is those bytes followed by the 12-byte big-endian numeric row locator. Leaf
records store the logical-key bytes and locator fields separately, with the
locator fields little-endian on disk; the production physical comparator
constructs the comparison view without changing that representation. Exact
physical identity therefore includes both locator fields. Equal logical keys
with distinct locators remain separate and ordered by locator. Existing
insert/remove checks use both logical key and locator for exact duplicate or
removal behavior; those write paths were not changed.

Before R3Y, hosted equality lookup relied on increasing physical order to stop
at the first greater logical key, but it did not call the full leaf validator
on that read path. R3Y validates the page's slot bounds, non-overlap, and
strict physical ordering before it returns a candidate. Corrupt order now
returns the existing hosted `DbStatus::CorruptPage` rather than producing a
partial or misleading result.

## Selected contract and portable API

`gxIndexLeafLookup` takes a complete physical-key byte span and explicit
lengths for that span and the page payload. It returns a
`GxIndexLeafLookupResult` containing:

- `hasEntry`: whether a lower-bound candidate exists in this leaf.
- `entryPosition`: candidate slot position, or `entryCount` at valid
  end-of-leaf.
- `entry`: borrowed candidate view when `hasEntry` is true.
- `exactMatch`: equality of the complete physical key, including the 12-byte
  locator suffix.
- `continuation`: a cursor immediately after the candidate, or at end-of-leaf.

`GxStatus::Ok` with `exactMatch == false` and a candidate means a valid
insertion/lower-bound position; `GxStatus::Ok` with `hasEntry == false` means
valid end-of-leaf. A missing exact key is therefore distinct from malformed
input, which returns `InvalidArgument` or `CorruptData` with a specific
`GxIndexLeafError`. The result is cleared on ordinary failure. Output objects
are checked against the input spans to prevent output aliasing from mutating
page or key bytes.

The operation reuses `gxIndexLeafValidate`, `gxIndexLeafCursorInit`,
`gxIndexLeafCursorReadNext`, and `gxCompareIndexPhysicalKeyViews`. It retains
the established bounded slot-order scan; it introduces no binary-search
algorithm. Its scan is bounded by the validated entry count, which is itself
bounded by the supplied page size. The operation uses no STL, `DbValue`,
exceptions, RTTI, allocation, OS services, or page mutation.

The page and all returned key views/cursors are borrowed. The caller must keep
the page bytes alive and unchanged until it discards the result and every
cursor/view derived from it. The operation retains no pointer after that
lifetime and does not null-terminate keys. A leaf-local lower bound is not a
tree-wide first match unless the caller's tree descent establishes that
property.

## Hosted production integration

`IndexTree::lookupAll` now calls `gxIndexLeafLookup` on each page reached by
the existing `findLeaf` and sibling chain. It starts from the physical probe
formed with logical key plus locator `(0, 0)`, evaluates the candidate and
subsequent cursor entries using the existing logical-key comparator, and
copies matching keys and locators into its existing output vector. The page
payload remains owned by the local `DatabasePage` while each borrowed view is
consumed; no borrowed view escapes the iteration.

The integration retains outer page-type and owner-index checks in `readLeaf`,
the page-count and cycle checks on the leaf chain, the existing missing-key
result, duplicate locator ordering, and `lookupFirst` behavior through
`lookupAll`. It adds no writes and changes no page, transaction, locking,
recovery, or publication contract. Range scans, insertion, removal, splits,
merges, balancing, and page allocation remain hosted and unchanged.

## Behavioral equivalence and focused qualification

The R3Y regression target loads the writer-produced R3X leaf fixture. It
compares the new candidate and hosted `lookupAll` results with the prior
slot-order logical-key scan, including two equal Text keys with different
locators. It also verifies that hosted lookup reports a reordered page as
corrupt.

The focused target covers empty, single-entry, and multiple-entry leaves;
exact first/middle/last matches; missing keys before, between, and after
entries; lower-bound and end positions; repeated lookups; NULL, Boolean,
Int32/Int64 boundary encodings, empty Text, and embedded-zero Text; duplicate
logical keys with distinct row locators; exact physical equality; and physical
ordering. Malformed cases cover truncated page/entry data, invalid offsets,
oversized declared lengths, missing/truncated locator bytes, integer-overflow
attempts, incorrect entry ordering, invalid input spans, output-state clearing,
and output/page alias rejection. Input bytes are checked for non-mutation.

- R3Y focused target: **160 checks passed, 0 failed** after the final
  short-physical-key rejection case was added.
- R3X focused target: **196 checks passed, 0 failed**.
- R3W, R3V, R3U, R3T, R3S, and R3R focused targets passed **39, 232, 619,
  52, 121, and 51** checks respectively.
- R3Q passed **188** focused checks inside `database_index_test`; R3P passed
  **10,716** focused checks inside `database_heap_page_construction_test`.
- Portable runtime regressions: **20,912 checks passed, 0 failed**.
- Hosted-versus-portable equivalence: **passed** against the existing ordered
  scan and the production `IndexTree::lookupAll` path.

## Production freestanding proof and census

The existing production census uses 40 translation units and unchanged AMD64
C++14 flags, including `-ffreestanding -nostdlib -nostdinc++ -fno-exceptions
-fno-rtti`. The new operation is in the already-counted
`gx_index_leaf_entry.cpp`; the census remains **22/40 passing**, preserving all
22 previous passes. `database/database_index.cpp` remains the first index
translation-unit blocker at `database/database_index.h:27` because it includes
hosted `<string>`.

The production probe compiles the real portable lookup source, links it with
the real key and leaf production objects using `ld -r --no-undefined`, and
executes exact-match, valid missing/end-of-leaf, continuation, and malformed
page cases against serialized bytes. The linked object has no unresolved
symbols. The probe checks all previously qualified portable key, comparator,
cursor, and validator symbols plus `gxIndexLeafLookup`. It requires no hosted
`DbValue` or STL runtime.

The census passed **22/40** units and retained all prior passing units. The
leaf translation unit still has only its established undefined reference to
`gxCompareIndexPhysicalKeyViews`; the production probe resolves it from the
real comparator object and its final linked object has no undefined symbols.

## Qualification results

Full Debug CTest passed **29/29 targets**, with **162,906 assertions**,
**zero failed targets**, and **zero skipped targets**. CTest's total elapsed
time was **1,185.24 seconds**. The final R3Y target added three more input
validation checks after that full aggregate. At the final source state, the
full-suite equivalent is **162,909 checks** (162,906 aggregate checks plus
those three checks). The post-change focused reruns passed R3Y at **160
checks** and `database_index_test` at **2,940 checks**. The index target
includes its R3Q checks and existing point lookup, duplicate-key, range-scan,
insertion, corruption, and recovery regressions.

## Sanitizer availability

`SANITIZER_NOT_RUN — TOOLCHAIN_UNAVAILABLE`. The installed MinGW toolchain's
ASan/UBSan runtimes were unavailable in R3X (`ld` could not find `-lasan` or
`-lubsan`), and this host has no alternate `clang++` or MSVC compiler exposed
for a compatible sanitizer run. The new explicit bounds, ordering, alias, and
malformed-input tests remain mandatory and passed in the normal Debug build.

## Remaining architecture and recommended next phase

Portable index operations now include the existing NULL predicate, Boolean,
Int32/Int64 and Text key codecs, logical-key comparison, physical-key
comparison, leaf cursor traversal, leaf validation, and leaf-local physical
lower-bound lookup. Float64 remains outside the freestanding key codec
boundary.

The full B+ tree remains hosted. Internal-node decoding and descent, page
acquisition through `PageAccess::readPage`, catalog identity checks, and
cross-page traversal still use `DatabasePage` and STL containers. PageAccess
returns an owned `DatabasePage` whose payload is a vector; there is no portable
stable pin/view provider yet. A multi-page read operation must preserve page
type, index-owner, page-count, and depth checks and must read through the
correct transaction overlay so read-your-writes and recovery boundaries remain
consistent. The database index header and source also depend on hosted
`std::string`, `std::vector`, and `DbValue` APIs.

The next bounded production milestone should be **portable read-only B+ tree
traversal across pages**, beginning with a caller-owned page-view/provider
contract and a portable internal-node child/lower-bound reader. Its first
consumer should perform one bounded descent and then reuse this leaf lookup;
page ownership and transaction-overlay behavior should stay at an explicit
host adapter until a stable view lifetime is defined. Float64 should remain
deferred until a separate portable binary64 encoding, ordering, and
non-finite-value contract is qualified.
