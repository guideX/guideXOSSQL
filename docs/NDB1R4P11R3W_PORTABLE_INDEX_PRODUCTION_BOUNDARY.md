# NDB1R4P11R3W — Portable Index Production Boundary

## Outcome

**Outcome A — `PORTABLE_INDEX_PRODUCTION_BOUNDARY_VERIFIED`.** The production
physical-index-key comparison now has a freestanding-compatible interface and
implementation. The hosted B+ tree calls that implementation for planned leaf
ordering and insertion descent. Its byte ordering and stored-bound prefix
behavior are preserved. This qualifies a bounded comparison operation, not the
whole B+ tree or database index subsystem.

## Repository baseline and closeout

- Repository: `D:\dev\guideXOS_SqlDatabase`.
- Branch: `main`.
- Starting HEAD: `ed32aa49fefea41c59ffa5d219add98dcda55cff`, as requested.
- Starting worktree: clean; local `main` was 11 commits ahead of
  `origin/main`. That history was retained.
- `.phase` remained SQL9 → SQL10. `.native-sql.phase` remained NDB1R4P10 →
  NDB1R4P11.
- No reset, rebase, stash, history rewrite, or push was performed. The ending
  HEAD is the local R3W closeout commit that contains this report; its hash is
  recorded in the closeout response and Git metadata.

The R3V report records the starting qualification at 26/26 CTest targets and
162,514 checks. A fresh pre-edit CTest run passed its first two targets; the
relational target took 277.93 seconds, so that duplicate pre-edit aggregate was
stopped while the third target was running. The full post-edit aggregate below
was run to completion.

## Dependency audit

### Hosted declarations and dependencies

`database/database_index.h` is still a hosted B+ tree header. Its direct
includes are `<stdint.h>`, `<string>`, and `<vector>`, plus format, heap, page
store, result, schema, and value headers.

| Dependency | Declarations or use |
|---|---|
| `std::string` | `IndexValidation::message`, the human-readable result of structural validation. |
| `DbValue` | `encodeIndexKey` / `decodeIndexKey`; lower and upper values in `IndexRangeBound`. `DbValue` is declared by `database_types.h`, which itself includes string and vector. |
| `std::vector` | Encoded key adapters; `IndexEntry::key`; range and lookup output; hosted child/path collections; page entry decode/write helpers; split and scan methods. |
| Page and result types | `PageAccess`, `DatabasePage`, `DbResult`, `RowLocator`, page format constants, and schema types. |
| Transaction and portable services | The implementation includes `database_transaction.h`, `GxArray`, `GxAllocator`, and `GxPageVisitSet` for transaction-owned split planning, temporary descriptors, and cycle checks. |

There are additional transitive hosted dependencies. `database_heap.h` brings
memory, vector, relational, result, schema, and value declarations;
`database_pagestore.h` includes page and result headers; `database_page.h`
owns a vector payload; `database_result.h` owns a string and uses utility; and
`database_schema.h` and `database_types.h` include string/vector. In the
implementation, STL algorithms and vectors, page parsing and writes, page
allocation, result-message construction, and transaction staging remain
hosted. The index layer does not write WAL records itself; its staged page
changes flow through `Transaction`, whose commit/recovery services own WAL
semantics.

### Existing operation boundaries

Already-portable key encoding, decoding, NULL recognition, and logical-key byte
comparison live under `database/portable/`. Other index operations still
depend on page or transaction ownership: leaf/internal page decoding, page
allocation, lookup traversal, split planning/publication, mutation, range scan,
and full-tree validation. The existing `chooseChild` and
`chooseChildForInsert` scans are hosted because their inputs are hosted child
vectors, even though their comparisons are byte-based.

Hosted callers include `database_heap.cpp` and `database_relational.cpp` for
row/index maintenance, foreign-key and uniqueness checks, and `IndexTree`
calls; `database_sql.cpp` encodes lookup/range probes and selects index access
paths. The direct index and heap/page-construction test suites also exercise
`IndexTree`. They continue through the hosted adapters, which now call the
portable physical comparison for the two extracted decision sites.

The smallest existing operation with production use and no storage ownership
was the comparison of a logical key plus row locator against another physical
key. The previous implementation was already used to sort borrowed leaf-plan
descriptors and to compare an internal separator bound with an insertion key.
It can be isolated without moving page traversal or changing transaction
behavior.

The current dependency path is:

```text
fixed-width types + GxStatus
        ↓
portable key codecs and physical-key comparison
        ↓
hosted database_index.cpp adapters and B+ tree algorithms
        ↓
PageAccess + DatabasePage + Transaction/GxArray
        ↓
database file, buffer/cache, catalog, and WAL commit/recovery services
```

## Portable operation contract

Added `database/portable/gx_index_physical_key.h/.cpp` with
`gxCompareIndexPhysicalKeyViews`.

- Each view is an explicit borrowed byte span, optionally followed by the
  legacy 12-byte locator suffix: an 8-byte big-endian page id and 4-byte
  big-endian slot.
- With `hasLocator == false`, the span is already a complete physical bound.
  This preserves the old comparison of stored separator prefixes, including a
  shorter stored span sorting before an equal longer key prefix.
- Comparison is unsigned lexicographic order, normalized to -1, 0, or 1.
- A null input pointer is valid only for a zero-length span. The result pointer
  is required and must not overlap either input. The result is cleared on
  failure when available.
- The function checks `keyLength + 12` before reading when a locator suffix is
  requested. It reports `GxStatus::InvalidArgument` for invalid pointers or a
  missing result pointer and `GxStatus::Overflow` for a length overflow.
- Input storage is borrowed only for the call. There is no ownership transfer,
  retained pointer, allocation, STL, `DbValue`, exception, RTTI, file, or OS
  dependency.
- The caller remains responsible for validating index entry/page bounds and
  logical-key encodings. This operation compares the supplied byte views; it
  does not parse entries or validate offsets.

## Production extraction and hosted reuse

The byte-at-offset and unsigned comparison loop was moved from the anonymous
helpers in `database_index.cpp` into the portable implementation. The hosted
helpers now adapt existing `std::vector`/`RowLocator` values to borrowed views:

1. `comparePlannedLeafEntries` calls `comparePhysicalKeyParts`, which calls
   `gxCompareIndexPhysicalKeyViews`. The transaction split planner uses that
   comparator while sorting bounded, borrowed leaf-entry descriptors.
2. `IndexTree::findLeafForInsert` calls `chooseChildForInsert`, which calls
   `compareStoredBoundToInsertKey`, which calls the same portable operation.
   This keeps insertion descent's separator decision on the extracted logic.

The wrappers receive page-bounded, validated input and respect the existing
page-format key limit; the status failures are unreachable on those production
paths. The `database_index.h` hosted API remains source-compatible and retains
its legitimate hosted dependencies. Portable clients include the new
freestanding header directly; it does not include `database_index.h` or
transitively pull in `<string>`, `DbValue`, or STL.

No physical bytes are serialized by the new function. Existing bytes remain
`logicalKey || locator`, locator ordering remains unsigned big-endian, and the
key codec, duplicate/NULL policy, page format, split decisions, transaction
publication, and WAL behavior are unchanged.

## Focused behavioral evidence

`tests/database_ndb1r4p11r3w_test.cpp` passes **39 checks**. Its fixed expected
comparisons cover NULL, Boolean, Int32, Int64, binary Text with an embedded
zero, equality, locator page/slot order, empty prefixes, a full stored bound,
short and extended raw bounds, a guarded subspan, invalid pointer/result
inputs, and checked length overflow. Truncated or otherwise unusual byte spans
are treated as raw spans by contract; the operation does not claim to validate
malformed typed keys or page-entry offsets.

The same target creates a 512-byte-page database, inserts 72 rows with
duplicate indexed Text keys inside one transaction to force leaf splits, then
checks an equality lookup selects `IndexLookup` and returns all eight matching
rows. That exercises the hosted B+ tree path using the portable comparator.
The hosted result matches the existing physical ordering contract; no expected
outcome was changed.

Focused regression results:

| Qualification | Checks | Result |
|---|---:|---|
| R3W portable physical-key operation and hosted index integration | 39 | pass |
| R3V Boolean key codec | 232 | pass |
| R3U Int32/Int64 key codecs | 619 | pass |
| R3T portable index-key boundary | 52 | pass |
| R3S Text decoder | 121 | pass |
| R3R Text encoder | 51 | pass |
| R3Q index page-visit cycles | 188 | pass |
| R3P heap/index split planning | 10,716 | pass |
| Portable runtime | 20,912 | pass |

The pre-existing hosted index and page-construction suites also ran in the full
aggregate below, retaining duplicate-key, lookup, split, separator, range,
rollback, and recovery coverage.

## Full Debug CTest aggregate

The complete Debug aggregate passes **27/27 targets**, with **162,553 checks**,
zero failures, and zero skipped targets. The previous aggregate had 26 targets
and 162,514 checks; R3W adds the new 39-check target. The total was summed from
the per-target check summaries in `build/Testing/Temporary/LastTest.log`; the
R3Q 188-check diagnostic is included inside `database_index_test` and was not
counted twice.

## Freestanding production proof and census

The production source list in `tools/qualify_sql9_freestanding.ps1` now
includes `database/portable/gx_index_physical_key.cpp`. It uses the existing
Server AMD64 C++14 freestanding flags unchanged, including
`-ffreestanding -nostdlib -nostdinc++ -fno-exceptions -fno-rtti`. The new
implementation compiles as one more census unit and has no undefined symbols
in its object.

The extended portable probe includes the real interface, exercises physical
key ordering and stored-bound equality, and still calls all ten prior key
symbols. It is compiled with the freestanding production flags, relocatably
linked with the actual Text, Boolean, Int32, Int64, and physical-comparison
production objects using `ld -r --no-undefined`, and inspected with `nm`.
The linked object has no unresolved symbols and exports all eleven production
symbols, including `gxCompareIndexPhysicalKeyViews`. A small hosted runner
executes that freestanding-compiled probe successfully; the runner does not
provide a substitute implementation.

The census is **21/39 passing**. A comparison against the R3V census artifact
confirmed that all 20 previously passing translation units remain passing;
the new physical-key implementation is the additional pass. The other 18
units retain their previous hosted blockers. In particular,
`database/database_index.cpp` still stops at
`database/database_index.h:27 — <string>`. This phase did not alter census
flags or remove hosted includes to affect the result.

The exact successful source set is the R3V 20-unit portable set—format,
checksum, allocator, allocator-hosted adapter, array, page-visit set, Boolean,
Int32, Int64, owned-page adapter, heap-page builder, recovered pages, page
cache, page codec, WAL record/scan/page image, and transaction
pages/index/overlay—plus `gx_index_physical_key.cpp`.

## Remaining hosted work and readiness assessment

R3W makes the physical key comparison operation freestanding-compatible. It
does not make entry traversal, `IndexTree`, page decoding, B+ tree lookup,
mutation, split publication, range scans, or structural validation portable.
The full hosted translation unit still depends on `database_index.h`'s STL and
`DbValue` declarations, `DatabasePage`'s vector payload, `PageAccess`,
`Transaction`, hosted results/diagnostics, and transaction-owned allocation
and staging.

The next bounded production boundary should be **leaf-entry traversal and
validation over an explicit page-byte span**. It is the next useful building
block for lookup and split planning: accept a caller-owned page view and its
length, validate slot offsets/entry extents with checked arithmetic, and expose
borrowed key/locator fields without copying or taking page ownership. It should
reuse the existing leaf layout and error decisions. Keep page acquisition,
internal-node traversal, allocation, writes, and transaction publication in
their current hosted owners until those contracts are separately established.

Future Server integration will need an explicit page-buffer lifetime/pinning
contract, page-size and owner-index validation, a read-only page provider, and
later a transaction page allocator/staging contract for mutations. Evidence
before exercising lookup in guideXOS Server should include a freestanding
compile and no-undefined link of the actual traversal implementation, hosted
fixture equivalence for entry boundaries/corruption, and an integration test
against the Server's real page source that verifies duplicate/NULL lookup,
page ownership, failure propagation, and read-your-writes without changing
persistent bytes.

Float64 key migration remains deferred. It is independent of this comparator
boundary and still needs an explicit binary64 contract for non-finite values,
negative zero, and crafted payload decoding. It should follow a defined
portable page/index boundary rather than precede it.
