# NDB1R4P11R3AG — Native Immutable Index Linkage Qualification

## Result

**Outcome A — `NATIVE_IMMUTABLE_INDEX_LINKAGE_VERIFIED`.** The production SQL
native page provider and portable point/range reader compiled under the Server
AMD64 freestanding flags and linked into a candidate PE and ELF kernel image
with the checked Server production object set. The candidate includes the
production bridge and VFS objects. It adds no unresolved/import symbols beyond
those in the existing kernel image. A hosted qualification harness executed
the production SQL reader, provider, and actual Server bridge implementation
against closed, production-written immutable test images.

The hosted harness supplies file operations to the bridge through its existing
`GxDbNativeFileOps` table. It does not execute the production kernel VFS. The
kernel candidate was linked but not booted. The Server still lacks a native
catalog/root handoff and operation-lifetime immutable-file guarantee. Live
database access remains unsupported.

## Repository state and phase markers

- SQL repository: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `4584c7e22c4eb13d14eefee85e2f54cfde2f709d`; the starting
  worktree was clean. The prompt expected two commits ahead of local
  `origin/main`; verification found the starting local tracking ref at the same
  HEAD (zero ahead, zero behind). No fetch was performed.
- Implementation ending HEAD: `8ca093c05380eef6c3d18e9bb51c4a0f78dad258`.
  This report is committed separately as a documentation-only closeout.
- `.phase` remains SQL9 → SQL10; SHA-256
  `f84ef02a6b82d4c53cdb2bdd0b80c083a49c6fa2e5df46914d9aadd7555295f0`.
- `.native-sql.phase` remains NDB1R4P10 → NDB1R4P11; SHA-256
  `fe722cf1a92a04c9f16e91e556933c682a7e3475bf3aabb06ce994410a28a52a`.
- Server checkout: `D:\dev\guideXOSServer_NATIVE_DB_STORAGE`, branch
  `NATIVE_DB`, HEAD `f29850c93bbac0a0763a31e806d2816310156575`. No tracked or
  untracked Server source changes were made.
- R3AF and R3AE reports and the native provider, portable index, database
  header/catalog, and Server bridge/VFS implementations were read before the
  implementation work.
- No push, history rewrite, QEMU/QAC launch, guest execution, phase-marker
  edit, or persistent database/index/WAL format change occurred.

### Build-artifact side effect

During the initial Server artifact audit,
`mingw32-make -C Server\kernel ARCH=amd64 -q` unexpectedly bootstrapped and
recompiled `kernel/core/main.cpp`. It created or overwrote these ignored build
outputs in the Server checkout:

- `kernel/build/amd64/obj/core/main.o` (54,285 bytes)
- `kernel/build/amd64/obj/core/main.d` (6,332 bytes)

The Server Git status remained clean because these are ignored build outputs.
An attempt to remove those two exact generated files was rejected by automatic
command review with the reason “blocked by policy.” They remain in place; no
alternative cleanup command was attempted. The separate R3AG qualification
script wrote its new evidence directory under the SQL repository's ignored
`build/r3ag-native-link-runs` directory.

## Native dependency and ABI audit

The real Server declarations are in
`kernel/core/include/kernel/native_database_file.h`; the bridge implementation
is `kernel/core/native_database_file.cpp`, and its production storage calls
reach `kernel/core/vfs.cpp` and `kernel/core/include/kernel/vfs.h`.

| Boundary | Audited contract |
|---|---|
| Bridge ABI | The header declares a C ABI (`extern "C"`) with `uint32_t` `GxDbFileHandle`, `GxDbFileStatus`, and open-mode enums. The native target is AMD64 with 64-bit pointers and the Server's MinGW C/C++ ABI. No custom calling convention is specified. |
| Open and close | `gx_db_file_open` accepts a logical path, mode, truncate flag, and output handle. The provider opens with `GX_DB_OPEN_READ_ONLY`; it owns that bridge handle until close or an error cleanup path. The bridge uses generation-tagged handles and a fixed 32-slot table. |
| File size | `gx_db_file_get_size` obtains the physical file extent through the VFS. The bridge rejects extents above `0xffffffff`; the provider also bounds expected image size and page offsets to that positional-read extent. |
| Positional read | `gx_db_file_read_at` saves the VFS cursor, seeks, reads, and restores the cursor. Short transfers return `GX_DB_FILE_SHORT_READ`. This is not an atomic positional-read or immutable-snapshot operation. |
| Read-only semantics | The bridge rejects write calls through a read-only handle. Other handles or processes can still write the same file. |
| Native storage ops | `GxDbNativeFileOps` exposes the existing VFS open/close/read/write/seek/tell/file-size/flush/truncate/stat/unlink operations. Production defaults call the Server VFS. The hosted harness binds this existing table to file-backed callbacks. |
| Status | `GxDbFileStatus` distinguishes invalid/stale handles, read-only access, short I/O, corruption, resource limits, and general I/O errors. Provider errors are reported separately from `nativeFileStatus`; cleanup-close status is retained in `cleanupFileStatus`. |
| Buffers and allocation | The caller owns a stable provider context and a writable buffer at least one database page in size. The provider allocates no memory. It owns no returned bytes beyond the borrow; every successful page view points into the caller's reusable page buffer. |
| Synchronization | The bridge's operation table and fixed handle array and the VFS handle table are shared static state without an operation-lifetime file lock. No immutable lease or cross-handle writer exclusion exists. |

The provider depends on actual declarations for `gx_db_file_open`,
`gx_db_file_get_size`, `gx_db_file_read_at`, and `gx_db_file_close`. It also
uses `gxDatabasePageInspect`, `gxos::db::crc32`, database format constants,
and the portable page-provider interface. No native allocator or new operating
system API is introduced.

The kernel qualification compiled these 16 SQL translation units:

- `database/native/gx_native_sql_index_page_provider.cpp`
- `database/database_checksum.cpp` and `database/database_format.cpp`
- `database/portable/gx_page_codec.cpp`, `gx_allocator.cpp`, `gx_array.cpp`,
  `gx_page_visit_set.cpp`
- `database/portable/gx_index_boolean_key.cpp`,
  `gx_index_integer_key.cpp`, `gx_index_text_key.cpp`,
  `gx_index_physical_key.cpp`, `gx_index_leaf_entry.cpp`,
  `gx_index_leaf_traversal.cpp`, `gx_index_internal_node.cpp`,
  `gx_index_tree_descent.cpp`, and `gx_index_lookup.cpp`

The actual portable entry points are `gxIndexLookupAll` and
`gxIndexRangeScan`; both use the existing provider callbacks and root-to-leaf
descent. R3AG did not copy or replace B+ tree algorithms.

## Immutable image and root identity

The controlled image is created by the production `Database::create` path,
then populated through the existing table and index write paths. The test
captures page size/count, catalog root, each index root, and index owner ID
from the production hosted `DatabaseReadView`. It closes the writer before
opening the image through the native reader. Existing close/checkpoint behavior
establishes a closed fixture for this test. A separate hosted reopen and
`TableScan` supplies independent expected locators. The provider itself does
not open or replay the WAL sidecar.

For any use of this adapter, the caller must first recover the database if
needed, finish commit/checkpoint work, close all writers, and provide one
internally compatible database image. An image that requires unperformed
recovery is outside the qualified scope. The fixture test does not establish
that an active-writer file copy is safe.

The header stores page size, page count, and catalog root. The index root is a
`Catalog::IndexRecord` field, not a header field. The identity passed to
`gxNativeSqlIndexPageProviderOpen` includes page size/count, catalog root,
index root, owner index ID, and `GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE`.
The provider checks the header magic, supported format major, header size, and
CRC; checks supplied page size/count/catalog-root against the header; validates
page size/count and root bounds; verifies file length covers the logical image
and is page-aligned; and checks each accessed outer page's CRC, ID, payload
extent, and page type. Format minor is retained but not separately rejected,
consistent with `DatabaseHeader::parse`, which gates compatibility on the
major version.

The identity producer must read the index root and owner from the same
committed catalog view as the page metadata. The adapter does not parse the
catalog or discover roots. A wrong root/owner association is rejected when the
portable page decoder validates ownership on descent. The R3AG suite changes
the owner for a valid tree root and confirms corruption is returned without
publishing locators.

The image is stable only by caller attestation. The test calculates SHA-256
before and after reading and asserts equality. This is evidence that the
controlled test operation did not alter its source image; it is not a
filesystem lease and cannot prove absence of concurrent external writes.

## Integration and linkage evidence

`tools/qualify_ndb1r4p11r3ag_native_link.ps1` creates a unique output directory
under the SQL repository's `build/r3ag-native-link-runs`. It uses the checked
Server AMD64 flags, including `-std=c++14`, `-ffreestanding`,
`-fno-exceptions`, `-fno-rtti`, `-nostdlib`, `-nostdinc++`, `-fno-builtin`,
`-m64`, `-mcmodel=large`, `-mno-red-zone`, and the Server's no-SIMD/no-PIE
settings. The recorded compiler was MinGW-W64 `g++ 15.2.0`.

Evidence from the final run:

- All 16 SQL translation units compiled, plus the actual Server bridge source
  (`17_SOURCES_PASS`). The freshly compiled bridge's exported symbol set
  matched the checked production bridge object's exports.
- The candidate linked those SQL objects with 158 existing Server AMD64
  production object files, including `native_database_file.o` and `vfs.o`,
  using the Server AMD64 linker script and `libgcc`.
- The linker produced a candidate `guideXOSKernelR3AG.pe`; Server `objcopy`
  settings then produced `guideXOSKernelR3AG.elf`. Symbol inspection confirmed
  the provider, point/range reader, bridge, and VFS symbols are present.
- Undefined/import scans found 21 symbols in both the existing baseline kernel
  image and candidate, with zero SQL-added imports. The unchanged baseline set
  is `__dso_handle`, `mainCRTStartup`,
  `gxos::network_settings::validateCandidate(...)`,
  `gxos::settings_inventory::validStorageSnapshot(...)`, the 13 existing
  `kernel::app::KernelApp` event/update methods, and four existing
  `kernel::serial` output methods (`put_hex32`, `put_hex8`, `putc`, `puts`).
  Each is present in both image scans; the candidate introduced no additional
  unresolved/import symbol.
- The SQL object scan rejected hosted runtime dependencies such as STL,
  exception/RTTI machinery, allocation functions, and hosted I/O. The image
  link uses no VFS test stubs.
- The existing stale test-only `qemu_ata_multiboot_boot.o` object, absent from
  current Server source, was excluded because it defines a second `_start`.
- Evidence files are under
  `build/r3ag-native-link-runs/NDB1R4P11R3AG_20261101_153119_8944_525ba851c26a44d7a6068e3afdf507a3`.

This is **kernel-link-qualified** at candidate-image link level, beyond a
standalone relocatable link. It is not integrated into the Server's normal
source build and was not executed. The test target separately compiles the
actual `native_database_file.cpp` and runs that bridge implementation with its
existing file-ops seam; the test's VFS functions are hosted definitions. The
production `kernel::vfs` read/open/seek paths were not executed.

## Hosted immutable-image qualification

The R3AG target builds disposable 512-byte-page images for empty, leaf-only,
and multilevel indexes. The multilevel fixture has 80 rows. Its indexes cover
nullable Int64, Int32, Boolean, and Text values; the Text fixture includes
embedded zero bytes. Expected rows are derived independently by reopening the
closed image and scanning the heap table, then compared with the existing
hosted SQL index result and native-provider portable-reader result.

The reader qualification covers exact and missing point keys, duplicate keys
across leaves, NULL keys, exact `RowLocator` ordering, inclusive bounded range,
exclusive lower and upper bounds, lower-only, upper-only, and unbounded scans.
The tests exercise an Int32 `[1, 2)` range and Int64 key `7`, plus the other
typed and NULL lookups. For the 80-row fixture, key `7` has 73 non-NULL rows
(rows whose IDs are divisible by 13 have NULL); key `999` has no rows. The
Int32 `[1, 2)` range has 16 rows. The tests compare exact locators against the
independently generated scan results; because the fixture is generated anew,
physical page/slot locator lists are derived at runtime rather than stored as
a stable guest manifest.

The R3AG result also includes:

- Valid borrow/release, repeated reads through one caller buffer, close refusal
  during an outstanding borrow, invalid release, invalid page ID, and borrow
  release after visitor stop/failure.
- Invalid identity/context, unsupported live-consistency mode, invalid root,
  page-size/buffer errors, missing file, file-size failure, truncated file,
  malformed header/CRC, unsupported major version, wrong owner/root
  association, unexpected page type, and malformed index payloads.
- Storage status preservation on failed `get_size`, with cleanup close status
  recorded separately. Short-read and downstream provider failures are
  exercised, including failure after a visitor has seen rows.
- A staged-result helper discards partial visitor output unless the complete
  lookup succeeds. The visitor API can stream rows before a later error, so
  production callers that need atomic publication must stage their output.
- Byte comparison and SHA-256 before/after checks for each source image. The
  recorded before/after hashes matched for empty, leaf, and multilevel images:
  `daa48bcae2b5434f1a02976ee8aaa60e6bfc453d1073708f042d30fd9a2a207c`,
  `39e177c189ef13af8ba1a996d0f16430a4cb8d6a95044ccf9d8ace407ffc3c6b`, and
  `7e4c5d851ae1d496cc1c90e47f6fcca68f9c2f162fb8671872539d9c087e64f1`.

The hosted target tests category 1: production SQL lookup/range through a
controlled provider harness. The harness executes the production bridge
implementation with supplied file operations. It does not qualify category 2
(production Server VFS execution) or category 3 (native guest execution).
Native SQL writes are absent.

## Tests, freestanding census, and sanitizers

- Direct R3AF executable: **232 passed, 0 failed**.
- Direct R3AG executable: **292 passed, 0 failed**.
- Focused `database_ndb1r4p11r3*` CTest selection, including R3AE–R3AG and
  native linkage: **17/17 targets passed**.
- Full Debug CTest aggregate: **38/38 targets passed**, no failures, in
  **1360.11 seconds**. This includes portable runtime, index, R3Z–R3AG,
  transaction, WAL, recovery, storage, and native-linkage regressions. CTest
  reports target counts rather than a single aggregate assertion count.
- Existing freestanding census, with unchanged effective flags:
  **26/44 translation units passed**, preserving all previous 26 passes.
  Existing portable key/descent/lookup/range probes still pass. The native
  provider was additionally compiled in the R3AG 17-source proof. The first
  remaining hosted blocker in `database/database_index.cpp` is unavailable
  `<string>` under `-nostdinc++` (the hosted dependency surface also has earlier
  standard-library blockers in other database units). No census passes were
  added by relabeling a hosted dependency.
- `SANITIZER_NOT_RUN — TOOLCHAIN_UNAVAILABLE`. Clang executables and the GCC
  ASan/UBSan runtime libraries were unavailable. No toolchain was installed.
- `git diff --check` passed. The phase marker hashes above remain unchanged.

## Unsupported scenarios and guest-readiness boundary

The provider rejects the live-single-instance mode. It cannot establish that
another handle/process will not modify the image. Server positional reads
save/seek/read/restore a shared cursor, and bridge/VFS tables lack a lock
covering root capture through the final page release. The current Server has
no native catalog/index-root resolver, immutable-file lease, or publication
coordinator. An unrecovered image, a stale root/catalog view, a mutable file,
and an active writer are outside the qualified contract. No production
activation for those cases is appropriate.

No guest launch was authorized or performed. A separately authorized guest
phase will need to add or configure the SQL sources in the normal kernel build,
provide a controlled closed `.gxdb` image and expected-result manifest, capture
root/page metadata and index owner from that exact image, and mount/open it so
its lifetime excludes writers. It will need a kernel test entry point that
prints a serial start marker, point/range results, and a final pass/fail marker.
The production VFS and path mapping must be exercised in that phase.

Suggested guest fixture criteria, derived from the current 80-row generated
fixture:

- Point lookup: Int64 key `7`, exactly 73 ordered locators; key `999`, zero
  locators.
- Range scan: Int32 lower-inclusive `1`, upper-exclusive `2`, exactly 16
  locators ordered by logical key and then physical locator.
- Compare the full locator sequences with a manifest generated from the same
  closed image. Row counts alone do not verify order or root provenance.
- Require all provider borrows released, a successful close, no write access,
  and exact serial `PASS` output. Treat any error, missing/extra/reordered
  locator, or incomplete output as failure.

The generated fixture's physical locators vary with its newly created image;
the guest phase must preserve one concrete image and its manifest together.
The SQL repo contains no snapshot/backup facility added for this test. The next
bounded phase should focus on normal Server build integration and an explicitly
controlled guest test image. Live access should remain gated until root
provenance and writer exclusion are provided by a real Server contract.

## Commits

- Starting HEAD: `4584c7e22c4eb13d14eefee85e2f54cfde2f709d`.
- Implementation/source commit: `8ca093c05380eef6c3d18e9bb51c4a0f78dad258`.
- This report is a subsequent documentation-only local commit; the delivered
  final HEAD is reported with the closeout. Nothing was pushed.
