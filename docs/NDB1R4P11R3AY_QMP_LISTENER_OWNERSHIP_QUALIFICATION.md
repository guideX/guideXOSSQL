# NDB1R4P11R3AY — QMP Listener Ownership and Acceptance Harness Qualification

**Outcome:** NATIVE_SQL_QMP_OWNERSHIP_HOSTED_VERIFIED (Outcome A)
**Phase:** Offline tooling qualification
**Date:** 2026-11-12 (America/Los_Angeles)
**Guest executions:** 0
**QAC reservation requests:** 0

## Result

The R3AX-R1 failure was at the host listener verifier, after QAC had verified the one acceptance reservation and its managed QEMU child. The R3AX-R1 launcher requested a TCP QMP listener at 127.0.0.1:51288; the QAC record consistently identified reservation 4fdb8b5c6cf54cb8a64413d004c409aa, owner PID 25408, and managed QEMU PID 28664.

The first failed operation was the inherited R3AV listener(port, pid) probe. It ran netstat -ano -p tcp for up to five seconds and returned False when no parsed row matched the port and PID. The R3AX-R1 record contains that Boolean, but not the command's exit code, stdout, stderr, or parsed rows. Therefore the original OS error, if any, was not preserved. The records cannot establish whether the listener was absent, not ready, attributed to another PID, or omitted by the probe. The runner turned an inconclusive observation into a definitive ownership failure.

This is a confirmed verifier and evidence defect. A narrower host cause for R3AX-R1 remains unknown. R3AV exhibited the same netstat failure; a later fresh QAC/CIM/TCP check verified its QEMU PID 22660 as the owner of 127.0.0.1:57322, and the preserved QMP page-table walk passed. That confirms a safe owner-attribution route on this host without proving what the R3AX-R1 netstat call returned.

R3AY adds an isolated SQL-repository preflight using the demonstrated Windows process and TCP APIs. It verifies the current QAC relationship, child PID/parent/executable/start time, actual child command line and endpoint, and OS listener PID before it allows QMP connection or shutdown. Inspection errors and stale observations remain unknown and fail closed. The helper recognizes Windows' observed bound-listener representation where State is null and the remote endpoint is 0.0.0.0:0; it also preserves the raw PowerShell result for diagnosis.

No QMP command was sent to R3AX-R1 or to an existing guest. R3AX-R1 remains Outcome B with Gate 5 and Gate 20 failed. R3AY claims no guest acceptance.

## Confirmed evidence and limits

| Question | Confirmed evidence | Limit |
|---|---|---|
| Was QEMU configured for QMP? | Both launch-attempt.json argument arrays contain -qmp tcp:127.0.0.1:51288,server=on,wait=off. The passed QAC command contains the same pair. | The R3AX-R1 artifact does not contain a Win32 process command-line capture for PID 28664, so the actual child's consumed command line was not independently observed. |
| What transport/address family was requested? | TCP, explicit IPv4 loopback 127.0.0.1, port 51288. | No actual R3AX-R1 listener row was retained; actual bind family/address is unknown. |
| Did QAC and the host runner agree on owner and child? | Reservation ID, Running state, verified owner PID 25408, managed child PID 28664, executable path, and lifecycle agree across the R3AX-R1 admission and reconciliation records. | The R3AX-R1 runner did not record the current child's command line or a listener-to-PID row. |
| Was the R3AX-R1 listener missing, delayed, foreign, or owned? | Only that the netstat helper returned no matching parsed row within its polling window. | The raw probe result was not saved, so those alternatives cannot be distinguished. |
| Was the QMP parser reached? | No. No QMP JSONL exists in the R3AX-R1 attempt directory, and the runner stopped before connecting. | No R3AX-R1 live registers, QMP walk, or controlled shutdown were captured. |
| Did the same-host fallback work in R3AV? | Preserved qmp-recovery2-ownership.json identifies PID 22660, endpoint 127.0.0.1:57322, and one TCP row with OwningProcess=22660. runtime-page-table-walk-recovery.json records pass=true. | This is evidence for the earlier R3AV process only, not R3AX-R1. |

The first actual failed operation and the original error are recorded as follows:

- **Operation:** listener-to-process attribution in the inherited listener() function, using netstat -ano -p tcp.
- **Runner result:** no parsed TCP LISTENING row matched the requested port and QEMU PID during the bounded loop; the function returned False.
- **Original OS error:** **not recorded**. The helper did not retain the command's return code, stdout, stderr, or rows. No more specific error can be recovered from the preserved artifacts.
- **Consequently:** do not attribute the failure to QEMU, Windows networking, QAC, a race, or an unexpected listener without new evidence.

R3AX-R1 used the R3AV runner by importing its preserved source file. Its QMP listener check was not replaced in R3AX-R1. The QAC-managed child correlation succeeded before that check. This was not a kernel mapping or CR0/CR4 parsing failure.

## Repository and candidate identity

| Repository | State inspected for R3AY | Disposition |
|---|---|---|
| SQL | main, starting HEAD bc97e7926f7a92e89f782fa38f06cd25ab9c820c; clean before R3AY changes. | Only this isolated helper, focused Python tests, and this document are accepted for the local commit. |
| Server integration worktree | D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH, HEAD d40557a1710f8c21fe34fdf5be8e9c12f7f592dd. Existing untracked out/, kernel/out/, guideXOSBootLoader/out/, and scripts/__pycache__ entries were present and preserved. | No Server source or worktree content changed. The existing scripts\r3au_qmp_evidence.py hash remains fa58d785056528405dae378544fb7faf90fe8b6efac54ee80783c301fd165d28. |
| Original Server checkout | HEAD eb61ca10b48c0e96e1a04a302e82d6c52b828f41; existing untracked assets/Backgrounds/troncity.png. | Preserved without edits. |
| QAC | D:\dev\guideXOS_QAC, HEAD 1ea193258690f9e7b39390b101a117166ef8bbdd, clean. | No QAC infrastructure changes and no QAC admission request. |

The frozen R3AW ISO remains E:\CodexArtifacts\NDB1R4P11R3AW\candidate\guideXOS-NDB1R4P11R3AW-amd64.iso, **100,730,880 bytes**, SHA-256 **60ffbbbd16ff7f3c66a99ebf1f48389f807811afed62fc14e9d3e6c11d93a207**. It was read and hashed only; it was not modified, rebuilt, or repackaged.

R3AY's separate offline evidence directory is E:\CodexArtifacts\NDB1R4P11R3AY. It contains the final focused test log and qualification summary. R3AV, R3AX-R1, candidate, QAC, and Server evidence were left in place.

## Source changes

- tools/r3ay_qmp_preflight.py adds a phase-owned, fail-closed Windows ownership preflight and small QMP client.
  - Parses exactly one explicit TCP loopback -qmp configuration and validates the requested port, server=on, wait=off, and transport.
  - Requires a fresh Running QAC acceptance reservation, exact reservation ID, verified owner PID, managed child PID, executable identity, and matching QAC start-time identity.
  - Captures Win32_Process PID, parent PID, executable, creation time, and the actual child command line through Get-CimInstance.
  - Captures TCP listener rows through Get-NetTCPConnection for the requested port and child PID, preserving PowerShell exit code, stdout, stderr, and command.
  - Distinguishes listener-not-yet-ready attempts, final listener-missing, ownership-unknown, foreign listener, unexpected listener, wrong port, invalid config, and verified ownership.
  - Requires the OS listener PID to equal the QAC child PID, exact loopback endpoint and port, exact command-line endpoint, expected executable, parent owner PID, and process start identity.
  - Requires QAC snapshots and listener evidence to be fresh (two-second maximum); the bounded poll can receive a fresh QAC snapshot provider. Reused PID/port observations cannot authorize a connection.
  - After connecting, accepts only the passive greeting, then requires a fresh OS/QAC snapshot that attributes the accepted TCP peer tuple to the same child before sending capabilities or another command.
  - Rejects QMP connection, cont, and quit unless the evidence is verified and fresh for the same reservation, PID, and endpoint. The command surface is limited to negotiation, status, and read-only register/memory inspection.
  - Implements CRLF JSON framing, partial reads, strict UTF-8 without BOM, QMP greeting/capabilities, response-ID correlation, running-state validation, exact-width 8/16-digit register parsing, full 64-bit integer precision, cr3_active extraction, CR0.WP inspection, and CR4.LA57/four-level checks.
- tests/test_r3ay_qmp_preflight.py provides the focused offline qualification.
- No SQL production code, Server code, QAC code, frozen candidate, or prior acceptance report changed.

R3AV's preserved transcript validation reports 7 initial frames and 384 recovery frames with CRLF framing, no BOM, and no uncorrelated responses. Its raw QMP response includes CR0=80010033 and CR4=00000668. The Server parser's strict register parser requires 16 digits and therefore rejects those 8-digit forms; R3AX-R1 had a separate 1–16 digit raw-control recovery path, but was never reached. R3AY's owned helper accepts the source-supported 8- or 16-digit representation. The four-level page-table walker itself remains the existing Server implementation and was not rewritten.

## Offline qualification

Command:

    python -B -m unittest discover -s tests -p 'test_r3ay_qmp_preflight.py' -v

**Result: 13 tests passed.** The Windows integration test starts one local synthetic QMP server subprocess (not QEMU), observes its real TCP listener-to-PID association through Windows APIs, correlates a synthetic reservation to that process, and confirms the accepted TCP peer tuple belongs to that process before negotiating QMP. It observes a paused status, rechecks ownership before cont, reads a synthetic register response, rechecks ownership, and sends quit to that verified local process. A greeting alone is never accepted as ownership proof.

Coverage includes:

- Correct owned listener; missing listener; delayed readiness; foreign owner; unexpected listener; wrong port; wrong child PID.
- Stale/released reservation; PID reuse/start mismatch; wrong executable; command-line endpoint mismatch; OS attribution failure.
- Accepted TCP peer attribution to the same child; rejection when the peer tuple belongs to another process.
- IPv4, IPv6, and IPv4-mapped address representation; rejected wildcard and unsupported transport.
- QMP greeting, capabilities, status, partial reads, CRLF, UTF-8 without BOM, response correlation, invalid/truncated frames.
- Eight- and sixteen-digit registers, full 64-bit precision, missing/duplicate/invalid cr3_active, CR0.WP, and unsupported CR4.LA57.
- Refusal to connect or send quit with unverified, changed, or stale ownership.

The actual OS ownership check is positive only in the synthetic subprocess test model. This run did not connect to a QAC session or start a QEMU guest.

## R3AZ acceptance preflight and 20-gate contract

QEMU's installed -help output confirms -S freezes the CPU at startup. The R3AZ isolated runner should add -S so it can verify the configured QMP endpoint while the guest CPU is paused. This command was a help query only; no QEMU guest was launched. If QAC or the runner cannot preserve this paused-start behavior, stop before guest execution and resolve that preflight path first.

1. Before QAC admission, rehash the frozen R3AW ISO and validate the unique QMP option, TCP loopback endpoint, requested port, read-only media/firmware, fresh evidence directory, and expected executable identities.
2. Submit at most one normal exclusive QAC acceptance request. Do not bypass QAC, reuse an old reservation, or start a replacement guest.
3. Capture a fresh QAC snapshot and verify the exact reservation, owner, managed child PID, executable, and start time. Include observedAtUtc from the completed QAC snapshot.
4. While QEMU is paused with -S, use a fresh QAC snapshot provider and the bounded ownership poll. Preserve each PowerShell command, result, and error.
5. If configuration is invalid, do not request QAC. If a listener is not yet ready, continue only within the bound. If the listener remains missing, is foreign, unexpected, wrong-port, or ownership is unknown, send no QMP command and do not run the guest.
6. Once ownership is verified, connect to the exact endpoint and read the passive QMP greeting. Recheck fresh QAC/OS ownership and correlate the accepted TCP peer tuple to the same child before negotiating capabilities. Require a correlated non-running query-status. Refresh QAC and OS ownership, issue cont only for the same verified child, and require a correlated running-state response before evaluating guest serial output.
7. Continue the original 20 gates without relaxation:

| Gate | R3AZ acceptance requirement |
|---:|---|
| 1 | Frozen R3AW ISO, manifest, embedded ESP identity, and all pinned artifact hashes match. |
| 2 | Fresh QAC census and exclusive acceptance admission pass. |
| 3 | Reservation owner, exact managed QEMU child, executable, and process start identity correlate. |
| 4 | Approved UEFI firmware, actual launch configuration, read-only pflash/ISO, and no writable guest storage. |
| 5 | Verified owned QMP listener; correlated greeting, capabilities, and running-state response. |
| 6 | Live QMP CR3 matches guest cr3_active; CR0.WP is set; CR4.LA57 is clear. |
| 7 | BootInfo v3 and page-table sidecar integrity pass. |
| 8 | All 15,104 RAM-disk page bases and the final-byte probe pass. |
| 9 | Independent live page-table walks include the former 2 MiB boundary and match the sidecar frame set. |
| 10 | Read-only supervisor mappings, physical progression, CR0.WP, and kernel non-overlap pass. |
| 11 | Boot-sector validation, FAT32 classification, read-only mount, and /system registration pass. |
| 12 | Production VFS opens, reads all 12,937 bytes of FIXTURE.MAN, observes EOF, and closes successfully. |
| 13 | Production VFS access to MULTI.GDB passes. |
| 14 | Ordered 73-locator point lookup passes. |
| 15 | Ordered 32-locator range scan passes. |
| 16 | Invalid request is rejected without changing caller output. |
| 17 | Valid SQL request recovers successfully after rejection. |
| 18 | Full serial scan has no mandatory failure, panic, page fault, protection fault, triple fault, or memory fault. |
| 19 | All required final mapping, package, RAM-disk, manifest, and SQL PASS markers appear in order. |
| 20 | Recheck fresh QAC and OS ownership; send QMP quit only while the same child is still verified; verify QMP acknowledgement, child exit, reservation release, QAC cleanup, and final ISO hash. |

If QMP remains unavailable after QEMU starts, keep the CPU paused when possible, do not launch another guest, and use only the documented QAC-owned process-group interrupt fallback for safe cleanup. Record fallback cleanup separately; it does not satisfy Gate 20's QMP shutdown subgate. Preserve all 20 gate results and do not upgrade R3AX-R1 retroactively.

## Completion

Outcome A means the host-side verifier has a tested correction and an R3AZ procedure is prepared. It does not claim a new guest acceptance. The first R3AX-R1 operation failed at the netstat listener attribution predicate; its original OS error was not captured. Root cause of that specific missing row remains uncertain; the confirmed source defect was the boolean-only, non-diagnostic failure classification.

The QAC ownership checks, reservation lifecycle, no-second-guest rule, and QAC-controlled fallback remain intact. No QAC reservation was requested in R3AY.
