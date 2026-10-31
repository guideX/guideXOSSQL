# NDB1R4P11R3AF — Native Read-Side Integration Boundary

## Result

**Outcome B — `NATIVE_READ_SIDE_BOUNDARY_PARTIAL`.** A small allocation-free
provider now adapts the existing SQL page-provider callbacks to the actual
guideXOS Server native database file C ABI. It opens a caller-attested
immutable `.gxdb` image read-only, checks the database header and file extent,
and lends one validated index-page payload at a time to the production
`gxIndexLookupAll` and `gxIndexRangeScan` operations. The component compiles
against the checked Server declarations with the Server AMD64 freestanding
flags, and its hosted contract target executes through the real Server bridge
implementation.

This is not a native Server VFS execution or a general native SQL integration.
The Server checkout has no catalog/index-root resolver or immutable-file
lease, and the production VFS functions are replaced by explicit test-only
definitions in the hosted contract target. Live/concurrent access is rejected.

## Repository state and phase markers

- SQL repository: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `fb06228c7aad284328c5dee6c0c34e5c0d4a7a1f` (the requested
  starting revision). The worktree was clean.
- The prompt anticipated five commits ahead of local `origin/main`; the
  observed local tracking ref was at the same HEAD, so the measured state was
  zero ahead and zero behind. No fetch or remote operation was performed.
- `.phase` remains SQL9 → SQL10; SHA-256
  `f84ef02a6b82d4c53cdb2bdd0b80c083a49c6fa2e5df46914d9aadd7555295f0`.
- `.native-sql.phase` remains NDB1R4P10 → NDB1R4P11; SHA-256
  `fe722cf1a92a04c9f16e91e556933c682a7e3475bf3aabb06ce994410a28a52a`.
- The read-only Server source checkout was
  `D:\dev\guideXOSServer_NATIVE_DB_STORAGE`, branch `NATIVE_DB`, starting
  HEAD `f29850c93bbac0a0763a31e806d2816310156575`; it was clean and its
  `.native-db.phase` remains NDB1R3 → NDB1R4.
- Implementation commit: `2a461fa64d01932f9dab36bb9bbf2066792e9804`.
  This evidence file is finalized in a subsequent documentation-only commit.
- No phase marker, Server file, persisted database/index/WAL format, or
  recovery order was changed. Git history was extended only by local commits;
  no rewrite occurred. No push, QEMU, or QAC run was performed.

The prior SQL qualifications read were
`docs/NDB1R4P11R3AE_SQL_PAGE_PUBLICATION_READ_VIEW.md` and
`docs/NDB1R4P11R3AD_PORTABLE_INDEX_NATIVE_SNAPSHOT_PROVIDER.md`.

## Audited SQL interfaces

### Database metadata and root identity

`database/database_format.h` defines the fixed little-endian database header
and page layouts. Header page 0 carries page size, logical page count, and the
catalog root page ID. An index root is stored in the catalog's
`Catalog::IndexRecord::rootPageId` (`database/database_catalog.h`), not in the
database header. The catalog may span continuation pages. The format bounds
page size to 512–65536 bytes and logical page count to `2^32` pages.

`PageAccess` (`database/database_pagestore.h`) offers `pageSize`, `pageCount`,
`readPage`, `writePage`, and `allocatePage`; it has no snapshot, page pin, or
borrow/release operation. The hosted `DatabaseReadView`
(`database/database_relational.h`, `database/database_relational.cpp`) holds
the per-`Database` recursive operation mutex while it captures the selected
page view, page size/count, and catalog index root. It protects the
transaction/table/DDL facade publication paths described by R3AE. Its scope
does not include another `Database` handle, a Server VFS handle, another
process, direct low-level storage access, or an external file writer. It is a
hosted C++ object and is not available to the Server freestanding target.

### Existing portable provider and operations

`database/portable/gx_index_leaf_traversal.h` defines the existing
`GxIndexPageProvider` acquire/release callbacks and `GxIndexPageView` borrowed
payload span. Successful acquisition must be released once. The bytes must
remain immutable through release; the provider context and one logically
consistent root/page view must live for the complete operation.

`gxIndexLookupAll` and `gxIndexRangeScan` in
`database/portable/gx_index_lookup.h` are the production bounded point and
range operations. They own no storage and perform no allocation. They can
invoke a visitor before a later provider/traversal error, so a caller that
publishes a result must stage visitor output and discard it unless the final
status is successful. The hosted `IndexPageProviderContext` in
`database/database_index.cpp` uses an owned `DatabasePage`, validates its
identity/type/payload, and releases the provider borrow. Hosted lookup/range
results are staged before publication.

The R3AE `DatabaseReadView` satisfies consistency only for its single hosted
`Database` instance and facade writers. It does not automatically protect an
independent Server reader or make external file changes impossible.

## Audited guideXOS Server interfaces

The inspected Server declarations are in
`D:\dev\guideXOSServer_NATIVE_DB_STORAGE\kernel\core\include\kernel\native_database_file.h`
and `kernel/core/include/kernel/vfs.h`; the bridge implementation is
`kernel/core/native_database_file.cpp`, and the production VFS implementation
is `kernel/core/vfs.cpp`.

| Boundary | Existing contract and limitation |
|---|---|
| File open and lifetime | `GxDbNativeFileOps` exposes open/close and the bridge exports `gx_db_file_open`/`gx_db_file_close`. Handles are generation-tagged `GxDbFileHandle`s backed by a fixed 32-slot bridge table. The VFS uses an 8-bit handle and a mutable per-handle cursor. |
| Read-only access | `GX_DB_OPEN_READ_ONLY` maps to VFS `OPEN_READ`; the bridge rejects writes on that handle. This does not exclude writes through another handle or process. |
| Positional reads | `gx_db_file_read_at` saves `tell`, seeks, reads exact bytes, and restores the cursor (`native_database_file.cpp:319-343`). A short transfer is `GX_DB_FILE_SHORT_READ`. This sequence is not an atomic positional-read primitive and has no database/file lock around it. |
| File size | `gx_db_file_get_size` calls VFS `file_size`; the bridge rejects extents above `0xffffffff`. The SQL page count still comes from the checked database header. |
| Buffers and pages | The bridge reads into a caller buffer. It exposes no SQL page type, page borrow, page pin, or page-release API. |
| Status | `GxDbFileStatus` distinguishes stale handle, read-only, short read, corruption, resource limit, and I/O errors. The adapter retains both a provider error and the native file status. |
| Synchronization | The bridge's `s_ops` and 32-slot `s_files` arrays are static and have no locking. The VFS has a static file-handle table (`vfs.cpp:26`) with mutable cursor positions; the audited open/read/seek paths contain no synchronization. |
| SQL integration | The checkout has the NDB1R3 file bridge, but no native SQL database/catalog connection or root-publication API. `docs/NDB1R4_NATIVE_SQL9_INTEGRATION.md` records the SQL9 native build gate: authoritative SQL sources depend on STL headers/runtime absent from the freestanding AMD64 kernel target. |

No native SQL mount/service lifetime, root handoff, page cache borrow, or
writer-publication coordinator exists in the inspected Server tree. A VFS
read-only handle is a per-handle access mode, not an immutability lease.

## Selected consistency mode and root semantics

The implemented mode is **Mode A: caller-attested immutable read-only image**.
`GxNativeSqlIndexImageIdentity` carries page size/count, catalog root, index
root, owner index ID, and the consistency mode. The provider:

- rejects `GX_NATIVE_SQL_CONSISTENCY_LIVE_SINGLE_INSTANCE`;
- opens the logical `.gxdb` path using the real
  `gx_db_file_open(..., GX_DB_OPEN_READ_ONLY, ...)` declaration;
- checks the SQL header magic, major version, header size, and CRC using the
  persisted format and existing CRC implementation;
- matches page size, page count, and catalog root to the supplied identity;
- checks page-count multiplication, the Server 32-bit file extent, truncation,
  and physical page alignment (complete aligned orphan pages after the
  logical page count are ignored, matching `DatabaseFile` validation);
- checks the supplied index root is zero or within the captured page count.

The adapter does **not** parse `Catalog::IndexRecord` to discover the index
root. The identity producer must obtain the index root, owner, and page
metadata from the same committed image state. The R3AF fixture test records
that identity using the production hosted `DatabaseReadView`, closes the
production writer, then reads that unchanged production-written image. This
proves the provider on a controlled, immutable test image. The current Server
source has no native catalog resolver or lease that can make this identity
available while proving that all other writers are excluded. The adapter is
therefore not activated for a native SQL database.

Mode B remains unsupported. The R3AE mutex is handle-local and cannot cover
independent VFS file handles. The Server positional-read bridge also has no
file-scoped synchronization. A generation check or captured root alone would
not stabilize pages across a tree traversal.

## Implemented integration seam

Added:

- `database/native/gx_native_sql_index_page_provider.h`
- `database/native/gx_native_sql_index_page_provider.cpp`
- `tests/database_ndb1r4p11r3af_test.cpp`
- An optional CMake target enabled by
  `-DGX_GUIDEXOS_SERVER_SOURCE_DIR=<checked Server source root>`.

The provider context, single-page byte buffer, and identity are caller-owned;
the adapter owns the Server read-only file handle from open to close. The
provider uses the existing `GxIndexPageProvider` callbacks and
`gxDatabasePageInspect`; it does not define a competing page-provider API or
duplicate page parsing. It verifies exact outer-page CRC/ID, full index-page
payload extent, and leaf/internal page type, then returns a borrowed span from
the caller's one-page buffer. It refuses a second outstanding borrow and
refuses close while a borrow remains. Release validates the returned token and
span. Buffer reuse occurs only after release.

The adapter contains no STL, `DbValue`, exceptions, RTTI, or dynamic
allocation. `gxNativeSqlIndexPageOffset` checks `u64` multiplication and the
Server bridge's 32-bit positional extent before any page read. It reports
specific adapter errors and preserves `GxDbFileStatus`, so a storage failure
cannot become a not-found result. The portable index operations remain
responsible for their established internal-node, leaf-entry, successor,
owner, ordering, cycle, and traversal-limit validation.

The hosted test target compiles and links the actual Server
`native_database_file.cpp` against the actual Server headers. It binds the
bridge's existing `GxDbNativeFileOps` seam to file-backed hosted callbacks and
provides inert VFS symbol definitions solely to satisfy the bridge's default
operation table. This is explicitly a **hosted contract test**: it executes
the production bridge and production portable index operations against actual
SQL images, but it does not call production `kernel::vfs` implementations.

## Focused qualification

The R3AF target creates disposable 512-byte-page images through the production
SQL writer: an index with no allocated root, a leaf-root tree, and a
multilevel tree with 80 duplicate Int64 keys spanning leaves. It also indexes
nullable Boolean, Int32, and Text data, including Text containing embedded
zero bytes. It compares point and range output to the existing hosted index
facade and independently derived heap `TableScan` locators.

Coverage includes:

- Empty, leaf-only, and multilevel roots; exact point lookup, missing keys, and
  duplicate keys spanning leaves.
- Bounded, one-sided, and unbounded range scans; exact ordered locators; NULL,
  Boolean, Int32, Int64, Text, and embedded-zero Text keys.
- Read-only open, exact header/file extent checks, checked page offsets,
  read-only byte-for-byte source-image preservation, and provider error detail.
- Explicit page acquire/release, refusal to close during a borrow, safe buffer
  reuse after release, invalid/stale file handles, invalid roots/pages,
  truncation, short read after prior visitor output, and traversal-limit
  exhaustion.
- Unsupported live mode rejection and corruption on disposable copies:
  invalid internal-node signature, invalid leaf slot/entry, and invalid leaf
  successor. Corruption tests recompute the outer page checksum on the copy so
  the portable index parser receives the malformed index payload.
- Failure handling discards visitor results after a later provider error;
  the existing hosted R3AE output-staging path remains covered by its own
  regression target.

**R3AF result: 1/1 CTest target passed; the direct executable reported 205
checks passed and 0 failed.** The target's test images are generated afresh in the
temporary directory and removed. The source database bytes are compared
before and after provider operations. No existing user database is modified.

## Native source/compile/link/execution evidence

- **Source-compatible:** the adapter includes the exact Server
  `native_database_file.h`; its implementation uses the declared
  `gx_db_file_open`, `gx_db_file_get_size`, `gx_db_file_read_at`, and
  `gx_db_file_close` functions. The Server VFS and bridge sources were audited
  read-only.
- **Compile-compatible:** the adapter translation unit compiled with MinGW
  15.2.0 and the checked Server AMD64 freestanding flags, including
  `-ffreestanding -fno-exceptions -fno-rtti -nostdlib -nostdinc++`.
- **Linked:** the optional hosted target linked the adapter and actual Server
  `native_database_file.cpp`; the VFS symbols in this target are test
  definitions, not production VFS symbols. No native kernel link was claimed.
- **Executed in hosted qualification:** `gxIndexLookupAll` and
  `gxIndexRangeScan` executed against disposable production SQL images through
  the adapter and actual bridge C ABI implementation. Host file-ops callbacks
  and test-only VFS definitions back the bridge in this target.
- **Native guest-verified:** not tested. QEMU and QAC were not run.

The freestanding adapter object has only these expected external dependencies:
`gxDatabasePageInspect`, `gxos::db::kFileMagic`, `gxos::db::crc32`, and the
four `gx_db_file_*` calls for open/size/read/close. Those symbols are not
resolved in a native kernel link in this phase. The established portable
lookup/range probes continue to link with no undefined symbols.

## Regression qualification

The R3AF focused CTest target passed. The focused R3R–R3AF and R3P5–R3P8 set
passed **19/19**, including the R3Z/R3AA/R3AB/R3AC/R3AD/R3AE regressions.

The full Debug CTest aggregate passed **36/36 targets** in **2003.46 seconds**
with no failures or skips. This aggregate does not provide one combined
assertion count. The previously reported R3AE full regression was 35/35; this
phase's configured aggregate has 36 targets because the optional R3AF target
is enabled against the checked Server checkout.

The final freestanding census using
`tools/qualify_sql9_freestanding.ps1` and the checked Server AMD64 tree remains
**26/44 passing**, preserving all previously passing translation units. The
existing first `database/database_index.cpp` blocker remains the missing
`<string>` header under `-nostdinc++`; no new census blocker was introduced.
The production key-boundary, internal-descent, and complete-lookup/range
probes compiled, linked with `--no-undefined`, and executed with no unresolved
symbols. The R3AF adapter was separately compiled under the same kernel flags;
its native bridge/page-codec dependencies await the still-unavailable native
SQL integration link.

`SANITIZER_NOT_RUN — TOOLCHAIN_UNAVAILABLE`, preserved from R3AE. No toolchain
was installed and no build-infrastructure work was undertaken.

## Readiness assessment

| Area | R3AF assessment |
|---|---|
| Portable point lookup | Verified by existing production qualification and R3AF execution. |
| Portable range scan | Verified by existing production qualification and R3AF execution. |
| Hosted operation read view | R3AE verified for one hosted `Database` instance and facade writer paths only. |
| Native VFS positional reads | Source-compatible with the checked C ABI; actual production VFS not exercised by R3AF. |
| Native page-provider adapter | Implemented as immutable-image-only partial seam; compiles freestanding and executes in hosted contract test. |
| Root identity | Captured by hosted `DatabaseReadView` in tests; no native Server catalog/root resolver. |
| Immutable-image consistency | Proven for controlled, writer-closed test images; no general Server immutability lease. |
| Live publication coordination | Blocked; no Server database root/page publication lock or file-scoped coordinator. |
| Page borrow/release | Verified for one caller-owned page buffer with explicit release. |
| Native freestanding compilation | Adapter translation unit verified; native kernel link remains blocked. |
| Native guest execution | Not tested. |
| Cross-handle/process safety | Unverified and unsupported. |
| Native index writes | Not implemented. |
| Native WAL and recovery | Not qualified. |

## Unsupported access and next milestone

The adapter must not be used while any other handle/process can modify the
image. The Server read-only handle only blocks writes through itself. The
R3AE hosted mutex does not coordinate independent Server VFS readers or
writers, and the bridge's shared position-based implementation has no
operation lock. Concurrent access through one provider context is also
unsupported. No historical snapshot or cross-process coordination is
provided.

The smallest remaining prerequisite is a native owner that can provide both
(1) an index root and page metadata captured from one committed SQL catalog
state and (2) a verifiable immutable-image lifetime that excludes every
writer for the entire read operation. The next bounded phase should qualify
the provider on an actual guideXOS read-only image/mount and add the narrow
native catalog/root handoff required to select an index. Keep live reads
gated until a real Server publication coordinator covers root capture through
the last page borrow.

## Commits and limits

- Starting HEAD: `fb06228c7aad284328c5dee6c0c34e5c0d4a7a1f`.
- Implementation commit: `2a461fa64d01932f9dab36bb9bbf2066792e9804`; this
  final evidence update is a separate documentation-only local commit.
- No Server changes, push, history rewrite, QEMU/QAC run, native guest
  execution, live-writer enablement, or database-format change.
