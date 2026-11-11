# NDB1R4P11R3AX — Native SQL Full Read-Side Guest Acceptance

**Outcome:** `NATIVE_SQL_R3AX_GUEST_PENDING` (Outcome C)
**Phase:** `NATIVE_SQL_MANIFEST_AND_FULL_READ_SIDE_GUEST_ACCEPTANCE`
**Date:** 2026-11-11 (America/Los_Angeles)
**Guest executions:** 0
**QAC reservation requests:** 0

## Result

The R3AW candidate and all 20 candidate-manifest-pinned artifacts passed offline integrity checks. The candidate manifest pins SQL source commit `ecf93148d06b4ccf77e59dce35f32c478ae56b86` (an ancestor of the requested SQL HEAD) and Server source commit `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd` (the exact requested Server HEAD). The ISO was rehashed after the admission blocker and still matches its required size and SHA-256.

The fresh authoritative QAC census immediately before admission found an active QAC-managed exclusive acceptance reservation for project `APP_MODEL`. QAC reported that exclusive acceptance was unavailable. Following the R3AX procedure, no reservation request or QEMU launch was made. The other workstream was not queried beyond QAC’s census, stopped, displaced, or otherwise changed.

This is Outcome C. Gate 1 is the highest successful acceptance gate; gate 2 is blocked by QAC admission. No guest evidence exists for BootInfo, memory mapping, QMP walks, FAT32/VFS, the guest manifest read, SQL execution, or shutdown. In particular, offline verification of `FIXTURE.MAN` does not establish guest VFS access.

## Repository and source identities

| Repository | Branch | HEAD at admission preflight | Worktree state |
|---|---|---|---|
| SQL | `main` | `b3387672f0a972cfc18ae01c9d23a5c63e1ccfed` | Clean before this report; 3 commits ahead of `origin/main`. This report is the only follow-on change. |
| Isolated Server | `codex/ndb1r4p11rah-sql-read-integration` | `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd` | Tracked tree clean. Existing untracked build outputs and `scripts/__pycache__` were preserved. |
| QAC | `main` | `1ea193258690f9e7b39390b101a117166ef8bbdd` | Clean; read-only inspection and census only. |

The candidate manifest’s Server branch and commit match the requested isolated Server branch and HEAD. Its SQL source commit is an ancestor of the current SQL HEAD. The R3AW narrative also names `99f6ddead0ba89bbcb0124d19058a6557f689c73` as an earlier Server source commit; that object is absent from the local Server repository. I did not substitute another hash. This ancillary narrative lineage statement remains unverified; the candidate manifest’s final Server source pin is verified exactly.

The Server checkout already contained untracked generated output directories before this attempt. No reset, rebase, stash, clean, branch switch, merge, push, or production-source edit was performed.

## Frozen candidate and artifact integrity

Candidate ISO: `E:\CodexArtifacts\NDB1R4P11R3AW\candidate\guideXOS-NDB1R4P11R3AW-amd64.iso`

| Artifact | Bytes | Required SHA-256 | Result |
|---|---:|---|---|
| R3AW UEFI ISO | 100,730,880 | `60ffbbbd16ff7f3c66a99ebf1f48389f807811afed62fc14e9d3e6c11d93a207` | PASS, including final recheck |
| ESP `EFI-BOOT.IMG` | 100,663,296 | `6e89f79d4e84ab328e8b756c4a90d14047460f130ad1e644547b4bac1e8454db` | PASS |
| UEFI loader `BOOTX64.EFI` | 83,968 | `f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77` | PASS |
| Ordinary `kernel.pe` | 2,774,807 | `680a74cf3ff717d6b2f0c0d8ae1eef48da528b44eef1d3943801f3883c2d0ea4` | PASS |
| Ordinary `kernel.elf` | 2,670,368 | `43db7160b3620107a9d5af8de68c2f154f20e20dcec685e3919242f809214561` | PASS |
| Qualification `kernel.pe` | 2,886,019 | `a39a71495b82c4f404c7158479df153c929565425c53aace2f0480175ba48312` | PASS |
| Qualification `kernel.elf` | 2,773,768 | `d6d8632684af6dded22c2a3f795461e5599a461ba38009d29062cd052c47e152` | PASS |
| R3AR RAM disk `ramdisk.img` | 61,865,984 | `50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c` | PASS |
| SQL fixture `MULTI.GDB` | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` | PASS |
| Trusted `FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` | PASS |
| R3AW diagnostic expectation | 2,235 | `96d76267674549a76d18998db36f4ac22db69b9090565cba9abac2a59b8a23a3` | PASS |
| R3AR package manifest | 4,713 | `9f70ced501009c2e80bc625e6f60f2d2d0d0d54921553f8c3637cd259fc53d6f` | PASS |
| R3AR packaging provenance | 15,771 | `668bb7e1c1dc9d0e9acbe1e144fae4ed51e64558a18ac844b400b55644025a30` | PASS |
| R3AR independent validation | 2,091 | `965627ca86ae299ae254db8e58eb0643d2598ecbbb86ab3c132364a4f0564711` | PASS |
| ISO report `iso-report.json` | 677 | `3a205e658dd096efc984eed00303d810927a1b02dcaae7dc5225dd880617a0cd` | PASS |
| ISO verification `iso-verification.json` | 677 | `3a205e658dd096efc984eed00303d810927a1b02dcaae7dc5225dd880617a0cd` | PASS |
| ESP report `esp-report.json` | 576 | `5be43eaf459d4307a317a2454217a4df4fd9655a7a5ed3d5b317676862aefbb1` | PASS |
| ESP verification `esp-verification.json` | 576 | `5be43eaf459d4307a317a2454217a4df4fd9655a7a5ed3d5b317676862aefbb1` | PASS |
| Embedded SQL verification report | 1,463 | `ad0dd4b5e04b5f21aa0254ba35dc8a34866b7bcc86ba01742042c24e55927fcb` | PASS |
| RAM disk extracted from ISO | 61,865,984 | `50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c` | PASS |

The extracted ESP was also independently rechecked at 100,663,296 bytes with the same SHA-256 as `EFI-BOOT.IMG`. The extracted SQL files were independently rechecked at 250,880 bytes / `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` for `MULTI.GDB` and 12,937 bytes / `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` for `FIXTURE.MAN`. The ISO verification identifies a bootable UEFI El Torito entry (platform `0xEF`, no-emulation) containing the verified ESP. The ESP verifier matches the loader, qualification kernel, and RAM disk.

The candidate manifest is 8,068 bytes with observed SHA-256 `4419534a6fe848e8dbf498ca12aed3314b42f36f9f8662af0f93b52ea93555ae`; the R3AW report does not separately pin a candidate-manifest digest, so this is recorded as observed identity only. QEMU is 25,287,296 bytes with SHA-256 `a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02`. The accepted OVMF firmware file is 3,653,632 bytes with SHA-256 `2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792`.

## QAC census and launch disposition

The final pre-admission census was captured at **2026-11-11 19:52:18 UTC** (**11:52:18 AM PST**) using the current QAC CLI from the clean QAC repository at HEAD `1ea193258690f9e7b39390b101a117166ef8bbdd`.

- Coordinator: online, QAC 2.0.0, PID 8204.
- QEMU processes: 1, with 0 foreign processes.
- Managed sessions: 1; queued reservations: 0; blocked reservations: 0.
- Active reservation: `ae0799ade14644099c016988516211cd`, project `APP_MODEL`, mode `acceptance`, state `Running`.
- QAC-reported owner: `LEON2_RDT\guideX` (PID 1952), owner liveness verified.
- Managed QEMU child: PID 20620, `C:\Program Files\qemu\qemu-system-x86_64.exe`.
- Exclusive acceptance availability: **unavailable**.

No R3AX reservation request was submitted. No QEMU process was started and no QMP endpoint was opened. The R3AX runner stopped before writing `launch-attempt.json`; there is no R3AX `qac-admission.json`, serial transcript, or QMP transcript. The prepared-but-unlaunched configuration used the pinned QEMU and OVMF hashes above, read-only pflash, the exact ISO as read-only CD-ROM, 1024 MiB guest RAM, and no writable guest storage. Serial output path and a loopback QMP port were prepared; the port was not bound. This report does not claim that configuration was executed.

The first live census earlier in preflight showed QAC empty. The authoritative census immediately before admission found the active `APP_MODEL` reservation, so that later census controls the decision. No repeated polling was done. No final global-empty census is claimed; the unrelated reservation was active in the captured census and was left alone.

## Authoritative R3AX 20-gate status

| Gate | Status | Evidence / disposition |
|---:|---|---|
| 1. Exact frozen R3AW ISO, candidate manifest, and embedded ESP identity | **PASS** | ISO and all 20 manifest-pinned artifacts matched; embedded UEFI ESP and SQL files were verified. ISO was rehashed after the blocker and still matched. |
| 2. Fresh QAC census and exclusive acceptance admission | **FAIL — blocked before request** | QAC was online, but an unrelated `APP_MODEL` acceptance reservation was active. Exclusive admission was unavailable; no request was submitted. |
| 3. QAC reservation owner, managed child, and executable identity for R3AX | **NOT REACHED** | No R3AX reservation or guest child was created. The unrelated reservation was preserved. |
| 4. Approved UEFI firmware and actual guest configuration | **NOT REACHED** | QEMU/OVMF identities and intended read-only arguments were prepared; no guest was launched. |
| 5. Correlated QMP negotiation and running-state response | **NOT REACHED** | No QMP connection or exchange occurred. |
| 6. Live CR3 agreement, CR4.LA57 check, and raw-QMP control-register recovery | **NOT REACHED** | No guest registers were observed. The R3AU QMP parser was not modified. |
| 7. UEFI BootInfo v3 and version-2 page-table sidecar integrity | **NOT REACHED** | No BootInfo or sidecar serial/QMP evidence exists for R3AX. |
| 8. All 15,104 page-base checks and separate final-byte probe | **NOT REACHED** | No guest mapping diagnostic ran; `pages_checked=15105` was not observed. |
| 9. Former 2 MiB boundary and independent live page-table walks | **NOT REACHED** | No current-guest translations or table entries were read. |
| 10. Supervisor-only/read-only mappings, physical progression, CR0.WP, and kernel overlap | **NOT REACHED** | No live mapping evidence exists. |
| 11. Complete 512-byte package boot-sector check, FAT32 classification, and mount | **NOT REACHED** | Offline package validation passed, but no guest LBA 0 read or FAT32/VFS mount occurred. |
| 12. Read-only production VFS and full `FIXTURE.MAN` read, EOF, and close | **NOT REACHED** | Offline identity passed only. Guest `stat`, `open`, bounded reads, 12,937-byte count, EOF, and `close` were not observed. |
| 13. Production VFS access to `MULTI.GDB` | **NOT REACHED** | No guest fixture open/read occurred. |
| 14. Ordered 73-locator point lookup (`IX_Rows_K64`, root 234, owner 1) | **NOT REACHED** | Expected vector was loaded from the trusted manifest and matched the diagnostic source during preflight; production SQL did not execute. |
| 15. Ordered 32-locator range scan (`IX_Rows_K32`, root 148, owner 2) | **NOT REACHED** | Expected vector was loaded from the trusted manifest and matched the diagnostic source during preflight; production SQL did not execute. |
| 16. Invalid request rejection and unchanged caller output | **NOT REACHED** | No guest request was issued. |
| 17. Valid recovery request after rejection | **NOT REACHED** | No guest request was issued. |
| 18. Complete serial scan for FAIL/BLOCKED, panic, or memory faults | **NOT REACHED** | There is no guest serial stream to scan. |
| 19. Ordered final mapping, package, manifest, SQL, and combined PASS markers | **NOT REACHED** | No guest diagnostic markers were produced. |
| 20. Controlled shutdown, child exit, QAC release, and final reconciliation | **NOT REACHED** | No R3AX reservation or child existed, so no R3AX cleanup was needed. The unrelated QAC workstream remained untouched; no claim is made that QAC was globally empty. |

## Manifest and SQL acceptance frontier

The R3AW source contract and diagnostic expectation were inspected before admission. The manifest reader uses the production `/system` VFS path and a 1 KiB buffer, expects exactly 12,937 bytes, performs bounded reads and a one-byte EOF probe, and closes the handle. The expected successful serial sequence includes 13 data reads plus the EOF probe (`read_calls=14`) and `content_identity=NOT_CHECKED`. This source-level contract and the offline package hash do not prove that the guest exercised the read.

The trusted acceptance manifest’s full point and range locator sequences were loaded and compared against the candidate diagnostic arrays before admission. The vectors were 73 and 32 locators, respectively, with the required indexes, roots, and owners. This is preflight consistency evidence only; there were no production SQL results, negative-request results, recovery results, or resource-cleanup results from a guest.

Runtime SHA-256 authentication is not claimed. The R3AW manifest diagnostic is specified to prove packaged-file access through production VFS; package provenance and offline hashes establish content identity.

## Cleanup and evidence

No R3AX QAC reservation was acquired, no guest child was launched, and no R3AX guest shutdown was necessary. The active `APP_MODEL` reservation and child were not contacted or modified. The R3AW ISO final hash recheck passed.

Evidence is preserved under `E:\CodexArtifacts\NDB1R4P11R3AX`:

- `candidate-integrity-preflight.json` — 20 pinned artifact checks, embedded verification, source identities, launch readiness, and final ISO recheck.
- `attempt-20261111-acceptance\qac-prelaunch-census.json` — authoritative final census.
- `attempt-20261111-acceptance\qac-prelaunch-{status,list,queue}.{stdout,stderr}.txt` — raw CLI census outputs.
- `attempt-20261111-acceptance\candidate-integrity-preflight.json` — candidate hashes rechecked at admission time, full SQL vectors, QAC state, and blocker.
- `attempt-20261111-acceptance\qac-admission-blocker.json` and `outcome.json` — Outcome C, no request, no guest.
- `attempt-20261111-acceptance\r3ax_acceptance_runner.py` — acceptance-only runner preparation; it stopped at preflight before requesting QAC admission.

There is no `launch-attempt.json`, QMP capture, serial log, page-table walk, or post-run QAC release record because no R3AX guest was launched.

## Outcome and next bounded step

**Outcome C — `NATIVE_SQL_R3AX_GUEST_PENDING`.** The first blocker was QAC exclusive acceptance availability. Candidate integrity passed; guest acceptance gates remain not reached.

Retry this same frozen-candidate acceptance only after QAC reports that exclusive acceptance is available. Take one fresh authoritative census immediately before the single admission request. Do not rebuild or modify the candidate for this retry. After R3AX is accepted, the next SQL development milestone can expand the native read-only service beyond the fixed point/range vectors while preserving immutable VFS backing.
