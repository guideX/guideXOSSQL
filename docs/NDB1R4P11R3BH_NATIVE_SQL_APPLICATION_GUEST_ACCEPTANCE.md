# NDB1R4P11R3BH — Native SQL Application Guest Acceptance

**Outcome B — NATIVE_SQL_APPLICATION_GUEST_PENDING**

**Launch count:** 0. No R3BH QEMU process was started and no R3BH QAC reservation was requested.

## Decision

The frozen candidate and its pinned contents passed read-only integrity checks. The first QAC census was clear, but a new authoritative census immediately before admission found an active exclusive acceptance session. Per the R3BH admission rule, this run stopped without requesting a competing reservation or launching QEMU. The active QAC session was left untouched.

The initial clear census at 2026-11-15 19:22:10 UTC was not used for admission. The later census at 19:31:19–19:31:21 UTC is the admission decision evidence. It reported one managed QEMU process and one running acceptance reservation, zero foreign QEMU processes, and no queued or blocked requests. The same session remained present in the 19:32:20 UTC stop census.

## Acceptance stage record

| Stage | Status | Evidence |
|---|---|---|
| SQL, Server, and QAC repository identity | PASS | SQL main at 112a4107225fda07a20833f1f725372f4fcf1421; Server integration worktree at af0e1706d0ebac475a2b205938b14fa6471dc8af; QAC main at 1ea193258690f9e7b39390b101a117166ef8bbdd. |
| Repository preservation | PASS | Existing SQL pycache directories and Server untracked build/output/cache paths were recorded and left untouched. QAC checkout was clean. |
| Candidate manifest pins | PASS | 52 manifest references resolved to 37 unique file identities. Every pinned byte length and SHA-256 matched; zero failures. |
| Frozen ISO and embedded payload | PASS | ISO is 100,730,880 bytes with SHA-256 10d4a361c612c496ac2f39cb2cc9f272cd61d7166920c7458ad2bb6e8982e197. Its UEFI El Torito ESP, loader, qualification kernel ELF, FAT32 RAM disk, all 89 packaged files, SQLReadDemo manifest and ELF, database fixture, and trusted manifest matched their pinned identities. |
| Application ELF structure | PASS | Exact packaged file is ELF64 AMD64; gx_main is defined and the ELF has no undefined symbols. |
| QEMU and OVMF identity | PASS | Installed QEMU and the approved Server OVMF file matched the identities recorded by R3BF. |
| Historical R3BG input tooling qualification | PASS | R3BG recorded 30 offline tests passing. These qualify host tooling only and are not guest input evidence; tests were not rerun in this acceptance-only phase. |
| Fresh QAC census | PASS | Coordinator was online. The later pre-admission status/list/queue census was captured immediately before the admission decision and is preserved in the evidence directory. |
| QAC admission availability | FAIL | QAC reported an active running acceptance reservation, ID 1252f51696a348d3bb3bcdf1df31294e, with managed child PID 24348 and verified owner PID 22912. No foreign QEMU process was reported. |
| R3BH exclusive reservation | NOT REACHED | No reservation request was made because the acceptance lane was occupied. |
| R3BH QEMU launch | NOT REACHED | Launch count is zero. The active QAC child was not ours and was not contacted or modified. |
| R3BH QMP ownership, freshness, and accepted-peer correlation | NOT REACHED | No R3BH endpoint or QMP connection existed. The two-second guard was not relaxed. |
| Live QMP schema and input-device checks | NOT REACHED | No QMP commands were sent. |
| Initial desktop screenshot and guest-visible input response | NOT REACHED | No guest screenshot or input action was produced. |
| meta_l, Start menu, All Programs, and SQL Read Demo selection | NOT REACHED | No key or mouse event reached a guest. |
| Production App Model dispatch and Native ELF loader | NOT REACHED | No application launch was attempted. |
| gx_main entry or return | NOT REACHED | No application runtime was created. |
| Production SQL callbacks or database results | NOT REACHED | Open, point lookup, range scan, no-match, invalid request, recovery, explicit close, and stale-handle behavior were not exercised in a guest. Hosted R3BE results remain hosted evidence only. |
| Outstanding-session reclamation after normal application return | NOT REACHED | No second session was opened in a guest; production teardown cleanup was not observed. |
| R3BH QMP shutdown and reservation cleanup | NOT REACHED | There was no R3BH guest or reservation to shut down or release. The active QAC acceptance session was left running and untouched. |
| Post-stop ISO recheck | PASS | The ISO remained 100,730,880 bytes with the required SHA-256 after the stop decision. |
| R3BB Gate 9 | UNVERIFIED | The independent QMP page-table-walker gate remains open, as required. |

## QAC admission evidence

At 19:31:19–19:31:21 UTC, QAC 2.0.0 (coordinator PID 17584) reported:

- QEMU processes: 1, with 0 foreign.
- Managed sessions: 1; queued: 0; blocked: 0.
- Reservations: 1.
- Running acceptance reservation: 1252f51696a348d3bb3bcdf1df31294e, project guideXOSServer.
- Managed QEMU child PID: 24348; QAC-verified owner PID: 22912.

The status, list, queue, timing, and proposed free QMP-port observations are in E:/CodexArtifacts/NDB1R4P11R3BH/qac-pre-admission-census.json. The final read-only census is in qac-stop-census.json. No inspect, QMP connection, cancellation, shutdown, or reservation request was issued against that existing session.

## Candidate verification

The two frozen manifests had SHA-256 values:

- candidate-manifest.json: 81569e7628790237e870985436df7df94690cdb50785629d3d3cae17fab9dd88.
- native-sql-package-manifest.json: 7e416e7c5e1c64adea065298a96a44eacbcb6a2ef2bb84f697c10b233f9e30c6.

The manifest check covered 37 unique identities with no mismatch. The embedded verification reopened the ISO’s FAT32 ESP and RAM disk read-only, verified the 89-file payload, confirmed the SQL application and database fixture identities, and checked the packaged ELF symbol table. The first attempt used the ambient Python, which did not have pyfatfs; that attempt stopped before making an integrity claim. The check then passed using the existing Server release-tools environment with the pinned pyfatfs 1.1.0. No package was installed and no source or candidate file was changed.

## Repository and evidence disposition

The SQL repository started on main at 112a4107225fda07a20833f1f725372f4fcf1421, four commits ahead of origin/main, with only the existing untracked tests/__pycache__/ and tools/__pycache__/ entries. The Server worktree started at af0e1706d0ebac475a2b205938b14fa6471dc8af with existing untracked outputs and caches. The QAC repository remained clean at 1ea193258690f9e7b39390b101a117166ef8bbdd. These unrelated checkout contents were preserved.

R3BF and R3BG evidence were not changed. The new R3BH evidence is in E:/CodexArtifacts/NDB1R4P11R3BH, including the repository and firmware preflight, manifest and embedded-content verification, QAC admission and stop censuses, post-stop ISO recheck, and the verification scripts and outputs.

No source, input helper, candidate, or package was modified. No rebuild, guest launch, push, or merge occurred.

## Next milestone

Retry R3BH only after a new authoritative QAC census reports the exclusive lane available. Repeat the pre-admission checks, then use one fresh exclusive reservation and at most one guest launch. The specific blocker for this attempt is the already active QAC acceptance reservation recorded above.
