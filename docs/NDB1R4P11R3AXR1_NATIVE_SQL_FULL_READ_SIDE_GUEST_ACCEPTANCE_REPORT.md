# NDB1R4P11R3AX-R1 — Native SQL Full Read-Side Guest Acceptance Report

**Outcome:** `NATIVE_SQL_R3AXR1_GUEST_PARTIAL_OR_FAILED` (Outcome B)
**Phase:** `NATIVE_SQL_MANIFEST_AND_FULL_READ_SIDE_GUEST_ACCEPTANCE_RETRY`
**Date:** 2026-11-11 (America/Los_Angeles)
**Guest executions:** 1
**QAC reservation requests:** 1

## Result

The same frozen R3AW candidate passed its prelaunch integrity checks and was launched once under a fresh, clear QAC acceptance census. The guest emitted the BootInfo, full RAM-disk mapping, FAT32 read-only mount, full `FIXTURE.MAN` VFS read, SQL point/range/negative/recovery, and final qualification PASS markers.

The first failed gate was **Gate 5: QMP listener ownership was not verified**. QAC verified the acceptance reservation and managed QEMU child, but the listener check did not identify the requested QMP port (`127.0.0.1:51288`) as owned by QEMU PID `28664`. The runner stopped before opening QMP. No QMP greeting, capabilities response, running-state response, register response, live page-table walk, or QMP `quit` was captured. The QMP parser was not reached, so this is not a CR0/CR4 parser-width rejection and is not evidence of a guest mapping defect.

No second guest was launched. Because QMP was unavailable, cleanup used the QAC-documented owned process-group console interrupt fallback. The owned QEMU child exited, QAC reported the same reservation `Released`, both owned process IDs were absent afterward, and unrelated QAC resources remained unchanged. This fallback does not satisfy the required controlled-QMP-shutdown gate. No production defect was identified by the guest serial results.

## Repository and candidate identity

| Repository | Branch | HEAD at run | Worktree |
|---|---|---|---|
| SQL | `main` | `e836a12a8e05800e041cc1c94729e721ad2fc8ef` | Tracked and untracked trees clean before this report; four commits ahead of `origin/main`. |
| Isolated Server | `codex/ndb1r4p11rah-sql-read-integration` | `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd` | Tracked tree clean; existing untracked build outputs and `scripts/__pycache__` preserved. |
| QAC | `main` | `1ea193258690f9e7b39390b101a117166ef8bbdd` | Clean. |

The candidate manifest pins SQL source `ecf93148d06b4ccf77e59dce35f32c478ae56b86`, an ancestor of the SQL HEAD used for this retry, and Server source `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd`, exactly the current Server HEAD. The R3AW narrative reference `99f6ddead0ba89bbcb0124d19058a6557f689c73` is absent from the local Server repository history. It is an unverified narrative lineage hash, not the manifest's candidate pin, and was not treated as an integrity failure.

The frozen ISO was `E:\CodexArtifacts\NDB1R4P11R3AW\candidate\guideXOS-NDB1R4P11R3AW-amd64.iso`: **100,730,880 bytes**, SHA-256 **`60ffbbbd16ff7f3c66a99ebf1f48389f807811afed62fc14e9d3e6c11d93a207`**. All 20 pinned artifacts matched before launch and after shutdown. No image was rebuilt, repackaged, modified, or patched.

## Twenty-gate results

| Gate | Result | Evidence and disposition |
|---:|---|---|
| 1. Frozen R3AW candidate, manifest, and embedded ESP identity | **PASS** | Exact ISO length/hash, candidate manifest, UEFI ESP, embedded kernel/RAM disk, and all 20 pinned artifacts passed. Post-run rehashes also passed. |
| 2. Fresh QAC census and exclusive admission | **PASS** | Census at `2026-11-12T03:06:52.843Z` (Nov 11, 7:06:52 PM PST): coordinator online; zero QEMU, foreign, managed, queued, or blocked sessions. One exclusive acceptance request was granted. |
| 3. QAC reservation owner, managed child, and executable identity | **PASS** | Reservation `4fdb8b5c6cf54cb8a64413d004c409aa`; owner PID `25408`; managed QEMU PID `28664`; executable identity matched `C:\Program Files\qemu\qemu-system-x86_64.exe`. |
| 4. Approved UEFI firmware and actual guest configuration | **PASS** | The pinned QEMU and OVMF were used; pflash and ISO CD-ROM were read-only; no writable guest storage was attached; the UEFI guest executed. |
| 5. Correlated QMP negotiation and running-state response | **FAIL — first failure** | Listener ownership was false in `qac-admission.json`. No QMP connection was opened and no correlated greeting, capabilities, or running-state response exists. |
| 6. Live CR3 agreement, CR4.LA57, and raw-QMP control-register recovery | **NOT VERIFIED** | No raw QMP register response was captured. Guest MAP-E reported its active CR3 and `CR0.WP=1`, but independent live register correlation was unavailable. Parser recovery was not exercised. |
| 7. BootInfo v3 and page-table sidecar integrity | **PASS** | Guest emitted `status=BOOTINFO_VALID`, `address_contract=BootInfo_v3`, and sidecar `available=1`, count `33`, capacity `433`, range samples `30`. The guest mapping PASS follows its sidecar validation for bounds, root agreement, uniqueness, and checksum. Independent QMP agreement is covered by Gate 9. |
| 8. All 15,104 RAM-disk page bases and final-byte probe | **PASS** | Guest emitted `SQL_R3AU_MAP_RESULT=PASS pages_checked=0000000000003B01` (`0x3B01` = 15,105). The pinned validator checks every page base and then separately probes the extent's final byte. |
| 9. Former 2 MiB boundary and independent live page-table walks | **NOT VERIFIED** | No live QMP walks were captured at the alias base, 2 MiB boundary, final page, or final byte; the sidecar-to-live-frame-set agreement was not checked. |
| 10. Supervisor/read-only mappings, physical progression, CR0.WP, and kernel overlap | **PASS** | Guest `MAP_RESULT=PASS` followed the pinned in-kernel checks for effective permissions, physical progression, kernel overlap, and alias bounds. MAP-E reported `readonly=1` and `cr0_wp=1`. |
| 11. Boot-sector validation, FAT32 classification, and read-only mount | **PASS** | Guest verified the 512-byte boot sector, classified the volume as FAT32, observed `read=1 write=0`, mounted read-only, and registered `/system`. |
| 12. Production VFS open/read/EOF/close of `FIXTURE.MAN` | **PASS** | Actual guest markers show `/system/Apps/R3AI/FIXTURE.MAN`, read-only open, exactly 12,937 bytes, 14 read calls, EOF, successful close, and `content_identity=NOT_CHECKED`. This is guest access evidence, not offline-hash substitution. |
| 13. Production VFS access to `MULTI.GDB` | **PASS** | Guest opened `/system/Apps/R3AI/MULTI.GDB` and emitted `FIXTURE_OK length=250880`. |
| 14. Ordered 73-locator point lookup | **PASS** | Guest emitted count `73`, `ordered=PASS`, and cleanup PASS. The complete trusted-manifest vector matched the candidate source during preflight (`IX_Rows_K64`, root 234, owner 1). |
| 15. Ordered 32-locator range scan | **PASS** | Guest emitted count `32`, `ordered=PASS`, and cleanup PASS. The complete trusted-manifest vector matched the candidate source during preflight (`IX_Rows_K32`, root 148, owner 2). |
| 16. Invalid request preserves caller output | **PASS** | Guest emitted `INVALID_REQUEST`, count `0`, `output=UNCHANGED`, cleanup PASS. |
| 17. Valid recovery after rejection | **PASS** | Guest emitted the expected 73-result ordered recovery and cleanup PASS. |
| 18. Complete serial scan for failures, panic, and memory faults | **PASS** | No mandatory FAIL/BLOCKED, panic, page-fault, protection-fault, triple-fault, or memory-fault markers were found. |
| 19. Ordered final mapping, package, RAM-disk, manifest, and SQL PASS markers | **PASS** | All 9 expected mapping markers and 12 expected SQL markers appeared in order, including manifest PASS before SQL results and `SQL_NATIVE_R3AU QUALIFICATION END PASS`; no contradictory failure marker was present. |
| 20. Controlled QMP shutdown, owned exit, QAC release, and final ISO check | **FAIL — QMP shutdown unavailable** | No QMP `quit` could be sent. The QAC-owned process-group interrupt was used for cleanup; QEMU PID `28664` and owner PID `25408` exited, the same QAC reservation became `Released`, post-run QAC showed no sessions or QEMU, and the ISO hash remained exact. Cleanup subchecks passed, but the required QMP shutdown did not. |

## Cleanup and evidence

The fresh post-run QAC reconciliation reports reservation `4fdb8b5c6cf54cb8a64413d004c409aa` as `Released`, reason `Managed child exited.` The owned QEMU and QAC runner PIDs are absent. QAC reported zero QEMU processes, zero foreign processes, zero managed sessions, zero queued reservations, and zero blocked reservations. The ISO and all pinned artifacts were unchanged after the run.

New evidence is isolated under:

`E:\CodexArtifacts\NDB1R4P11R3AXR1\attempt-20261111-acceptance`

Key files: `candidate-integrity-preflight.json`, `candidate-integrity-postrun.json`, `qac-prelaunch-census.json`, `qac-admission.json`, `launch-attempt.json`, `serial.log`, `serial-analysis.json`, `qac-owned-group-shutdown-signal.json`, `qac-postrun-reconciliation.json`, `owned-process-exit-check.json`, and `outcome.json`. No QMP JSONL exists because the runner stopped before connecting. Provenance reconciliation is at `E:\CodexArtifacts\NDB1R4P11R3AXR1\provenance-reconciliation.json`. The earlier R3AX Outcome C report and evidence were not changed.

No production source, QMP parser, or frozen candidate was modified. No second guest was launched. This report is a separate R3AX-R1 record; it does not rewrite the historical R3AX Outcome C.
