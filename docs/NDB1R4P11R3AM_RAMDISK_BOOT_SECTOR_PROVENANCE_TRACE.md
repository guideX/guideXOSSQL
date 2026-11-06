# NDB1R4P11R3AM RAM Disk Boot-Sector Provenance Trace

**Phase:** `NATIVE_SQL_RAMDISK_BOOT_SECTOR_PROVENANCE_TRACE`  
**Outcome:** A1 — `NATIVE_RAMDISK_BYTE_PROVENANCE_BOUNDARY_IDENTIFIED`  
**Run date:** 2026-11-05 (UTC)

## Repository identity and changes

| Repository | Starting identity | Ending identity |
|---|---|---|
| SQL: `D:\dev\guideXOS_SqlDatabase` | `main`, `cf46c197ea945e271e89763d9497b350ed3fd923` | `main`, this report commit (exact ending SHA recorded in the completion summary) |
| Server integration: `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH` | `codex/ndb1r4p11rah-sql-read-integration`, `109eaf4107592864f9de819b103a98a87eaa69a5` | same branch, `83e5438cd883cdd1158761f99b0aee1a3943452e` |
| Original Server checkout | baseline `f29850c93bbac0a0763a31e806d2816310156575` | unchanged |

There was no separate loader worktree. The UEFI loader is the `guideXOSBootLoader` project inside the isolated Server integration worktree. Its source and the kernel diagnostics were committed locally in Server commit `83e5438c` (`Add R3AM RAM disk provenance tracing`). The SQL production source was not changed. This report is the only intended SQL repository change.

The integration worktree has generated, untracked build and evidence directories. R3AM output was kept under `out\ndb1r4p11r3am` and `guideXOSBootLoader\out\r3am`; earlier untracked R3AI/R3AJ/R3AK/R3AL outputs were preserved. None were staged into the Server commit. No reset, clean, stash, rebase, push, or merge was performed.

## Offline reference and package identity

The reference was read from the actual `ramdisk.img` embedded in the candidate ESP, not from the ESP or ISO boot sector. The payload was the unchanged R3AI source RAM disk used by the frozen R3AL package.

| Property | Value |
|---|---|
| RAM disk | `out\ndb1r4p11r3ai\boot-candidate-source\ramdisk.img` |
| Length | 61,865,984 bytes (`0x03B00000`) |
| SHA-256 | `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| LBA 0 bytes 0–15 | `EB 58 90 47 55 49 44 45 58 4F 53 00 02 10 20 00` |
| BPB bytes-per-sector at `0x0B–0x0C` | `00 02` = 512 bytes, little endian |
| Boot-sector signature | `55 AA` |
| Logical sector size / volume offset | 512 bytes / LBA 0 |
| Fixture | 250,880 bytes, SHA-256 `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Fixture manifest | SHA-256 `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |

The fixture remained at `/Apps/R3AI/MULTI.GDB`, with the manifest at `/Apps/R3AI/FIXTURE.MAN`. Offline reference and package checks passed. The original R3AL ISO remains 100,730,880 bytes with SHA-256 `698698865d2701c2977ccc3f8bf7efab6662d452b9b6fc5deddaa336408ab87a`; its ESP and ISO verification passed before and after this run.

## Production path and diagnostic instrumentation

The data path was traced through the production implementation:

1. `guideXOSBootLoader/main.cpp::LoadFile` opens `ramdisk.img`; the loader obtains its size, allocates `EfiLoaderData` pages, reads the complete file into that allocation, then records `RamdiskBase` and `RamdiskSize` in BootInfo. The trace records the full-read status, byte count, pointer, length, first 16 bytes, and decoded BPB sector size after the read and again immediately before handoff.
2. The loader's page-table setup (`guideXOSBootLoader/paging.cpp::BuildIdentityPageTables`, followed by `MapRange`) determines the mapping visible after the CR3 switch. The handoff trampoline switches to those page tables and transfers control to the kernel.
3. At kernel entry, `kernel/core/main.cpp` reads the RAM disk through the BootInfo base value. The RAM disk is then attached by `kernel/core/ramdisk.cpp::create_readonly_at`; the block device borrows the handoff allocation rather than copying it. The diagnostic records attachment result, device and driver IDs, source/backing addresses, length, sector geometry, and bytes.
4. The registered `kernel::ramdisk::read_sectors` callback handles the first LBA 0 request. Its diagnostic records device/driver identity, requested LBA/count, sector size, backing offset and length, status, source bytes, destination address, and copied bytes.
5. `kernel/core/fs_fat.cpp` reads LBA 0 into its FAT boot-sector buffer and validates the BPB in `try_mount_fat32_boot_sector`. The trace records the selected device, returned buffer and length, I/O status, first bytes where serial capture remains intact, and BPB validation result.

The instrumentation is gated by `GX_NATIVE_RAMDISK_PROVENANCE_TRACE`, disabled by default, and constrained to the first 16 bytes and the first relevant LBA 0 read. Kernel build configuration requires AMD64 plus the existing SQL qualification and diagnostic flags. Diagnostic-only controlled fault hooks are compiled for hosted qualification; they do not alter normal Server startup. No buffer ownership or copy behavior was changed.

The source change is Server commit `83e5438cd883cdd1158761f99b0aee1a3943452e`. Relevant implementation points include `guideXOSBootLoader/main.cpp` (`LoadFile` read and UEFI trace), `guideXOSBootLoader/paging.cpp` (`MapIdentityRange`, `MapRange`, `BuildIdentityPageTables`), `guideXOSBootLoader/elf.cpp::LoadElf`, `kernel/core/main.cpp` (kernel handoff observation), `kernel/core/ramdisk.cpp` (`create_readonly_at` and `read_sectors`), `kernel/core/block_device.cpp::read_sectors`, and `kernel/core/fs_fat.cpp` (FAT read and BPB validation).

## Hosted qualification and builds

Hosted suites completed successfully:

| Suite | Result |
|---|---:|
| R3AM production-component RAM disk provenance and controlled fault suite | 34/34 |
| R3AK mount regression baseline | 60/60 |
| R3AL hosted qualification | 73/73 |
| R3AI production VFS diagnostic | 22/22 |
| SQL Debug CTest baseline | 39/39 in the earlier unchanged SQL baseline; SQL production sources were unchanged |

The R3AK, R3AL, and R3AI run logs are under the integration worktree's `out\ndb1r4p11r3am\hosted` directory. No hosted expectation was used as a substitute for the guest SQL result.

Both ordinary and diagnostic AMD64 kernels built with MinGW-W64 GCC 15.2.0 and GNU binutils 2.46.0.20260210. The freestanding build uses C++14, optimization, warnings, no exceptions or RTTI, no standard library, no builtins, and AMD64 kernel flags including large code model, no red zone, no SIMD, and no PIE/PIC. The diagnostic build additionally enabled native SQL qualification, SQL diagnostics, and `GX_NATIVE_RAMDISK_PROVENANCE_TRACE=1`.

The ordinary and diagnostic UEFI loaders built with MSBuild 18.11.1, MSVC 19.51.36260, linker 14.51.36260, and toolset `v145` for Release x64. The ordinary binaries do not contain `SQL_R3AM_` markers; the gated diagnostic binaries do.

| Artifact | Size | SHA-256 |
|---|---:|---|
| Ordinary kernel | 2,670,376 bytes | `c5788717feabaede109c697105815a5b7af92873328c85c988c0576d6b1b7f14` |
| Diagnostic kernel ELF | 2,749,256 bytes | `47ce74f02dcc3956ec84fab3eb7185cf4d5b06eab90eff73cae6f7f9dc739071` |
| Diagnostic kernel PE | — | `36a8fd5113ff3ef6b29cdd4b6d31940cb3298006f77a9794a834b851f7f7bea8` |
| Ordinary loader | 56,832 bytes | `31dd47708089dbf8305c72aa804085e506cb7a1fb802a5401e61f973ce22f8e6` |
| Diagnostic loader | 59,904 bytes | `7b3282cb79fcc75783fc3c31a7f4cc41af5f334056989205803c46136602037e` |

## Candidate image

The diagnostic candidate is `out\ndb1r4p11r3am\boot-candidate\guideXOS-NDB1R4P11R3AM-amd64.iso`.

| Property | Value |
|---|---|
| ISO size | 100,730,880 bytes |
| ISO SHA-256 | `669e375cae8ac628c99cd27fe0d0fdd2103ab11d7555cc9dfc231880b2c337fa` |
| ESP size / SHA-256 | 100,663,296 bytes / `dc9694f14b29c2e5cfc8bc9514edccddc42dd9c65e19a387115c806ec833ca40` |
| UEFI El Torito boot image | LBA 32; FAT32 ESP |
| Embedded RAM disk | 61,865,984 bytes; reference SHA-256 above |

Independent ESP and ISO verifiers passed. The candidate is a diagnostic build, not a byte-identical copy of R3AL. Its hash remained unchanged after the guest run.

## QAC admission and cleanup

The phase authorized at most one guest boot. At `2026-11-05T23:42:18Z`, QAC v2.0.0 was online, its queue and managed-session census were empty, and acceptance admission was available. The run used exactly one QAC-managed `guideXOSServer` acceptance launch with the exact candidate ISO as a read-only CD-ROM, read-only pflash, no writable storage, isolated serial capture, and loopback QMP. No standalone QEMU launch was used.

The runner did not receive its launch notification or reservation ID within its bounded wait and initially recorded a pending outcome. Before recovery, the authoritative QAC reservation inspection and process relationship identified reservation `fa9ffd5a63e34f54a984e1633a29a45a`, QAC wrapper PID 24016, and its owned QEMU child PID 24968 (`qemu-system-x86_64.exe`, acceptance mode, verified owner). The QMP endpoint was confirmed to belong to that process. No second launch was attempted. QMP reported the VM running with the exact ISO mounted read-only; `quit` returned `{}`.

After shutdown, QEMU PID 24968 and QAC wrapper PID 24016 had exited. QAC remained online with zero QEMU processes, sessions, reservations, queued or blocked requests. The owned reservation was released; no other workstream resources were modified. Evidence is in `out\ndb1r4p11r3am\guest-runner\qac-launch-reconciliation.json`, `qmp-observations.jsonl`, and `qac-postrun.json`.

## Guest provenance observations

The values below refer to the same embedded RAM disk payload. Hex strings in serial output omit spaces.

| Observation | Identity and provenance | Address / length / request | First 16 bytes | BPS | Status |
|---|---|---|---|---:|---|
| Offline package | Embedded `ramdisk.img`, verified by SHA-256 | LBA 0; 61,865,984-byte backing | `EB 58 90 47 55 49 44 45 58 4F 53 00 02 10 20 00` | 512 | Reference; signature `55 AA` |
| UEFI load | `ramdisk.img` read into `EfiLoaderData`; BootInfo at `0x3DD3F000` | Base `0x155C1000`; length and bytes read `0x03B00000`; offset 0 | `EB58904755494445584F530002102000` | `0x0200` | Read/load status 0; complete length; valid handoff fields |
| UEFI pre-handoff | Same BootInfo pointer, base, and length | Base `0x155C1000`; length `0x03B00000` | `EB58904755494445584F530002102000` | `0x0200` | Read/load status 0; valid |
| Kernel first handoff dereference | BootInfo valid; owner reports `UEFI_EfiLoaderData` | BootInfo `0x3DD3F000`; base `0x155C1000`; length `0x03B00000` | `00000000000000000000000000000000` | 0 | First observed mismatch; full record emitted before attachment |
| RAM disk attachment | Device 1 / driver 1; source and borrowed backing both `0x155C1000` | Length `0x03B00000`; sector size 512; sector count `0x1D800`; copied 0 | Serial bytes field was interrupted by IRQ output | — | Registration succeeded; read-only borrowed storage |
| Block callback source | Device 1 / driver 1; LBA 0, one sector | Offset 0; byte count `0x200`; backing length `0x03B00000`; source `0x155C1000` | `00000000000000000000000000000000` | 0 | Status `00` (`BLOCK_OK`) |
| Block callback destination | Same device, driver, and LBA | Destination `0x213AF540`; copy length `0x200` | `00000000000000000000000000000000` | 0 | Status `00` (`BLOCK_OK`); copy completed |
| FAT consumer / BPB | FAT read uses device 1, LBA 0, read buffer `0x213AF540` | Buffer length `0x200` | FAT-read serial bytes field was IRQ-interrupted; adjacent parser reports BPS 0 and signature `0000` | 0 | Read status `00`; rejected as `FAT32_BYTES_PER_SECTOR_INVALID` |

The trace's `address_mode=identity_mapped_physical` label in the kernel marker records the expected access model; it was not validated against the active PTE and must not be read as proof of the effective translation. The attachment and FAT serial lines were partly interleaved with IRQ output, but the kernel handoff, block source, block destination, and BPB rejection records are intact and agree on zero bytes.

## Earliest boundary and cause assessment

The last verified correct observation is `SQL_R3AM_UEFI_HANDOFF`. The first observed incorrect value is `SQL_R3AM_KERNEL_HANDOFF`, when the kernel dereferences the unchanged BootInfo base and length. Thus the proven mismatch interval is after the UEFI pre-handoff read and by the first kernel-main RAM disk dereference, before attachment. This identifies the byte-provenance boundary but does not alone prove the ultimate cause.

The strongest source-supported explanation is a virtual mapping collision:

- Runtime loader output gives kernel virtual minimum `0x00100000`, kernel span `0x22AB3E80` (exclusive end `0x22BB3E80`), and physical allocation base `0x190C1000`.
- The RAM disk base `0x155C1000` lies inside that kernel virtual span; the RAM disk physical range ends exactly at `0x190C1000`.
- `BuildIdentityPageTables` first identity maps requested ranges, including the RAM disk. The later `MapRange(kernelMinVaddr, kernelPhysBase, kernelSpanBytes)` overwrites mappings across the kernel virtual span.
- At virtual address `0x155C1000`, that mapping formula targets physical `0x190C1000 + (0x155C1000 - 0x00100000) = 0x2E671000`, inside the kernel allocation `[0x190C1000, 0x3BB74E80)`.
- `LoadElf` zeros the complete kernel allocation before loading ELF segments. The mapped address is in the non-file-backed gap, consistent with the all-zero bytes observed at the kernel handoff.

The RAM disk registration preserves the supplied base and borrows it; the LBA 0 callback selects device 1, uses offset 0 and a 512-byte copy, and reports success. Source and destination are both zero. These observations do not support a wrong device, LBA, offset, incomplete read, failed copy, or later FAT interpretation as the first divergence.

**Limit:** the active CR3 page-table entry for `0x155C1000` was not dumped. The mapping collision is a strong source-derived explanation corroborated by runtime addresses and bytes, not a directly sampled PTE or a final proof of the effective translation. A narrow follow-up should capture the effective mapping around the CR3 transition before changing page-table behavior.

## Read-only behavior and SQL frontier

The guest mounted the ISO read-only, RAM disk attachment used the production read-only registration path, and the VFS diagnostic reported `read=1 write=0`; no VFS write callback was enabled. The FAT probe rejected LBA 0 because the consumer saw BPS 0. No FAT mount was registered.

SQL emitted:

```text
SQL_NATIVE_R3AI BEGIN fixture=NDB1R4P11R3AH_MULTILEVEL
SQL_NATIVE_R3AI FAIL code=VFS_UNAVAILABLE_OR_WRITABLE
SQL_NATIVE_R3AI END FAIL
```

The fixture was not opened. The point lookup on `IX_Rows_K64` (root 234, owner 1, 73 locators) and range scan on `IX_Rows_K32` (root 148, owner 2, 32 locators) did not run. Native SQL guest acceptance was not reached; hosted results do not change that status.

## Evidence and final status

- Full serial: `out\ndb1r4p11r3am\guest-runner\serial.log`, 49,938 bytes, SHA-256 `9d4e293d4e42e2fdcdbbf75b48fae8a47c315f5786d090a7dd36a1960d11feee`.
- QMP: `out\ndb1r4p11r3am\guest-runner\qmp-observations.jsonl`, 5,479 bytes, SHA-256 `494eec96ad05579ba04e86aec687b2928fb80f2bf25092344a4c61c700929ec9`.
- Consolidated trace and observation strings: `out\ndb1r4p11r3am\guest-runner\provenance-summary.json`.
- Initial pending outcome retained at `out\ndb1r4p11r3am\guest-runner\runner-initial-outcome.json`; final classification and cleanup are in `out\ndb1r4p11r3am\guest-runner\outcome.json` and `qac-postrun.json`.
- The diagnostic ISO and original R3AL ISO hashes matched before and after shutdown. The fixture and manifest hashes remained the pinned values above.
- Exactly one guest boot occurred. QAC cleanup is verified; no R3AM guest resources remain.
- SQL fixture access and both SQL queries did not execute. No filesystem, RAM disk, page-table, or SQL repair was made in this phase.

## Recommended next milestone

Perform a narrowly bounded mapping investigation: record the effective CR3 PTE for virtual address `0x155C1000` immediately before and after the loader's page-table switch, and compare it with the RAM disk's physical allocation and kernel span. If it confirms the source-derived alias, implement only the mapping correction needed to preserve the RAM disk view, keep ordinary tracing disabled, then rebuild and requalify under a new phase with current guest authorization. Do not change FAT32 geometry or SQL code based on this run.
