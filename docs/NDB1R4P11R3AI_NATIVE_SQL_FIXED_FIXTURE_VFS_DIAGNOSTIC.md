# NDB1R4P11R3AI — Native SQL Fixed-Fixture VFS Diagnostic

## Outcome

**Outcome A — `NATIVE_SQL_VFS_DIAGNOSTIC_READY`.** The Server now has a
compile-time gated, fixed-fixture SQL point/range diagnostic wired through its
production VFS bridge and normal AMD64 kernel build. The exact production FAT
VFS, read-only RAM disk, SQL provider, and lookup/range implementation passed a
hosted integration run. A fresh UEFI ESP image and ISO were built and
structurally verified. This is readiness for a separately authorized guest
qualification; no QEMU, QAC, or guest execution occurred in R3AI.

## Repository state

| Repository | Branch | Starting HEAD | Ending HEAD |
|---|---|---|---|
| SQL | `main` | `80a594e4a30b652a2d207ac60d8100b214885c4a` | Final local report commit (exact ending HEAD returned at phase completion) |
| Server | `codex/ndb1r4p11rah-sql-read-integration` | `552dd4ee17b26cf0b367f03dd1d8a46a443ff29c` | `3e102c3bce7e1a3ecef8634a6fa640b05e72ec10` |

The Server branch started at integration baseline
`f29850c93bbac0a0763a31e806d2816310156575`. The original Server checkout
`D:\dev\guideXOSServer_NATIVE_DB_STORAGE` was not modified. Its protected
`main.o` and `main.d` were not rebuilt, removed, or used. No reset, rebase,
stash, clean, push, merge, broad clean target, or `mingw32-make ... -q` was
used. Build, package, and test outputs are under the isolated Server branch's
`out\ndb1r4p11r3ai` tree and phase-specific kernel build directories.

The SQL production tree was not changed; the SQL commit made for this phase
adds this report. The candidate manifest therefore records SQL source commit
`80a594e4a30b652a2d207ac60d8100b214885c4a`.

## Startup and fixture delivery audit

The real AMD64 startup path initializes serial output at the start of
`kernel_main`, initializes block devices and filesystem drivers, initializes
the RAM disk and VFS, then attempts its normal persistent-storage mount. On
UEFI BootInfo, the existing `set_wallpaper_image_pack` path attaches
`BootInfo::RamdiskBase` and `RamdiskSize` with
`ramdisk::create_readonly_at`. If there is no persistent root, that code mounts
the boot image at `/` and aliases `/system` to it. If a persistent root exists,
it mounts the boot image at `/system`. In either case, the RAM-disk block
device has a read callback and no write callback, so `vfs::mount_type` marks
the mount read-only. The diagnostic call is after this mount attempt and before
desktop reload/discovery work:

```text
serial init
  → block/filesystem/RAM-disk init
  → VFS init
  → normal persistent-storage mount attempt
  → BootInfo ramdisk attach and / or /system mount
  → gated SQL diagnostic
  → diagnostic closes provider/file resources
```

The selected delivery is the existing UEFI bootloader `ramdisk.img` handoff,
not a new filesystem or a direct physical-disk path. The package places
`MULTI.GDB` and `FIXTURE.MAN` under `/Apps/R3AI`; the established VFS mount
exposes the fixture as:

```text
/system/Apps/R3AI/MULTI.gxdb
```

The existing SQL bridge maps that logical `.gxdb` path to FAT's `/system/Apps/R3AI/MULTI.GDB` name. Its production operations call Server `vfs::stat`, `open`,
`file_size`, `seek`, `read`, and `close`. The bridge checks offset/length
bounds and exact positional-read lengths and maps VFS errors into the SQL file
status contract. The native index page provider opens read-only, holds a
single fixed-size page buffer, allows one active page borrow, and releases it
after decoding. The diagnostic uses the existing production
`gxIndexLookupAll` and `gxIndexRangeScan` algorithms; it introduces no second
tree traversal, STL use, exception/RTTI support, or unbounded allocation.

The hosted R3AI harness compiles and executes the production Server
`vfs.cpp`, FAT filesystem, RAM-disk, block-device, native database bridge,
native SQL provider, and SQL production lookup sources. It is an actual
production-VFS run in a hosted process, not a mock bridge test. It is not a
native kernel guest run.

## Fixture provenance and runtime checks

The selected input is the existing closed, production-written R3AH multilevel
fixture. The writer was closed before packaging and its `.gxwal` sidecar is
absent. The trusted manifest and earlier production verification tie the
header, database ID, creation time, catalog root, index records/owners, encoded
root pages, and all 17 expected query results to that same image state. That
verification obtains roots from the database catalog/read view and asks the
production provider to read the selected encoded roots; it does not guess a
root by searching page contents.

Package-time checks require the committed fixture to be 250,880 bytes with
SHA-256
`cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57`, and the
trusted manifest SHA-256 to be
`43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3`. The
package script checks the expected format 1.0, page size 512, page count 490,
catalog root 1, selected index names/roots/owners, and point/range result
counts before copying. The release helper checks the staged file hashes.

The manifest binds database UUID
`32b46622-aa73-493d-a26d-e826de2bb790`, creation timestamp
`1793556264120817800` Unix nanoseconds, and the v3 table-1 catalog to this
image. The selected roots are internal index pages: `IX_Rows_K64` root 234,
owner 1; `IX_Rows_K32` root 148, owner 2. Its fixed queries are manifest
`query.1` (`duplicate_k64_7`, 73 locators) and `query.10`
(`range32_inclusive_1_2`, 32 locators).

At runtime, the diagnostic checks that `/system` is an active FAT32 read-only
mount backed by a device with a read callback and no write callback. The
provider checks the header and fixed image dimensions. The diagnostic checks
format 1.0, exact database ID and creation timestamp, file length, page size,
page count, and catalog-root value. It acquires each selected index root via
the provider, requires an encoded internal-index page, and validates the
decoded internal-node owner ID. It does not contain a general catalog parser
or a kernel cryptographic hash verifier; catalog/root metadata is handed off
from the trusted offline fixture package and checked against fixed runtime
expectations. The manifest is trusted provenance for this controlled test,
not a cryptographic signature.

The fixture is stable for the intended diagnostic interval because it is a
prebuilt boot RAM-disk snapshot, attached read-only, with no VFS/block-device
write path and no concurrent writer. The hosted test also verifies that the
fixture bytes are identical before and after repeated runs. This is
immutability through the supported storage/VFS interface; the RAM image is
still ordinary kernel memory and is not protected against arbitrary kernel
code writing directly to its address. No such writer is used by this
diagnostic.

## Diagnostic gate and results

The normal build defaults both options to off. The diagnostic requires both
explicit flags and AMD64:

```text
GX_NATIVE_SQL_READ_QUALIFICATION=1
GX_NATIVE_SQL_READ_DIAGNOSTIC=1
```

The missing qualification gate was checked directly and fails at Makefile
parse time with `GX_NATIVE_SQL_READ_DIAGNOSTIC requires
GX_NATIVE_SQL_READ_QUALIFICATION=1`. With the normal configuration, the
diagnostic translation unit and startup call are excluded; no fixture,
diagnostic initialization, or SQL runtime startup dependency is required.

The diagnostic is bounded and deterministic. It uses fixed stack buffers and
fixed result arrays, emits no database contents, closes the provider after
each operation, and returns controlled failure codes without panicking. It
was run twice successfully on the same mounted image and twice with the
fixture missing; the missing case ended in controlled failure and left no VFS
handle open. An intermediate failure cannot emit `END PASS`.

Expected successful serial sequence:

```text
SQL_NATIVE_R3AI BEGIN fixture=NDB1R4P11R3AH_MULTILEVEL
SQL_NATIVE_R3AI FIXTURE_OK length=250880 page_size=512 page_count=490 catalog_root=1
SQL_NATIVE_R3AI POINT_PASS index=IX_Rows_K64 root=234 owner=1 count=73
SQL_NATIVE_R3AI RANGE_PASS index=IX_Rows_K32 root=148 owner=2 lower=1 upper=2 count=32
SQL_NATIVE_R3AI END PASS
```

Failures emit `SQL_NATIVE_R3AI FAIL code=<stable-code>` followed by
`SQL_NATIVE_R3AI END FAIL`. The missing-fixture path reports
`FIXTURE_OPEN_OR_HEADER`; mount readiness failure reports
`VFS_UNAVAILABLE_OR_WRITABLE`. Header/metadata, root, point, range, and cleanup
failures have distinct fixed codes in
`kernel/core/native_sql_read_diagnostic.cpp`.

## Exact fixed lookup contract

The diagnostic encodes Int64 key 7 for `IX_Rows_K64`, root 234, owner 1. It
requires this complete ordered page:slot result sequence (73 locators):

```text
3:1, 3:2, 3:3, 3:4, 3:5, 3:6, 3:7, 10:0, 10:1, 10:2,
10:3, 10:4, 10:6, 10:7, 22:0, 22:1, 22:2, 22:3, 22:4, 22:5,
22:6, 22:7, 71:0, 71:1, 71:3, 71:4, 71:5, 71:6, 71:7, 130:0,
130:1, 130:2, 130:3, 130:4, 130:5, 130:6, 190:0, 190:1, 190:2,
190:3, 190:4, 190:5, 190:6, 190:7, 250:0, 250:1, 250:2, 250:3,
250:5, 250:6, 250:7, 308:0, 308:1, 308:2, 308:3, 308:4, 308:5,
308:6, 360:0, 360:1, 360:3, 360:4, 360:5, 360:6, 360:7, 422:0,
422:1, 422:2, 422:3, 422:4, 422:5, 422:6, 479:0
```

The inclusive Int32 range `[1, 2]` for `IX_Rows_K32`, root 148, owner 2,
must return this complete ordered sequence (32 locators):

```text
3:1, 10:1, 10:5, 22:1, 22:5, 71:5, 130:1, 130:5,
190:1, 250:1, 250:5, 308:1, 308:5, 360:6, 422:2, 422:6,
3:2, 3:6, 10:6, 22:2, 22:6, 71:2, 130:2, 130:6,
190:2, 190:6, 250:6, 308:2, 308:6, 360:3, 422:3, 422:7
```

## Hosted and build qualification

### Production-VFS hosted test

The phase-specific test executable mounted the exact candidate FAT ramdisk
using production `ramdisk::create_readonly_at`, `vfs::mount_type`, and the
production FAT32 driver. It checked byte identity with the committed source
fixture, no block write callback, rejected a VFS truncate mutation, ran the
production diagnostic twice, verified unchanged bytes and zero leaked VFS
handles, and exercised repeatable missing-fixture failure.

**Result: 22/22 checks passed.** The compiler emitted two pre-existing VFS
warnings (`write_root_dir_sector` unused and an always-false `nameLen > 255`
check); there were no diagnostic/provider warnings.

The captured output is
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ai\boot-candidate-final-v4\hosted-production-vfs-test.log`
(3,776 bytes, SHA-256
`6b0873d697d240d3e7b1dd3fcdddcc775d4aaa83bc3f142b675b44a1059f3486`).

The existing R3AG provider qualification in the same aggregate continues to
cover bridge argument/mode checks, page-offset overflow boundaries, short
reads, stale/invalid handles, unsupported format headers, corrupt and
unexpected page types, root/owner mismatch, visitor/provider errors, duplicate
keys, exact lookup/range results, and borrow/handle cleanup. The R3AH fixture
target independently checks the production-written catalog/root/manifest
association and all 17 fixture query cases. R3AI adds the missing actual
production-VFS mount, read-only RAM-disk enforcement, diagnostic transition,
and fixture-preservation evidence.

### SQL regression aggregate

The full Debug CTest aggregate was run from a fresh `build-r3ai-final`
directory after all source changes, using the existing 39-target configuration.
It includes storage/transaction/WAL/recovery regressions, SQL7/SQL8/SQL9,
portable index regressions from R3AA through R3AE, the R3AF provider suite,
R3AG native linkage, and the R3AH trusted fixture suite. It was built with
`GX_GUIDEXOS_SERVER_SOURCE_DIR` pointed at the original Server baseline for
the established native-link qualification; that checkout remained read-only.

**Result: 39/39 passed, 0 failed, 0 skipped; total time 1,501.92 seconds.**

The clean build command was:

```powershell
cmake.exe -S . -B build-r3ai-final -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DGX_GUIDEXOS_SERVER_SOURCE_DIR='D:/dev/guideXOSServer_NATIVE_DB_STORAGE'
cmake.exe --build build-r3ai-final --parallel 8
ctest.exe --test-dir build-r3ai-final --output-on-failure
```

The build completed all 143/143 steps successfully. In this R3AI aggregate,
`database_ndb1r4p11r3ag_test` reported 340 passed/0 failed and the
`database_ndb1r4p11rah_fixture_test` reported 340 passed/0 failed. R3AH's
preceding full aggregate was also 39/39.

### Server builds and symbols

The isolated Server branch built the ordinary and diagnostic configurations
through its normal AMD64 Makefile rules:

```powershell
mingw32-make.exe -j8 ARCH=amd64 BUILD_DIR=build/amd64-r3ai-disabled
mingw32-make.exe -j8 ARCH=amd64 BUILD_DIR=build/amd64-r3ai-diagnostic `
  GX_NATIVE_SQL_READ_QUALIFICATION=1 GX_NATIVE_SQL_READ_DIAGNOSTIC=1 `
  GX_SQL_DATABASE_ROOT=D:/dev/guideXOS_SqlDatabase
```

Both PE links and ELF conversions succeeded. The disabled image has no native
SQL diagnostic or lookup symbols. The diagnostic image includes the native
provider, `gxIndexLookupAll`, `gxIndexRangeScan`, and
`kernel::native_sql_read_diagnostic::run_after_vfs_mount`.
`nm -u -C` reports 21 undefined symbols in each image, with identical symbol
identities (zero set delta). PE inspection reports zero imports in each image.
No new forbidden unresolved symbols or imports were introduced.

The diagnostic-enabled Makefile gate was also invoked without
`GX_NATIVE_SQL_READ_QUALIFICATION=1`; it stopped at parse time with exit code 2
and the expected gate error.

The UEFI bootloader was built in the isolated output tree with
Visual Studio Community/MSBuild 18.11.1-1.26513.3 Release x64. It completed
with zero errors; its three compiler
option override warnings are from the existing project settings. No original
ESP, ISO, or kernel output was overwritten.

```powershell
MSBuild.exe guideXOSBootLoader\guideXOSBootLoader.vcxproj /m /t:Build `
  /p:Configuration=Release /p:Platform=x64 `
  /p:OutDir=D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ai\bootloader\ `
  /p:IntDir=D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ai\bootloader-obj\ `
  /p:BuildProjectReferences=false
```

The boot package used the committed fixture packager and the committed
candidate-preparation script, then the existing `tools/release_esp.py` and
`tools/release_iso.py` helpers against an isolated staging tree. The Server
checkout does not contain the `requirements-release.txt` file referenced by
its wrapper script, so the two existing Python helpers were run from an
isolated phase venv with `pyfatfs 1.1.0`, `fs 2.4.16`, `pycdlib 1.16.0`, and
`setuptools 80.9.0`; no global Python packages were installed. The helpers
verified all staged ESP payload hashes and the UEFI ISO structure. FAT
directory creation-time tenths vary between independent packaging runs, so a
rebuild can have a different whole-image hash while preserving the same
verified payloads. R3AJ must use the exact ISO bytes and hash listed here,
without rebuilding or substituting a new package.

The isolated packaging tools were prepared with:

```powershell
python.exe -m venv out\ndb1r4p11r3ai\release-tools\venv
& out\ndb1r4p11r3ai\release-tools\venv\Scripts\python.exe -m pip install `
  pyfatfs==1.1.0 fs==2.4.16 pycdlib==1.16.0 setuptools==80.9.0
```

### Freestanding census

The established census was rerun with MinGW-W64 `g++ 15.2.0` against the
isolated Server kernel flags. It preserved the baseline **26/44 passing
translation units**. The 18 existing failures remain in units requiring
hosted C++ headers/runtime facilities (for example `<cstdio>` and `<memory>`);
no unrelated STL removal or census adjustment was made. The new native
diagnostic itself compiled and linked under the Server's actual freestanding
flags (`-fno-exceptions -fno-rtti -nostdlib -nostdinc++`).

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File tools/qualify_sql9_freestanding.ps1 `
  -Compiler 'C:\mingw64\bin\g++.exe' `
  -ServerKernelRoot 'D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\kernel' `
  -OutputDirectory 'D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ai\freestanding-census'
```

## Candidate artifacts and hashes

The final package and candidate are phase-specific:

```text
Server output root:
D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ai

UEFI ISO:
out\ndb1r4p11r3ai\boot-candidate-final-v4\guideXOS-NDB1R4P11R3AI-amd64.iso
Candidate manifest:
out\ndb1r4p11r3ai\boot-candidate-final-v4\r3ai-boot-candidate-manifest.json
```

The exact package commands were:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File scripts/package-native-sql-r3ai-fixture.ps1 `
  -SqlRoot 'D:\dev\guideXOS_SqlDatabase' `
  -OutputRoot 'out\ndb1r4p11r3ai\final-fixture-package'
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File scripts/prepare-native-sql-r3ai-boot-candidate.ps1 `
  -SqlRoot 'D:\dev\guideXOS_SqlDatabase' `
  -PackageRoot 'out\ndb1r4p11r3ai\final-fixture-package' `
  -OutputRoot 'out\ndb1r4p11r3ai\boot-candidate-final-v4'
```

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| Diagnostic kernel ELF | 2,723,168 | `bfe3360b0d6437858e06381b0713be6c400a28c9f4f2b1bf6249b1510c6ac8fb` |
| Diagnostic kernel PE | 2,831,612 | `937ec72a17d7751ff13e17b868ae05f0c4abb7f1dcf91fca99d835383e8638ef` |
| UEFI bootloader `BOOTX64.EFI` | 56,832 | `002ce8def31974997f10179e6139b08e6985816eef31b641411bded60ce59b1e` |
| Fixture ramdisk | 61,865,984 | `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| Fixed database fixture | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted fixture manifest | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| ESP FAT32 image | 100,663,296 | `3670d859e3b386039bcc72be06a2fa2e64c59cf77ace3e326f3add8041289e0f` |
| UEFI El Torito ISO | 100,730,880 | `f22e3f3627b2ce6541bf9499934a4d726135574b9b654aa12a20351ed860d497` |

The existing release helpers independently checked the FAT32 ESP payload
hashes and UEFI El Torito structure (`platform ID 0xEF`, bootable,
no-emulation). This verifies package structure and payload identity; it does
not prove that firmware boots the kernel. The exact final ESP/ISO hashes are
copied from `r3ai-boot-candidate-manifest.json` before report commit.

The kernel binaries were built at Server commit
`b9cb6fd998a016dc267cdacdfe31f59d4a2288d8`, which contains all kernel and
Makefile changes. The later Server commits `a88ac83f6cba95ecbcd8caaede44facdd6f3f0a7`
and `3e102c3bce7e1a3ecef8634a6fa640b05e72ec10` update only the candidate
manifest-generation script with locator and serial expectations; they do not
change any kernel build input. The final candidate manifest and package were
generated at Server HEAD `3e102c3bce7e1a3ecef8634a6fa640b05e72ec10`.

## R3AJ guest acceptance contract

R3AI leaves guest execution for the separately authorized next phase. The
candidate manifest is the machine-readable source for the selected ISO and
fixture hashes and also records the exact expected serial marker strings and
the full ordered locator arrays.

1. Admit the run only when a QAC or QEMU reservation is explicitly authorized
   and available, no other guest run is active, and the candidate ISO matches
   the recorded hash. Run the exact ISO listed above, with no file replacement
   between verification and launch.
2. Preserve the whole serial log and host process exit status. Observe normal
   kernel boot separately from SQL qualification. A kernel boot marker alone
   is not SQL success.
3. Require a `/system` mount log from the boot RAM-disk path, followed by the
   exact five successful `SQL_NATIVE_R3AI` lines above in order. `FIXTURE_OK`
   proves the fixed file was opened and its metadata checked through the
   production VFS path; `POINT_PASS`, `RANGE_PASS`, and `END PASS` prove the
   selected production SQL operations returned the full exact locator arrays.
4. Missing fixture must produce `FAIL code=FIXTURE_OPEN_OR_HEADER` and
   `END FAIL`, without panic or `END PASS`. A corrupt header/root/leaf must
   produce a stable failure code and `END FAIL`; no SQL success is accepted if
   any marker is missing, reordered, duplicated, or inconsistent.
5. Bound the guest run to 180 seconds for the final SQL marker after process
   launch. On timeout, preserve the serial output, record `SQL_RESULT=TIMEOUT`,
   then terminate only the QAC/QEMU process started for this acceptance. Do
   not signal or seize another user's guest process/reservation.
6. Retain the exact ISO, candidate manifest, ISO verification report, serial
   log, launch command/configuration, host exit status, and before/after ISO
   hash. Close the guest process and any owned host handles on success or
   failure. Report guest boot status and SQL diagnostic status as separate
   fields.

Expected corruption classification for the fixed image path:

| Injected condition | Diagnostic code |
|---|---|
| Missing/unopenable fixture or invalid header/CRC/major version | `FIXTURE_OPEN_OR_HEADER` |
| Header UUID, creation time, minor version, or fixed dimensions differ | `FIXTURE_METADATA` |
| Selected point/range internal root has bad page data/type/owner | `POINT_ROOT` / `RANGE_ROOT` |
| Read or decode failure while traversing an index | `POINT_LOOKUP` / `RANGE_SCAN` |
| Traversal succeeds but locator list differs | `POINT_RESULTS` / `RANGE_RESULTS` |

## Architecture checkpoint and next work

1. **Guest readiness:** yes, the fixed native read diagnostic is ready for a
   separately authorized guest qualification; it has not been guest-verified.
2. **Production VFS:** the real production VFS operations are wired to the SQL
   provider and were executed in the hosted production-VFS harness. The
   diagnostic was also compiled/linked into the native kernel candidate.
3. **Fixture stability:** VFS/block writes are unavailable on the boot RAM
   disk, and no concurrent writer participates. Raw kernel RAM is not
   hardware-protected against arbitrary direct writes.
4. **Catalog/root handoff:** trusted manifest roots and owners plus runtime
   header/root decoding are sufficient for this fixed diagnostic. They do not
   replace a general native catalog reader.
5. **Remaining runtime dependencies:** UEFI BootInfo ramdisk handoff, serial,
   block-device and RAM-disk initialization, FAT32 mount, VFS, native SQL file
   bridge/provider, and the portable index implementation.
6. **General database support:** yes, it will need a native catalog reader to
   discover tables/index metadata rather than embedding one fixture's roots.
7. **Open concurrency work:** no live writer coordination, database locking,
   snapshot protocol, or consistent read view against concurrent writes is
   established by this fixture test. Native write/WAL access remains disabled.
8. **Portability:** further broad read-side portability work can wait until
   guest evidence identifies an actual blocker; existing census failures are
   unrelated hosted-header dependencies.
9. **Safe writes/recovery:** require a native write provider, durable and
   ordered write/flush semantics, transaction and WAL integration, crash-safe
   recovery, concurrent-reader/writer coordination, and power-failure tests.
10. **Stored procedures and Database Manager:** these belong after catalog and
    schema services, a native execution/service interface, transactions and
    recovery, and controlled app-facing APIs are defined. They are outside
    this fixed read diagnostic.

The recommended next bounded milestone is **NDB1R4P11R3AJ — native guest
acceptance of this exact ISO**, after explicit QAC/QEMU admission. The next
result should answer whether the qualified kernel reaches the real VFS and
emits both exact SQL result sequences, without adding general SQL services or
native writes.
