# NDB1R4P11R3T — Portable Index Header Boundary

## Outcome

**Outcome A — PORTABLE_INDEX_HEADER_BOUNDARY_VERIFIED.** The production index
key-byte operations now have one freestanding-facing header, with their
existing implementations used by hosted adapters. The bounded interface
compiles and links under the Server AMD64 freestanding flags without STL,
`DbValue`, exceptions, RTTI, allocation, or unresolved symbols. Existing
hosted signatures and key bytes are retained.

This verifies the index-key byte boundary only. The complete
`database_index.cpp` translation unit remains hosted.

## Repository and accepted baseline

- Repository: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `ed2affaf19a0be1e55886892236ac75e260638c7`, as requested.
- Initial worktree: clean; local `main` was five commits ahead of
  `origin/main`.
- `.phase`: `last_completed=SQL9`, `next_expected=SQL10`.
- `.native-sql.phase`: `last_completed=NDB1R4P10`,
  `next_expected=NDB1R4P11`.
- The accepted R3S report records **23/23 Debug CTest targets and 161,611
  checks**, including R3S 121, R3R 51, R3Q 188, R3P 10,716, and portable
  runtime 20,912. Its freestanding census is **18/36**.
- Before changes, the focused Debug rerun passed the index target (2,940
  checks, including R3Q), R3R (51), R3S (121), R3P (10,716), and portable
  runtime (20,912). The unchanged Server AMD64 census script again reported
  18/36, with `database/database_index.cpp` blocked at
  `database/database_index.h:27` on `<string>`.
- The census compiler is MinGW-W64 `g++ 15.2.0`. The script retains its
  established C++14 Server AMD64 flags, including `-ffreestanding`,
  `-nostdlib`, `-nostdinc++`, `-fno-exceptions`, and `-fno-rtti`.

## Header and type dependency audit

The existing hosted header is not made freestanding by removing one include.
Its dependency graph is:

```text
database/database_index.h
├── <string>                         IndexValidation::message,
│                                     validateNode(..., std::string&)
├── <vector>                         encoded keys, entries, scan results,
│                                     children, bounds, and private plans
├── database_format.h                fixed-width format types/constants
├── database_heap.h                  <memory>, <vector>, relational/schema/value
├── database_pagestore.h             DatabasePage and DbResult
├── database_result.h                <string>, DbResult diagnostics
├── database_schema.h                 <string>, <vector>, database_types.h
└── database_types.h                  <string>, <vector>, DbValue::Text/Blob
```

`database_index.h:27` is the first freestanding compiler failure. The next
direct standard-library include is `<vector>` at line 28. The included heap,
schema, result, page, and value headers add further hosted dependencies.
`database_index.cpp` also uses hosted algorithms/math/string memory helpers,
transaction interfaces, and hosted page/index operations. Its consumers
include `database_heap.cpp`, `database_relational.cpp`, and
`database_sql.cpp`, as well as the existing index, SQL, transaction, and
storage tests. These dependencies are why the full `IndexTree` declaration
and implementation remain in the hosted header/source.

The independently usable key boundary has this include graph:

```text
database/portable/gx_index_key.h
└── gx_index_text_key.h
    ├── compiler <stddef.h>
    ├── compiler <stdint.h>
    └── gx_status.h
```

It has no transitive hosted header, STL container, `DbValue`, `DbResult`, page,
transaction, or allocator dependency. The freestanding compile probe's `-H`
trace contains only this local header chain and compiler-provided C integer
headers; `-nostdinc++` is also active.

### Declaration classification

| Existing index declaration | Dependency and treatment |
|---|---|
| `encodeIndexKey` / `decodeIndexKey` | Hosted `DbValue` and `std::vector` APIs retained with their original signatures. Their Text byte paths continue to call the portable Text encoder/decoder. Other types stay hosted. |
| `encodedKeyIsNull` / `compareEncodedKeys` | Hosted pointer/length wrappers retained with their original names and signatures. Their byte predicate and comparator now call portable production functions. |
| `IndexEntry`, `IndexRangeBound`, `IndexValidation` | Hosted vector, `DbValue`, and string results retained. |
| `IndexTree` and private traversal/planning declarations | Hosted `PageAccess`, `DbResult`, transaction, vector, and diagnostic string dependencies retained. No traversal or storage ownership moved. |
| `GxStatus`, `GxTextByteView`, `uint8_t`, `size_t` | Safe shared types for the standalone portable boundary. `GxTextByteView` remains borrowed and Text-specific. |

## Selected portable interface

`database/portable/gx_index_key.h` is the production-facing key-byte header.
It reuses the existing `gx_index_text_key` declarations and implementation,
and adds two byte-level operations that were already production operations in
`database_index.cpp`:

- `gxIndexKeyIsNull` checks for the exact one-byte `{0x00}` key and reports
  failure through `GxStatus`.
- `gxCompareIndexKeys` compares explicit pointer/length spans as unsigned
  lexicographic bytes, with a shorter equal prefix sorting first. The result
  is explicitly normalized to `-1`, `0`, or `1`.
- `gxIndexTextKeyEncodedSize` and `gxEncodeIndexTextKey` retain the existing
  allocation-free `0x01 || textBytes` encoder and checked size result.
- `gxDecodeIndexTextKey` retains the existing allocation-free decoder for
  exact `{0x00}` NULL and `0x01 || textBytes` Text keys.

These operations accept caller-owned byte spans and do not allocate. Invalid
nonzero spans with null pointers return `GxStatus::InvalidArgument`; output
slots are cleared on failure. The generic comparison and NULL predicate do
not validate non-NULL type-specific key payloads; they preserve their existing
byte comparison and exact NULL-predicate meanings.

`GxTextByteView` owns no bytes. A decoded Text view points into the encoded
input, including when its length is zero; the caller must keep the input alive
and unchanged for the view's use. Embedded zero bytes are ordinary payload.
An exact `{0x00}` key yields `outIsNull=true` and an empty null view. `{0x01}`
yields an empty Text view (`data == encodedBytes + 1`, length zero), distinct
from NULL. Text keys have no payload length field, so a shorter supplied
suffix is a valid shorter Text key and cannot be identified as a truncated
version of another key.

No portable encoder or decoder was duplicated. The existing
`gx_index_text_key.cpp` production unit now implements the byte predicate and
comparator too; the existing hosted wrappers delegate to these operations.

## Hosted compatibility

The declarations in `database/database_index.h` were not moved or changed.
Hosted callers keep `DbValue` conversion and `std::string` support. Text
encoding still emits exactly `0x01 || raw bytes`; Text decoding copies the
borrowed portable view into an owning `DbValue::Text` only at that hosted
boundary. NULL, empty Text, embedded zero bytes, and malformed Text-key
behavior are unchanged. Non-Text value decoding and all tree traversal,
comparison order, mutation, page ownership, WAL, recovery, and SQL behavior
remain unchanged.

The production archive defines the established hosted symbols
`encodeIndexKey`, `decodeIndexKey`, `encodedKeyIsNull`, and
`compareEncodedKeys`. R3T exercises all four through the hosted API. The
portable hosted adapters produce the same normalized lexicographic ordering
and exact NULL result for valid encoded keys.

## Production boundary probe

Added `tests/freestanding/gx_index_key_boundary_probe.cpp`. It includes only
`gx_index_key.h` and uses the production encoder, decoder, NULL predicate, and
comparator. Its stack-owned encoded buffer remains live through view use; it
checks that the borrowed view aliases the suffix and observes a byte change
to that buffer. It has no `DbValue` or standard-library include.

`tools/qualify_sql9_freestanding.ps1` compiles this probe using the established
Server AMD64 freestanding flags, then relocatably links it with the actual
production `gx_index_text_key.cpp` object. `nm` verifies definitions for all
four portable symbols and reports no unresolved symbols. The probe is
separate from the production translation-unit census, so the established
36-unit census denominator is unchanged.

## Regression qualification

Added the R3T target in `CMakeLists.txt` and a three-translation-unit focused
test. Two independent consumers include the portable header and call its
linked production functions. The 52 R3T checks cover:

- portable NULL predicate and unsigned, prefix-sensitive byte ordering;
- invalid pointer spans and output failure results;
- portable/hosted Text encoding byte equality;
- embedded-zero round trip and borrowed-view aliasing;
- empty Text versus NULL, malformed tags/shapes, and the key format's
  undetectable-shorter-suffix rule;
- hosted Text and NULL decoding, failure preservation, and the legacy hosted
  comparison and NULL wrappers;
- inclusion and linkage from more than one translation unit.

The full Debug CTest result and focused counts are recorded after the
qualification run below.

## Debug CTest results

The full Debug CTest run passed **24/24 targets**, **161,663 checks**, with
zero failures and zero skipped targets. The run completed in **1,202.83
seconds**. This is the accepted 23-target, 161,611-check baseline plus R3T's
52 checks.

Focused results from the final run:

| Qualification | Checks | Result |
|---|---:|---|
| Existing index target (includes R3Q) | 2,940 | pass |
| R3Q cycle-membership focused mode | 188 | pass, 0 failed |
| R3R portable Text encoder | 51 | pass |
| R3S portable Text decoder | 121 | pass |
| R3T portable index-key header | 52 | pass |
| R3P heap/page construction | 10,716 | pass |
| Portable runtime | 20,912 | pass |

All other Debug CTest targets also passed; no target was skipped or removed.

## Freestanding census

The unchanged production source list and Server AMD64 flags were repeated
before and after the change. The final production census remains **18/36
passing, 18/36 blocked**. All 18 prior passes are preserved. The new separate
boundary probe compiles and links successfully, resolves all four portable
symbols, and has no undefined symbols. The production
`database/portable/gx_index_text_key.cpp` also compiles with an empty undefined
symbol list.

`database/database_index.cpp` remains blocked at
`database/database_index.h:27` on `<string>`; it did not progress past that
dependency. The remaining 18 first failures are:

| Translation unit | First hosted dependency |
|---|---|
| `database/database_io.cpp` | `database_io.h`: `<cstdio>` |
| `database/database_native_io.cpp` | `database_native_io.h`: `<memory>` |
| `database/database_header.cpp` | `database_header.h`: `<string>` |
| `database/database_page.cpp` | `database_page.h`: `<vector>` |
| `database/database_file.cpp` | `database_file.h`: `<memory>` |
| `database/database_engine.cpp` | `database_engine.h`: `<memory>` |
| `database/database_buffer.cpp` | `database_file.h`: `<memory>` |
| `database/database_catalog.cpp` | `database_catalog.h`: `<string>` |
| `database/database_index.cpp` | `database_index.h:27`: `<string>` |
| `database/database_heap.cpp` | `database_heap.h`: `<memory>` |
| `database/database_wal.cpp` | `database_wal.h`: `<memory>` |
| `database/database_transaction.cpp` | `database_transaction.h`: `<memory>` |
| `database/database_relational.cpp` | `database_relational.h`: `<memory>` |
| `database/database_sql_token.cpp` | `database_sql_token.h`: `<string>` |
| `database/database_sql_tokenizer.cpp` | `database_sql_tokenizer.h`: `<string>` |
| `database/database_sql_parser.cpp` | `database_sql_parser.h`: `<vector>` |
| `database/database_sql_expr.cpp` | `database_sql_expr.h`: `<vector>` |
| `database/database_sql.cpp` | `database_sql.h`: `<map>` |

The complete index translation unit is not claimed portable. Its next direct
header dependency after `<string>` is `<vector>`, and its public `DbValue`,
`IndexValidation`, result, entry, and traversal surfaces remain hosted.

## Recommended next bounded phase

Move the remaining Boolean, Int32, Int64, and Float64 logical-key byte
encoding/decoding into this same explicit pointer/length portable boundary.
Keep the current hosted `DbValue` APIs as adapters and qualify byte-for-byte
ordering and malformed-input behavior. Handle validation diagnostics and
vector-based tree/traversal interfaces in later, separately bounded work.

## Closeout

- Starting HEAD: `ed2affaf19a0be1e55886892236ac75e260638c7`.
- Ending implementation HEAD: `da78060497076a9be6c1704ab310df6eae93a666`
  (`Implement portable index key boundary`).
- Full Debug CTest: 24/24 targets; 161,663 checks; 0 failures; 0 skipped.
- Freestanding census: 18/36 production units, with the separate boundary
  probe compiled and linked successfully.
- This qualification report is committed as a separate local closeout.
- Phase markers remain SQL9→SQL10 and NDB1R4P10→NDB1R4P11.
- No push, history rewrite, stash, or phase-marker change was performed.
