# NDB1R4P11R3V — Portable Index Boolean Key Codec

## Outcome

**Outcome A — `PORTABLE_INDEX_BOOLEAN_KEY_CODEC_VERIFIED`.** The production
Boolean index-key encoder and decoder are freestanding-compatible and preserve
the existing hosted bytes, NULL distinction, ordering, and malformed-input
behavior. All previous portable key symbols still link, and the census gains
one production translation unit.

This qualifies the Boolean codec only. The B+ tree implementation and its
hosted declarations remain non-freestanding.

## Repository baseline and closeout

- Repository: `D:\dev\guideXOS_SqlDatabase`; branch: `main`.
- Starting HEAD: `50ce94774d48ecaaf5c7b440cbbcf91beded6aff`, matching the
  requested R3U closeout HEAD.
- Starting worktree: clean. Local `main` was ten commits ahead of
  `origin/main`; this existing history was preserved.
- `.phase` remained SQL9 → SQL10. `.native-sql.phase` remained NDB1R4P10 →
  NDB1R4P11.
- No reset, rebase, stash, history rewrite, force-push, or phase-marker change
  was performed. Changes are committed locally and were not pushed.

## Legacy Boolean key contract

The key's first byte is a NULL/non-NULL discriminator, not a serialized
`DbType` number. The column definition supplies the value type.

| Value | Exact existing bytes | Length |
|---|---|---:|
| NULL | `00` | 1 |
| false | `01 00` | 2 |
| true | `01 01` | 2 |

The hosted pre-migration `encodeIndexKey` implementation produced these exact
fixtures when called directly from the starting build's
`build/libgxos_database.a`; the capture was made before changing the
implementation. The production source independently confirms the same
`0x01` tag and one-byte `0`/`1` payload. Keys are fixed width for non-NULL
values. Unsigned lexicographic key comparison gives the documented
`NULL < FALSE < TRUE` order. NULL is therefore distinct from false.

The old hosted decoder required exactly one byte for NULL or two bytes for a
non-NULL Boolean and rejected payloads greater than 1. The pre-migration probe
confirmed `01 02` and `01 FF` are rejected; failures leave the hosted output
`DbValue` unchanged. The portable decoder preserves this strict, canonical
contract. There are no accepted noncanonical Boolean payloads to preserve.

## Portable implementation and hosted adapter

Added `database/portable/gx_index_boolean_key.h/.cpp` and exposed the API from
`gx_index_key.h`:

- `gxEncodeIndexBooleanKey` writes only the legacy NULL, false, or true bytes.
  It takes explicit capacity, reports `GxStatus`, clears the output length on
  failure, and leaves bytes untouched when capacity is insufficient.
- `gxDecodeIndexBooleanKey` validates the exact tag, length, and canonical
  payload. It has no allocation and resets available outputs on failure.
  NULL returns `outIsNull = true` with the Boolean value cleared.
- The implementation includes only fixed-width/size headers and `GxStatus`;
  it uses no `DbValue`, STL, allocation, exception, RTTI, or hosted runtime.

`encodeIndexKey` and `decodeIndexKey` retain their hosted signatures. Their
Boolean branches now adapt to the portable codec. Existing NULL acceptance,
wrong-value-type failure bytes (`01`), failed-decode output preservation, and
all non-Boolean `DbValue` paths remain unchanged. No SQL evaluation, index
traversal, database, or WAL behavior changed.

## Focused and aggregate qualification

The dedicated R3V regression target passes **232 checks**. It covers the legacy
fixtures, hosted/portable adapter equivalence, both Boolean round trips, NULL
distinctions, tags, ordering, repeated deterministic output, insufficient
capacity, guarded output bounds, malformed tags/lengths/payloads, and decoder
failure state.

The full Debug CTest aggregate passes **26/26 targets**, **162,514 checks**,
with zero failures and zero skipped targets. The prior 25-target baseline was
162,282 checks; R3V adds its 232 checks.

Focused regressions also pass:

| Qualification | Checks | Result |
|---|---:|---|
| R3Q index cycle membership | 188 | pass |
| R3R portable Text encoder | 51 | pass |
| R3S portable Text decoder | 121 | pass |
| R3T portable index-key boundary | 52 | pass |
| R3U portable Int32/Int64 codecs | 619 | pass |
| R3P portable heap/index split planning | 10,716 | pass |
| Portable runtime | 20,912 | pass |
| R3V portable Boolean codec | 232 | pass |

## Production freestanding linkage and census

The unchanged qualification script used the Server AMD64 configuration and
its C++14 flags, including `-ffreestanding`, `-nostdlib`, `-nostdinc++`,
`-fno-exceptions`, and `-fno-rtti`. The production census is **20/38 passing**.
All 19 units that passed before R3V remain passing; the actual new Boolean
translation unit passes. The first census blocker for
`database/database_index.cpp` remains `database/database_index.h:27`:

```text
fatal error: string: No such file or directory
```

No new census compile failure was exposed. The portable Boolean object has no
undefined symbols. The freestanding probe compiled and relocatably linked the
actual Boolean, Int32, Int64, and Text production objects with
`--no-undefined`. `nm` found no unresolved symbols and confirmed definitions
for all ten production key symbols:

`gxEncodeIndexTextKey`, `gxDecodeIndexTextKey`,
`gxEncodeIndexBooleanKey`, `gxDecodeIndexBooleanKey`,
`gxEncodeIndexInt32Key`, `gxDecodeIndexInt32Key`,
`gxEncodeIndexInt64Key`, `gxDecodeIndexInt64Key`,
`gxIndexKeyIsNull`, and `gxCompareIndexKeys`.

The portable codec translation unit itself does not pull in hosted value,
STL, exception, or RTTI dependencies. This does not make the full index
translation unit portable.

## Integration reassessment

Portable production encoders and decoders now cover Boolean, Int32, Int64,
and Text. Float64 remains hosted-only; its finite-value acceptance,
negative-zero normalization, IEEE-754 assumptions, and crafted-payload decode
behavior deserve a separate bounded contract if migrated. Blob has no logical
index-key representation and remains rejected; `Unknown` is not a concrete
value type.

The immediate `database_index.h` blocker is its direct `<string>` include at
line 27 for `IndexValidation::message`. The next line includes `<vector>`;
the header also exposes vector-backed entries and imports heap, page-store,
result, schema, and `DbValue` declarations. Removing one include would not
make the B+ tree API or implementation freestanding.

## Recommended next phase

Start a bounded index declaration/implementation separation phase before
adding another isolated scalar helper. Define the freestanding index surface
and its ownership/error contracts, then separate the hosted diagnostics and
`DbValue`/container adapters from the B+ tree boundary. Reassess Float64 as a
separate codec after that boundary is defined, with explicit binary64 and
non-finite/negative-zero compatibility requirements. Do not claim full index
portability until the production tree translation unit and its dependencies
compile and link under the freestanding configuration.
