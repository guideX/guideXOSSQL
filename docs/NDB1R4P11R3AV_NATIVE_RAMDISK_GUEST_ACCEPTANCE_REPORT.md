# NDB1R4P11R3AV — Native RAM-Disk Guest Acceptance Report

- **Outcome:** NATIVE_SQL_R3AV_GUEST_PARTIAL_OR_FAILED (Outcome B)
- **Phase:** NATIVE_RAMDISK_FULL_EXTENT_GUEST_ACCEPTANCE
- **Acceptance date:** 2026-11-11 (America/Los_Angeles)
- **Guest executions:** one QAC-managed execution; no retry

## Result

The guest booted the exact frozen R3AU ISO and passed the complete 15,104-page RAM-disk mapping check, the independent live QMP page-table inspection, the standards-conforming FAT32 mount, and the R3AQ point/range, invalid-request, and recovery diagnostics.

Outcome A is not claimed because R3AV procedure gate 12 also requires the guest to access the packaged SQL manifest through the read-only /system VFS. The guest opened and read MULTI.GDB, but the serial trace and qualification source do not establish an open/read of /system/Apps/R3AI/FIXTURE.MAN. The mount was read-only and its block device had a read callback with no write callback; that does not prove the manifest file was accessed. This mandatory gate remains **unverified**, so the run is Outcome B under the procedure.

The prior 2026-11-10 preflight ended as Outcome C because another managed acceptance reservation occupied QAC. That report and evidence remain preserved in the prior SQL commit and at E:\CodexArtifacts\NDB1R4P11R3AV. This report records the fresh 2026-11-11 execution in the separate attempt-20261111-acceptance directory.

## Frozen candidate and provenance

The launched image was:

- E:\CodexArtifacts\NDB1R4P11R3AU\candidate\guideXOS-NDB1R4P11R3AU-amd64.iso
- Size: 100730880 bytes
- SHA-256: 749f909c38d20c9c6e61426ceec85bf397fea7112f8c2ec36aca22b45273194e

The preflight verified the ISO’s UEFI El Torito entry and embedded ESP. It recomputed the pinned identities below before admission and rechecked the artifacts after shutdown; all remained unchanged.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| EFI-BOOT.IMG | 100663296 | b408175820b5190be63c1c7584d72c188c81953af52c5d94beb82400f9a0ce57 |
| BOOTX64.EFI | 83968 | f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77 |
| Qualification kernel.elf | 2764480 | f58a277b6021b1ac89054607beed72f09d083e12e5ef43949e7274c032605ae3 |
| Qualification kernel.pe | 2878679 | a359fbde37f902760e8ea343fde28e9d2c463df70b5e0ce98dcd2e9ea1c3f1e2 |
| ramdisk.img | 61865984 | 50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c |
| SQL fixture MULTI.GDB | 250880 | cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57 |
| Trusted SQL manifest FIXTURE.MAN | 12937 | 43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3 |
| R3AU candidate manifest | 12818 | 4c326831441e37fd5da86f903be2ac6625197099ecb0a39f31d5059e5bcd976e |
| R3AU diagnostic expectation | 1762 | 09c2c50fbb12036404c566529dccb9ac3369c71ec14d8ac9ac5ff8b91aa5585d |
| R3AR package manifest | 4713 | 9f70ced501009c2e80bc625e6f60f2d2d0d0d54921553f8c3637cd259fc53d6f |
| R3AR packaging provenance | 15771 | 668bb7e1c1dc9d0e9acbe1e144fae4ed51e64558a18ac844b400b55644025a30 |
| R3AR independent validation | 2091 | 965627ca86ae299ae254db8e58eb0643d2598ecbbb86ab3c132364a4f0564711 |
| R3AS 512-byte expectation header | 3895 | 17c2976f34a1f679bac22d8dd1c8c5609bab7eaf6b0789db17d3bfdd45c773ec |

The complete preflight and post-run checks are in E:\CodexArtifacts\NDB1R4P11R3AV\attempt-20261111-acceptance\candidate-integrity-preflight.json and artifact-integrity-postrun.json.

Repository identities and source provenance:

| Repository | Branch / HEAD at run start | Result |
|---|---|---|
| SQL database | main, ba807a33d61d23a434727de584283ad8b595ed21 | Clean. The supplied expected HEAD 49f48d99c8049e694aa0cc75e9c4a4c67faad14b is its parent; the intervening commit only recorded the earlier R3AV admission-block report. Candidate SQL source 6b58cc154251bfdf290c74d2e6239ed5e1d62212 is an ancestor. No SQL production source changed. |
| Isolated Server integration worktree | codex/ndb1r4p11rah-sql-read-integration, e1c8c4a71c1a01fec7d7a6ddce095fd5c4986082 | Expected HEAD; tracked tree clean. Existing untracked output directories were preserved. |
| QAC | main, 1ea193258690f9e7b39390b101a117166ef8bbdd | Expected HEAD; clean. |

The candidate manifest pins Server source 0dff95ea05e956cff0527e18b1dd1500cd487f7c and SQL source 6b58cc154251bfdf290c74d2e6239ed5e1d62212; both are ancestors of their run-time repositories. No candidate was rebuilt or repackaged. The R3AS, R3AR, R3AQ, and earlier candidates were left untouched. The SQL Debug CTest suite was not rerun.

## QAC admission and guest configuration

The fresh prelaunch census at 2026-11-11T13:13:07Z showed QAC 2.0.0 online with zero QEMU processes, managed sessions, reservations, queued requests, or blocked requests. Exclusive acceptance admission was available.

QAC granted reservation 0de8bfc4f0944614b83d1bf18a138f43 for project guideXOSServer, mode acceptance. The verified owner was PID 26812; QAC registered managed QEMU child PID 22660 as C:\Program Files\qemu\qemu-system-x86_64.exe. The process command line was reconciled with the QAC reservation before QMP access.

The one launch used QEMU SHA-256 a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02 and the previously accepted read-only OVMF pflash at D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\OVMF.fd, SHA-256 2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792. The exact ISO was attached as a read-only CD-ROM, with 1024 MiB RAM and no writable guest storage. QEMU wrote serial capture to the attempt directory and exposed loopback QMP on port 57322.

The launch controller’s first listener check returned false. QAC’s list/inspect and the host process census still showed the same verified owner and managed child. A subsequent socket census associated port 57322 with PID 22660, and a live QMP greeting and correlated commands confirmed the endpoint. The existing child was reconciled and used; no second launch was made.

## Boot, sidecar, and full-extent mapping

The serial trace records UEFI loader execution, BootInfo v3 handoff (status=BOOTINFO_VALID), and active MAP-E CR3 0x000000003DD3C000. MAP-E reported:

- Alias base 0xFFFF900000000000
- Physical backing 0x00000000155BE000
- Length 0x03B00000
- CR0 0x0000000080010033, CR0.WP=1
- Read-only alias and UEFI_EfiLoaderData backing

The loader reported a version-2 sidecar available at 0x000000003DD3F254, with count=33, capacity=433, and range_samples=30. The live QMP inspection read the actual sidecar and verified magic 0x4E334152, version 2, 32-byte header, active root 0x3DD3C000, count/capacity, 33 unique aligned frames within physical-address bounds, root registration, and FNV-1a-64 checksum 0x2C9C12509B67FF43. The checksum recomputed from the actual frame list matched. The list contains 30 leaf PT frames and three shared parent/root frames and exactly matches the page-table frames observed in the live walks.

The guest emitted SQL_R3AU_MAP_RESULT=PASS pages_checked=0000000000003B01, which is 15,105 checks: 15,104 4 KiB page-base translations plus the separate final-byte probe. The final byte was 0xFFFF900003AFFFFF, mapping to physical 0x190BDFFF; the end-exclusive virtual address is 0xFFFF900003B00000.

The former R3AT failure boundary passed. At 0xFFFF9000001FF000, the live mapping resolved to 0x157BD000 through leaf PT frame 0x3DAF7000. At 0xFFFF900000200000, the PD index changed from 0 to 1 and the mapping resolved to 0x157BE000 through leaf PT frame 0x3DAF6000. That required frame was present in the verified sidecar. The next-page and final-page checks also passed.

The QMP walks found 4 KiB leaves with effective writable and user-accessible permissions both false, expected physical progression, and no kernel-image overlap. The RAM disk’s physical end-exclusive address 0x190BE000 meets the reported kernel physical start 0x190BE000 without overlap. CR0.WP was enabled.

## Independent QMP evidence and parser limitation

The successful independent walk used the live active CR3 and live CR4 value 0x0000000000000668; LA57 bit 12 was clear, and the unmodified parser’s four-level-paging check and page-table walker were applied. QMP reported the guest running before inspection. The walker covered all 30 2 MiB regions plus the required focused offsets, sidecar paths, final page, and final byte. It validated present paging structures and 64-bit physical addresses, including the 2 MiB transition.

Two raw QMP JSONL captures are preserved. The successful walk capture contains 384 parsed frames; the initial register observation contains 7. Both are CRLF-framed, have no UTF-8 BOM, and have no response without a matching request ID. The unmodified parser SHA-256 is fa58d785056528405dae378544fb7faf90fe8b6efac54ee80783c301fd165d28. Synthetic parser tests and historical CR3/PTE values were not used.

The first register-parser call exposed a tooling limitation: QEMU’s correlated info-registers response prints CR0=80010033 and CR4=00000668 with eight hex digits, while parse_qmp_hmp_register requires exactly 16. Those strict calls failed and the errors plus raw response are preserved in qmp-control-register-parser-findings.json. An evidence-only exact-field extractor read the single correlated CR0/CR4 fields from the raw live response; it confirmed CR0 matched MAP-E and CR4 was 0x668. The existing parser’s CR3 parser, four-level check, and QMP page walker were used unchanged. No parser source was modified. A separate bounded parser repair should accept valid short control-register output while retaining exact response correlation and width/range checks.

## FAT32 and read-only VFS

The guest attached the RAM disk read-only. It performed a successful one-sector LBA 0 read into a 512-byte buffer. The R3AU package diagnostic compared the complete package-specific 512-byte boot sector and emitted SQL_R3AU_PACKAGE_RESULT=PASS. The observed BPB reported 512 bytes per sector, one sector per cluster, 32 reserved sectors, two FATs, 937 sectors per FAT, 120,832 total sectors, and 118,926 data clusters.

The production driver classified the volume at runtime as FAT32 (cluster_class=FAT32), accepted the FAT32 variant, mounted it at /, and registered it read-only. /system was mounted as an alias of that volume. The SQL fixture was opened through /system/Apps/R3AI/MULTI.GDB, and the production SQL provider completed its VFS reads.

The fixture manifest portion of gate 12 is unverified. The run did not log an open/read of /system/Apps/R3AI/FIXTURE.MAN, and the qualification source checks the read-only FAT32 mount and block callbacks but does not open that manifest. The unrelated /certs/ca-bundle.manifest read later in the desktop trace is not the SQL fixture manifest and is not counted.

## SQL service results

The trusted SQL manifest SHA-256 is 43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3. Before admission, its complete ordered locator vectors were compared against the candidate diagnostic arrays; both source-to-manifest comparisons passed. The production guest diagnostic compares each returned locator with those expected arrays before emitting ordered=PASS.

- Point: duplicate_k64_7, index IX_Rows_K64, root 234, owner 1; **73/73 ordered locators matched**.
- Range: range32_inclusive_1_2, index IX_Rows_K32, root 148, owner 2; **32/32 ordered locators matched**.
- Invalid key: rejected with INVALID_REQUEST, count 0, caller output UNCHANGED, cleanup PASS.
- Recovery: the valid point request immediately after rejection returned 73 ordered locators and cleanup PASS.

The complete vectors are retained in sql-locator-comparison.json. The serial trace emitted SQL_NATIVE_R3AQ END PASS and SQL_NATIVE_R3AU QUALIFICATION END PASS. These markers confirm the tested point/range and negative/recovery service behavior; they do not waive the separate manifest-access gate.

## Diagnostic order and failure scan

The serial trace contains, in order, the R3AU mapping PASS with 15,105 checks, package PASS, RAM-disk diagnostic PASS, LBA 0/package/FAT32/VFS success, point PASS, range PASS, invalid-request PASS, recovery PASS, SQL end PASS, and final combined qualification PASS. The complete serial scan found no mandatory mapping/package/SQL FAIL or BLOCKED marker, panic, page fault, protection fault, triple fault, or memory fault.

A separate evidence-tool ordering issue is recorded in serial-marker-reconciliation.json: the candidate manifest’s expectedMappingSerial array places SQL_R3AU_TABLE_LIST available=1 after MAP-D, but the actual serial/source order places that loader marker after MAP-C and before MAP-D. All markers are present, and the authoritative acceptance results occur in their required order. This is an evidence expectation ordering defect, not a guest diagnostic failure.

## Authoritative 20-gate status

| Gate | Status | Result |
|---:|---|---|
| 1. Frozen ISO and embedded ESP identity | **Passed** | All pinned artifacts matched before and after the run. |
| 2. Fresh QAC census and exclusive admission | **Passed** | Coordinator online; prelaunch QAC state empty; one acceptance reservation granted. |
| 3. Reservation owner and managed child | **Passed** | Verified owner PID 26812 and QEMU child PID 22660. |
| 4. Approved firmware and guest configuration | **Passed** | Pinned QEMU/OVMF, read-only pflash and ISO, no writable guest storage. |
| 5. QMP negotiation and running state | **Passed** | Correlated capabilities and running-status responses captured. |
| 6. Live CR3/CR0 and CR4.LA57 | **Passed** | CR3 matched; CR0.WP=1; observed CR4.LA57=0. Strict CR0/CR4 field-width parser limitation is documented above. |
| 7. BootInfo v3 and version-2 sidecar | **Passed** | Actual sidecar header, checksum, root and 33 frames verified. |
| 8. Complete mapping and final byte | **Passed** | All 15,104 page bases and separate endpoint probe passed. |
| 9. Independent live page-table walk | **Passed** | Active tables, 2 MiB boundary, selected offsets and final byte matched. |
| 10. Permissions, physical progression, CR0.WP and overlap | **Passed** | Read-only supervisor mappings and non-overlap confirmed. |
| 11. Package boot sector, FAT32 classification and mount | **Passed** | Full 512-byte check, runtime FAT32 and mount passed. |
| 12. Read-only /system, fixture and manifest access | **Unverified** | Fixture access and no-write block capability passed; packaged SQL FIXTURE.MAN access was not observed. |
| 13. 73-result point query | **Passed** | Exact ordered sequence matched the trusted manifest vector. |
| 14. 32-result range query | **Passed** | Exact ordered sequence matched the trusted manifest vector. |
| 15. Invalid-request rejection | **Passed** | Status and unchanged-output invariants passed. |
| 16. Recovery after rejection | **Passed** | Valid 73-result point query succeeded immediately afterward. |
| 17. Required diagnostic PASS sequence | **Passed** | Mapping, package, RAM-disk, FAT32/VFS, SQL, recovery and final PASS evidence appeared in order. |
| 18. Contradictory FAIL/BLOCKED/fault scan | **Passed** | No mandatory failure or fault marker found. |
| 19. Controlled shutdown | **Passed** | QMP quit response and SHUTDOWN event captured; child exited without forced termination. |
| 20. QAC release and post-run reconciliation | **Passed** | Reservation Released, child absent, and final QAC census empty. |

Gate 12 prevents Outcome A. No guest implementation or candidate was changed to close it during this acceptance-only phase.

## Shutdown, integrity, and evidence

QMP returned a correlated quit response {} and a SHUTDOWN event. QEMU PID 22660 exited; QAC reported reservation 0de8bfc4f0944614b83d1bf18a138f43 as Released and no longer listed its child. The fresh final QAC census showed zero QEMU processes, sessions, reservations, queued requests, or blocked requests. No other workstream was active or altered. The frozen ISO hash and all pinned artifacts matched again after shutdown.

Primary evidence is under E:\CodexArtifacts\NDB1R4P11R3AV\attempt-20261111-acceptance:

- candidate-integrity-preflight.json, artifact-integrity-postrun.json
- qac-prelaunch-census.json, qac-admission.json, qac-launch-reconciliation.json, qac-postrun-summary.json, qac-final-census.json
- launch-attempt.json, guest-launch-config.json, qemu-final-process-census.json
- serial.log — 81,036 bytes, SHA-256 78760aba1dd656a5a5be907213d7278ad1b346719fe4f539867c8d75dd2c8519
- qmp-observations.jsonl — 8,443 bytes, SHA-256 e63525aa1a17660382593c082057bf52f0e91474f617ce23e7a738f6d9939e33
- qmp-observations-recovery.jsonl — 137,650 bytes, SHA-256 fe521b530f0883e4a0cac2cb30ad1cdd8f1e2ecead791914da02d68a25f62cfb
- runtime-page-table-walk-recovery.json, qmp-control-register-parser-findings.json, qmp-transcript-validation.json
- sql-locator-comparison.json, serial-marker-reconciliation.json, acceptance-gates.json, outcome.json

No production SQL/Server/QMP-parser code, candidate, fixture, manifest, FAT geometry, or unrelated checkout was modified. No guest retry occurred.

## Next step

Close gate 12 in a separate, bounded Server diagnostic/candidate phase by opening and reading /system/Apps/R3AI/FIXTURE.MAN through the production read-only VFS and recording an explicit success marker. Then perform a new acceptance against that separately identified candidate. Keep this run’s mapping and SQL evidence intact.

After that prerequisite is closed, the next SQL feature milestone should expand the native read-only service beyond its fixed point/range diagnostic vectors while retaining immutable VFS backing. This acceptance does not establish writable databases, concurrent access, or recovery.