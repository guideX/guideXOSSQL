# NDB1R4P11R3BI Application-Facing SQL Catalog Discovery

**Outcome:** `NATIVE_SQL_CATALOG_API_HOSTED_VERIFIED`

R3BI adds bounded read-only database and index discovery to the Native ELF SQL App Model. The SDK caller discovers the approved database and two currently supported indexes, then uses the returned identities for production point and range queries. Hosted qualification and native build checks pass. No QAC admission or guest execution was requested or performed.

## Repository revisions

| Repository | Starting revision | R3BI implementation revision |
| --- | --- | --- |
| SQL (`main`) | `7cb2181729508e676acfac49bd5be7d8976abd88` | `e3f88872a25d2ee6b17a3776e7101d8f435fd20d` |
| Server integration (`codex/ndb1r4p11rah-sql-read-integration`) | `af0e1706d0ebac475a2b205938b14fa6471dc8af` | `d24d8e79a2c4d679e9e159a79b38034ff94bd8e2` |

Both repositories were verified at the expected starting revisions. The SQL and Server implementation commits are local only. This report is committed as a following SQL documentation commit. No reset, clean, stash, rebase, push, or merge was performed. Existing untracked caches, prior output trees, and acceptance artifacts were retained.

## Catalog architecture and metadata sources

The approved database is public identity `GX_SQL_DATABASE_R3AH_MULTILEVEL` (`1`) and display name `NDB1R4P11R3AH_MULTILEVEL`. The server resolves it to the fixed `/system/Apps/R3AI/MULTI.gxdb` fixture. Applications cannot provide a VFS path, catalog root, page address, or trusted-manifest location. The production reader checks the immutable image header, database UUID, version, size, page size/count, creation time, and catalog root against the approved package configuration.

The fixture has a persisted SQL catalog: version 3, root page 1, six stored index records, and a catalog continuation page. Its manifest records the index names and IDs. The native read engine currently supports only two of those six indexes. R3BI validates the persisted catalog root page through the production VFS: page checksum and type, catalog magic/version, root-header size, supported index count bounds, record-count bounds, continuation bounds, and reserved fields. The reader does not parse or publish the entire hosted STL catalog or its continuation records.

The displayed names, public identities, key types, comparison semantics, capability flags, result limit, physical roots, and owner IDs are therefore sourced from one reviewed R3AH package configuration shared by catalog enumeration and query dispatch. At runtime, the reader checks the catalog root header and validates every exposed physical index root and owner through the production read-only page provider before publishing catalog metadata. Names and key types are not re-derived from serialized catalog records. This is an explicit trusted-package dependency, not arbitrary database discovery.

| Exposed index | Public identity | Key type | Supported operation | Ordering | Maximum results | Validated root / owner |
| --- | ---: | --- | --- | --- | ---: | --- |
| `IX_Rows_K64` | `1` | Int64 | Point lookup | Signed ascending; nulls first | 73 | Page 234 / owner 1 |
| `IX_Rows_K32` | `2` | Int32 | Bounded range scan | Signed ascending; nulls first | 73 | Page 148 / owner 2 |

The other persisted fixture indexes (`IX_Rows_KBool`, `IX_Rows_KText`, `IX_Rows_Edge64`, and `IX_Rows_Edge32`) are not reported as query-capable because this reader has no supported query path for them. The application API never exposes physical roots.

## Public API and ABI compatibility

The public callback `gx_host_calls.sql_catalog_query` is appended at byte offset 288. The existing 288-byte host-table prefix and callback slots at 256, 264, 272, and 280 remain unchanged; `GX_API_VERSION` remains unchanged. The current full host table is 296 bytes. `gx_sql_query_catalog()` checks the application-context size, host pointer, and host-table size before reading the optional callback. A legacy 288-byte host table returns `GX_SQL_STATUS_UNSUPPORTED` without reading beyond its prefix.

One bounded operation supports two query kinds:

- `GX_SQL_CATALOG_QUERY_DATABASES`: `databaseHandle` must be zero. It returns the approved database list (currently one entry).
- `GX_SQL_CATALOG_QUERY_INDEXES`: requires an opened, owner-associated database session. It returns the indexes supported by that SQL reader (currently two).

Requests, entries, and results carry explicit sizes. The request is 24 bytes, each fixed entry is 92 bytes with a 64-byte NUL-terminated UTF-8 name field, and the result is 20 bytes. Enumeration uses `startIndex` and `maxEntries`; the maximum chunk is two. Ordering is stable by public identity. The kernel checks full caller spans, pointer arithmetic, structure sizes, capacity bounds, session ownership, and overlap before writing. Entries are staged and copied only after package/catalog/root validation and successful provider cleanup. Insufficient capacity returns `CAPACITY_EXCEEDED` with `requiredCapacity`, zero entries, and no partial entry publication.

Status distinctions include `INVALID_DATABASE` for an unknown public identity, `PACKAGE_INVALID` for an inaccessible or invalid package, `DATABASE_UNAVAILABLE` for read/close/provider availability failures, and `DATABASE_CORRUPT` for malformed catalog/index data. Unknown index IDs use the existing `INVALID_INDEX`; unsupported query combinations use `UNSUPPORTED`.

## Production service and SDK caller

The database mapping is shared by open and catalog operations. The reader’s single `kIndexes[]` configuration drives both the public metadata and query identity/dispatch, so an identity returned by enumeration is accepted by the matching query path. A range operation on K64 and a point operation on K32 are rejected as unsupported. The existing read-only provider and portable index algorithms remain responsible for query execution; no key codec, path selection, or write path was added.

The established `OpenReadOnlyDatabase`, `CloseDatabase`, `LookupIndex`, and `ScanIndexRange` callbacks retain their positions and behavior. Database handles remain opaque, generation-tagged, owner-associated, and bounded to 16 concurrent sessions. Application exit still reclaims the owner’s sessions.

The Native ELF `SQLReadDemo` now uses the SDK catalog helper to enumerate the database, identify it by returned metadata, open it, enumerate both index entries, and select operations by returned identity, key type, and capabilities. It executes the real 73-locator Int64 point scenario and 32-locator Int32 range scenario, checks ordered locator results, then covers unknown database/index, unsupported operation, invalid input, recovery, and close behavior. It does not print a catalog copied into application source.

## Hosted qualification

The hosted App Model harness compiles and runs the real SDK sample entry point together with the production SQL service, production VFS/FAT code, read-only provider, and portable reader. The final run passed with `nativeSqlR3aqServiceChecks=54/54` and `R3BC hosted Native ELF SQL application API test PASS`.

The run covers approved database and index enumeration; accurate K64/K32 metadata and stable ordering; required capacity/no partial publication; unknown identities; invalid and stale handles; owner mismatch; malformed structure sizes, enums, offsets, and pointer spans; legacy host-table compatibility; unsupported-operation rejection; catalog-root and index-root corruption; failed VFS reads and closes; missing package classification; application-exit session cleanup; real point/range locator regressions; and byte-for-byte fixture immutability.

The first SQL Debug CTest run completed 38/39. Its one failure was the R3AG native-link attribution guard, which requires its checked Server source tree to have no tracked or untracked changes; at that time the requested integration checkout had the R3BI edits plus existing and R3BI-generated untracked artifacts. The edits are now committed locally, and all untracked artifacts remain preserved. The complete Debug build and CTest suite were rebuilt and rerun from `build/ndb1r4p11r3bi-debug` against the pre-existing clean Server baseline checkout (`D:\dev\guideXOSServer_NATIVE_DB_STORAGE`). The final run passed **39/39** in **1,519.28 seconds**. Both CTest logs and the fresh build/configure logs are retained under `E:\CodexArtifacts\NDB1R4P11R3BI`.

## Native build and ABI qualification

- The ordinary AMD64 kernel build passed.
- The AMD64 Native SQL qualification/diagnostic build passed.
- The `SQLReadDemo` SDK build produced a 64-bit x86-64 Native ELF executable. LLVM inspection found `gx_main` and no undefined ELF symbols for the application. The retained `sqlread-final.elf` SHA-256 is `BD9191D7B3023DFF653B5259A93E0D9172BBEEDDED389AF069969F18A54E6113`.
- Ordinary and qualification kernel PE images have no COFF import descriptors; SQL/VFS code is linked into the kernel image. The qualification image contains the Native SQL read and catalog validation symbols.
- The R3AG freestanding native-link qualification compiled all 17 SQL sources and linked 158 checked Server objects. It recorded 21 undefined imports in both the baseline and candidate, with **0 SQL-added undefined imports**.
- `native_abi_layout_test` passed, including preservation of the old callback offsets, the 288-byte host prefix, the 296-byte extended table, and fixed catalog structure layouts.
- No database write API or newly enabled database write path was added.

Command outputs are retained in `E:\CodexArtifacts\NDB1R4P11R3BI`, including the hosted API run, SQL Debug configure/build/CTest logs, both kernel builds and import inspections, SDK build and ELF inspection, and ABI layout test.

## Acceptance status and limits

R3BH-R3 remains pending. Its single FIFO QAC request (`e1f2abc890bc4a20a6c1c7fa7f67cb10`) waited at queue position 1 until the 1,800-second deadline expired with QAC exit code 124. Launch count was zero; no R3BH guest or SQL application execution was observed. The existing reservation was left untouched. SQL Read Demo guest acceptance remains open for a separately authorized R3BH-R4 phase when QAC is available.

The frozen R3BE ISO was not rebuilt or modified. Its SHA-256 remains `10d4a361c612c496ac2f39cb2cc9f272cd61d7166920c7458ad2bb6e8982e197` at `E:\CodexArtifacts\NDB1R4P11R3BE\candidate-final\guideXOS-NDB1R4P11R3BE-amd64.iso`.

R3BB independent QMP page-table Gate 9 remains `UNVERIFIED`; R3BI does not claim to close it. No QAC admission, QEMU launch, ISO rebuild, or R3BE candidate change occurred.

## Recommended next functionality milestone

When guest acceptance capacity is available, resume the SQL Read Demo lifecycle checks in R3BH-R4 without changing this catalog contract. A later catalog milestone can replace the trusted index metadata configuration with a bounded native parser for persisted catalog records and verify names, IDs, column types, and roots directly, then extend discovery only for query algorithms that are actually implemented.
