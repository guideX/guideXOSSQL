# NDB1R4P11R3AZ-R1 — Native SQL Full Read-Side Acceptance

**Outcome:** NATIVE_SQL_R3AZR1_GUEST_PENDING (Outcome C)
**Phase:** NATIVE_SQL_FULL_READ_SIDE_FINAL_QMP_ACCEPTANCE_RETRY
**Acceptance date:** 2026-11-13 (America/Los_Angeles)
**QEMU guest launches:** 1, started paused with -S
**Guest CPU resumes:** 0
**QAC reservation requests:** 1, granted and released

## Result

The frozen R3AW candidate and all 20 manifest-pinned artifacts passed preflight. A fresh QAC census found the exclusive lane clear, and QAC granted reservation 8e96c080ff29446bb7dfd0e4c4470751 to this attempt. QEMU PID 2588 started under the verified QAC owner PID 18620, with the configured loopback QMP listener and paused -S launch.

The R3AY ownership preflight refused QMP access because its QAC snapshot was 6.263 seconds old, beyond the helper’s two-second freshness limit. The OS observation showed the configured listener owned by QEMU PID 2588, but the required fresh QAC/OS ownership proof was not established. No QMP connection, greeting, capabilities negotiation, status query, register read, memory read, cont, or quit occurred. The guest remained paused; the serial file is empty. This is the first failed gate, Gate 5, and no guest code or SQL qualification ran.

The required QMP-controlled shutdown could not be performed. The documented QAC-owned process-group CTRL_BREAK_EVENT was used only after revalidating the exact reservation, owner, QEMU child, command line, and listener. The first console attachment attempt failed with Win32 error 5. A fresh revalidation then freed the caller’s console, attached to the verified QAC owner, and successfully sent CTRL_BREAK_EVENT to its process group. QAC subsequently reported the reservation Released because the managed child exited. This fallback completed cleanup; it does not pass the QMP shutdown gate.

Outcome C applies because the required pre-execution QMP ownership gate prevented guest CPU execution. No production defect is inferred from this attempt.

## Repository identity and preservation

| Repository | Starting branch / HEAD | Disposition |
|---|---|---|
| SQL | main / 2629e4ce3bf99c54396e1573dea45025f2f981b1 | Only this R3AZ-R1 report was added. Pre-existing untracked tests/__pycache__/ and tools/__pycache__/ were preserved. |
| Isolated Server integration | codex/ndb1r4p11rah-sql-read-integration / d40557a1710f8c21fe34fdf5be8e9c12f7f592dd | No tracked changes. Existing untracked build and acceptance outputs were preserved. |
| QAC | main / 1ea193258690f9e7b39390b101a117166ef8bbdd | Clean; no QAC source or infrastructure changes. |

The acceptance-runner copy and all attempt evidence are isolated under E:\CodexArtifacts\NDB1R4P11R3AZR1. The frozen ISO was not modified, rebuilt, or repackaged. No push or merge was performed.

## Frozen candidate and QAC admission

The ISO is E:\CodexArtifacts\NDB1R4P11R3AW\candidate\guideXOS-NDB1R4P11R3AW-amd64.iso, exactly **100,730,880 bytes**, SHA-256 **60ffbbbd16ff7f3c66a99ebf1f48389f807811afed62fc14e9d3e6c11d93a207**. The manifest SHA-256 is 4419534a6fe848e8dbf498ca12aed3314b42f36f9f8662af0f93b52ea93555ae. All 20 unique manifest-pinned artifact records matched before admission and after cleanup.

The authoritative pre-admission census completed at 2026-11-13T13:51:19.366845Z:

- QAC 2.0.0 was online (coordinator PID 17584).
- QAC reported zero QEMU processes, zero managed sessions, zero queued reservations, and zero blocked reservations.
- One exclusive guideXOSServer acceptance request was submitted and granted.
- Reservation 8e96c080ff29446bb7dfd0e4c4470751 identified owner PID 18620 as Verified and managed child PID 2588 as C:\Program Files\qemu\qemu-system-x86_64.exe.

The actual QEMU command line matched the prepared argument list exactly: QEMU -S, read-only OVMF pflash, read-only ISO media, no writable guest storage, serial file, and QMP at 127.0.0.1:61410. The OS TCP observation associated the listening endpoint with QEMU PID 2588.

## Twenty-gate ledger

| Gate | Result | Evidence / disposition |
|---:|---|---|
| 1. Frozen R3AW ISO, manifest, embedded ESP, and pinned artifacts | **PASS** | Exact ISO length/hash; all 20 manifest pins matched before admission and in the final post-cleanup recheck. |
| 2. Fresh QAC census and exclusive acceptance admission | **PASS** | Fresh status/list/queue census was clear; the single exclusive reservation was granted. |
| 3. Reservation owner, exact QEMU child, executable, and start identity | **PASS** | QAC verified owner PID 18620; QEMU PID 2588, executable, parent, and process start identity correlated. |
| 4. Approved UEFI firmware and exact paused read-only launch configuration | **PASS** | Actual child argv matched the prepared -S command; OVMF and ISO were read-only, with no writable guest storage. |
| 5. Owned QMP listener, negotiation, paused state, and running transition | **FAIL — first failed gate** | The QMP preflight rejected the QAC snapshot as stale by 6.263 seconds. No QMP connection or command was made; cont was not sent. |
| 6. Live CR3, CR0.WP, and CR4 four-level paging policy | **NOT REACHED** | No live QMP register response exists. |
| 7. BootInfo v3 and version-2 sidecar integrity | **NOT REACHED** | Guest CPU did not run; no BootInfo or sidecar diagnostics exist. |
| 8. All 15,104 RAM-disk page bases and separate final-byte probe | **NOT REACHED** | No guest mapping diagnostic exists. |
| 9. Independent live page-table walks and 2 MiB boundary translation | **NOT REACHED** | No QMP memory reads or page-table walks exist. |
| 10. Read-only supervisor mappings, CR0.WP, and kernel non-overlap | **NOT REACHED** | No runtime mapping evidence exists. |
| 11. FAT32 classification and read-only /system mount | **NOT REACHED** | No guest VFS diagnostics exist. |
| 12. Complete guest FIXTURE.MAN read, EOF, and close | **NOT REACHED** | No guest manifest access occurred. |
| 13. Production guest access to MULTI.GDB | **NOT REACHED** | No guest database access occurred. |
| 14. Exact ordered 73-locator point lookup | **NOT REACHED** | No native SQL request ran. |
| 15. Exact ordered 32-locator range scan | **NOT REACHED** | No native SQL request ran. |
| 16. Invalid request rejected without output mutation | **NOT REACHED** | No negative request ran. |
| 17. Valid recovery request after rejection | **NOT REACHED** | No recovery request ran. |
| 18. Complete serial scan for failures, panic, and faults | **NOT VERIFIED** | Serial output is zero bytes because the guest remained paused. |
| 19. Ordered final mapping, package, manifest, and SQL PASS markers | **NOT REACHED** | No guest serial markers exist. |
| 20. Guarded QMP shutdown, owned-child exit, QAC release, cleanup, and final ISO hash | **FAIL — QMP shutdown subgate** | No QMP quit was sent. QAC-owned process-group fallback then exited the owned child and owner; QAC reports Released, zero QEMU/resources, and the final ISO hash matches. Fallback cleanup does not pass QMP-controlled shutdown. |

The raw runner’s acceptance-result.json contains a derived firstFailedGate=2 because its Gate 2 boolean also required the reservation to be released. That condition belongs to Gate 20; the fresh census and QAC grant passed Gate 2. This report preserves the raw file and applies the established 20-gate contract, identifying Gate 5 as the first failure. Gates 6–19 are recorded as not reached or not verified rather than as guest failures.

## QMP ownership, shutdown, and cleanup evidence

- qmp-preconnect-ownership.json records ownership_unknown and the 6.263-second stale-snapshot reason. Its OS observation records the listener on 127.0.0.1:61410 owned by QEMU PID 2588.
- qmp-execution-error.json records that QMP ownership was unverified and that no connection or guest resume was permitted. No qmp-raw-io.jsonl exists because no QMP socket connection was made.
- The serial log is zero bytes. Native SQL point/range, invalid-request, recovery, manifest, and database-access results are **not run**, not failed product results.
- qac-owned-group-shutdown-signal-retry.json records a successful QAC-owned CTRL_BREAK_EVENT after fresh owner/child revalidation. It records qmpCommandSent=false.
- final-cleanup-and-integrity-v2.json records the final status/list/queue/inspect outputs, reservation Released with reason Managed child exited., both owned PIDs absent, all 20 artifact pins unchanged, and the exact final ISO hash. QAC reported no remaining QEMU, session, queued, or blocked resources.

## Evidence locations

Attempt evidence: E:\CodexArtifacts\NDB1R4P11R3AZR1\attempt-20261113-acceptance

Key records include:

- candidate-integrity-preflight.json, launch-readiness.json, and launch-attempt.json
- qac-pre-admission-census.json, qac-admission.json, and QAC run stdout/stderr
- qmp-preconnect-ownership.json, qmp-execution-error.json, and Windows process/listener observations
- serial.log, serial-analysis.json, and acceptance-result.json
- Initial and retry QAC-owned cleanup records, final-cleanup-and-integrity-v2.json, and the final QAC census output files

The runner copy is E:\CodexArtifacts\NDB1R4P11R3AZR1\r3azr1_acceptance_runner.py. The runner changes were confined to that evidence copy to use the qualified R3AY endpoint/evidence API and correlated walker request IDs; no source changes were made to SQL, Server, or QAC.

## Completion

**Outcome C — NATIVE_SQL_R3AZR1_GUEST_PENDING.** One QEMU process was launched under QAC in the required paused state, but no guest CPU execution occurred because Gate 5 ownership could not be established within the helper’s freshness limit. The guest was cleaned up using the authorized QAC-owned fallback. The mandatory QMP-controlled shutdown gate remains failed, and no native SQL acceptance is claimed.
