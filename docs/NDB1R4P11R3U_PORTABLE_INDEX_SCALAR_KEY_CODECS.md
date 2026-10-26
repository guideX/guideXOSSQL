# NDB1R4P11R3U — Portable Index Scalar Key Codecs

## Outcome

**Outcome A — PORTABLE_INDEX_SCALAR_KEY_CODECS_VERIFIED.** The existing
Int32 and Int64 index-key formats now have production portable encoders and
decoders. Hosted `DbValue` calls adapt to the portable implementation, and the
persistent bytes and SQL ordering remain unchanged. The codecs compile and
link under the established freestanding flags without hosted symbols.

This qualifies the signed integer key family only. The index tree and its
hosted declarations remain non-freestanding.

## Repository baseline and closeout

- Repository: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `484892de4c6394d9d7a2b0d92cc66f2770ddbb97`.
- Starting worktree: clean; local `main` was seven commits ahead of
  `origin/main`.
- Starting phase markers: `.phase` remained SQL9 → SQL10;
  `.native-sql.phase` remained NDB1R4P10 → NDB1R4P11.
- Ending implementation HEAD, before this report: `0b5831fab74bf65704d488b3d4a131bdc61f9aa0`.
- Implementation commits: `ca500bd` (codec and integration), followed by
  `0b5831f` (expanded scalar qualification cases).
- No push, reset, stash, rebase, history rewrite, or phase-marker change was
  performed.

## Existing scalar codec audit

The logical key tag identifies NULL versus non-NULL; the column's `DbType`
provides the scalar type. No type id is serialized into the key. The existing
tree compares key bytes as unsigned lexicographic spans.

| Type | Existing encoding and ordering | Decoder and ownership behavior |
|---|---|---|
| Boolean | `00` for NULL; otherwise `01 00` (false) or `01 01` (true). | Requires two bytes and payload 0 or 1; scalar `DbValue`, no owned payload. |
| Int32 | `00` for NULL; otherwise `01` plus four-byte big-endian `static_cast<uint32_t>(value) XOR 0x80000000`. | Requires exactly five bytes; every payload pattern maps to a signed value. The former hosted decoder cast an out-of-range unsigned value to signed. |
| Int64 | `00` for NULL; otherwise `01` plus eight-byte big-endian `static_cast<uint64_t>(value) XOR 0x8000000000000000`. | Requires exactly nine bytes; every payload pattern maps to a signed value. The former hosted decoder used the same out-of-range cast pattern. |
| Float64 | `00` for NULL; otherwise `01` plus eight-byte big-endian ordered IEEE-754 bits. The encoder rejects non-finite values and normalizes negative zero to positive zero. | Requires nine bytes. The existing decoder does not reject non-finite crafted payloads or a negative-zero representation; these semantics are not changed here. |
| Text | `00` for NULL; otherwise `01` plus raw bytes. | The payload is variable length, so any supplied suffix is a valid Text value. The portable view borrows the key bytes; hosted `DbValue::Text` copies them into owned `std::string` storage. |
| Blob | No logical index-key representation. | The encoder and decoder reject non-NULL Blob values. |

There is no unsigned SQL integer type in `DbType`. The persistent format guide
[`SQL6_BTREE_INDEXES_CONSTRAINTS.md`](SQL6_BTREE_INDEXES_CONSTRAINTS.md)
documents the same integer sign-flip and big-endian contract. NULL is exactly
one `00` byte for each supported type; a valid zero is tagged `01` and has a
fixed-width payload.

## Selected family and rationale

This phase moves signed Int32 and Int64 together. They share one established
format: a non-NULL tag, fixed-width big-endian payload, and sign-bit flip.
Their encoded byte order already agrees with signed numeric order. Exact width
checks and the common NULL rule form a bounded migration boundary.

Boolean is a separate one-byte domain. Float64 remains hosted because its
finite-value policy, negative-zero normalization, IEEE representation, and
crafted-payload behavior need a separate qualification. Text already has its
portable codec. No SQL type or persistent format was added.

## Portable implementation and byte contract

Added [`gx_index_integer_key.h`](../database/portable/gx_index_integer_key.h)
and [`gx_index_integer_key.cpp`](../database/portable/gx_index_integer_key.cpp),
and exposed them through `gx_index_key.h`.

The interface provides `gxEncodeIndexInt32Key` / `gxDecodeIndexInt32Key` and
`gxEncodeIndexInt64Key` / `gxDecodeIndexInt64Key`. Encoders accept an explicit
output pointer and capacity, clear the output length before work, and report
failure with `GxStatus`. They need one byte for NULL, five bytes for Int32, or
nine bytes for Int64. Insufficient capacity returns `ResourceLimit` without
changing output bytes. Missing required output pointers return
`InvalidArgument`.

Decoders accept only the exact existing shapes. A NULL tag with extra bytes,
an empty input, an unknown tag, a truncated payload, an extra payload byte, or
a key of the other integer width returns `CorruptData`. A null input paired
with a nonzero length and missing output pointers return `InvalidArgument`.
Available outputs are reset on failure; on a successful NULL decode the
integer output is zero and `outIsNull` is true. A valid zero has tag `01` and
is not NULL.

Encoding converts signed values to the corresponding unsigned width using
C++'s defined modulo conversion, flips the established sign bit, and writes
bytes by shifts. Decoding reconstructs the mathematical signed value from the
ordered payload using bounded magnitudes; it does not type-pun, perform
unaligned loads, or cast an out-of-range unsigned value to signed. The
fixed-width implementation uses no input-derived offset and performs no
length-dependent addition beyond its fixed constants. Oversized reported
lengths are rejected before reading the payload.

The portable implementation includes only fixed-width integer and size types
plus `GxStatus`. It uses no `DbValue`, STL, exceptions, RTTI, allocation, or
hosted runtime facility.

## Hosted adapter compatibility

`encodeIndexKey` and `decodeIndexKey` retain their declarations and hosted
signatures. Int32 and Int64 paths now delegate to the portable codec; they do
not retain a second integer serialization algorithm. Vector and `DbValue`
ownership stay at the hosted boundary.

The hosted encoder still accepts NULL before checking the value's concrete
type. A non-NULL value with a mismatched type retains the prior failure result
and partial `{01}` output. The hosted decoder assigns its `DbValue` only after
a successful portable decode, so malformed input preserves the caller's
existing output value. Boolean, Float64, Text, Blob, index comparison, tree
traversal, and persistent pages are otherwise unchanged.

## Golden byte evidence and ordering

The fixtures below were independently checked against the pre-migration
encoder in the starting HEAD and the SQL6 persistent-format documentation.
They cover sign transitions and both signed extremes.

| Value | Int32 bytes | Int64 bytes |
|---:|---|---|
| minimum | `01 00 00 00 00` | `01 00 00 00 00 00 00 00 00` |
| -1 | `01 7F FF FF FF` | `01 7F FF FF FF FF FF FF FF` |
| 0 | `01 80 00 00 00` | `01 80 00 00 00 00 00 00 00` |
| 1 | `01 80 00 00 01` | `01 80 00 00 00 00 00 00 01` |
| maximum | `01 FF FF FF FF` | `01 FF FF FF FF FF FF FF FF` |

R3U compares all 28 pairs in each of two eight-value sequences containing the
minimum, adjacent minimum, negative values, zero, positive values, adjacent
maximum, and maximum. For every pair, portable encoded comparison, hosted
encoded comparison, and decoded integer comparison agree with signed numeric
order. It also verifies NULL sorts before valid zero. The golden fixtures
compare portable bytes with the hosted adapter byte-for-byte.

## Qualification results

The final Debug CTest run passed **25/25 targets**, **162,282 checks**, with
zero failures and zero skipped targets. The previous 24-target baseline had
161,663 checks; the expanded R3U target contributes 619.

| Qualification | Checks | Result |
|---|---:|---|
| Existing index target (includes R3Q) | 2,940 | pass |
| R3Q focused mode | 188 | pass, 0 failed |
| R3R portable Text encoder | 51 | pass |
| R3S portable Text decoder | 121 | pass |
| R3T portable index-key boundary | 52 | pass |
| R3U portable scalar key codecs | 619 | pass |
| R3P heap/page construction | 10,716 | pass |
| Portable runtime | 20,912 | pass |

The R3U target covers golden bytes, hosted adapter equivalence, NULL versus
zero, signed extremes and adjacent values, round trips, pairwise ordering,
type-width mismatch, malformed and oversized lengths, invalid pointers,
insufficient capacity, output-reset semantics, and no-write-on-capacity
failure.

## Production freestanding linkage and census

The established Server AMD64 qualification flags were retained, including
C++14, `-ffreestanding`, `-nostdlib`, `-nostdinc++`, `-fno-exceptions`, and
`-fno-rtti`. The production census adds the new codec translation unit, so its
denominator is now 37 rather than the prior 36. **19/37 units pass and 18/37
remain blocked.** All 18 previously passing units are preserved; the new
`gx_index_integer_key.cpp` is an additional passing unit.

The production freestanding probe calls Text, Int32, Int64, NULL-predicate,
and comparison operations. It compiles and relocatably links against the real
`gx_index_text_key.cpp` and `gx_index_integer_key.cpp` objects. `nm` confirms
definitions for all eight portable symbols and no unresolved symbols. The
integer codec object itself has no undefined symbols.

The full `database/database_index.cpp` translation unit did **not** progress
beyond the existing first blocker, `database/database_index.h:27` on
`<string>`. Its next direct standard-library include is `<vector>` at line 28;
the header also imports hosted heap, page, result, schema, and `DbValue`
surfaces. The subsystem is not claimed freestanding-ready.

The other census failures remain at their first hosted dependencies:

| Translation unit | First dependency |
|---|---|
| `database/database_io.cpp` | `<cstdio>` |
| `database/database_native_io.cpp` | `<memory>` |
| `database/database_header.cpp` | `<string>` |
| `database/database_page.cpp` | `<vector>` |
| `database/database_file.cpp` | `<memory>` |
| `database/database_engine.cpp` | `<memory>` |
| `database/database_buffer.cpp` | `<memory>` |
| `database/database_catalog.cpp` | `<string>` |
| `database/database_heap.cpp` | `<memory>` |
| `database/database_wal.cpp` | `<memory>` |
| `database/database_transaction.cpp` | `<memory>` |
| `database/database_relational.cpp` | `<memory>` |
| `database/database_sql_token.cpp` | `<string>` |
| `database/database_sql_tokenizer.cpp` | `<string>` |
| `database/database_sql_parser.cpp` | `<vector>` |
| `database/database_sql_expr.cpp` | `<vector>` |
| `database/database_sql.cpp` | `<map>` |

Qualification artifacts are in
`C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3U-scalar-20261026`.

## Recommended next bounded phase

Port the Boolean key encoder and decoder using its existing `00` / `01 00` /
`01 01` representation. Keep Float64 as a separate phase that explicitly
qualifies finite values, negative zero, IEEE-754 assumptions, and malformed
crafted payloads. Address the hosted index declarations and traversal APIs in
later work rather than treating this scalar codec as full subsystem
portability.
