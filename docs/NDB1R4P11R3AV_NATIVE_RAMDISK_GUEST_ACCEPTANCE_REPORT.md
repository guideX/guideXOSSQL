# NDB1R4P11R3AV — Native RAM-Disk Guest Acceptance Report

- **Outcome:** `NATIVE_SQL_R3AV_GUEST_PENDING` (Outcome C)
- **Phase:** `NATIVE_RAMDISK_FULL_EXTENT_GUEST_ACCEPTANCE`
- **Acceptance date:** 2026-11-10 (America/Los_Angeles)
- **Guest execution:** Not started. No R3AV QAC reservation was requested.

## Result

The frozen candidate and all pinned supporting artifacts passed the pre-admission identity checks. The fresh authoritative QAC census then found an existing managed `guideXOSServer` acceptance reservation with one managed QEMU child. The queue was empty, but exclusive admission was not available. Per the R3AV procedure, I stopped before requesting a reservation. I did not start QEMU, connect to the other guest, cancel or release its reservation, or change its files.

This is an admission-blocked pending result, not a guest failure or an acceptance. No guest serial, runtime CR3/CR4, sidecar read, page walk, FAT/VFS execution, SQL query, shutdown, or R3AV post-run cleanup evidence exists. The actual guest mapping and SQL service remain unverified.

## Frozen candidate and artifacts

The ISO matches the required size and SHA-256. The embedded boot image and all manifest-pinned artifacts also match their expected byte counts and SHA-256 values. The complete machine-readable verification is in `E:\CodexArtifacts\NDB1R4P11R3AV\candidate-integrity-preflight.json`.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| ISO | 100730880 | `749f909c38d20c9c6e61426ceec85bf397fea7112f8c2ec36aca22b45273194e` |
| EFI-BOOT.IMG | 100663296 | `b408175820b5190be63c1c7584d72c188c81953af52c5d94beb82400f9a0ce57` |
| BOOTX64.EFI | 83968 | `f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77` |
| kernel.elf | 2764480 | `f58a277b6021b1ac89054607beed72f09d083e12e5ef43949e7274c032605ae3` |
| kernel.pe | 2878679 | `a359fbde37f902760e8ea343fde28e9d2c463df70b5e0ce98dcd2e9ea1c3f1e2` |
| ramdisk.img | 61865984 | `50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c` |
| MULTI.GDB | 250880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| FIXTURE.MAN | 12937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| Candidate manifest | 12818 | `4c326831441e37fd5da86f903be2ac6625197099ecb0a39f31d5059e5bcd976e` |
| Diagnostic expectation | 1762 | `09c2c50fbb12036404c566529dccb9ac3369c71ec14d8ac9ac5ff8b91aa5585d` |
| Package manifest | 4713 | `9f70ced501009c2e80bc625e6f60f2d2d0d0d54921553f8c3637cd259fc53d6f` |
| Packaging provenance | 15771 | `668bb7e1c1dc9d0e9acbe1e144fae4ed51e64558a18ac844b400b55644025a30` |
| Independent validation | 2091 | `965627ca86ae299ae254db8e58eb0643d2598ecbbb86ab3c132364a4f0564711` |
| R3AS expectation header | 3895 | `17c2976f34a1f679bac22d8dd1c8c5609bab7eaf6b0789db17d3bfdd45c773ec` |
| ISO verification report | 677 | `a9eaf8450c29200bf7ce1026ad990bd785e1079f844d2a2f2cd0c6074012bf43` |
| ESP verification report | 576 | `71c32b8ed925a63dc2e6194c35994921543384eac14fb185c79de8548e2c7e37` |

The ISO verifier reports a bootable UEFI image (`0xEF`); the embedded ESP inventory matches the pinned loader, ELF kernel, and RAM disk. The package expectation specifies a 512-byte boot sector. The recorded package metadata specifies FAT32 with 118,926 data clusters. These are hosted artifact checks only; no guest read or mount occurred.

The trusted manifest identity was verified. Its 73 point-lookup locators (`duplicate_k64_7`, `IX_Rows_K64`, root 234, owner 1) and 32 range locators (`range32_inclusive_1_2`, `IX_Rows_K32`, root 148, owner 2) were compared with the compiled expected sequences in the Server diagnostic source. Both source-to-manifest comparisons passed. This does not establish guest SQL execution.

## Repository and source provenance

| Repository | Branch | HEAD at preflight | Result |
|---|---|---|---|
| SQL database | `main` | `49f48d99c8049e694aa0cc75e9c4a4c67faad14b` | Clean before this report |
| Isolated Server worktree | `codex/ndb1r4p11rah-sql-read-integration` | `e1c8c4a71c1a01fec7d7a6ddce095fd5c4986082` | Tracked tree clean; pre-existing untracked outputs preserved |
| QAC | `main` | `1ea193258690f9e7b39390b101a117166ef8bbdd` | Clean |

The SQL candidate source commit `6b58cc154251bfdf290c74d2e6239ed5e1d62212` and Server candidate source commit `0dff95ea05e956cff0527e18b1dd1500cd487f7c` are ancestors of the recorded candidate repositories. At preflight, the SQL delta since its image source was limited to the R3AU and R3AV procedure documents. The Server delta since its image source was limited to `scripts/r3au_qmp_evidence.py` and `scripts/test-r3au-qmp-evidence.py`. This acceptance added only this report; no production code, tests, build outputs, candidate files, or other checkout were modified. The complete identity and provenance snapshot is `E:\CodexArtifacts\NDB1R4P11R3AV\repository-identities.json`.

The corrected QMP parser file hash was recorded as `fa58d785056528405dae378544fb7faf90fe8b6efac54ee80783c301fd165d28`; its required live-observation APIs were present. Synthetic parser tests were not used. QEMU and OVMF hashes also matched the previously pinned executables. These checks did not exercise QMP against a guest.

## QAC admission

The fresh prelaunch census at `2026-11-10T20:46:12-08:00` showed the coordinator online at version 2.0.0, one existing managed QEMU process, one running managed session and reservation, and zero queued reservations. The existing session was an acceptance reservation with a verified owner and registered child. It was not an R3AV reservation. QAC reported zero foreign QEMU processes; the active managed workstream still made the required empty, exclusive census false.

No QAC request was submitted, so there is no R3AV reservation ID, QAC-owned R3AV child, or R3AV launch configuration. The QEMU/OVMF hashes, approved read-only pflash and ISO arguments, no-writable-storage configuration, and capture destinations were prepared for the authorized run, but no guest process was launched. The QAC evidence is preserved in:

- `E:\CodexArtifacts\NDB1R4P11R3AV\qac-prelaunch-census.json`
- `E:\CodexArtifacts\NDB1R4P11R3AV\qac-admission-refused.json`
- `E:\CodexArtifacts\NDB1R4P11R3AV\outcome.json`
- `E:\CodexArtifacts\NDB1R4P11R3AV\r3av_acceptance_runner.py`

The raw census contains the existing reservation and process details. No QAC-wide empty-state claim is made.

## Authoritative 20-gate status

| Gate | Status | Evidence / reason |
|---:|---|---|
| 1. Verify frozen ISO and embedded ESP | **Passed** | Required ISO size/hash and ESP verification report match; all pinned artifacts match. |
| 2. Healthy empty QAC census and exclusive admission | **Failed prerequisite** | Fresh census found one other managed acceptance guest; admission was unavailable. No request was made. |
| 3. Verify the R3AV reservation, owner, and managed QEMU child | **Not reached** | No R3AV reservation exists. |
| 4. Launch exact ISO with approved UEFI/read-only configuration | **Not reached** | QEMU was not started. |
| 5. Negotiate QMP and observe running status | **Not reached** | No guest or QMP connection. |
| 6. Compare live CR3/CR0 and observe CR4 with LA57 disabled | **Not reached** | No guest register observations. |
| 7. Verify BootInfo v3 and live version-2 sidecar identity/checksum | **Not reached** | No boot or sidecar read. |
| 8. Verify all 15,104 page bases and final-byte probe | **Not reached** | No guest mapping diagnostic. |
| 9. Independently walk QMP page tables, including the 2 MiB transition | **Not reached** | No QMP page-table reads; transition at `0xFFFF900000200000` remains unverified. |
| 10. Verify physical progression, 4 KiB supervisor read-only mappings, CR0.WP, and non-overlap | **Not reached** | No live mappings or protection state. |
| 11. Validate the 512-byte package boot sector, classify FAT32, and mount | **Not reached** | Hosted package metadata passed; no guest LBA read or mount. |
| 12. Verify read-only VFS registration and fixture/manifest reads | **Not reached** | No guest VFS execution. |
| 13. Execute the 73-locator point lookup and cleanup | **Not reached** | Only the trusted expected sequence was source-cross-checked. |
| 14. Execute the 32-locator range scan and cleanup | **Not reached** | Only the trusted expected sequence was source-cross-checked. |
| 15. Reject invalid input without publishing partial output | **Not reached** | No guest request executed. |
| 16. Execute the valid recovery request after rejection | **Not reached** | No guest request executed. |
| 17. Observe required ordered diagnostic PASS markers and final qualification PASS | **Not reached** | No serial stream exists. |
| 18. Scan serial for contradictory FAIL/BLOCKED/fault markers | **Not reached** | No serial stream exists. |
| 19. Send QMP quit and verify normal owned-child exit | **Not reached** | No R3AV guest exists. |
| 20. Verify R3AV reservation release, owned-child absence, and unrelated-resource preservation | **Not reached** | There was no R3AV reservation to release. The other managed workstream was not touched. |

## Guest results and cleanup

No actual CR3 or CR4 was observed. No actual sidecar magic, version, frame list, capacity, or checksum was read. The complete 15,104-page mapping and separate final-byte probe were not run; neither the former 2 MiB boundary nor CR0.WP/PTE permission gates were exercised. No QMP JSONL, serial log, FAT/VFS result, ordered SQL result, negative-request result, recovery result, or final serial marker was generated.

There was no R3AV guest to shut down and no R3AV reservation to release. The one pre-existing managed workstream remained under its existing QAC ownership; the R3AV action did not stop, cancel, or otherwise alter it. The evidence records the census at admission time and does not assert its later state.

## Acceptance gates

Only the artifact identity gate passed. QAC exclusivity was unavailable, and all guest-dependent gates remain unverified because execution was correctly stopped before admission. Therefore Outcome A and Outcome B do not apply; the result is Outcome C, `NATIVE_SQL_R3AV_GUEST_PENDING`. No guest acceptance credit is claimed.

## Recommended next milestone

After the existing acceptance workstream releases its QAC reservation, perform a new fresh QAC census and one R3AV admission attempt against this unchanged ISO. Keep this pending record and its evidence intact; place any later attempt in a new subdirectory under `E:\CodexArtifacts\NDB1R4P11R3AV` so the admission-blocked record remains auditable. Continue to use the existing 20-gate procedure and do not rebuild or patch the candidate.
