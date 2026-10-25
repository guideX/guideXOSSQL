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

Implementation evidence, final results, census details, next compiler
blocker, deferred work, ending HEAD, and commit are recorded below after the
change is complete.
