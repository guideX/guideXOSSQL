# NDB1R4P11R3AN-R1 QAC Preflight

**Outcome:** C — NATIVE_RAMDISK_MAPPING_GUEST_PENDING  
**Phase:** NATIVE_RAMDISK_MAPPING_GUEST_QUALIFICATION_RETRY  
**Preflight time:** 2026-11-07 15:16:24 UTC (07:16 PST)  
**Guest boots:** 0

## Repository state

| Repository | Verified identity |
|---|---|
| SQL | main, starting HEAD 2fadfa7077d4e2a406090f40ba930c9bbeb84ebd; clean before this record |
| Isolated Server integration worktree | codex/ndb1r4p11rah-sql-read-integration, HEAD 5af7597497ddead12e63ccad87160ffaf5077ee5; existing untracked output directories were preserved |
| R3AN manifest Server source base | 83e5438cd883cdd1158761f99b0aee1a3943452e; ancestor of the R3AN diagnostic source commit above |
| Dedicated original Server baseline worktree | D:\dev\guideXOSServer_SQL_READ_BASELINE_NDB1R4P11RAH, HEAD f29850c93bbac0a0763a31e806d2816310156575, clean |
| Separate D:\dev\guideXOSServer checkout | HEAD 47a5c19c1608b3ea91fbc5706b53ea2397f85322, with unrelated local QAC script/test edits and an untracked background image; not used or modified |

The SQL candidate source commit in the manifest is 33f989e3525f130afc3740b8e2368dc836132064, an ancestor of the verified SQL HEAD. Changes since that source commit are documentation-only; SQL production files were not changed. The manifest records Server base 83e5438c; the R3AN diagnostic implementation is committed in Server 5af75974, matching the expected integration worktree HEAD. The frozen binaries were hash-checked below.

## Frozen candidate and fixture identity

Manifest: D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3an\boot-candidate\r3an-boot-candidate-manifest.json  
Manifest SHA-256: fdd936d226889a8a38e78ca32ebfc6810615ce0fb6886d4e0ef2fcba56426527

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| R3AN ISO | 100,730,880 | 95c74d7d36deaad08fd712961cad42c283d5746e78b9bd37d552b439c5a0183a |
| ESP image | 100,663,296 | 290b5c4bdaf5adb95c299ebfdce1289bb6a783c6073def42aed24b8c6647fca6 |
| Diagnostic kernel ELF | 2,757,608 | 7c8ce60f88e3a5eb32297ef61d5d3a03f96ad3cd594d7b07a4c182d79d876c17 |
| Diagnostic kernel PE | 2,866,763 | cf1193728c04fd45b5302568094cba9a5a233eafbb5f2eb36d74694617a55573 |
| Instrumented UEFI loader | 73,216 | 42cdba007939dad5c3dc8f0d328c3bcf5b817361c22a34605ae9d8e7b64fec7f |
| Embedded RAM disk | 61,865,984 | 22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78 |
| Trusted MULTI.GDB fixture | 250,880 | cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57 |
| Trusted FIXTURE.MAN | 12,937 | 43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3 |

The RAM disk was read as FAT32 and the embedded Apps/R3AI/MULTI.GDB and Apps/R3AI/FIXTURE.MAN were extracted. Both matched their trusted source files byte-for-byte and matched the full hashes above. The expected RAM disk first 16 bytes are EB 58 90 47 55 49 44 45 58 4F 53 00 02 10 20 00.

The manifest’s complete SQL expected locator sequences were compared in order with the committed diagnostic arrays: 73 point locators and 32 range locators. This was source/fixture verification only; no guest SQL query ran. The expected mapping markers are MAP-A, MAP-B, MAP-D, and MAP-E. MAP-C is unavailable by source ordering.

QEMU and OVMF runner inputs were present and hash-checked: C:\Program Files\qemu\qemu-system-x86_64.exe SHA-256 a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02; OVMF.fd SHA-256 2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792. The QMP endpoint was not opened because no guest was admitted.

## QAC refusal evidence

QAC v2.0.0 was online (coordinator PID 5860). Its current census reported:

- One QEMU process, zero foreign processes.
- One managed session, zero queued reservations, zero blocked reservations.
- Active acceptance reservation 5b982bfa75f14424b899629b04cd871f, project guideXOSServer.
- Verified reservation owner PID 8824; managed direct child PID 19364, C:\Program Files\qemu\qemu-system-x86_64.exe, started 2026-11-07T15:15:07.7255785Z.
- No queued reservation was ahead of this active exclusive acceptance.

QAC’s FIFO acceptance lane was occupied by that verified reservation. The R3AN-R1 runner made no reservation request, launched no QEMU, did not inspect or control the other guest’s QMP endpoint, and did not alter its reservation or process. This was the required stop condition.

Raw status, reservation list, queue snapshot, and preflight details are preserved at E:\CodexArtifacts\NDB1R4P11R3ANR1\qac-preflight.json. Embedded fixture verification is at E:\CodexArtifacts\NDB1R4P11R3ANR1\embedded-fixture-verification.json; ordered source/manifest locator cross-check is at E:\CodexArtifacts\NDB1R4P11R3ANR1\manifest-locator-source-crosscheck.json. No R3AN serial or QMP guest trace exists.

## Cleanup and next step

There was no R3AN-owned reservation or guest to clean up. The active guideXOSServer acceptance reservation remains owned by its verified QAC client; it was left untouched. No repair, rebuild, repackaging, production-code edit, or regression run occurred.

The next justified step is to retry this same frozen R3AN candidate after QAC’s acceptance lane is free, using one fresh census and at most one QAC-managed boot.
