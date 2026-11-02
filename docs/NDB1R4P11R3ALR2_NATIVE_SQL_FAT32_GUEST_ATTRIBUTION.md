# NDB1R4P11R3AL-R2 — Native SQL FAT32 Guest Attribution

## Outcome

**Outcome B — `NATIVE_SQL_FAT32_ATTRIBUTION_PARTIAL`.** One authorized, QAC-managed boot of the exact frozen R3AL candidate reached the production FAT/VFS mount path and reproduced a first failure: the FAT32 BPB bytes-per-sector check received a zero value from the guest read buffer and rejected the volume as `FAT32_BYTES_PER_SECTOR_INVALID`. The packaged sector is valid and contains a 512-byte sector-size field. The guest trace does not establish why the block read returned a buffer whose BPB fields were zero, so the underlying cause is not yet sufficiently attributed for a bounded repair.

The VFS did not register `/` or `/system`. The gated SQL diagnostic stopped at `VFS_UNAVAILABLE_OR_WRITABLE`; it did not open the fixture or execute either index operation. The 7,518-cluster geometry was not reached as a cause and remains only the previously documented compatibility concern.

## Repository identities and preservation

| Repository | Identity during this phase | Closeout |
|---|---|---|
| SQL | `main`, `d1e3bbf694c9499c6df01515988fcf674a582917` | This report is documentation-only; no production SQL source changed. |
| Isolated Server integration | `codex/ndb1r4p11rah-sql-read-integration`, `109eaf4107592864f9de819b103a98a87eaa69a5` | Tracked source unchanged; prior and R3AL-R2 untracked evidence preserved. |
| Original Server checkout | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575` | Unchanged. |
| QAC | `main`, `1ea193258690f9e7b39390b101a117166ef8bbdd` | Read-only; no QAC source or coordinator state was changed. |

The frozen candidate was built from Server commit `109eaf4107592864f9de819b103a98a87eaa69a5` and SQL commit `87d17f7639e85ff1de0f7b089b96752a84884534`. The current SQL HEAD descends from that candidate commit; the only intervening committed files are the R3AL and R3AL-R1 documentation reports. This was recorded and enforced by the R2 runner. No reset, rebase, clean, stash, push, or merge was performed.

## Candidate integrity and provenance

The exact ISO is:

`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3al\boot-candidate\guideXOS-NDB1R4P11R3AL-amd64.iso`

It is **100,730,880 bytes**, SHA-256 **`698698865d2701c2977ccc3f8bf7efab6662d452b9b6fc5deddaa336408ab87a`**. The candidate manifest is 5,564 bytes, SHA-256 `9975a5a8213656794ac8c1744a4791ebd77c4d145c158ab6d09f069f1dfbedca`. All nine accepted identity entries were rehashed before and after the boot and matched their recorded size and SHA-256:

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| ISO | 100,730,880 | `698698865d2701c2977ccc3f8bf7efab6662d452b9b6fc5deddaa336408ab87a` |
| Embedded ESP image | 100,663,296 | `d8e4b2844d52774da5d7f8905bafd512644ce8a915216e5b5c52c36bb9380bc8` |
| Kernel PE | 2,848,523 | `84dda104c889bdd8c54e82b4c85680f5eec5ce0d2cb62c3e8a40d74186b90440` |
| Kernel ELF | 2,735,872 | `6c82f6be10e8540ee602431349b6fe8bcdbc7522bc59fd59249fa25b093093e9` |
| RAM disk | 61,865,984 | `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| SQL fixture `MULTI.GDB` | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted `FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| Candidate manifest | 5,564 | `9975a5a8213656794ac8c1744a4791ebd77c4d145c158ab6d09f069f1dfbedca` |
| Bootloader | 56,832 | `002ce8def31974997f10179e6139b08e6985816eef31b641411bded60ce59b1e` |

The ESP and manifest cross-pins match the embedded kernel, bootloader, RAM disk, fixture, and trusted manifest. The guest diagnostic's ordered expected point and range locator arrays were compared with the candidate manifest before admission; they matched at 73 and 32 locators respectively. No candidate artifact was rebuilt, modified, or repackaged.

## QAC admission and launch

The single fresh preflight recorded QAC 2.0.0 online, with zero QEMU processes, reservations, sessions, queued requests, and blocked requests. It also recorded the applicable authorization and reviewed launch configuration. QAC granted exclusive acceptance reservation **`fd67fc0b9ae14147887bf464c5aa1eb4`** for project `guideXOSServer`, mode `acceptance`. The verified owner was PID `20396`; its direct QEMU child was PID `21140`.

The runner did not surface QAC's child-launch text within its bounded 30-second admission observation, so it recorded an unresolved notification with no reservation ID. No second launch was attempted. A read-only QAC census and process-parent/command-line inspection then reconciled the exact reservation and direct child to the saved launch marker. The child command used the exact ISO and read-only OVMF pflash and ISO media, `pc,usb=off`, 1024 MiB RAM, no display, a serial file in the evidence directory, loopback-only QMP on port `56622`, UTC RTC, and `-no-reboot`. No writable storage was attached. The QEMU executable SHA-256 was `a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02`; OVMF SHA-256 was `2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792`.

The single guest boot reached kernel entry with UEFI BootInfo, registered the read-only `wallimg` block device, and executed the production FAT/VFS mount. No restart or configuration change occurred.

## Production mount trace and first failure

The serial trace shows this ordered path (the `volume_boot_read_ok` text is interrupted by an IRQ serial message, but is recoverable across adjacent lines):

1. `[KERNEL] guideXOS kernel_main entered` and `[KERNEL] Boot method: UEFI BootInfo`.
2. `[RAMDISK] Attached read-only image 'wallimg' with 0001D800 sectors`.
3. `[VFS-RO-DIAG] mount_begin device=01 fs_type=FAT32 generation=0000000000000001`.
4. `[FAT-RO-DIAG] mount_begin device=01 name=wallimg generation=0000000000000001 sectors=000000000001D800 sector_size=00000200 read=1 write=0`.
5. `[FAT-RO-DIAG] volume_boot_read_ok lba=0000000000000000 count=1` (split in the serial stream by `[IRQ] dispatch irq=00`).
6. `[FAT-RO-DIAG] boot_sector lba=0000000000000000 bps=0000 spc=00 reserved=0000 fats=00 total16=0000 total32=00000000 fat16=0000 fat32=00000000 root=00000000 hidden=00000000 fsinfo=0000 backup=0000 flags=0000 version=0000 type_hex=0000000000000000 signature=0000`.
7. `[FAT-RO-DIAG] probe_reject=FAT32_BYTES_PER_SECTOR_INVALID`.
8. `[FAT-RO-DIAG] mount_failure_final code=FAT32_BYTES_PER_SECTOR_INVALID` and `[VFS-RO-DIAG] filesystem_probe_rejected code=FAT32_BYTES_PER_SECTOR_INVALID`.
9. `[VFS] ERROR: Filesystem mount failed`; there is no `mount_registered` marker.

The first rejecting production operation is the BPB bytes-per-sector guard in `fs_fat::try_mount_fat32_boot_sector()` at `kernel/core/fs_fat.cpp:481`. The mount path reads LBA 0 at `fs_fat.cpp:716`; the diagnostic's `volume_boot_read_ok` marker is emitted after `block::read_sectors()` returns `BLOCK_OK`. The failed FAT result propagates through `vfs::mount_type()` and is reported at `kernel/core/vfs.cpp:526`. This is not a block-read status failure, sector-size mismatch, partition-offset issue, FAT capacity rejection, allocation failure, read-only-capability rejection, or VFS publication failure.

### Observed evidence versus deductions

**Directly observed:** The mounted device reported 120,832 sectors, 512-byte sectors, a read callback, and no write callback. The read-completion marker indicates the LBA 0 block read returned success. The subsequent production parser reported zero for every logged BPB field and for the boot signature, then rejected the zero bytes-per-sector value. VFS did not publish the mount.

**Candidate-media comparison:** The identity-pinned RAM disk's first 32 bytes are `eb58904755494445584f5300021020000200000000f800000000000000000000`; its BPB bytes-per-sector value is 512, sectors-per-cluster is 16, and signature is `55aa`. Therefore the guest-observed BPB bytes do not match the verified source media bytes.

**Source-supported path:** `main.cpp:1020-1022` passes BootInfo `RamdiskBase` and `RamdiskSize` to the wallpaper-pack attachment. `desktop.cpp:5347-5355` passes that pointer to `ramdisk::create_readonly_at()`. `ramdisk.cpp:363-414` stores the supplied pointer and registers the read-only block device; its callback at `ramdisk.cpp:121-147` copies from `disk.data + offset` into the read buffer and returns success. `fs_fat.cpp:716-728` then probes the copied buffer. These source facts locate the path but do not prove which pointer or byte contents were present at runtime.

**Not established:** The trace does not record the guest `RamdiskBase`, bytes at the UEFI load destination, bytes at kernel attachment, or source/destination bytes in the RAM-disk callback. It cannot distinguish an incorrect or inaccessible backing pointer, memory contents changing after the UEFI read, or a callback/copy-path issue. The 7,518-cluster geometry is not implicated by this earlier validation failure.

The smallest additional observation for a bounded repair is one gated record of `RamdiskBase` and bytes 0-15 after UEFI load and at kernel attachment, plus the same 16 source and destination bytes during the first RAM-disk LBA 0 callback with its returned status. This will localize where the valid sector bytes cease to be visible. Do not change FAT behavior, package geometry, or storage access until that observation identifies the cause.

## SQL execution frontier

The only SQL markers were:

- `SQL_NATIVE_R3AI BEGIN fixture=NDB1R4P11R3AH_MULTILEVEL`
- `SQL_NATIVE_R3AI FAIL code=VFS_UNAVAILABLE_OR_WRITABLE`
- `SQL_NATIVE_R3AI END FAIL`

No `/system` mount registered. The fixture was not opened; database header, page count, catalog root, index roots, and owners were not checked. `IX_Rows_K64` root 234 / owner 1 did not execute, and no 73-locator sequence was observed. `IX_Rows_K32` root 148 / owner 2 did not execute, and no 32-locator sequence was observed. There is no guest SQL acceptance claim. The trusted expected arrays and prior hosted qualifications are not guest results.

## Evidence paths and cleanup

All new run evidence is under:

`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3al-r2-guest-attribution-20261102`

Key records:

- `candidate-preflight.json`, `accepted-candidate-identity.json`, `r3al-boot-candidate-manifest.json`
- `qac-preflight.json`, `launch-attempt.json`, `qac-admission.json`, `qac-live-observation.json`
- `serial.log` — 39,609 bytes, SHA-256 `52e8a265acd0573aa96f6da95224c391cfc214bde6aff281c1a7d78f89039b5b`
- `serial-analysis.json`, `fat32-attribution.json`
- `qmp-observations.jsonl` — 735 bytes, SHA-256 `9dcddf20c36d3197ad9658c13dac05d70780f69c195fba314ffae92b78feaeaa`
- `shutdown-and-release.json`, `qac-postrun.json`, `post-cleanup-integrity.json`, `outcome.json`

The owned QMP endpoint reported the guest running; `quit` returned successfully and QMP emitted `SHUTDOWN` with reason `host-qmp-quit`. QEMU PID 21140 and QAC run PID 20396 both exited. QAC then reported zero QEMU processes, sessions, reservations, queued requests, and blocked requests. QAC released only this run's reservation after its child exited. No unrelated reservation or process was altered. A post-cleanup verification again matched all nine candidate artifacts and confirmed both requested repository HEADs and branches; the original Server checkout remains at baseline.

## Recommended next bounded milestone

Use a separate, narrowly gated handoff-observation phase to record the first 16 RAM-disk bytes at UEFI load, kernel attachment, and callback source/destination, plus the returned block status. Preserve the frozen R3AL candidate and read-only path. Then select a bounded repair only if those observations identify where the verified sector bytes are lost. Keep SQL guest acceptance pending until the mount succeeds and both complete ordered locator sequences and required marker order are observed in a guest.
