# NDB1R4P11R3AK — Native SQL Read-Only FAT32 Mount Repair

## Outcome

**Outcome B — `NATIVE_SQL_FAT32_MOUNT_PARTIAL`.** R3AJ-R1 reaches the
production FAT32 mount call and fails before VFS registration or SQL fixture
access. The exact packaged bytes and hosted production path were audited; the
exact image still mounts and the production SQL diagnostic passes in hosted
execution. The guest-only mount cause is not established. This phase adds
bounded FAT geometry checks, gated mount attribution, and focused regression
coverage, but does not claim that those changes repair the R3AJ-R1 guest
failure.

No new boot candidate was prepared. R3AK did not run QEMU or QAC, and it does
not establish a successful guest mount or native guest SQL execution.

## Repository identities and preservation

| Repository | Starting identity | Phase state |
|---|---|---|
| SQL | `main`, `813fedb92bf19883a925faa8b8f858c52568ce2e` | Production SQL files and phase markers unchanged; this report is the only SQL source change. |
| Isolated Server integration | `codex/ndb1r4p11rah-sql-read-integration`, starting at `3e102c3bce7e1a3ecef8634a6fa640b05e72ec10` | R3AK mount attribution and regression work is committed as `70ab616acb5901b95e5d6e46b0676d28c8747eb8`. |
| Original Server checkout | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575` | Tracked working tree verified clean; no build output was written there. |

The existing isolated Server `out/ndb1r4p11r3ai/` and R3AJ evidence directories
were preserved. New R3AK tests and build outputs use separate paths under
`out/ndb1r4p11r3ak/` and `kernel/build/amd64-r3ak-*`. No reset, rebase, stash,
clean, push, merge, or overwrite was used.

## R3AJ-R1 failure frontier

The complete 40,202-byte `serial.log` was reviewed chronologically. Its SHA-256
is `71098cfbe8c1d21751ff1ac4c380410ce6cf40c2aba7c24b8c6ea501debaf8a5`.

1. UEFI loads the bootloader and kernel; `[KERNEL] guideXOS kernel_main entered`
   confirms kernel entry.
2. The kernel initializes block and filesystem drivers, creates `ram0`, then
   initializes VFS.
3. BootInfo reports the 61,865,984-byte `ramdisk.img`. The kernel finds it,
   calls `ramdisk::create_readonly_at(..., "wallimg")`, logs an attachment of
   `0x1D800` (120,832) 512-byte sectors, and finds the registered `wallimg`
   block device by name.
4. `desktop::set_wallpaper_image_pack` calls
   `vfs::mount_type("/", blockIndex, FS_TYPE_FAT32)`. The first failure marker
   is `[VFS] ERROR: Filesystem mount failed`; the caller then reports
   `[desktop] failed to mount boot runtime image at /`.
5. `native_sql_read_diagnostic::run_after_vfs_mount` then fails its `/system`
   mount/read-only preflight and emits
   `SQL_NATIVE_R3AI FAIL code=VFS_UNAVAILABLE_OR_WRITABLE`.

The last confirmed storage operation is read-only RAM-disk registration and
device lookup. The first failed production operation is the FAT driver mount
probe called by `vfs::mount_type`. `mount_type` returned `0xFF`; no VFS mount
was registered. The old serial log has no FAT probe details, so it cannot show
whether the first failure was a block read, boot-sector/volume validation,
driver volume allocation, or a later driver recognition step. That leaves the
frontier inside `fs_fat::mount`, before VFS registration. The evidence does not
distinguish the user's B and C subcases and does not support calling the SQL
preflight code the root cause.

No `/system` mount, fixture open, header/page validation, root/owner check,
point lookup, range scan, or SQL provider cleanup was reached. The SQL
diagnostic's combined error is downstream symptom reporting, not root-cause
attribution.

### R1 guest configuration and cleanup

The R1 launch configuration confirms QAC acceptance mode with direct QEMU
child ownership, `pc,usb=off`, 1024 MiB RAM, read-only OVMF firmware, the exact
R3AI ISO attached as a read-only CD-ROM, serial capture, QMP at
`127.0.0.1:62829`, and `-no-reboot`. The ISO hash matched before launch and
after shutdown. The UEFI boot entry, kernel entry, RAM-disk attachment, and
kernel main loop were observed; the FAT32 mount marker and all SQL success
markers were absent.

QMP `quit` shut down the owned child. The recorded QAC reservation was
released, and post-run reconciliation recorded no QEMU process, reservation,
managed session, or queued run. The complete launch, serial, QMP, QAC, and
post-run evidence remains in
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3aj-r1-guest-acceptance-20261101`.
R3AK did not start another guest or infer authorization from that earlier
empty census.

## Exact R3AI artifact audit

The original R3AI ISO remains unchanged at
`out/ndb1r4p11r3ai/boot-candidate-final-v4/guideXOS-NDB1R4P11R3AI-amd64.iso`:

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| UEFI ISO | 100,730,880 | `f22e3f3627b2ce6541bf9499934a4d726135574b9b654aa12a20351ed860d497` |
| Embedded UEFI ESP image | 100,663,296 | `3670d859e3b386039bcc72be06a2fa2e64c59cf77ace3e326f3add8041289e0f` |
| Embedded RAM disk | 61,865,984 | `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| `MULTI.GDB` fixture | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| `FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| R3AI candidate manifest | 12,703 | `f8dea45ea84bf6b117fb9d0a761499b1e3e2ff0e27de4cbadef896d18a7ff835` |

The ISO's UEFI boot image extracts byte-identically to the pinned ESP image.
The hosted, final-fixture-package, boot-candidate, and ISO-embedded RAM-disk
payloads all match the pinned RAM-disk hash. No fixture or manifest was
regenerated.

The RAM disk is an unpartitioned FAT volume beginning at LBA 0; it has no MBR
partition table. Its BPB and geometry are:

| Field | Value |
|---|---:|
| Bytes per sector | 512 |
| Sectors per cluster | 16 (8 KiB clusters) |
| Reserved sectors | 32 |
| FAT count | 2 |
| FAT size | 256 sectors each |
| Total sectors | 120,832 |
| First data sector | 544 |
| Data sectors / clusters | 120,288 / 7,518 |
| Root cluster | 2 |
| FSInfo / backup boot sector | 1 / 6 |
| Boot-sector signature | `55 AA` |
| Type label | `FAT32   ` |

Independent `pyfatfs 1.1.0` read-only inspection found `/wall`, `/certs`,
`/config`, and `/Apps/R3AI`; it opened `MULTI.GDB` and `FIXTURE.MAN` with the
pinned lengths and hashes. It warned that the cluster count is too small to
reliably classify the volume and guessed FAT32. The BPB labels it FAT32, but
7,518 clusters is below the usual FAT32 cluster-count threshold. This is a
real package-compatibility concern to correct before another candidate. It is
not established as the R1 failure cause: the production driver at the R1
revision accepts this layout, and the exact same bytes mount and read through
the hosted production driver.

## Production mount and read-only contract

`ramdisk::create_readonly_at` registers a 512-byte-sector device with a read
callback and a null write callback. The filesystem type is passed explicitly
as `FS_TYPE_FAT32`; R1 did not use filesystem type autodetection or
`mount_partition`. The production FAT driver probes LBA 0 and then checks an
MBR's primary FAT partition entries if the first sector is not a supported
volume boot sector. The R3AI image is a superfloppy volume at offset 0.

After the filesystem driver succeeds, `vfs::mount_type` registers the mount
and sets `MountPoint.readOnly` from the block device's write callback. Read
opens use `OPEN_READ`; VFS mutation operations reject read-only mounts. The
block layer rejects writes when `writeFn` is null. No FAT write is required for
the tested directory traversal and file reads. The bootloader-owned RAM-disk
buffer remains alive for the guest lifetime. Raw kernel pointers can still
address RAM directly; the proven protection is through the block and VFS
interfaces, not hardware write protection.

The hosted R3AI test used the exact packaged RAM-disk bytes, the production
read-only RAM-disk API, production block layer, production FAT mount, VFS file
operations, and SQL diagnostic. It did not model UEFI BootInfo memory mapping,
kernel physical/virtual mappings, or the guest's static startup state. That is
the key remaining difference between the passing hosted path and R1.

## R3AK changes and reproduction

The isolated Server branch now adds:

- FAT mount preconditions for supported sector and cluster geometry, boot
  signature, block-sector-size agreement, volume bounds, root-cluster bounds,
  and FAT table capacity. The same geometry/capacity checks cover FAT16 mount
  compatibility. These checks reject malformed media without granting write
  capability.
- Compile-time-gated `GX_NATIVE_SQL_READ_DIAGNOSTIC` attribution in FAT/VFS
  mount paths. It records device identity/generation, sectors, sector size,
  read/write callback presence, BPB geometry/signature, block read failures,
  partition boot-read failures, rejection codes, and successful VFS
  read-only mount registration. Ordinary builds omit this diagnostic API and
  its serial output.
- `tests/native_sql_r3ak_mount_regression_test.cpp` and
  `scripts/run-native-sql-r3ak-mount-regression-test.ps1`, exercising
  production FAT/VFS behavior against the pinned image plus controlled block
  media.

The new instrumentation cannot retrospectively diagnose R1 because the R1
candidate was built before it existed. No change to query algorithms,
database/WAL formats, fixtures, manifests, or SQL phase markers was made.

### Focused hosted results

The new R3AK test passed **60/60 checks**. It covers the exact unpartitioned
image, a valid partitioned FAT32 image at nonzero LBA, production read-only
RAM-disk callbacks, case-insensitive fixture paths, byte-identical file
reads, read-only capability propagation, an unsupported block write, SQL
point/range execution, a writable-mount rejection, missing device, missing
fixture, block-sector mismatch and oversize, corrupt boot signature,
unsupported cluster geometry, over-32-KiB clusters, truncated media, invalid
partition offset, mount ordering, failed-mount publication, and handle cleanup.
The SQL diagnostic ran on the exact packaged image and left its bytes
unchanged. The read-only block device has no write callback; attempted block
write returned `BLOCK_ERR_UNSUPPORTED`. No writes were issued during SQL
qualification or the writable-capability mismatch preflight. The captured
output is
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ak\hosted-mount-regression.log`
(3,442 bytes, SHA-256
`6e9fa049ed2e30f4ce6a015f19fb9e40d054bfcabbd12e2a8e5f86d1e6c11fec`).

The existing R3AI hosted production-VFS diagnostic was rerun after the final
source changes and passed **22/22 checks**. It also confirmed VFS mutation
rejection and repeatable missing-fixture failure. Its captured output is
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ak\r3ai-vfs-compatibility.log`.

The complete point and range locator arrays in the unchanged production
diagnostic were independently compared with the trusted R3AI candidate
manifest: **73/73 point locators and 32/32 range locators matched in order**.
Hosted SQL results remain the production point lookup for `IX_Rows_K64`
(root 234, owner 1, 73 locators) and range scan for `IX_Rows_K32` (root 148,
owner 2, 32 locators). R1 did not execute either query.

### SQL regression results

The fresh Debug build is `build-r3ak-debug`, configured against the protected
original Server checkout for the established native-link tests. It built all
143/143 build steps. Full Debug CTest was run with:

```powershell
ctest.exe --test-dir build-r3ak-debug --output-on-failure
```

**Final result: all 39 of 39 targets passed with zero failures in 1,735.64
seconds.** The complete CTest record is in
`build-r3ak-debug/Testing/Temporary/LastTest.log` (48,369 bytes). This is the
R3AK run; the R3AI baseline remains independently documented as 39/39. No
combined assertion total is inferred from CTest.

The aggregate includes the established R3AH fixture, R3AG native linkage,
R3AF provider, R3AE read-view, R3AD provider-contract, portable lookup/range,
transaction, WAL/recovery, storage, and remaining Debug targets.

## Server build and freestanding qualification

Using the Server's normal AMD64 Makefile rules and freestanding flags
(`-ffreestanding -fno-exceptions -fno-rtti -nostdlib -nostdinc++`):

| Configuration | Result |
|---|---|
| Ordinary SQL qualification/diagnostic disabled, `build/amd64-r3ak-disabled` | PE link and ELF conversion passed; no SQL diagnostic or index lookup/range symbols. |
| SQL reader and diagnostic enabled, `build/amd64-r3ak-diagnostic` | Production SQL objects compiled; PE link and ELF conversion passed; diagnostic and production lookup/range symbols present. |
| Diagnostic enabled without qualification gate | Make stopped at parse time with the expected `GX_NATIVE_SQL_READ_DIAGNOSTIC requires GX_NATIVE_SQL_READ_QUALIFICATION=1` error. |

Both PE outputs have zero import DLL declarations. Each ELF has the same 21
unique undefined symbol identities as its corresponding existing R3AI build;
the ordinary and gated R3AK builds have zero undefined-symbol set delta from
those baselines. The ordinary image has no native SQL diagnostic, lookup, or
range-scan symbols, so it has no SQL startup activity.

The established freestanding census ran against the isolated Server kernel
tree and remained **26/44 passing units**, with all 26 prior passing units
preserved. The same 18 hosted-header failures remain; this phase did not
expand into STL removal.

## Candidate and guest readiness

No R3AK ISO or release candidate was generated because the R1 root cause is
still unknown and the current package's FAT32 cluster geometry needs a
compatibility correction. The R3AI ISO and all R3AJ-R1 evidence remain
immutable baselines with the hashes above. No candidate identity or hash is
claimed for R3AK.

No R3AJ-R2 launch contract can yet name an exact repaired candidate. A future
acceptance should bind an exact new ISO hash before QAC admission and report,
in order, kernel boot, RAM-disk attachment, FAT probe result, FAT32 mount,
VFS read-only registration, fixture access, metadata validation, point lookup,
range scan, and cleanup. Missing or reordered markers must remain failures.
That acceptance requires separate explicit QAC authorization.

## Recommended next bounded phase

Correct the isolated R3AK packaging path to produce a standards-compatible
FAT32 volume geometry while preserving the existing fixture and manifest
bytes. Build and host-qualify a separately identified diagnostic candidate
with the new gated mount attribution. Then, in a separately authorized
R3AJ-R2 guest phase, use the exact candidate to distinguish a block read,
boot-sector/geometry rejection, filesystem-driver mount failure, or VFS
registration failure. Only after that evidence should the remaining runtime
repair be selected and a repaired candidate described as ready.
