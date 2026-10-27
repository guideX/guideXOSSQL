# NDB1R4P11R3X — Portable Index Leaf Entry Traversal

## Outcome

**Outcome A — `PORTABLE_INDEX_LEAF_ENTRY_TRAVERSAL_VERIFIED`.** The existing
leaf payload now has a freestanding-compatible borrowed-byte cursor and
structural/order validator. Hosted index reads, split planning, transaction
preflight, and structural validation reuse the production cursor or validator.
The existing page writer, persistent format, page ownership, and transaction
publication path remain hosted and unchanged.

## Repository baseline and closeout

- Repository: `D:\dev\guideXOS_SqlDatabase`.
- Branch: `main`.
- Starting HEAD: `71b3adca4996a643cb1a3ba868001146f27d4e9b`.
- Starting worktree: clean.
- `.phase` remained SQL9 → SQL10. `.native-sql.phase` remained NDB1R4P10 →
  NDB1R4P11.
- The starting qualification baseline was 27/27 CTest targets and 162,553
  checks. The freestanding census was 21/39.
- The ending HEAD is the local R3X closeout commit containing this report; its
  exact hash is recorded in the task closeout and Git metadata.
- No reset, rebase, stash, history rewrite, or push was performed.

Before changing the read path, the original `database_index_test` and R3W
focused target passed (2/2). The existing corruption cases exercised wrong
page type, leaf signature, leaf version, entry count, slot offset, and owner
index. A 480-byte leaf payload was also captured from the starting revision's
hosted `IndexTree::writeLeaf`; it is stored as
`tests/fixtures/NDB1R4P11R3X_hosted_leaf_512.hex` and used as a golden fixture.

## Existing leaf-page format

The ordinary 32-byte database-page header is outside the supplied payload
span. For a supported database page size of 512 through 65,536 bytes, the leaf
payload is 480 through 65,504 bytes.

The 24-byte leaf header is serialized little-endian:

| Payload offset | Field | Encoding |
|---:|---|---|
| 0 | Leaf signature `GXIL` | u32 |
| 4 | Leaf page version (`1`) | u16 |
| 6 | Reserved flags | u16; the hosted reader has historically tolerated any value |
| 8 | Next leaf page ID | u64 |
| 16 | Entry count | u32 |
| 20 | Owner index ID | u32 |

Each inline entry is `u32 keyLength`, `keyLength` logical-key bytes, `u64
rowPageId`, and `u32 rowSlot`. The entry overhead is 16 bytes. The row locator
fields in the page are little-endian. The physical ordering key is formed for
comparison from the logical-key bytes followed by the existing 12-byte
big-endian numeric locator suffix; those big-endian bytes are not stored as a
second copy in each entry. No leaf entry has an external payload reference.

The slot directory grows backward from the end of the payload. Slot `i` is an
8-byte little-endian `{u32 offset, u32 length}` record at
`payloadLength - 8 * (i + 1)`. Hosted writers pack entry records upward from
offset 24 and write one slot per entry. The slot offset, not a linked entry
chain, identifies each entry during traversal.

Before this phase, `readLeaf` checked the outer `PageType`, exact payload
capacity, signature/version, owner ID, and whether the declared slot count fit.
`readLeafEntries` then bounded each slot and entry, required at least the
16-byte entry overhead, checked the page-size-derived maximum key length, and
copied keys and locators into hosted objects. It did not check ordering;
`validateNode` separately required strictly increasing physical keys using
`compareEncodedKeys`. `readLeafPlan` and the transaction insertion preflight
had separate byte-decoding loops with the same on-page layout and additional
caller-specific checks.

## Portable view and validation contract

`database/portable/gx_index_leaf_entry.h/.cpp` adds:

- `gxIndexLeafCursorInit` to validate the bounded payload header and expose
  next-leaf ID, entry count, and owner index ID.
- `gxIndexLeafCursorReadNext` to return a borrowed key span, numeric row
  locator, entry offsets, and an explicit `hasEntry` result.
- `gxIndexLeafValidate` to traverse the page without allocation, reject
  overlapping entry regions, and check strict physical-key ordering with the
  existing `gxCompareIndexPhysicalKeyViews` comparator.

The implementation reads integers byte-by-byte, checks subtraction bounds
before forming pointers, enforces the production page-size and key-size limits,
and returns `GxStatus::CorruptData` with a specific `GxIndexLeafError` for
malformed data. A successful read with `hasEntry == false` is end-of-page;
corrupt input is a distinct error, and a failed cursor is not reusable. The
cursor advances by its validated slot index, so repeated offsets cannot cause
an unbounded traversal.

The byte span and returned key views are borrowed. The caller must keep the
page storage alive and unchanged until it discards the cursor and all returned
views. The cursor retains no pointer beyond that lifetime, takes no ownership,
does not null-terminate keys, and does not modify the page. Output objects must
not overlap the page span. The interface uses no STL, `DbValue`, exceptions,
RTTI, allocation, or hosted services.

Logical-key bytes are opaque to this operation, as they were to the existing
entry reader. Type-specific codec validation remains with the key codec and
its caller; this validator checks stored byte boundaries and physical ordering
without imposing new key canonicalization.

The portable view validates the leaf payload format. It cannot determine the
outer page type, compare the owner ID to a catalog index ID, or check a sibling
page ID against the current page count; those checks need page/store context
and remain with the hosted owner. Reserved header flags remain tolerated to
preserve the existing reader's behavior. A zero-length encoded key remains
readable by the generic cursor, while the split-planning caller retains its
existing rule that planned keys must be nonempty.

The cursor rejects an entry that crosses into the reverse slot directory. Full
validation additionally rejects overlap between two entry regions. These are
malformed layouts under the existing writer format; existing writer-produced
pages and their byte representation are unchanged. Both conditions report
`DbStatus::CorruptPage` through hosted adapters.

## Hosted production reuse

`IndexTree::readLeaf` now obtains header metadata through the portable cursor
initializer while retaining the existing outer page-type, payload-size, and
owner-index checks. The hosted `readLeafEntries` adapter iterates the portable
cursor and copies each borrowed key and locator into the same `IndexEntry`
vector as before. The transaction insertion preflight and bounded split-plan
reader also use the portable cursor; their existing uniqueness, zero-key,
allocation, and page-capacity rules remain at the hosted call sites.

`IndexTree::validateNode` calls `gxIndexLeafValidate` for physical ordering and
entry-region validation, then retains its page-count check for row locators.
The existing "index leaf entries not strictly ordered" error remains in use.
All read-only existing-leaf decoding paths now share the portable traversal
implementation. The candidate-page validation used while preparing writes is a
separate write-side check and was left unchanged. No page serialization,
transaction staging, locking, or recovery behavior changed. Borrowed key views
are copied before the local `DatabasePage` payload goes out of scope.

The golden payload contains seven writer-produced entries: NULL, Boolean,
Int32, Int64, Text with an embedded zero byte, and two equal Text keys with
different row locators. The focused test checks each borrowed key and locator
against that original fixture, verifies the successful end indication, and
checks that traversal and validation leave input bytes unchanged. A hosted
`IndexTree` fixture also verifies duplicate lookup ordering and runs both valid
and malformed pages through the production read and validator paths.

## Qualification results

The new R3X focused target passes **196 checks**. It covers empty, single, and
multiple-entry pages; first/middle/last entries; explicit end; minimum and
maximum key/entry lengths; both supported page-size boundaries; NULL,
Boolean, Int32, Int64, and binary Text keys; duplicate logical keys with
different locators; borrowed views; and deterministic non-mutating traversal.
Malformed cases cover truncated headers, bad signature/version, excessive
entry counts, short entry records, offsets and lengths outside the page,
overflow attempts, invalid key lengths, missing locator bytes, overlap with
the slot directory, overlapping entries, and incorrect physical order. Hosted
checks cover outer page type and owner-index errors.

The complete Debug aggregate passed **28/28 CTest targets**, with zero failed
or skipped targets. The checked total is **162,749 assertions**: the established
162,553-check aggregate plus the new 196-check R3X target. The index target
passed in 376.65 seconds and includes the existing lookup, scan, split,
duplicate-key, recovery, R3Q cycle (188 focused checks), and R3P split-planning
coverage (10,716 focused checks). R3R through R3W also passed in the aggregate;
their focused counts remain 51, 121, 52, 619, 232, and 39 respectively. The
portable runtime target passed 20,912 checks.

The established sanitizer option was attempted for R3X. The installed MinGW
compiler does not include its `asan` and `ubsan` runtime libraries (`ld` could
not find `-lasan` or `-lubsan`), so that executable could not link. No sanitizer
result is claimed; the normal Debug qualification passed.

## Freestanding production proof and census

The qualification script's source census now contains **40 translation
units**. The existing AMD64 C++14 flags were unchanged, including
`-ffreestanding -nostdlib -nostdinc++ -fno-exceptions -fno-rtti`. The census is
**22/40 passing**: all 21 previously passing units remain passing, and
`gx_index_leaf_entry.cpp` is the additional freestanding compile pass. Its
only undefined production symbol is the existing physical-key comparator,
which is supplied by the linked production object.

The freestanding probe compiled and executed the real leaf cursor/validator
against representative valid and malformed page bytes. `ld -r --no-undefined`
linked the actual leaf source with the production key implementations, and the
linked object had no unresolved symbols. It exports the 11 existing portable
key symbols plus `gxIndexLeafCursorInit`, `gxIndexLeafCursorReadNext`, and
`gxIndexLeafValidate`.

The census still reports `database/database_index.cpp` blocked at
`database/database_index.h:27 — <string>`. The portable leaf operation is
freestanding-ready; the full hosted `IndexTree` and database index subsystem
are not.

## Remaining boundary and recommendation

Portable index-key work now includes the existing key codecs, logical and
physical-key comparisons, and bounded leaf-entry traversal/validation over a
caller-owned payload span. It does not include page acquisition, internal-node
traversal, complete tree lookup, writes, split publication, merge/balancing,
or transaction staging.

`PageAccess::readPage` still returns a hosted `DatabasePage` with vector-owned
payload bytes. A portable multi-page operation will need a stable page-view
or pin lifetime, page-type and owner-index context, page-size/page-count
validation, and explicit transaction-overlay/read-your-writes behavior before
it can own page acquisition. Write allocation, page-image staging, and
transaction publication remain hosted. This phase changed none of those
contracts.

The next coherent production boundary is a bounded one-leaf lookup over this
validated byte view, reusing the physical comparator and preserving duplicate
locator order. Page-provider integration should follow only after the buffer
ownership and lifetime contract is explicit. Float64 migration remains
deferred pending a separate binary64 ordering and decoding contract.
