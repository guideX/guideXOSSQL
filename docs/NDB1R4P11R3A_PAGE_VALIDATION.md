# NDB1R4P11R3A: Length-Aware Page Validation

## Outcome

**Outcome A — `PAGE_VALIDATION_BOUNDARY_VERIFIED`.** The authoritative SQL
page parser now receives an explicit source extent, and its existing portable
page inspector validates that extent and the serialized page type before it
reads the page header or payload. Production file, cache, transaction, WAL,
recovery, and open-validation paths pass extents from their actual byte
vectors, fixed-size allocations, or page-image owners. Validation failure
returns before a `DatabasePage` or cache page is published.

The full hosted suite passed 19/19 targets with 145,271 checks and zero
failures. The requested freestanding qualification compiled the selected
portable codec; the existing census remains 15/33 passing objects. Parent
NDB1R4P11 remains open. Neither phase marker advanced.

## 1. Starting state and preserved reports

- SQL checkout: `D:\dev\guideXOS_SqlDatabase`.
- Branch and starting HEAD: `main`,
  `0ca7e2e173214e1a261c0496810f60fbbccfc4ac`.
- The authorized untracked R3 checkpoint,
  `docs/NDB1R4P11R3_PAGE_MATERIALIZATION.md`, was preserved byte-for-byte and
  is included with this continuation's local commit.
- The P11 ownership audit, R1 allocator report, R2 heap-page report, and R3
  materialization checkpoint were read before implementation.
- `.phase` remains SQL9 → SQL10.
- `.native-sql.phase` remains NDB1R4P10 → NDB1R4P11.
- No Server checkout, branch, file, or marker was changed.

The R3 checkpoint identified the pointer-only `DatabasePage::parse` contract
and missing persisted page-type validation. No R3A implementation was
present at preflight.

## 2. Parser and codec audit

`DatabasePage::parse` in `database/database_page.cpp` is the hosted SQL page
materializer. Before this continuation it accepted a page pointer and the
configured page size, then called the allocation-free
`gxDatabasePageInspect` codec. It checked page identity, payload bounds, and
CRC, but neither the parser nor codec knew the readable extent of the source,
and the codec accepted every 16-bit page-type value. After inspection, the
parser copied the payload into a temporary `DatabasePage` and moved that page
to its output.

The existing `gxDatabasePageInspect` in
`database/portable/gx_page_codec.cpp` remains the only serialized page
inspector. It now accepts `imageLength` and the configured `pageSize`. It
checks the source pointer, output pointer, supported page size, and exact
extent before reading any header byte. The exact-length comparison avoids
length addition or subtraction on caller-controlled extents. It then checks
the existing page id, payload bound, CRC, and supported page type. It writes
`GxDatabasePageInfo` only after every check succeeds.

The production call sites audited and updated are:

| Path | Source extent supplied |
|---|---|
| `DatabaseFile::validateOpenedPages` root and full-page validation | `std::vector<uint8_t>::size()` after `readPageBytes` |
| `DatabaseFile::readPage` | `std::vector<uint8_t>::size()` after `readPageBytes` |
| `DatabaseFile::readPageImage` / `writePageImage` | Caller image extent, checked against `_header.pageSize` before inspection or I/O |
| `BufferManager::materialize` | `_cache.pageSize`, the fixed allocation extent of each resident cache image |
| `BufferManager::pinPage` cache miss | `_cache.pageSize`, the extent requested from the cache image allocator |
| `Transaction::readPage` and commit-time page installation | `pageSize()`, the fixed extent of transaction-owned page images |
| Recovered-page retention | `imageSize`, explicitly checked against the recovered collection page size |
| Transaction-page clone | `source->pageSize`, the transaction image owner's fixed allocation extent |
| WAL page-image validation | `pageImageLength`, checked against the configured page size |

The length contract is exact: a serialized page source must contain exactly
one configured page. A null pointer is `InvalidArgument`; a non-null source
with a short or oversized extent is malformed and returns `CorruptData`,
which the SQL parser maps to `DbStatus::CorruptPage`. Unsupported page sizes
remain `InvalidArgument`. No extent is inferred from a page header, and no
caller supplies a guessed value. Fixed-size cache and transaction owners
allocate every image at their recorded page size; vector-backed reads pass
their actual vector sizes.

`DatabasePage::parse` continues to use a temporary page and moves it into the
caller destination only after validation and payload materialization succeed.
Malformed input therefore leaves an existing destination unchanged. The
portable inspector itself performs no allocation and is freestanding
compatible.

## 3. Supported page-type matrix

The accepted persisted values follow the existing `PageType` definitions in
`database/database_format.h`:

| Serialized value | Type | Parser result |
|---:|---|---|
| 1 | `Catalog` | Accepted |
| 2 | `Data` | Accepted |
| 3 | `IndexLeaf` | Accepted |
| 4 | `IndexInternal` | Accepted |
| 0 | `Unknown` | Rejected as `CorruptPage` |
| Other 16-bit values | Undefined | Rejected as `CorruptPage` |

`Unknown` remains the in-memory default for a newly constructed
`DatabasePage`; it is not a supported serialized page kind. Existing page
header offsets, 32-byte header layout, little-endian encoding, payload
capacity rules, CRC calculation, and padding semantics remain unchanged.

## 4. Focused corruption, boundary, and integration coverage

Added checks to the existing portable page and storage targets:

- Null pointer with zero extent; non-null zero extent; header-short input;
  source ending inside the declared payload; one-byte-short page; and a
  `SIZE_MAX` extent. The validator rejects each before reading the header.
- All four supported page types pass. `Unknown` and `0xFFFF` fail with a
  valid recomputed CRC, proving type validation is independent of checksum
  validation.
- CRC corruption remains rejected. Failed inspection leaves its output-info
  structure unchanged.
- The 512-byte minimum page and standard 4,096-byte page validate. Existing
  storage coverage also creates, reads, closes, and reopens databases across
  the supported configured page sizes.
- `DatabasePage::parse` preserves every destination field and payload across
  null, zero-length, truncated header, truncated payload, oversized extent,
  CRC-corrupt, and unsupported-type inputs. A valid retry then updates that
  same destination successfully.
- A real `Database::readPage` cache miss reads a page whose type was changed
  to `0xFFFF` while keeping a valid CRC. The checked file-image read returns
  `CorruptPage` before cache publication, preserves the caller destination,
  then succeeds after the page is repaired and the database is closed and
  reopened.
- Existing page I/O, SQL, transaction, commit/reopen, WAL, recovery, and
  savepoint tests exercise valid production materialization through the
  buffer and transaction paths.

The focused five-target CTest run passed 5/5. Together the new boundary
assertions account for the 98-check increase over the R2 hosted baseline.

## 5. Freestanding qualification

Ran the unmodified `tools/qualify_sql9_freestanding.ps1` script on
2026-10-19 with the configured Server AMD64 kernel include roots. The result
is **15/33 production objects passing**, matching the accepted R2 census.
`database/portable/gx_page_codec.cpp` compiled successfully with the new
extent and page-type validation. Its undefined references remain the
project's CRC functions. The portable recovered-page, WAL page-image, and
transaction-page objects also compiled against the updated inspector API.

The same 18 hosted translation units remain blocked at their previously
recorded standard-library includes. `database/database_page.cpp` remains
blocked by `<vector>` in `database/database_page.h`; this continuation does
not claim that the hosted page materializer or its owning payload is
freestanding. The qualification is object compilation, not a native link or
runtime test. Logs are in
`%TEMP%\guideXOSSql-NDB1R4P11R3A-freestanding-verified-20261019`.

## 6. Full hosted regression

Built the complete existing CMake/Ninja project and ran
`ctest --test-dir build --output-on-failure` against the final implementation:

| Result | Value |
|---|---:|
| CTest targets | 19/19 passed |
| Checks reported by the individual targets | 145,271 passed |
| Failures | 0 |
| CTest elapsed time | 1,507.80 seconds |

The total is the sum of each target's check count in
`build/Testing/Temporary/LastTest.log`. It is 98 checks above the accepted R2
baseline of 145,173 because of the focused R3A assertions. The run includes
the full SQL9, index, foreign-key, transaction, commit/reopen, WAL,
savepoint, and recovery targets.

## 7. Hosted payload boundary and R3B recommendation

The validator prevents malformed or truncated pages from being read or
published. After a page passes validation, `DatabasePage::parse` still copies
the payload into `std::vector<uint8_t>`. That allocation remains hosted and
may throw `std::bad_alloc`; it is not converted into a portable allocator
status by this continuation. Existing buffer and transaction paths retain
their allocation catches, while allocation behavior in the hosted parser,
file reads, and page copies remains part of the later ownership work.
`DatabasePage::payload` remains public and vector-owned. No allocator
provenance, payload lifetime, general page allocator, or page-copy API was
changed.

R3B should define the page-payload allocator lifetime and provenance first,
including caller-owned pages that may outlive their `Database`. Then add a
move-only RAII payload owner with bounded resize and status-returning clone,
migrate page materialization and the audited production copy sites, and test
allocation failure before cache or caller publication. Migrate page producers
only with the same allocator contract in place. Do not advance the native
marker as part of that work.

## 8. Closeout

Both phase markers remain unchanged. The existing R3 audit is preserved. The
SQL branch remains `main`; accepted R3A implementation, tests, and both R3
reports are committed locally. No push, branch change, Server modification,
format change, or broad payload conversion was made. Parent NDB1R4P11 remains
open; R3B was not started.
