# NDB1R4P11R3BJ SQL Index Capability Expansion

**Outcome:** `NATIVE_SQL_THIRD_INDEX_HOSTED_VERIFIED`

R3BJ adds a third persisted index, `IX_Rows_KBool`, to the production read-only SQL service and public catalog. The Native ELF SDK demo discovers the index through the catalog and queries its persisted Boolean values through the public App Model callbacks. All locator expectations below were independently decoded from the fixture bytes. R3BJ is a hosted functionality qualification only: no QAC admission, QEMU launch, or guest execution occurred.

## Repository revisions and preserved state

| Repository | Branch | Starting revision | R3BJ implementation revision |
| --- | --- | --- | --- |
| SQL | `main` | `9e1b365d150f7043c04c119320a229efb90ada0f` | `e17da72e1ed92d698909bba2e1ba5f2f151dd21b` |
| Server integration | `codex/ndb1r4p11rah-sql-read-integration` | `d24d8e79a2c4d679e9e159a79b38034ff94bd8e2` | `9cb9787e766694ab6ecc8e7b83003c6c18201c96` |

Starting HEADs matched the requested identities. The SQL checkout was `main...origin/main [ahead 2]`; the Server integration checkout contained existing untracked output trees and caches. Those files and all earlier acceptance evidence were retained. No reset, stash, clean, rebase, push, or merge was used. Implementation and this report are local commits only.

The frozen R3BE candidate was not rebuilt or changed. Its SHA-256 remains `10d4a361c612c496ac2f39cb2cc9f272cd61d7166920c7458ad2bb6e8982e197` at `E:\CodexArtifacts\NDB1R4P11R3BE\candidate-final\guideXOS-NDB1R4P11R3BE-amd64.iso`. Since R3BE predates the catalog and Boolean API changes, it is not evidence of these capabilities.

## Persisted index inventory

The independent verifier `tests/verify_r3ah_persisted_index.py` reads the fixture with Python `struct` and `zlib`; it does not call the production SQL reader, portable codecs, or service. It follows the persisted v3 catalog root (page 1) and continuation page 2, resolves table columns, checks page identities and CRCs, validates index page ownership and entry counts, traverses internal children, and compares DFS leaf order with each persisted leaf chain. Every root is an internal node, and all six observed entry counts match the persisted catalog and trusted package metadata.

Fixture: `tests/fixtures/NDB1R4P11R3AH/multilevel.gxdb`, SHA-256 `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57`, 250,880 bytes, 512-byte pages, 490 pages, table `Rows` ID 1, 80 rows, six index records.

| Persisted index | Type / column | Root / owner | Tree shape | Observed ordering | Persisted key groups and duplicate behavior | Existing codec / traversal and current capability |
| --- | --- | --- | --- | --- | --- | --- |
| `IX_Rows_K64` ID 1 | Int64 / `K64` ordinal 1 | 234 / 1 | 37 internal, 62 leaves, 3 root-to-leaf edges; 80 entries | Signed ascending; NULL first | NULL ×7, `7` ×73; duplicates ordered by locator | Int64 codec and multilevel descent/leaf traversal; exposed point lookup |
| `IX_Rows_K32` ID 2 | Int32 / `K32` ordinal 2 | 148 / 2 | 12 internal, 31 leaves, 2 edges; 80 entries | Signed ascending; NULL first | NULL, 0, 1, 2, 3 each ×16 | Int32 codec and multilevel descent/range traversal; exposed bounded range scan |
| `IX_Rows_KBool` ID 3 | Boolean / `KBool` ordinal 3 | 124 / 3 | 14 internal, 39 leaves, 2 edges; 80 entries | NULL, false, true | NULL ×12, false ×34, true ×34; duplicates ordered by locator | Existing Boolean codec and multilevel descent/leaf traversal; newly exposed point lookup |
| `IX_Rows_KText` ID 4 | Text / `KText` ordinal 4 | 375 / 4 | 22 internal, 45 leaves, 3 edges; 80 entries | Encoded text bytes ascending; NULL first | NULL ×8, UTF-8 byte sequence `61 00 62` ×24, `6f 74 68 65 72` ×48 | Text codec and multilevel traversal exist. The public fixed key union has no byte-span representation, so R3BJ does not advertise it. Embedded NUL is present in a real key. |
| `IX_Rows_Edge64` ID 5 | Int64 / `Edge64` ordinal 5 | 218 / 5 | 42 internal, 68 leaves, 3 edges; 80 entries | Signed ascending; no NULLs present | 80 distinct non-NULL values; signed endpoints `INT64_MIN` through `INT64_MAX` | Int64 codec and traversal exist, but no trusted public identity/query configuration is exposed for this index. |
| `IX_Rows_Edge32` ID 6 | Int32 / `Edge32` ordinal 6 | 259 / 6 | 38 internal, 66 leaves, 3 edges; 80 entries | Signed ascending; no NULLs present | 80 distinct non-NULL values; signed endpoints `INT32_MIN` through `INT32_MAX` | Int32 codec and traversal exist, but no trusted public identity/query configuration is exposed for this index. |

All six record names, IDs, table/column ordinals, roots, owners, recorded counts and page types agree with the reviewed `fixture.manifest`. Persisted column declarations establish the logical key types; raw leaf bytes are decoded independently. The persisted page headers establish actual root type, page ownership, physical tree shape, and observed entries. The existing production service intentionally remains package-config-driven for names, public identities, types, roots, and owners; at runtime it validates the persisted catalog root and each exposed index root/owner through the read-only provider. It does **not** parse and publish all serialized catalog records. This is not a claim of generic on-disk catalog parsing.

The integer codecs encode NULL as `{00}` and signed non-NULL values as `{01}` plus sign-adjusted big-endian payload bytes; their physical order is signed ascending with NULL first. Text uses `{00}` for NULL and `{01}` plus the original text bytes for non-NULL keys, ordered bytewise. The Boolean codec encodes NULL as `{00}`, false as `{01,00}`, and true as `{01,01}`, ordered NULL < false < true. Physical index comparison then orders equal logical keys by the big-endian 64-bit locator page and 32-bit slot. The raw verifier independently confirms null-first order and ascending locator order within duplicate groups. It also confirms the leaf chain matches the internal-tree traversal and that each index contains exactly 80 entries.

### Selection

`IX_Rows_KBool` is the lowest-risk additional public index. The fixture contains three actual Boolean groups, including duplicate rows and NULLs; the Boolean codec and existing tree traversal are already portable; the tree is a normal multilevel internal/leaf tree; and the current public request can carry Boolean values in its existing 8-byte key union slot without changing structure size. Point lookup reuses `LookupIndex` and the production read-only VFS path. R3BJ does not add Boolean range support, text spans, a new expression engine, or writes.

## Production reader and catalog

The trusted supported-index table now contains:

| Public identity | Index | Key type | Advertised operation | Ordering | Maximum results | Validated physical root / owner |
| ---: | --- | --- | --- | --- | ---: | --- |
| 1 | `IX_Rows_K64` | Int64 | Point lookup | Signed ascending, NULL first | 73 | Page 234 / owner 1 |
| 2 | `IX_Rows_K32` | Int32 | Bounded range scan | Signed ascending, NULL first | 73 | Page 148 / owner 2 |
| 3 | `IX_Rows_KBool` | Boolean | Point lookup | Boolean ascending, NULL first | 73 | Page 124 / owner 3 |

The Boolean query is encoded by the existing Boolean codec and sent through `gxNativeSqlExecuteRead`. The existing database handle, approved immutable image, internal identity resolution, root and owner validation, internal-node descent, leaf traversal, duplicate ordering, output staging, and provider cleanup remain in force. The application cannot provide a root, physical offset, or provider pointer. Result capacity is checked before publication; insufficient capacity returns an explicit error with no partial locators. Unsupported Boolean range queries are rejected. The remaining three indexes are hidden from the service catalog.

Catalog enumeration remains stable by identity and bounded to two entries per response. It exposes the third index on the next `startIndex` chunk, with key type Boolean, point-only capability, Boolean ascending/NULL-first comparison semantics, and a 73-locator limit. Roots and owners remain internal. The existing catalog callback and structures are unchanged.

## Application API compatibility

The public API adds identity `GX_SQL_INDEX_ROWS_KBOOL`, key tag `GX_SQL_KEY_BOOLEAN`, and comparison tag `GX_SQL_COMPARISON_BOOLEAN_ASCENDING_NULLS_FIRST`. The key carries `boolean_value` in the existing union; `gx_sql_key` remains 16 bytes, its payload offset remains 8, and `gx_sql_range` remains 48 bytes. Non-NULL Boolean values must be 0 or 1. No ABI version change was needed.

Callback offsets 256/264/272/280 and catalog offset 288 remain unchanged. The original 288-byte host-table prefix, the 296-byte extended host table, legacy-host behavior, opaque generation-tagged handles, owner checks, the 16-session maximum, session-generation checks, and owner cleanup on application teardown are preserved. Older callers can ignore a new key-type tag. Catalog chunk size remains two.

## SDK demonstration and expected locators

The updated Native ELF `SQLReadDemo` discovers the approved database, opens it, enumerates catalog chunks, finds `IX_Rows_KBool` by the returned name/key type/capabilities, then uses that returned identity and type for Boolean queries. It queries true, false, and NULL and compares every page/slot pair with arrays obtained from the raw persisted-page verifier. The service does not synthesize or hardcode its result rows.

Boolean true, 34 ordered locators:

`3:2, 3:4, 3:6, 10:0, 10:2, 10:4, 22:0, 22:2, 22:4, 22:6, 71:0, 71:2, 71:6, 130:0, 130:2, 130:4, 130:6, 190:0, 190:4, 190:6, 250:0, 250:2, 250:4, 250:6, 308:2, 308:4, 308:6, 360:1, 360:3, 360:5, 422:1, 422:3, 422:5, 422:7`

Boolean false, 34 ordered locators:

`3:1, 3:3, 3:5, 10:1, 10:3, 10:5, 10:7, 22:1, 22:3, 22:7, 71:1, 71:3, 71:5, 71:7, 130:1, 130:5, 130:7, 190:1, 190:3, 190:5, 190:7, 250:3, 250:5, 250:7, 308:1, 308:3, 308:5, 360:2, 360:4, 360:6, 422:0, 422:2, 422:4, 479:0`

Boolean NULL, 12 ordered locators:

`3:0, 3:7, 10:6, 22:5, 71:4, 130:3, 190:2, 250:1, 308:0, 360:0, 360:7, 422:6`

The demo retains the 73-locator K64 point result and 32-locator K32 range result, and keeps the K64 no-match case. It also exercises Boolean capacity exhaustion at 33 slots, wrong key type, malformed Boolean value 2, unsupported Boolean range, and a successful Boolean query after rejection, before closing the database. The raw verifier’s complete output and hash are retained under `E:\CodexArtifacts\NDB1R4P11R3BJ\raw-persisted-index-verification.log`.

## Hosted qualification

- The focused production query-service run passed **78/78** checks. It includes all 34 true, 34 false, and 12 NULL locators; existing K64/K32 results; malformed key rejection; capacity no-truncation; corrupt root checksum; checksum-valid wrong owner; failed and short VFS reads; recovery after rejection; cleanup; and byte-for-byte fixture immutability.
- The hosted R3BC Native ELF App Model/public API harness passed. It runs the real SDK entry point through the public callbacks and production SQL/VFS code, checks K64 point and K32 range, discovers Boolean metadata in the third stable catalog chunk, verifies all three Boolean result groups, rejects insufficient capacity, wrong type, malformed Boolean and unsupported range, performs a valid query after rejection, and closes the session. The embedded service run passed **78/78**.
- The production FAT32/VFS regression passed **17/17**, including fixture and manifest byte equality, read-only mount behavior, write rejection, query/cleanup diagnostic, and no candidate mutation.
- The independent raw persisted-page verifier passed and recorded all six index trees and ordered Boolean expectations.
- Fresh SQL Debug configure and full build passed; the complete CTest run passed **35/35** with zero failures in **2,156.44 seconds**. This is the count from the current configured checkout, distinct from the earlier R3BI historical 39/39 result.

The initial R3AQ run stopped at 68/77 because the hosted test exhausted available in-memory ramdisk slots while mounting extra isolated fixtures. The test now mutates and restores the existing candidate ramdisk in place; the fresh final run passed 78/78. The first R3BC compilation also found duplicate unnamed-namespace expected-array names in its unified test translation unit; the arrays were renamed, and the hosted API runner passed. A later fresh runner additionally asserted the false-value SDK log and passed.

## Native builds, imports, and security limits

- Ordinary AMD64 Server kernel build: passed from the updated integration checkout into the R3BJ artifact directory.
- AMD64 SQL qualification/diagnostic kernel build: passed.
- Native ELF SDK `SQLReadDemo`: built and staged with `app.json`. ELF inspection reports ELF64, x86-64, nonzero entry point, and no non-null undefined symbols. The current final sample SHA-256 is `EBA4F880457318AFED7FA36E5834075BEAC6AE5C71A1FDA88AF7F2F3425AB747`.
- `native_abi_layout_test`: passed against the current public headers, including the SQL callback positions, unchanged key/range layouts, host-table prefix, and catalog structures.
- The R3AG freestanding native-link qualification compiled 17 portable SQL/provider sources and linked 158 objects from the clean Server baseline (`f29850c93bbac0a0763a31e806d2816310156575`). Baseline and candidate each had 21 undefined/import symbols; the SQL component added **zero**. The updated ordinary and qualification kernels also each match the same 21-symbol baseline exactly and link the production query service and App Model integration.
- No database write API or write path was enabled. Key lengths, arithmetic, buffer capacities, query status mapping, staged outputs, provider cleanup, session ownership, and generation validation remain checked.

Native ELF apps currently execute in kernel address space. This qualification makes no application memory-isolation claim.

All hosted logs, build logs, the SDK ELF, kernel PE/ELF outputs, ABI executable, independent verifier output, and R3AG link evidence are stored under `E:\CodexArtifacts\NDB1R4P11R3BJ`.

## Guest acceptance and remaining indexes

R3BH-R3 remains pending after its FIFO reservation expired at the 1,800-second coordinator deadline without guest launch. The existing application-lifecycle guest acceptance remains open. R3BJ does not close it. The frozen R3BE ISO predates this work and is not used as guest evidence.

R3BB independent QMP page-table Gate 9 remains `UNVERIFIED`. R3BJ does not claim to close that item either. No QAC admission or QEMU execution occurred.

The remaining unsupported indexes are `IX_Rows_KText`, `IX_Rows_Edge64`, and `IX_Rows_Edge32`. A suitable later SQL milestone is a separately qualified query path for one of those persisted identities, or a bounded production parser for all serialized catalog metadata. Text requires an additive byte-span query representation; whichever index is next must retain explicit production capability limits and independent page-derived expected results. Guest execution of the expanded API requires its own source-qualified candidate and separate QAC authorization.
