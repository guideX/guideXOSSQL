# NDB1R4P11R3S — Portable Index Text Decoder

## Pre-implementation audit and selected boundary

This design record was written after repository and baseline verification and
before changing implementation code.

### Repository and accepted baseline

- Repository: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `b01ecb677fb132455911d1e3181f9b1c4669b378`.
- Initial worktree: clean. Local `main` is two commits ahead of
  `origin/main`; those commits are preserved.
- `.phase`: `last_completed=SQL9`, `next_expected=SQL10`.
- `.native-sql.phase`: `last_completed=NDB1R4P10`,
  `next_expected=NDB1R4P11`.
- The R3R portable Text key encoder is present in
  `database/portable/gx_index_text_key.{h,cpp}` and already belongs to the
  production static library, portable runtime target, and 36-unit
  freestanding census.
- The census toolchain is MinGW-W64 `g++ 15.2.0` with the Server AMD64 flags
  supplied by `tools/qualify_sql9_freestanding.ps1`; `nm` is available.
- Pre-change focused Debug baseline: index **2,940** checks (including R3Q's
  **188** focused checks), R3R **51**, portable runtime **20,912**, and R3P
  heap/page-construction **10,716**; all four targets passed.
- Pre-change freestanding census: **18/36** pass, **18/36** blocked. The
  index unit's first diagnostic remains `database/database_index.h:27` —
  missing `<string>`.
- The accepted R3R report records the full Debug baseline as **22/22** CTest
  targets and **161,490** checks.

### Existing decoder dependency graph

`decodeIndexKey(DbType, bytes, length, DbValue&)` is declared in
`database/database_index.h` and implemented in `database/database_index.cpp`.
For Text it validates the `0x01` non-NULL tag, then constructs an owning
`std::string` from every remaining byte and places it in `DbValue`. The
`0x00` NULL representation is accepted only when it is the sole byte. The
decoder is currently used by focused tests and diagnostics; persistent index
traversal compares the encoded bytes directly.

The Text decoder's `std::string` is needed only to materialize the hosted
`DbValue` result. The portable operation needs only the encoded buffer's
pointer and length and can return a borrowed view. The public index header
also has independent hosted string dependencies: `IndexValidation::message`
and the `validateNode` diagnostic parameter. `database_types.h` defines
`DbValue::Text` using `std::string`; index entries and APIs use vectors. These
declarations keep the full index unit hosted even after Text decoding is
separated.

The hosted decoder currently assumes a non-null pointer whenever `length >
0`; it reads the tag at `key[0]`. For Text, the existing persistent key
format is exactly `0x01 || rawTextBytes`; it has no payload length prefix,
terminator, normalization, or encoding conversion. Thus every suffix after
`0x01`, including an empty suffix or bytes containing zero, is valid Text.
Truncation of a Text payload cannot be detected from this format because a
shorter suffix is itself a valid Text value. NULL must remain exactly
`{0x00}`. There is no Text length prefix to validate.

### Selected portable boundary

Extend the existing `gx_index_text_key` module with a small
freestanding-compatible `GxTextByteView` and Text-key decoder. The view
contains a `const uint8_t*` and explicit `size_t` length. A null pointer is
valid only for a zero-length view; a non-null pointer with zero length is also
valid. The decoder will return status plus an explicit NULL flag, so an empty
Text value is distinct from SQL NULL and malformed input.

The decoder will accept only the existing Text key tags: exact one-byte
`0x00` for NULL, or `0x01` followed by an arbitrary byte suffix for Text. It
will check output pointers and the encoded span before reading, inspect only
the tag byte, use a checked subtraction after confirming a non-empty input,
and return a borrowed view into the caller's buffer. No bytes are copied or
allocated. The caller must keep the encoded buffer alive and unchanged for the
view's use. Failed decodes will clear output view/NULL state; malformed
encodings will use `GxStatus::CorruptData`, invalid API pointers will use
`GxStatus::InvalidArgument`.

The hosted `decodeIndexKey` Text path will call this single portable decoder,
then create `DbValue::null()` or copy the borrowed bytes into the existing
`std::string`-owned `DbValue`. Other SQL value decoders remain untouched. The
existing `<string>` header and diagnostics stay in place; this phase does not
claim the complete index translation unit is freestanding.

## Implementation and qualification

### Portable view and decoder

Extended the existing `database/portable/gx_index_text_key.{h,cpp}` module.
`GxTextByteView` stores only `const uint8_t* data` and `size_t length`; it owns
no bytes, requires no terminator, and supports embedded zero bytes. A zero
length view is valid with either a null or non-null pointer, while a nonzero
length requires a non-null pointer. The caller must keep the encoded input
alive and unchanged for the lifetime of a borrowed result. The decoder does
not allocate, copy, scan, normalize, or inspect payload bytes.

`gxDecodeIndexTextKey` reads only the first tag byte and forms the suffix view
after checking that the encoded length is nonzero. It subtracts one only after
that check; no unchecked multi-byte or unaligned access is used. Exact `{0x00}`
returns success with `outIsNull=true` and an empty null view. `0x01` plus any
remaining bytes returns success with `outIsNull=false`; an empty Text result
has length zero and points one byte past the tag. Thus valid empty Text is
distinct from NULL and from malformed input.

| Input or API condition | Portable result | Output state |
|---|---|---|
| Exact NULL key `{0x00}` | `GxStatus::Ok` | null flag true; empty null view |
| Text key beginning `0x01`, including tag-only | `GxStatus::Ok` | null flag false; borrowed suffix view |
| Empty encoding, unknown tag, or NULL with trailing bytes | `GxStatus::CorruptData` | view cleared; null flag false |
| Null input with nonzero length or missing output pointer | `GxStatus::InvalidArgument` | each available output is cleared |

Text keys have no encoded payload length prefix, so there is no length-prefix
field to validate and no way to identify a shortened Text suffix as truncated:
it is itself a valid shorter Text value. The caller-provided input span is the
bound. The decoder performs no `length + 1` calculation; the shared encoder
size helper's `SIZE_MAX` overflow case remains covered.

### Hosted `DbValue` adapter

The public `decodeIndexKey(DbType, ..., DbValue&)` signature is unchanged. Its
Text path now calls `gxDecodeIndexTextKey` and copies the borrowed view into
the existing owning `std::string` only at the hosted `DbValue` boundary. NULL
still becomes `DbValue::null()`. On malformed input the adapter returns
`false` without changing the caller's `DbValue`, matching its prior failure
behavior. Non-Text decoding and all index traversal, comparison, sorting,
collation, and normalization behavior remain unchanged.

The portable header/source use only compiler `stddef.h`/`stdint.h` and the
local `gx_status.h`; the portable decoder contains no STL, exceptions, RTTI,
allocation, or hosted runtime calls. `database_index.h` still includes hosted
`<string>` and `<vector>` for its independent APIs, so this phase does not
claim that `database_index.cpp` is freestanding-ready.

### Exact-format compatibility evidence

The format remains byte-for-byte `0x01 || textBytes[0..length)` for non-NULL
Text and the existing one-byte `0x00` for NULL. The portable encoder was not
changed. The decoder returns the exact suffix bytes in place; the hosted
adapter copies those same bytes, including embedded zeros and multibyte UTF-8
sequences treated as raw bytes. R3S tests round-trip empty, single-byte,
ASCII, multibyte, embedded-zero, and maximum supported key payloads; compare
the borrowed bytes and hosted `DbValue` bytes to the original payload; verify
the existing encoded-byte comparison ordering; and check that the view points
at the supplied buffer and reflects a mutation while that buffer remains
alive. No persistent key, page, catalog, WAL, recovery, or SQL format changed.

### Regression results

The full Debug CTest run passed **23/23 targets**, **161,611 checks**, with
zero failures and zero skipped targets. It completed in **1,882.94 seconds**.
Per-target check counts were read from this run's `build/Testing/Temporary/LastTest.log`:

| Debug CTest target | Checks | Result |
|---|---:|---|
| `database_storage_tests` | 223 | pass |
| `database_relational_test` | 10,411 | pass |
| `database_transaction_test` | 2,578 | pass |
| `database_sql_test` | 1,779 | pass |
| `database_index_test` | 2,940 | pass (includes R3Q's 188 focused checks) |
| `database_ndb1r4p11r3r_test` | 51 | pass |
| `database_ndb1r4p11r3s_test` | 121 | pass |
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
| `database_heap_page_construction_test` | 10,716 | pass (R3P) |
| `database_owned_page_test` | 251 | pass |
| `database_cache_publication_test` | 4,901 | pass |
| **Total** | **161,611** | **23/23, 0 failures, 0 skipped** |

Focused results from the same full run: R3R **51**, R3Q **188** (inside the
index target), R3P **10,716**, portable runtime **20,912**, and R3S decoder
**121** checks passed. The previous 22-target baseline was **161,490**; the
new standalone R3S target accounts for the increase of 121 checks.

### Freestanding census

The unchanged `tools/qualify_sql9_freestanding.ps1` source list and Server
AMD64 flags were used with MinGW-W64 `g++ 15.2.0`. The flags include C++14,
`-ffreestanding`, `-nostdlib`, `-nostdinc++`, `-fno-exceptions`, `-fno-rtti`,
and the existing Server ABI/code-generation options.

| Census | Production translation units | Pass | Blocked |
|---|---:|---:|---:|
| Pre-change R3S baseline | 36 | 18 | 18 |
| R3S result | 36 | 18 | 18 |

All 18 pre-existing passing units remain passing:
`database_format.cpp`, `database_checksum.cpp`, and the portable allocator,
hosted allocator, array, page-visit set, index Text key, owned page, heap page
builder, recovered pages, page cache, page codec, WAL record, WAL scan, WAL
page image, transaction pages, transaction page index, and transaction page
overlay implementations.

The same `database/portable/gx_index_text_key.cpp` translation unit that now
contains the decoder passes the census under the real freestanding flags. Its
`nm -u -C` output is empty: it introduces no undefined symbols, STL calls,
hosted allocation, exception, RTTI, or C runtime references. The remaining 18
units fail at their prior first hosted dependencies; no compile option,
translation unit, or exclusion was weakened or removed.

The complete index translation unit remains blocked at
`database/database_index.h:27` on `<string>`. This is still a genuine hosted
declaration dependency through `IndexValidation::message` and
`validateNode`'s diagnostic string, alongside `DbValue::Text` and vector-based
index APIs. Removing that include alone would expose other hosted string and
vector dependencies. `database_index.cpp` is **not** claimed to be
freestanding-ready.

Qualification artifacts:

- Baseline: `C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3S-baseline-20261025`
- R3S: `C:\Users\guideX\AppData\Local\Temp\guideXOSSql-NDB1R4P11R3S-post-20261025`

### Deferred migration work

- Keep all non-Text SQL key decoders hosted and unchanged.
- Keep `DbValue` ownership and the public index API unchanged.
- Split hosted validation diagnostics and vector-based entry/result APIs only
  in a later bounded phase.
- Do not change key comparison/order, page traversal, or index mutation as
  part of this decoder boundary.

### Outcome and closeout

**Outcome A — PORTABLE_INDEX_TEXT_DECODER_VERIFIED.** The borrowed Text view
and decoder are independent of hosted strings, pass the real freestanding
translation-unit census with no undefined symbols, preserve all 18 previous
census passes, retain exact key bytes and hosted behavior, and pass all
requested Debug regressions.

- Starting HEAD: `b01ecb677fb132455911d1e3181f9b1c4669b378`.
- Pre-implementation boundary commit: `7dc11212747a3f1f518a46cd7082f7cb7bd7817f`.
- Ending implementation HEAD: `f0888cac8471fcf6bf8607605243d7df16e628f6`
  (`Implement portable index Text key decoder`).
- This qualification report is committed as a separate local closeout.
- Phase markers remain unchanged at SQL9→SQL10 and NDB1R4P10→NDB1R4P11.
- No reset, rebase, stash, push, or remote change was performed.

Recommended next bounded phase: isolate the remaining hosted index
declarations—starting with validation diagnostics and `DbValue`/vector
surfaces—without migrating traversal or mutation behavior in the same change.
