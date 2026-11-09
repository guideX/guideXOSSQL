# NDB1R4P11R3AV — Native RAM-Disk Guest Acceptance Procedure

**Status:** Prepared for a separate guest-acceptance phase. This procedure was not run during R3AU.

## Frozen candidate

Use only this immutable R3AU ISO:

- `E:\CodexArtifacts\NDB1R4P11R3AU\candidate\guideXOS-NDB1R4P11R3AU-amd64.iso`
- Size: `100730880` bytes
- SHA-256: `749f909c38d20c9c6e61426ceec85bf397fea7112f8c2ec36aca22b45273194e`
- Candidate manifest: `E:\CodexArtifacts\NDB1R4P11R3AU\candidate\r3au-boot-candidate-manifest.json`

Before admission, recompute the ISO length and hash and compare them with the values above and the candidate manifest. Do not rebuild, replace, or relabel the ISO during acceptance. Write R3AV serial, QMP, QAC, and verification outputs under a new `E:\CodexArtifacts\NDB1R4P11R3AV` directory.

## Controlled run procedure

1. Confirm the candidate identity and its embedded UEFI ESP verification report.
2. Confirm a healthy, empty QAC census. Request one exclusive acceptance reservation for project `guideXOSServer`; do not start QEMU outside QAC.
3. Verify that QAC reports the same reservation ID, acceptance mode, project, verified owner PID/liveness, managed QEMU child PID, and expected QEMU executable path. Do not open QMP until this relationship is verified.
4. Launch the exact ISO with the accepted UEFI firmware and read-only media. Do not attach writable guest storage. Capture serial output and every raw QMP JSONL frame with CRLF framing.
5. Negotiate QMP and require a correlated `query-status` response showing the guest running.
6. On the R3AU MAP-E serial record, parse the 16-digit `cr3_active`, alias, backing, extent, CR0, and `cr0_wp` with `scripts/r3au_qmp_evidence.py`. Query `info registers` through the owned QMP connection and require its correlated 64-bit CR3 to identify the same active root. Parse the observed CR4 as well and require `CR4.LA57=0`; the bounded walker supports four-level paging only. Preserve the raw request and response.
7. Require BootInfo v3 acceptance, alias base `0xFFFF900000000000`, physical backing from the live BootInfo, length `0x03B00000`, and the expected version-2 sidecar marker. For the pinned geometry, the sidecar should report 33 unique frames: 30 leaf PTs and three shared parent/root frames.
8. Require the guest's full-extent result to cover all 15,104 4 KiB page bases and the final byte at end-exclusive minus one. The current `pages_checked` field counts that separate endpoint probe, so its expected successful value is 15,105. Require `SQL_R3AU_MAP_RESULT=PASS` and the package/mapping diagnostic PASS markers. A host-side parser PASS does not substitute for these guest results.
9. Independently inspect the active tables with the corrected QMP walker while the verified QAC-owned guest remains running. Call `walk_qmp_page_tables` with the observed CR4 value and a QMP exchange callback that logs every correlated `xp /1gx` response. Capture paths for the alias base and offsets `0x001FE000`, `0x001FF000`, `0x00200000`, `0x00201000`, `0x00202000`, the final page, and the final byte. Confirm the PD entry changes at the 2 MiB transition and each leaf resolves through the frame recorded in the live walk.
10. For each observed RAM-disk leaf, require present=1, effective writable=0, effective user=0, 4 KiB page size for the loader-created alias, and physical address equal to the live physical backing plus virtual offset. Confirm `CR0.WP=1`, no kernel-image physical overlap, and an end-exclusive boundary at `0xFFFF900003B00000`.
11. Require complete validation of the 512-byte boot sector against the pinned R3AR package expectation, then require FAT32 classification and successful production mount.
12. Require read-only VFS/block capabilities for `/system`; verify fixture and manifest access without a write callback.
13. Require the SQL point request to return 73 ordered locators and clean up all handles.
14. Require the SQL range request to return 32 ordered locators and clean up all handles.
15. Require invalid-request rejection with count zero and the caller's output buffer unchanged.
16. Require the valid recovery request after the negative case to succeed with the expected ordered result.
17. Require final mapping, package, RAM-disk, and SQL PASS markers, including `SQL_NATIVE_R3AU QUALIFICATION END PASS`.
18. Scan the complete serial stream for contradictory mandatory mapping/package/SQL FAIL, BLOCKED, panic, or memory-fault markers. Any such marker prevents acceptance.
19. Send a controlled QMP `quit`, preserve the SHUTDOWN event and correlated response, and wait for the owned QEMU child to exit normally.
20. Query QAC after shutdown. Require the same reservation to be `Released`, the owned process absent, no active reservation for this run, and unrelated QAC resources unchanged. Recheck the ISO hash and record serial/QMP/QAC evidence hashes.

## Acceptance evidence and result

Keep the QAC admission/relationship, launch configuration, serial log, raw QMP JSONL, page-walk summary, package/mount/SQL markers, shutdown result, post-run QAC census, and candidate integrity report together under the R3AV output directory. Preserve the raw frames; do not reconstruct or substitute CR3 values.

Classify the run as accepted only if all 20 gates pass. A serial mapping failure, QMP page-walk error, guest fault, contradictory mandatory failure, unexpected process identity, or unreleased QAC reservation is a failed or partial acceptance and must remain visible in the report.

