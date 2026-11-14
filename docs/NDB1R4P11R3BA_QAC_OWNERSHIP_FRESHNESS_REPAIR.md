# NDB1R4P11R3BA — QAC Ownership Freshness Repair

**Outcome:** `NATIVE_SQL_QAC_FRESHNESS_HOSTED_VERIFIED` (Outcome A)  
**Phase:** `QAC_OWNERSHIP_SNAPSHOT_FRESHNESS_QUALIFICATION`  
**Date:** 2026-11-14 (America/Los_Angeles)  
**QEMU launches:** 0  
**QAC reservations:** 0  
**Guest executions:** 0

## Result

The R3AZ-R1 Gate 5 refusal was caused by the helper's ordering: it obtained a current QAC snapshot, then synchronously queried Windows for the QEMU process and listener, and only after that query checked the snapshot age. The QAC snapshot was current when acquired. It aged past the two-second limit while the OS observation ran.

`tools/r3ay_qmp_preflight.py` now performs bounded listener-readiness observations first, then obtains a fresh authoritative QAC snapshot and correlates that snapshot with the most recent Windows process/listener observation. A provider-backed check uses the initial QAC record only to pin the expected reservation and child identity during readiness; it takes a second, fresh record after readiness before it can return verified evidence. Reservation changes, release, stale timestamps, PID reuse, listener uncertainty, and TCP-peer mismatches still fail closed.

The existing two-second requirement is unchanged. Focused offline tests pass, including a Windows integration test using one local synthetic QMP process and the real Windows process and TCP-table APIs. R3AZ-R1 remains historical Outcome C. R3BB is ready for a separately authorized one-launch guest acceptance using the same frozen R3AW candidate.

## Starting repositories and preservation

All requested repository identities matched before work began.

| Repository | Starting HEAD | Starting state and disposition |
|---|---|---|
| SQL | `c7a020521f13f16bc3e594c7567e1e7c86d61bf0` | `main`, ahead of `origin/main` by 3. Pre-existing untracked `tests/__pycache__/` and `tools/__pycache__/` were preserved. |
| Server integration worktree | `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd` | Existing untracked build and acceptance outputs were preserved; no Server files were changed. |
| QAC | `1ea193258690f9e7b39390b101a117166ef8bbdd` | Clean; no QAC source or shared infrastructure was changed. |

The SQL helper and test changes are in local commit `f354b186cf78859501dbdd81411ee0ae3faa0c37`. The report is committed as a documentation-only follow-up. The final SQL HEAD and final working-tree status are recorded in `E:\CodexArtifacts\NDB1R4P11R3BA\final-state.json` and in the phase completion message. No push or merge was performed.

## R3AZ-R1 failure and exact snapshot sequence

R3AZ-R1 acquired reservation `8e96c080ff29446bb7dfd0e4c4470751`, with QAC owner PID `18620` and QEMU child PID `2588`. QEMU ran with `-S`; the configured endpoint was `127.0.0.1:61410`. Gate 5 rejected ownership evidence and no QMP connection or command, guest resume, guest execution, native SQL request, or QMP `quit` occurred. R3AZ-R1 remains Outcome C.

The runner did **not** pass its saved `qac-admission.json` as the ownership proof. It passed a live `admission_provider` to `poll_ownership`. Each provider call ran a current QAC census and set `observedAtUtc` to that census's completion time. The two preserved provider snapshots were:

| Snapshot | Started (UTC) | Completed/observed (UTC) | Census duration |
|---|---|---|---:|
| `owner-poll-01` | 2026-11-13 13:51:50.635672 | 2026-11-13 13:51:51.170790 | 0.535118 s |
| `owner-poll-02` | 2026-11-13 13:51:51.170790 | 2026-11-13 13:51:51.709786 | 0.538996 s |

The helper used the second snapshot, then called `observe_windows_process_and_listeners`, then validated ownership. The failure reported the snapshot as **6.263 seconds old**. Its two-second age limit was crossed at `2026-11-13T13:51:53.709786Z`. The preconnect record was written at `2026-11-13T05:51:57.972818-08:00`, 6.263032 seconds after the recorded QAC observation time, consistent with the helper's rounded age report.

That Windows observation succeeded: PowerShell exited 0 with empty stderr; it reported process PID `2588`, parent PID `18620`, the expected QEMU executable and live command line, and a `Listen` row for `127.0.0.1:61410` owned by PID `2588`. The records therefore show a correct listener observation and a late freshness check. The source order and timestamps place the age increase between the final provider result and the post-observation validator. The preserved records do not split that interval into CIM, PowerShell startup, and TCP-table command durations, so no narrower Windows subcomponent is blamed.

QAC CLI polls themselves took about 0.54 seconds each. QEMU had started roughly 31 seconds before these owner polls, so QEMU startup was not the six-second gap. The runner did not use a cached report for the helper call. There is no evidence of a QAC/helper clock-domain mismatch: the runner's `observedAtUtc` is stamped on the same host after its current census, the helper parses offsets as aware UTC, and helper age checks use the same host UTC clock. The helper uses a monotonic clock for elapsed age of the resulting evidence. The defect was lifecycle ordering, not the two-second calculation.

The raw sequence is preserved under `E:\CodexArtifacts\NDB1R4P11R3AZR1\attempt-20261113-acceptance`. The condensed measurements are in `E:\CodexArtifacts\NDB1R4P11R3BA\r3azr1-snapshot-timeline.json`.

R3AZ-R1's QAC cleanup record reports reservation `Released`, the managed child exited, the QAC owner and QEMU child absent, and no remaining QAC sessions, queues, or blocked reservations. A later read-only census in the preserved R3AZ-R1 directory completed at `2026-11-14T00:31:23.240264Z`; QAC 2.0.0 reported zero QEMU processes, reservations, managed sessions, queued reservations, and blocked reservations. That historical census is not reused as live evidence for any later guest operation.

## QAC 2.0.0 ownership contract reviewed

Reviewed the QAC repository at the requested clean HEAD, including `docs/QAC2-design.md`, `src/GuideXOS.QAC.Core/Reservations.cs`, `src/GuideXOS.QAC.Core/IpcContracts.cs`, and the CLI `inspect` implementation in `src/GuideXOS.QAC.Cli/Program.cs`.

QAC's documented reservation identity includes an opaque reservation ID, acceptance mode, owner process identity, owner liveness, and managed child PID, process start time, and executable. QAC uses process start identity because Windows may reuse PIDs. The read-only `inspect` command returns the current reservation record. The R3AZ runner's provider combines current status/list/queue/inspect output, checks the reservation and listed-child relationship, and timestamps the completed census on the host. R3BA did not change this QAC API or request a reservation.

## Corrected helper lifecycle and freshness semantics

`poll_ownership(provider, endpoint, ...)` now follows this sequence:

1. Call the provider once and require a fresh, exact `Running` acceptance reservation to establish the expected reservation ID, owner PID, QEMU child PID, executable, and QAC child start identity.
2. Perform bounded Windows process/listener readiness observations against that fixed identity. The first QAC record is an identity anchor only; it cannot authorize QMP after readiness.
3. When the listener appears, obtain one new authoritative QAC census. Require the same reservation ID, owner PID, child PID, executable, and child start identity. Do not refresh repeatedly until a mismatch disappears.
4. Correlate that fresh QAC record with the last Windows observation: parent/owner PID, process PID, executable, process start time, exact live QEMU command-line endpoint, and a single listener on the configured address and port.
5. Validate snapshot age after correlation and return evidence only if all checks pass. If the listener remains absent at the bound, make one final QAC check so a reservation release or identity change is reported explicitly. A missing listener still fails.
6. After connection, accept only the passive greeting before another fresh ownership check attributes the accepted TCP peer tuple to the same QAC child. Capabilities and subsequent QMP commands follow only after that check.
7. Immediately before `cont` and `quit`, the caller obtains new evidence through the provider-backed lifecycle. The client also requires the same reservation, owner, PID, executable, parent, process start identity, endpoint, and accepted TCP peer as the connection it already verified.

Freshness remains exactly **2.0 seconds maximum**. QAC observation age is calculated from an aware UTC timestamp; timestamps up to 0.5 seconds in the future retain the existing clock-skew tolerance. The local evidence age is measured with a monotonic clock and must be between 0 and 2.0 seconds. The exact 2.0-second boundary is accepted; older, invalid, missing, naive, or excessively future-dated timestamps are rejected. A stale dictionary supplied directly by a caller is rejected before any OS observation; the helper never rewrites its timestamp.

The helper continues to require the exact QAC reservation and managed child relationship, live process start identity, expected executable and parent, exact endpoint in the QEMU command line, listener-to-process attribution, and accepted TCP peer attribution. Foreign or additional listeners, wrong ports, process mismatch, PID reuse, QAC release, OS inspection error, and uncertain ownership do not authorize connection, `cont`, or `quit`. The existing QMP framing, negotiation, read-only command allowlist, and register parser remain in place.

## Focused offline qualification

Command:

```powershell
python -B -m unittest discover -s tests -p 'test_r3ay_qmp_preflight.py' -v
```

**Result: 20 tests passed.** The test log is `E:\CodexArtifacts\NDB1R4P11R3BA\offline-tests.log`.

The suite covers the R3AZ-R1 6.263-second rejection, exact two-second boundary behavior, a controlled 6.263-second delay followed by a newly acquired authoritative snapshot, stale caller input, UTC-offset normalization, invalid and missing timestamps, mismatched reservation and child PID, release during listener readiness, listener absence and delayed readiness, foreign and unexpected listeners, PID reuse, accepted TCP peer correlation, no QMP command when ownership is uncertain, and fresh evidence eligibility for the QAC-owned cleanup path. The Windows integration test launches one local synthetic QMP server subprocess (not QEMU), checks its real process/listener attribution and TCP peer tuple, negotiates QMP, revalidates before `cont`, and revalidates before `quit`.

QMP register parsing remains compatible with eight-digit values such as `CR0=80010033` and `CR4=00000668`, and sixteen-digit values. The test also retains CRLF framing, partial-read, UTF-8, and response-ID checks. No parser rewrite was made.

## Frozen R3AW candidate integrity

The candidate was read and hashed only; it was not rebuilt or repackaged.

| Artifact | Required and observed |
|---|---|
| ISO | `E:\CodexArtifacts\NDB1R4P11R3AW\candidate\guideXOS-NDB1R4P11R3AW-amd64.iso` |
| Size | 100,730,880 bytes |
| SHA-256 | `60ffbbbd16ff7f3c66a99ebf1f48389f807811afed62fc14e9d3e6c11d93a207` |
| Trusted manifest SHA-256 | `4419534a6fe848e8dbf498ca12aed3314b42f36f9f8662af0f93b52ea93555ae` |
| Manifest-pinned artifact records | 20; the R3AZ-R1 final integrity record reports all 20 unchanged. |

R3BA's current size and hash are saved in `E:\CodexArtifacts\NDB1R4P11R3BA\candidate-integrity.json`.

## R3BB separate guest acceptance procedure

R3BB is a new acceptance attempt. It requires its own explicit **one-launch authorization** and a fresh exclusive QAC acceptance reservation. It must not reuse or reopen R3AZ-R1's exhausted launch budget. R3BA made no QAC reservation and authorized no guest launch.

Before admission, verify the repositories and the frozen R3AW ISO/manifest again. Obtain a fresh read-only QAC status/list/queue census, then request one exclusive acceptance reservation from the current QAC CLI. Require the exact reservation ID, owner PID and verified owner liveness, and one registered direct QEMU child with matching PID, process start time, and executable. Launch that one child paused with `-S`, the exact approved read-only firmware/media configuration, and the prepared QMP endpoint.

The runner must construct a **live provider**, not pass a cached admission object. Each provider call must perform current QAC status/list/queue/inspect operations, preserve their raw outputs and times, correlate the exact reservation and child, and set `observedAtUtc` to the completed census time. For preconnect readiness, call `poll_ownership(provider, endpoint, expected_executable=..., timeout_seconds=5.0, interval_seconds=0.2)`. Use the returned evidence only if it is verified. Use the same provider-backed call for post-greeting revalidation, immediately before `cont`, and immediately before `quit`; do not implement another QMP client or manually take a QAC snapshot and then run a potentially slow OS observer.

`connect_verified` must receive a revalidation callback. It may receive the passive QMP greeting, but it must correlate a fresh QAC/process/listener snapshot and the accepted TCP peer tuple before sending `qmp_capabilities` or `query-status`. Confirm the guest is paused and verify the exact live QEMU command line against the approved launch arguments while still paused. If ownership becomes uncertain or the reservation is released at any point before `cont`, do not resume the guest. Stop the attempt safely; do not retry with another launch. Use only the separately documented QAC-owned cleanup path after a new exact owner/child/process/listener check.

R3BB must pass the existing R3AZ 20-gate contract:

1. Frozen candidate ISO, manifest, embedded ESP, and every pinned artifact match.
2. Fresh QAC census is clear and the exclusive acceptance reservation is granted.
3. Exact reservation owner, QEMU child PID, executable, parent, and process start identity match.
4. The single QEMU launch uses the exact approved paused `-S`, read-only firmware/media configuration.
5. QAC owner, live process command line, endpoint listener, accepted TCP peer, QMP negotiation, paused status, and verified `cont` transition all pass.
6. Live CR0/CR3/CR4 observations confirm CR0.WP and the four-level paging policy.
7. BootInfo v3 and version-2 sidecar integrity pass.
8. All 15,104 RAM-disk page bases and the separate final-byte probe pass.
9. Independent live page-table walks and the 2 MiB boundary translation pass.
10. Read-only supervisor mappings, CR0.WP, and kernel non-overlap pass.
11. FAT32 classification and read-only `/system` mount pass.
12. The complete 12,937-byte guest manifest read reaches EOF and closes successfully.
13. Production guest access to `MULTI.GDB` succeeds.
14. Production SQL point lookup returns the exact 73 ordered locators.
15. Production SQL range scan returns the exact 32 ordered locators.
16. Invalid request is rejected with unchanged output.
17. A valid recovery request succeeds after rejection.
18. Complete serial output contains no unresolved failure, panic, or fault.
19. Every mandatory final mapping, package, manifest, and SQL marker is present and PASS; there are no unresolved mandatory FAIL markers.
20. Fresh ownership is revalidated before QMP `quit`; the QAC-managed QEMU child and its owner process are confirmed exited, QAC reports the reservation released, the final census is clear, and the candidate hash still matches.

Preserve all QAC, OS process/listener, QMP, guest, SQL, serial, cleanup, and final-integrity evidence for R3BB in its own artifact directory. If any ownership check fails while paused, Gate 5 fails and gates 6–19 are not reached; do not claim guest or SQL failures for work that did not run. R3AZ-R1's Outcome C remains unchanged.

## Remaining limits and final state

The R3BA correction was qualified offline on Windows, including real Windows process and TCP-peer attribution for a synthetic local QMP process. It was not exercised against an active QAC-owned QEMU guest because R3BA authorizes no QAC reservation or guest execution. The historical current census is useful only as evidence that the earlier R3AZ-R1 cleanup completed; R3BB must obtain fresh evidence of its own.

The QAC CLI census is a sequence of current read-only commands rather than one atomic multi-command snapshot. The helper requires the reservation, owner, and child identities to agree across the resulting records and uses the host completion time for freshness. R3BB must preserve each command's raw output and timings so a future mismatch or delay can be diagnosed without weakening the contract.

Only `tools/r3ay_qmp_preflight.py`, `tests/test_r3ay_qmp_preflight.py`, and this report are accepted SQL repository changes. Server production code, QAC source/shared infrastructure, the R3AZ-R1 report and artifacts, and the R3AW candidate were not changed. The SQL implementation/test commit is `f354b186cf78859501dbdd81411ee0ae3faa0c37`; final report commit and final worktree state are recorded in the R3BA completion evidence.
