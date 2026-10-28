# NDB1R4P11R3AA — Portable Index Internal-Node Descent

## Result

**Outcome A — `PORTABLE_INDEX_INTERNAL_NODE_DESCENT_VERIFIED`**

Starting branch and HEAD: `main`, `bde7bcc847ddad0304d0be01485878e453baae5d`. The starting worktree was clean and four commits ahead of `origin/main`. `.phase` and `.native-sql.phase` were unchanged. The implementation and qualification commit is `c0d19e7`; this report is a documentation-only follow-up, and no implementation source changed after qualification. No push, reset, rebase, stash, or history rewrite was performed.

The existing SQL6 B+ tree format and writer are unchanged. A freestanding reader validates existing internal-node payloads, bounded iterative descent handles internal or leaf roots, and hosted `IndexTree::lookupAll` uses it before the existing portable cross-leaf lookup.

## Production format and routing contract

The catalog supplies the root page id; page id zero is reserved, and valid ids are in `[1, pageCount)`. An empty index uses a leaf root. Each outer database page has its normal 32-byte header. The provider supplies its payload together with the persisted page type: `IndexLeaf=3` or `IndexInternal=4`.

An internal payload has a 24-byte little-endian header:

| Offset | Encoding | Meaning |
| ---: | --- | --- |
| 0 | `u32` | `GXIN` magic (`0x4E495847`) |
| 4 | `u16` | version (`1`) |
| 6 | `u16` | reserved; the hosted reader does not require a particular value |
| 8 | `u64` | reserved; the hosted reader does not require a particular value |
| 16 | `u32` | child count |
| 20 | `u32` | owner index id |

Child records grow forward from payload offset 24. Each record contains `u64 childPageId`, `u32 separatorLength`, then separator bytes. Each 8-byte slot (`u32 recordOffset`, `u32 recordLength`) grows backward from the payload end; slot `i` is at `payloadLength - 8 * (i + 1)`. Separators are complete encoded physical keys, including the 8-byte page id and 4-byte slot locator. The first bound may be empty (negative infinity) or finite: a production split may create a non-root right-half node with a finite first bound. Later bounds are nonempty.

The valid child count is at least two and at most `floor((payloadLength - 24) / 20)`, accounting for an 8-byte slot and 12-byte minimum child record. For a 512-byte page the payload is 480 bytes and the maximum count is 22. Child ids must be in `[1, pageCount)` and distinct within the node. Separators must be strictly increasing under the full physical-key ordering. The portable reader checks header, count, owner, slot and record bounds, directory and record overlaps, record lengths, the required 12-byte physical locator suffix, key-length bounds, child-id domain, repeated children, and separator ordering. It ignores reserved header bytes as the hosted reader does.

Routing preserves `IndexTree::chooseChild`: default to child 0; for children `i >= 1`, select the rightmost separator `<=` the complete physical search key; equality routes right. Child 0 remains the default even when its finite bound is greater than the probe. The reader uses the existing portable physical-key comparator. R3AA also compares decisions against the pre-extraction hosted `compareEncodedKeys` route at separators in production root and next-level nodes, including equality and adjacent locator probes.

`lookupAll` appends `RowLocator(0, 0)` to the encoded logical key before descent and leaf lookup. This minimum physical locator positions the search before valid rows for that key. The leaf-chain operation then visits all matching logical keys across successor leaves, preserving locator identity and ordering.

## Portable interfaces, ownership, and bounds

`database/portable/gx_index_internal_node.h` exposes `gxIndexInternalNodeCursorInit` (full-node validation), `gxIndexInternalNodeCursorReadNext` (borrowed child/separator view), and `gxIndexInternalNodeSelectChild` (production route selection). The cursor borrows the caller's read-only payload; the bytes must remain unchanged until the cursor and its views are discarded.

`database/portable/gx_index_tree_descent.h` exposes `gxIndexTreeDescendToLeaf`. It accepts a provider, root id, page count, owner id, complete physical key, depth limit, caller-owned visited-id span, and result. It validates provider-supplied page id/type, internal pages, and the terminal leaf; it returns leaf id, page count, and depth, never a borrowed pointer.

Descent is iterative, allocation-free, and nonrecursive. `maxDepth` counts edges, is capped at `kMaxIndexDepth` (32), and requires `maxDepth + 1` scratch ids. A root leaf succeeds at depth zero; the maximum path is 33 pages. Repeated ids return `Cycle`; exhausting the depth bound returns `TraversalLimit`, not successful not-found. Argument, page-id, missing-page, provider, page-type, format, cycle, and depth errors have explicit statuses/details.

A successful acquire creates one borrowed view, stable until release. The provider may reuse the same buffer on the next acquire. Descent releases every successful borrow before another acquire and on every exit; it retains only ids in caller scratch. The portable operations do not allocate, mutate pages, or use STL, `DbValue`, exceptions, RTTI, or hosted runtime services.

## Hosted integration and behavior evidence

The hosted adapter still owns `DatabasePage`, reads through the existing synchronous `PageAccess` and transaction overlay, and validates identity, persisted type, and payload size. `lookupAll` calls portable descent with a fixed `kMaxIndexDepth + 1` path buffer, then calls `gxIndexLeafLookupChain` with the existing hosted `GxPageVisitSet` for leaf-chain cycle reporting. The adapter releases each page borrow, preserves the original storage error, and maps portable errors to existing `DbResult` categories.

Other existing `findLeaf` callers remain for compatibility insert, remove, and range lower-bound navigation; those flows were not migrated. `lookupAll` no longer uses hosted root navigation. No range-chain migration, internal-node mutation, page writes, splits, merges, balancing, WAL/recovery, SQL, or Server-repository changes were made.

The dedicated R3AA test covers an empty leaf root, one and multiple internal levels, child positions and separator boundaries, misses, NULL/Boolean/Int32/Int64/Text encodings, embedded-zero Text, provider and depth failures, cycles, malformed headers/records/count/order/id/type, buffer reuse, release behavior, and read-only payloads. Its production-written tree has at least three levels and 420 entries: 160 keys below a 100-row duplicate run and 160 above. It compares selected leaf ids to an independent legacy hosted route and compares full `lookupAll` results and locators with pre-extraction behavior. The duplicate run spans leaves.

`tests/fixtures/NDB1R4P11R3AA_production_internal_tree.hex` is generated by the actual `IndexTree` writer. Its header is `512 767 37 615 100 107`: page size 512, page count 767, owner 37, root 615, duplicate count 100, and 107 serialized path/duplicate-chain pages. Each serialized payload is 480 bytes.

## Qualification

| Qualification | Direct result |
| --- | ---: |
| R3AA portable internal descent | 4,018 passed, 0 failed |
| R3Z portable cross-page traversal | 4,186 passed, 0 failed |
| `database_index_test` (including R3Q coverage) | 2,940 passed, 0 failed |
| R3Y / R3X | 160 / 196 passed, 0 failed |
| R3W / R3V / R3U / R3T / R3S / R3R | 39 / 232 / 619 / 52 / 121 / 51 passed, 0 failed |
| R3Q / R3P | 188 / 10,716 passed, 0 failed |
| Portable runtime regressions | 20,912 passed, 0 failed |
| Full Debug CTest | 31/31 targets passed, 0 failed, 0 skipped |

The final Debug aggregate took 800.61 seconds. R3Z's source-equivalent total was 167,095 checks. Adding the R3AA target's 4,018 checks gives **171,113 full-suite-equivalent checks**. This total is calculated; CTest reports target results and elapsed time, not a grand assertion count.

The unchanged freestanding census configuration was extended from 41 to 43 production translation units. It reports **25/43 passing**, preserving all 23 previously passing units; both new production sources compile. The first remaining `database_index.cpp` blocker is `database/database_index.h:27:10: fatal error: string: No such file or directory` under `-nostdinc++`. The same SQL9 script flags were retained: C++14 AMD64 freestanding flags including `-ffreestanding -nostdlib -nostdinc++ -fno-exceptions -fno-rtti`, the architecture flags, and the existing include roots.

The production proof compiles the actual internal reader and descent, then links them with the actual physical comparator, leaf-entry reader, leaf validator, and leaf-chain traversal using `ld -r --no-undefined`. `nm` reports no unresolved symbols and confirms the internal cursor, child selector, tree descent, leaf validation, and leaf-chain symbols. The freestanding-compiled operation executes through a test provider over the production-writer fixture and checks multilevel descent, cross-leaf duplicates, invalid child ids, provider failure, and release counts. The hosted runner only loads fixture bytes and invokes the compiled probe; it is not part of the freestanding production object.

`SANITIZER_NOT_RUN — TOOLCHAIN_UNAVAILABLE`. MinGW 15.2.0 has no ASan/UBSan libraries, and neither `clang++` nor MSVC `cl` is installed. No toolchain was downloaded.

## Remaining boundary and next milestone

Portable root-to-leaf navigation now exists and composes with portable leaf-local and cross-page lookup. `lookupAll` no longer owns the separator decision. Hosted code still encodes typed keys, supplies `PageAccess` and transaction-consistent page views, maps storage errors, and materializes its public `std::vector`; write and range callers retain their existing paths. Float64 migration is not needed for this descent boundary, but freestanding lookup of Float64 indexes remains unavailable until that codec exists.

The next useful production boundary is a bounded end-to-end portable read-only lookup that composes descent and leaf-chain lookup around one provider, encoded logical key, traversal scratch, and match visitor. Keep transaction-consistent page views and error translation in the hosted adapter. A Server adapter would need to supply catalog root/page-count/owner metadata, existing encoded keys, and stable provider borrows/releases within its transaction snapshot, then consume row locators without adding hosted dependencies to the portable objects. Keep Float64 migration and write-side page mutation separate.
