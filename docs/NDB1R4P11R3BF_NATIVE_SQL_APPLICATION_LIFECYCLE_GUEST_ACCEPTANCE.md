# NDB1R4P11R3BF Native SQL Application Lifecycle Guest Acceptance

**Outcome:** `NATIVE_SQL_APPLICATION_LAUNCH_BLOCKED` (Outcome D)

**Run date:** 2026-11-14 local / 2026-11-15 UTC  
**Guest launches:** 1 QAC-managed QEMU guest  
**SQL Read Demo selections/runs:** 0  
**First terminal gate:** guest Start-menu input could not be safely delivered and verified under the approved QAC/QMP ownership checks.

## Decision

The frozen candidate passed repository, manifest, embedded-content, ABI, QAC-admission, UEFI/boot, and package-discovery checks. The guest reached the guideXOS desktop and registered `SQL Read Demo` in the Native ELF App Model. I could not verify opening Start or selecting the package. The host UI control did not open the guest Start menu; the approved QMP path then stopped when its required QAC ownership snapshot was older than two seconds. No guest pointer/key event was sent through QMP, no package was selected, and `gx_main` did not run.

This is a launch-input block, not a demonstrated SQL, loader, or ABI defect. The single authorized guest launch was consumed. No second guest, source repair, SDK change, rebuild, or ISO repackage occurred.

## Repository and change boundary

| Repository | Branch | Starting HEAD | Operational ending HEAD before report commit | State at preflight |
|---|---|---|---|---|
| SQL | `main` | `03919326a2e232298e94feb482342109eff32731` | `03919326a2e232298e94feb482342109eff32731` | Existing `tests/__pycache__/` and `tools/__pycache__/` untracked; preserved |
| Server | `codex/ndb1r4p11rah-sql-read-integration` | `af0e1706d0ebac475a2b205938b14fa6471dc8af` | `af0e1706d0ebac475a2b205938b14fa6471dc8af` | Existing generated outputs and caches untracked; preserved |
| QAC | `main` | `1ea193258690f9e7b39390b101a117166ef8bbdd` | `1ea193258690f9e7b39390b101a117166ef8bbdd` | Clean |

The operational ending heads above are the repository heads at guest-run closeout, before this documentation commit. The final SQL `main` HEAD includes the report commit and is recorded in the closeout response.

No production source, SDK application, candidate ISO, or QAC source was changed. The only repository change is this acceptance report, committed locally on SQL `main`; the commit hash is recorded in the closeout response. No push or merge was performed. Host-side evidence and QMP-control records are outside the repositories under `E:\CodexArtifacts\NDB1R4P11R3BF`.

The pre-report SQL HEAD remained `03919326a2e232298e94feb482342109eff32731`. The report commit is the only intended SQL-repository delta. Unrelated untracked files were not staged.

## Frozen candidate and application integrity

Candidate ISO:

- Path: `E:\CodexArtifacts\NDB1R4P11R3BE\candidate-final\guideXOS-NDB1R4P11R3BE-amd64.iso`
- Length: 100,730,880 bytes
- SHA-256: `10d4a361c612c496ac2f39cb2cc9f272cd61d7166920c7458ad2bb6e8982e197` — matches the required hash before and after the guest run.
- UEFI El Torito boot entry and no-emulation ESP were present.

Both manifests parsed. `candidate-manifest.json` contained 38 path/size/hash record occurrences (37 candidate artifact references plus its report-document reference); all 38 verified. `native-sql-package-manifest.json` contained 14 references; all 14 verified. There were no hash or size mismatches.

The ISO’s actual embedded ESP and RAM disk were extracted read-only and compared with the manifest evidence. The embedded ESP was 100,663,296 bytes; the qualification kernel ELF was 2,785,000 bytes with SHA-256 `6e77de41b3278178483040afe9e6a15b82bb2ccdf2df4de22f863f6937530802`. The embedded RAM disk was 61,865,984 bytes with SHA-256 `902387311c6fb014f83fcb4f02541834f2fe6a6d2da5363748d99995aee7c237`. Its FAT32 geometry validated and all 89 files matched the packaging report.

| Packaged item | Bytes | SHA-256 |
|---|---:|---|
| UEFI loader `/EFI/BOOT/BOOTX64.EFI` | 83,968 | `f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77` |
| App registration `/Apps/SQLReadDemo/app.json` | 756 | `0951fccc0cb92ebe847dcaf17da5daa7eea53cecf955b33ce506678b1355ce68` |
| SQL Read Demo `/Apps/SQLReadDemo/bin/amd64/sqlread.elf` | 7,312 | `96812b627bda58bc6e176271c94fa4fb92d352fa07fae1a301bb41d29584c13e` |
| Read-only SQL fixture `/Apps/R3AI/MULTI.GDB` | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted SQL manifest `/Apps/R3AI/FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |

The SDK ELF and embedded ELF hashes agree. The ELF is ELF64, little-endian AMD64 `ET_EXEC`; it defines `gx_main` and has zero undefined symbols. The manifest registers `com.guidexos.sqlread`, display name `SQL Read Demo`, `NativeElf`, `amd64`, `gx_main`, `guidexos-c-abi-v1`, and `database.read`.

## Public ABI and baseline

Static/source inspection confirmed the existing R3BC ABI contract: 24-byte application context; 288-byte `gx_host_calls`; SQL callback slots at offsets 256, 264, 272, and 280; existing prefix compatibility; expected callback order and signatures for `OpenReadOnlyDatabase`, `CloseDatabase`, `LookupIndex`, and `ScanIndexRange`. No ABI was introduced or changed.

R3BE’s hosted baseline remains `NATIVE_SQL_APP_LIFECYCLE_HOSTED_VERIFIED` (50/50 hosted lifecycle checks and supporting regressions, per its report). It was not rerun or counted as guest evidence in this phase.

## QAC admission, launch, and guest boot

The fresh pre-admission QAC census at `2026-11-15T04:17:45.9800138Z` reported QAC online with zero QEMU processes, managed sessions, queued reservations, and blocked reservations. One acceptance reservation was granted:

- Reservation: `1ab43779720c484abb3fc2a7dfc9d9f5`
- Project/mode: `guideXOSServer` / `acceptance`
- QAC owner PID: `27392` (verified)
- QEMU child PID: `12384`, direct QAC child, approved executable path and exact command line verified
- QMP endpoint: `127.0.0.1:62528`
- Approved QEMU SHA-256: `a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02`
- OVMF SHA-256: `2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792`

QEMU started paused with `-S`. Pflash and ISO were read-only, no writable guest storage was attached, networking was disabled, and serial/debug output was directed into the R3BF attempt directory. Before `cont`, QMP ownership was verified against the QAC reservation, owner/child PIDs, executable, command line, listener, and accepted peer; the snapshot was within the required two-second limit. QMP reported `prelaunch`/`running:false`, then `cont`, then `running:true`.

UEFI, BootInfo v3, the FAT32 RAM disk, desktop initialization, and Native ELF package discovery completed. Serial output included:

- `[NATIVE-ELF] App Model discovery root=/Apps source=bare-metal-VFS`
- Package `SQLReadDemo`, id `com.guidexos.sqlread`, display name `SQL Read Demo`
- `NativeElf`, `amd64`, `gx_main`, ABI `guidexos-c-abi-v1`, executable bytes `0x1C90`

The guest screenshot (`guest-current-qmp-view.png`) shows the 1280×800 desktop and closed Start menu. QEMU’s accessibility tree exposed the host window only. CUA clicks at the observed Start control did not visibly open the menu, and the screen/coordinate mapping could not be made reliable.

The serial log also contains the candidate’s pre-existing kernel boot diagnostic `SQL_NATIVE_R3AQ`: the fixture opened read-only; its kernel diagnostic reported 73 point locators, 32 range locators, invalid-key rejection with unchanged output, and recovery. These are boot-time kernel diagnostic results, **not** SQL Read Demo execution and not evidence of public application callback dispatch. They do not satisfy R3BF’s application acceptance gates.

## Input stop and exact frontier

The existing Server QMP display-input procedure uses `input-send-event` with PS/2-relative movement. I prepared an evidence-only QMP helper that called the same `require_qac_owned_guest` relationship validator and checked the two-second freshness limit before each command. The helper and its backups are in the attempt evidence directory; no production file was edited.

The first capability probe stopped before QMP connect when the ownership snapshot measured 2.040 seconds against the 2.000-second limit. After a host-helper-only change to parallelize read-only QAC census calls, a second probe connected and received a successful `qmp_capabilities` response. Its next freshness check measured 3.618 seconds and stopped before `query-status`. No `input-send-event` command was sent. The raw QMP log has no guest mouse/key event. The helper’s QMP acknowledgement was not treated as evidence that guest input reached the desktop.

At this exact frontier, the guest was booted and the package was discovered, but Start-menu visibility/selection was not verified. The acceptance stopped; the app was never selected, loaded, or invoked.

## Acceptance gates

| Gate | Status | Evidence / disposition |
|---|---|---|
| Repository preflight and preservation | PASS | Expected repository identities/HEADs confirmed; unrelated caches and generated outputs preserved. |
| Frozen candidate and embedded contents | PASS | Both manifests, ISO, ESP, loader, kernel, RAM disk, package, fixture, and trusted manifest verified. |
| R3BE hosted baseline | PASS (prior phase only) | Referenced as historical baseline; not rerun or substituted for guest proof. |
| Public host-call ABI | PASS (static/source) | Sizes, offsets, order, signatures, and sample ELF contract agree. |
| Authentic Start-menu input | FAIL | Start did not open through visible host UI control; QMP input was blocked by stale ownership evidence before any guest event. |
| QAC admission and one guest launch | PASS | One acceptance reservation and one QEMU child only. |
| UEFI/kernel/BootInfo/Desktop and package discovery | PASS | Serial confirms boot and `SQLReadDemo` registration. Start-menu entry visibility remains UNVERIFIED. |
| Native ELF load and `gx_main` execution | NOT REACHED | No app selection or launch request was confirmed. |
| Application SQL callback dispatch | NOT REACHED | Zero `SQL_APP_API`, `SQL_APP_LIFECYCLE`, `OWNER_SESSION_RELEASE`, or `SQL_EXECUTE_READ` markers. |
| Point/range/no-match/invalid/recovery app results | NOT REACHED | Kernel boot diagnostics are excluded; no app-owned buffers/results were observed. |
| Explicit close and stale-handle rejection | NOT REACHED | No application session was opened. |
| Return with an outstanding session and production detach cleanup | NOT REACHED | No `gx_main` return, detach, owner cleanup, or application completion was observed. |
| Correlated application lifecycle trace | NOT REACHED | No app lifecycle sequence exists to correlate. |
| Runtime security/resource checks | NOT REACHED | No app runtime occurred; no additional isolation or privilege claim is made. |
| R3BB independent QMP page-table-walker Gate 9 | UNVERIFIED | This phase did not run or close that independent witness. |
| Controlled shutdown and QAC reconciliation | PASS | QMP quit succeeded; QEMU exited 0; QAC released the reservation. Final QAC census is clear. |
| Final frozen ISO integrity | PASS | Final SHA-256 still matches the required R3BE hash. |

## Faults, shutdown, and cleanup

No Native ELF application fault occurred because the application did not run. No kernel panic, exception, or unexpected QEMU guest error was observed before shutdown; `qemu-debug.log` is empty. Serial contains VFS file-not-found messages for desktop preference paths and standard-folder fallback notices; these are recorded and were not treated as application SQL failures.

After stopping at the input gate, I revalidated ownership and sent QMP `quit` to the same QAC-owned process. QAC reported QEMU PID `12384` exited with code 0 and released reservation `1ab43779720c484abb3fc2a7dfc9d9f5`. The final census at `2026-11-15T04:44:57.977261Z` reported 0 QEMU processes, 0 managed sessions, 0 queued and blocked reservations; reservation state `Released`. The process/port check found no PID `12384` and no listener or peer on port `62528`. The ISO’s final size and SHA-256 remained unchanged.

R3BB Gate 9 remains `UNVERIFIED`. The SQL and ramdisk boot diagnostics in this serial log are not the independent QMP page-table-walker witness and do not retroactively close it. The current Native ELF model also continues to provide no claim of user/kernel memory isolation, signed application identity, strong malicious-application sandboxing, or general per-user privilege separation. The supported SQL surface remains read-only against the approved immutable fixture with the existing 16-session, 73-result, K64 point, and K32 range limits.

## Evidence index

All paths below are under `E:\CodexArtifacts\NDB1R4P11R3BF` unless stated otherwise.

- `preflight-20261114\repository-preflight.json`
- `preflight-20261114\candidate-manifest-verification.json`
- `preflight-20261114\candidate-embedded-verification.json`
- `preflight-20261114\candidate-embedded-ramdisk-files.json`
- `attempt-20261114-single-launch\launch-readiness.json`
- `attempt-20261114-single-launch\qac-pre-admission-census.json`
- `attempt-20261114-single-launch\qac-qmp-before-connect.json`
- `attempt-20261114-single-launch\qmp-resume.json`
- `attempt-20261114-single-launch\input-delivery-blocker.json`
- `attempt-20261114-single-launch\guest-marker-summary.json`
- `attempt-20261114-single-launch\serial.log` (complete guest serial output)
- `attempt-20261114-single-launch\qemu-debug.log`
- `attempt-20261114-single-launch\qmp-raw-io.jsonl`
- `attempt-20261114-single-launch\guest-current-qmp-view.png` (QMP desktop capture converted from its PPM raster for viewing)
- `attempt-20261114-single-launch\qmp-shutdown.json`
- `attempt-20261114-single-launch\qac-final-census.json`
- `attempt-20261114-single-launch\guest-closeout-process.json`

## Recommended next bounded milestone

First qualify one approved QAC-compatible guest-input route end-to-end: fresh ownership snapshots under two seconds for every QMP command, live input capability/schema capture, and a screenshot-confirmed Start → All Programs menu selection in the same owned guest. Then, in a separately authorized one-guest acceptance phase, exercise SQL Read Demo through `gx_main`, correlate the public callback markers and exact ordered results, explicitly close/stale-check one handle, and verify production detach reclaims a deliberately outstanding second session. Keep R3BB Gate 9 as a separate independent follow-up.

