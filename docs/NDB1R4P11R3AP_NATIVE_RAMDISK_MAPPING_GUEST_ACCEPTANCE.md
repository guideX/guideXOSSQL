# NDB1R4P11R3AP — Native RAM Disk Mapping Guest Acceptance

## Purpose and admission

This procedure is for a separate R3AP guest phase. It has not been executed.
R3AO hosted results and a bootable ISO do not constitute guest acceptance.

Run only after an authorized **exclusive QAC reservation** is granted for
R3AP. Before starting, confirm the reservation owns the guest and no unrelated
QEMU/QAC process or reservation is active. Use QAC for the guest lifecycle;
do not launch a separate raw QEMU process. Keep storage read-only and attach
no writable disk.

## Frozen candidate

- ISO:
  `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ao\candidate-r3\guideXOS-NDB1R4P11R3AO-amd64.iso`
- ISO SHA-256: `305b56a6001597afa049340501c4a4b30fa47ec6fe63ead8a8accc9185c750f0`
- Candidate manifest:
  `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ao\candidate-r3\r3ao-boot-candidate-manifest.json`
- Manifest SHA-256: `e89385508bb82863c4cb3e1fc66d9947156947fa59b073b427f9121115cd730d`
- Server commit: `04bc1900398debffa44611e89af4024edd68cf54`
- SQL source commit: `6d2d52ab812ae8c89a14da8dc6908499bdec9a77`

Verify both hashes and the exclusive reservation identity in the preflight
record. Abort before boot if either candidate identity differs. Preserve the
old R3AN and R3AL ISOs.

## Guest checks

Capture QAC reservation identity, candidate preflight, complete serial output,
the QMP physical read record, and post-run process/release state under a new
evidence directory, for example
`E:\CodexArtifacts\NDB1R4P11R3APR1`. Do not reuse R3AN evidence files.

1. **UEFI boot:** Start the frozen ISO through the exclusive QAC reservation.
   Confirm UEFI loads `EFI/BOOT/BOOTX64.EFI` and reaches the loader's
   ExitBootServices/handoff path.
2. **Kernel entry:** Confirm the kernel entry marker appears and the
   BootInfo-v3 validation accepts the handoff.
3. **Active-CR3 mapping:** Require the loader mapping records for MAP-A through
   MAP-D and the kernel's MAP-E record. At MAP-E, verify the active CR3,
   `RamdiskBase` virtual alias, `RamdiskPhysicalBase`, complete RAM-disk
   length, BootInfo virtual-to-physical relationship, and read-only
   supervisor PTE permissions. Require
   `SQL_R3AO_MAP_RESULT=PASS`.
4. **Physical backing bytes:** Take the physical base from the MAP-E record;
   do not assume the old R3AN allocation address. Use the QAC QMP physical
   read to confirm the first 16 backing bytes are
   `EB 58 90 47 55 49 44 45 58 4F 53 00 02 10 20 00`. Confirm the virtual
   alias diagnostic reports the same bytes and that its PTE resolves to that
   physical backing.
5. **LBA 0:** Confirm the production RAM-disk callback returns sector 0 with
   those same expected bytes and a successful block status.
6. **FAT32 mount:** Confirm FAT32 accepts the production LBA 0 bytes and reports
   512 bytes per sector, then mounts the volume successfully.
7. **Read-only VFS registration:** Confirm the mounted device is registered
   read-only and has no block write callback.
8. **Fixture open:** Confirm the diagnostic opens
   `/system/Apps/R3AI/MULTI.gxdb` through the production read-only VFS path and
   closes the file/provider resources after use.
9. **Header and root validation:** Require the fixture metadata marker for
   250,880 bytes, 512-byte pages, 490 pages, and catalog root 1. Confirm the
   selected index roots and owners: `IX_Rows_K64` root 234/owner 1 and
   `IX_Rows_K32` root 148/owner 2.
10. **Point lookup:** Require
    `SQL_NATIVE_R3AI POINT_PASS index=IX_Rows_K64 root=234 owner=1 count=73`.
    The production diagnostic must complete the ordered 73-locator comparison
    against the R3AO candidate manifest's `pointIndex.expectedLocators` array;
    a count-only result is insufficient.
11. **Range scan:** Require
    `SQL_NATIVE_R3AI RANGE_PASS index=IX_Rows_K32 root=148 owner=2 lower=1 upper=2 count=32`.
    The production diagnostic must complete the ordered 32-locator comparison
    against `rangeIndex.expectedLocators`; a count-only result is
    insufficient.
12. **Final diagnostic PASS:** Require the final
    `SQL_NATIVE_R3AI END PASS` marker after all prior mapping, storage, fixture,
    metadata, ordered-query, and resource-cleanup checks. Any `FAIL`, missing
    stage, mismatched locator, or hang is a failed guest run.
13. **Shutdown and release:** Shut down the guest through QAC, confirm its
    process has exited, release the exclusive reservation, and retain the
    QAC post-run state showing no R3AP guest remains active.

## Acceptance decision

Accept R3AP only when all 13 checks pass in one authorized guest run and the
serial, QMP, candidate identity, reservation, shutdown, and release evidence
agree. If the guest fails before SQL begins, preserve the exact failure
frontier and candidate; do not relabel a hosted PASS as native FAT32/SQL
success. Record a new report and commit it locally after evidence review.
