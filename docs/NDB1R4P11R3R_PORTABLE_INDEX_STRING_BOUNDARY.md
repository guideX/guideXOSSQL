# NDB1R4P11R3R — Portable Index String Boundary

## Outcome

**Outcome A — PORTABLE_INDEX_STRING_BOUNDARY_VERIFIED.** The byte emission for
Text logical index keys now runs through a freestanding, allocation-free,
length-aware operation. The hosted `encodeIndexKey` API remains available, the
encoded bytes and ordering are retained, all focused and aggregate hosted
regressions pass, and the freestanding census keeps every prior pass while
adding the new portable translation unit.

This verifies Text key byte emission only. The complete `database_index.cpp`
translation unit remains hosted and is not claimed as freestanding.

## Repository and phase audit

- Repository: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `c2975b60bf47a0ece2f6bc15159b209729fb069a`, as requested.
  The checkout was clean before work began.
- At initial inspection, local `main` was two commits ahead of its stale
  `origin/main` tracking ref. During this phase the remote tracking ref moved
  to `c2975b60bf47a0ece2f6bc15159b209729fb069a` at 12:08 PDT with reflog
  reason `update by push`; local HEAD did not move. No push was issued as part
  of this phase.
- `.phase` remains `last_completed=SQL9`, `next_expected=SQL10`.
- `.native-sql.phase` remains `last_completed=NDB1R4P10`,
  `next_expected=NDB1R4P11`.
- The read-only Server reference remains on branch `NATIVE_DB_STORAGE` at
  `f29850c93bbac0a0763a31e806d2816310156575`.
- The compiler used by the census is MinGW-W64 `g++ 15.2.0`, with the same
  Server AMD64 flags and source list policy used in R3Q.
- Pre-change `database_index_test` passed 2,940 checks, including 188 R3Q
  cycle and allocation-failure checks.

## Dependency investigation and selected boundary

`database/database_index.h` directly includes `<string>` and `<vector>`.
`IndexValidation::message` and the private `validateNode(..., std::string&,
...)` declaration use `std::string`. The header also exposes hosted vectors in
the key, entry, scan, and child APIs. Its direct `database_types.h` dependency
defines `DbValue::Text` as `std::string` and `DbValue::Blob` as
`std::vector<uint8_t>`.

The production index implementation's string uses were:

| Operation | Original use | Role |
|---|---|---|
| `encodeIndexKey` for Text | Reads `DbValue::textValue()` and appends its `std::string` iterators after the `0x01` tag. | The emitted bytes are production-critical: they define persisted key ordering and equality. The `DbValue` string is a hosted value container. |
| `decodeIndexKey` for Text | Constructs a `std::string` from the encoded byte span and returns it in `DbValue`. | Hosted value construction; decoding is a separate operation and remains deferred. |
| `IndexTree::validate` / `validateNode` | Builds and copies diagnostic reasons through `std::string`. | Hosted diagnostic convenience; these messages are not persisted. |

The selected smallest useful conversion is the Text key emission step: encode
`0x01 || textBytes[0..textLength)` into a caller-owned buffer. The byte
transformation is separated from `DbValue`, vectors, and diagnostics. The
existing hosted function extracts the Text byte pointer and length and calls
the portable operation. This does not change the public hosted API.

## Portable implementation

Added `database/portable/gx_index_text_key.h` and
`database/portable/gx_index_text_key.cpp` and included the source in the
production library, the independently built portable-runtime target, and the
freestanding census.

`gxEncodeIndexTextKey` receives a byte pointer and explicit length, a caller
buffer and capacity, and an output length. It writes the one-byte non-NULL tag
`0x01` followed by exactly the requested payload bytes. It does not scan for a
terminator, append a NUL, allocate storage, or depend on hosted C++ headers.
Input and output ranges may overlap; the helper moves bytes in the safe
direction before writing the tag.

`gxIndexTextKeyEncodedSize` checks the `textLength + 1` calculation. The
encoder validates all inputs and capacity before changing the output buffer.
When `outEncodedLength` is non-null, errors set it to zero and leave output
bytes unchanged:

| Condition | Result |
|---|---|
| Missing output pointer/length, or null input with nonzero input length | `GxStatus::InvalidArgument` |
| `textLength + 1` is not representable | `GxStatus::Overflow` |
| Output capacity is too small | `GxStatus::ResourceLimit` |
| Valid input and sufficient capacity | `GxStatus::Ok` and exact encoded length |

The selected operation owns no storage, so allocation failure and `NoSpace`
do not apply inside it. There is no allocator lifetime to retain. The hosted
adapter still resizes the caller's `std::vector<uint8_t>` as the existing API
requires; vector allocation behavior remains hosted and unchanged. The
portable buffer-capacity error remains explicit as `ResourceLimit`.

The `DbValue` adapter calls `.data()` and `.size()` on the Text value and
passes those bytes directly. Embedded zero bytes are preserved. The resulting
logical key remains byte-for-byte `0x01 || text`, so unsigned lexicographic
comparison, prefix ordering, duplicate equality, physical locator tie-breaking,
and persisted key bytes are unchanged. NULL remains the separate one-byte
`0x00` key. No database format, WAL, recovery, SQL, or phase-marker files were
changed.

## Regression qualification

Added `database_ndb1r4p11r3r_test`, with **51 checks**. Its test-local
reference applies the prior hosted behavior (`0x01` followed by the raw byte
range) and compares both the portable operation and hosted adapter against it.
Cases cover:

- Empty Text, ordinary Text, embedded zero, high-bit bytes, and repeated binary
  input equivalence.
- Prefix ordering, equality, and unsigned `0x7f` / `0x80` comparison through
  the existing encoded-key comparator.
- The maximum 1,024-byte logical key allowed by `maxIndexKeyBytes(kMaxPageSize)`
  (tag plus 1,023 payload bytes).
- Null/malformed pointers, too-small capacity, unchanged output on failure,
  `SIZE_MAX` length overflow, and overlapping input/output buffers.
- Hosted `encodeIndexKey` compatibility for empty, binary, ordinary, and
  maximum-length Text values.

The operation is allocation-free, so an allocation-failure test is not
applicable to the portable boundary. Existing R3Q traversal allocation-fault
tests remain in place and pass.

## Hosted validation

Built all Debug targets with `cmake --build build --parallel 4`. The focused
R3Q index run reported **188 passed, 0 failed**; the R3P heap/page-construction
target reported **10,716 passed, 0 failed**; the portable-runtime executable
reported **20,912 passed, 0 failed**; and the new R3R target reported **51
passed, 0 failed**.

The full Debug CTest aggregate completed with the following counts in
1,529.84 seconds:

| CTest target | Checks | Result |
|---|---:|---|
| `database_storage_tests` | 223 | pass |
| `database_relational_test` | 10,411 | pass |
| `database_transaction_test` | 2,578 | pass |
| `database_sql_test` | 1,779 | pass |
| `database_index_test` | 2,940 | pass |
| `database_ndb1r4p11r3r_test` | 51 | pass |
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
| **Total** | **161,490** | **22/22, 0 failures, 0 skipped** |

The accepted R3Q cycle, allocation-failure, and `NoSpace` behavior remains
covered. Existing R3P planning, split, rollback, commit/recovery, and WAL
regressions are included in the passing aggregate. No expected test was
removed or skipped.

## Freestanding census

The unchanged qualification script was run before and after the migration
against the same Server AMD64 compiler configuration. It uses C++14,
`-ffreestanding`, `-nostdinc++`, `-fno-exceptions`, `-fno-rtti`, `-nostdlib`,
`-fno-builtin`, and the Server's AMD64 code, stack, and ABI flags.

| Census | Production translation units | Pass | Blocked |
|---|---:|---:|---:|
| R3Q baseline | 35 | 17 | 18 |
| R3R result | 36 | 18 | 18 |

All 17 prior passing translation units remain passing. The new
`database/portable/gx_index_text_key.cpp` compiles under the real flags and
has no undefined symbols. Its source and header depend only on compiler
`stddef.h` / `stdint.h` and local `gx_status.h`; they contain no STL,
allocation, exception, or RTTI use.

`database/database_index.cpp` remains blocked at
`database/database_index.h:27` on `<string>`. The remaining hosted declarations
are genuine dependencies of the current header: `IndexValidation` diagnostics,
`DbValue::Text`, vector-based entries/results, and the Text decoder. The
migrated production byte operation itself is in its own freestanding source
and is independently qualified. The complete index translation unit has not
yet reached its later vector or implementation diagnostics.

The 18 still-blocked translation units report these first actionable errors:

| Production unit | First failure |
|---|---|
| `database/database_io.cpp` | `database/database_io.h:13`: missing `<cstdio>` |
| `database/database_native_io.cpp` | `database/database_native_io.h:5`: missing `<memory>` |
| `database/database_header.cpp` | `database/database_header.h:7`: missing `<string>` |
| `database/database_page.cpp` | `database/database_page.h:7`: missing `<vector>` |
| `database/database_file.cpp` | `database/database_file.h:17`: missing `<memory>` |
| `database/database_engine.cpp` | `database/database_engine.h:9`: missing `<memory>` |
| `database/database_buffer.cpp` | `database/database_file.h:17`: missing `<memory>` |
| `database/database_catalog.cpp` | `database/database_catalog.h:35`: missing `<string>` |
| `database/database_index.cpp` | `database/database_index.h:27`: missing `<string>` |
| `database/database_heap.cpp` | `database/database_heap.h:26`: missing `<memory>` |
| `database/database_wal.cpp` | `database/database_wal.h:30`: missing `<memory>` |
| `database/database_transaction.cpp` | `database/database_transaction.h:25`: missing `<memory>` |
| `database/database_relational.cpp` | `database/database_relational.h:21`: missing `<memory>` |
| `database/database_sql_token.cpp` | `database/database_sql_token.h:11`: missing `<string>` |
| `database/database_sql_tokenizer.cpp` | `database/database_sql_tokenizer.h:12`: missing `<string>` |
| `database/database_sql_parser.cpp` | `database/database_sql_parser.h:13`: missing `<vector>` |
| `database/database_sql_expr.cpp` | `database/database_sql_expr.h:18`: missing `<vector>` |
| `database/database_sql.cpp` | `database/database_sql.h:16`: missing `<map>` |

Qualification artifacts are at:

- Baseline: `C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3R-baseline-20261025`
- R3R: `C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3R-post-20261025`

## Deferred work and next boundary

`decodeIndexKey` still constructs hosted Text `DbValue`s through `std::string`.
`database_index.h` still exposes hosted vector and diagnostic types, so removing
its `<string>` include alone would not make the index unit freestanding. Keep
those declarations and the broader page/result ownership conversion out of
this bounded phase.

The next bounded phase should establish a freestanding-safe Text byte view and
key-decoding operation, then keep a hosted `DbValue` adapter at the API edge.
First split only the key/value declarations needed by encode/decode from
`database_index.h`'s hosted result, entry, and validation APIs. Leave index
mutation, split planning, `DatabasePage::payload`, and unrelated hosted
containers for separate phases.

## Commit closeout

- Starting HEAD: `c2975b60bf47a0ece2f6bc15159b209729fb069a`.
- Ending implementation HEAD: `1dddc04c6f863645de3d0170cd3067295cb0a914`
  (`Add portable index text key encoder`).
- This report is committed in a separate local documentation closeout commit.
- No phase marker, persistent format, WAL behavior, or other repository was
  changed. Existing history was not rewritten. No remote push was performed by
  this phase.
