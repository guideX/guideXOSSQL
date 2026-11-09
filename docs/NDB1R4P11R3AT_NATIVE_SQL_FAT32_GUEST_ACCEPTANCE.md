# NDB1R4P11R3AT Native SQL FAT32 Guest Acceptance

**Outcome B - NATIVE_SQL_FAT32_GUEST_ACCEPTANCE_PARTIAL_OR_FAILED**

Run date: 2026-11-09 UTC. This was the single authorized QAC-managed guest execution for R3AT. The guest booted the frozen candidate, but acceptance stopped at the first mandatory full-extent mapping check. No repair or retry was performed.

## Scope and repository identity

This was an acceptance-only run. No SQL or Server production source, fixture, manifest, FAT32 package, kernel mapping logic, loader, candidate image, or phase marker was modified.

| Repository | Starting identity | Final identity |
|---|---|---|
| SQL | main, 5a5ea9367cea89b80a6e1044452fa7518292a02f | main, this report commit (see Git history) |
| Isolated Server integration worktree | codex/ndb1r4p11rah-sql-read-integration, 12df41bdc29d6608055d0dbbe16d997c661a219d | same HEAD; pre-existing untracked output directories preserved |
| QAC | main, 1ea193258690f9e7b39390b101a117166ef8bbdd | same HEAD |

The SQL change is documentation only. No test suite was rerun. Nothing was pushed or merged.

## Frozen candidate and offline verification

The launched ISO was exactly:

- Path: E:\CodexArtifacts\NDB1R4P11R3AS\candidate\guideXOS-NDB1R4P11R3AS-amd64.iso
- Length: 100,730,880 bytes
- SHA-256: 1388b65415bd7501e4bf3a3dd00d90d2799ea5ef822d46d18b600a82789050f9

Prelaunch and post-run verification matched the R3AS pins. Post-run artifact integrity evidence reports all checked artifacts unchanged.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| R3AS ESP | 100,663,296 | 0435b8961fc2bede92c1cf08b0ef61977922d2d861f8308423cf3d3c12f9c022 |
| UEFI loader | 81,920 | f5695f68cd6210f3cccc78598697e6b31187607e861f22da263766cbf6d5d50b |
| Kernel PE | 2,878,167 | 50621cdfb408ded4552d907f181d6c621f0cb410efff80aa12c779c41997dec5 |
| Kernel ELF | 2,764,480 | 64de3f113a900597e344cea86592c63c93fa9d7fd75b2642323834915d4ff839 |
| Embedded RAM disk | 61,865,984 | 50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c |
| SQL fixture | 250,880 | cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57 |
| Trusted SQL manifest | 12,937 | 43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3 |
| R3AS diagnostic expectation header | 3,895 | 17c2976f34a1f679bac22d8dd1c8c5609bab7eaf6b0789db17d3bfdd45c773ec |
| R3AS diagnostic expectation report | 1,762 | 09c2c50fbb12036404c566529dccb9ac3369c71ec14d8ac9ac5ff8b91aa5585d |
| R3AS candidate manifest | 21,294 | acfac3d835bf82e00b77e1dcfd3eb05017a8434617940433dd932513f69559ed |
| R3AS final verification report | 7,093 | 44dddff976ca809079d94602ea37d7b1ff53dacb414d8e091a075ec7e8302673 |

The ISO verifier confirmed a bootable UEFI El Torito platform (0xEF) and the expected embedded ESP hash. The ESP verifier reopened the FAT32 ESP and matched the loader, kernel ELF, and RAM disk hashes. The R3AS RAM disk was also inspected offline without writes: the complete 512-byte boot sector matched SHA-256 369abefec78107f9a884d3ae2e69029c3509d7273d5ddb2cad843241825e5c62 and ended in 55AA. Its geometry matched the expectation: 512 bytes per sector, 1 sector per cluster, 32 reserved sectors, 2 FATs, 937 sectors per FAT, 120,832 total sectors, first data sector 1,906, 118,926 data clusters, root cluster 2, FSInfo sector 1, backup boot sector 6, and volume ID 1196971856. Offline packaging verification found the expected FAT32 classification and embedded fixture/manifest hashes. This offline check is not guest mount evidence.

The preserved R3AO, R3AQ, and R3AR candidate ISOs also retained their pins:

| Preserved ISO | SHA-256 |
|---|---|
| R3AO | 305b56a6001597afa049340501c4a4b30fa47ec6fe63ead8a8accc9185c750f0 |
| R3AQ | 32cf00799e5c5677eaf507456bf54380247485febf15e08cdc10537bd8b5beaf |
| R3AR | 75ead34a8f22888c1d7856a1d5e0afacf4ae44baa0b2c786e84872c5a10acf1b |

The kernel runtime does not establish a full-image SHA-256; this acceptance makes no such claim.

## QAC admission and guest configuration

The immediate prelaunch QAC census was healthy and exclusive: coordinator version 2.0.0 online, zero QEMU processes, zero foreign QEMU processes, zero reservations/sessions, and zero queued or blocked work. The QAC policy review confirmed acceptance exclusivity, FIFO admission, foreign-QEMU blocking, and managed lifecycle. The operator's task submission authorized this one R3AT guest execution.

The QMP framing/encoding preflight passed using the approved helper: UTF-8 without BOM, CRLF JSON framing, socket sendall, capabilities, and query-status. The guest used QAC's acceptance mode and the pinned QEMU executable. It used OVMF as read-only pflash, the exact R3AS ISO as read-only CD-ROM, 1024 MiB RAM, serial capture, and loopback QMP. No writable disk or other writable storage was attached.

QAC granted reservation c501ab445c16431b8bdff14c88a41820 to the run owner and reconciled the managed child as QEMU PID 17580. The launch notification was missing, so the reservation, owner, process identity, and running child were independently reconciled before control continued. No second guest was started.

## Guest execution and first failure

The ordered serial trace reached UEFI loader execution, BootInfo handoff, kernel entry, and the R3AS active-CR3 mapping diagnostic.

The loader logged the expected first 16 bytes, EB58904D5357494E342E310002012000, and 512 bytes per sector. It also logged sample mappings for the intended high alias at its first page, second page, and last page. Those samples translated to the expected physical backing, were present, supervisor-only, and read-only; the samples reported no physical overlap. The loader's MAP-C/MAP-D probes say root_active=0, so they are sample translations through the constructed root, not a full active-CR3 validation.

At kernel entry, MAP-E reported active CR3 000000003DD3C000, RAM-disk virtual base FFFF900000000000, physical base 00000000155BE000, length 0000000003B00000 (61,865,984 bytes), CR0 0000000080010033, and CR0.WP=1. The first decisive result was:

- SQL_R3AS_MAP_RESULT=PAGE_TABLE_READER_UNAVAILABLE
- pages_checked=0000000000000201 (513 pages)
- failing_va=FFFF900000200000 (alias offset 0x200000, 2 MiB)
- SQL_R3AS_PACKAGE_RESULT=SKIPPED_MAPPING_FAILURE
- SQL_R3AS_RAMDISK_DIAGNOSTIC=FAIL

The expected complete check would cover 15,104 4 KiB pages plus the endpoint. Therefore the active-CR3, full-extent PTE validation did not pass. The reader reported that it could not read a required page-table entry; the trace does not establish that the mapping at the failing address had wrong backing or permissions.

### Source-based likely cause

The R3AS loader creates the sidecar list of page-table frames by walking only three RAM-disk sample addresses: the base, base plus one page, and the final page. The sidecar had five unique table pages. The kernel callback deliberately refuses to dereference a table frame not in that list. The validator then walks every 4 KiB page in the full RAM-disk extent. The failure is exactly at the 2 MiB boundary after page 512, where a different leaf page-table page is required. This makes incomplete sidecar coverage the likely cause of PAGE_TABLE_READER_UNAVAILABLE. This is a source-based explanation consistent with the trace, not direct runtime observation of the missing frame's identity.

No code was changed to address this finding in R3AT.

## BootInfo, package, FAT32/VFS, and SQL results

| Acceptance requirement | Guest result |
|---|---|
| UEFI loader and BootInfo handoff | Reached. Loader logged valid=1 for the BootInfo handoff contract and the intended high alias. |
| Full BootInfo v3 and active mapping validator | Not passed. Kernel entered MAP-E with the expected active CR3 and CR0.WP=1, but the full walk stopped with PAGE_TABLE_READER_UNAVAILABLE. |
| Sample high-alias translations | First, second, and last samples matched intended physical backing and showed present, supervisor, read-only PTEs. These samples do not prove the intervening extent. |
| Complete RAM-disk mapping | Failed at page 513 of 15,104, at alias +2 MiB. |
| Full 512-byte package-specific boot-sector validation | Skipped after the mapping failure. The UEFI loader's 16-byte prefix/BPS trace is not a substitute. |
| Runtime FAT32 classification and mount | Not established. No R3AS package PASS, FAT32 mount, or package-volume VFS registration/access marker appeared. Generic filesystem-driver and VFS initialization messages are not mount evidence. |
| SQL fixture open through production VFS | Not reached or established. |
| IX_Rows_K64 point lookup, root 234 / owner 1 / 73 ordered locators | Not run; no point result or locator comparison marker appeared. |
| IX_Rows_K32 range scan, root 148 / owner 2 / 32 ordered locators | Not run; no range result or locator comparison marker appeared. |
| Invalid-key rejection and unchanged caller output | Not run. |
| Valid recovery request after rejection | Not run. |

The fail-closed wrapper behaved as required after the mandatory gate failed: RAM-disk use was marked skipped, the native R3AS qualification was BLOCKED for mandatory mapping/package failure, and the final wrapper marker was QUALIFICATION END FAIL. The expected SQL_NATIVE_R3AQ END PASS and SQL_NATIVE_R3AS QUALIFICATION END PASS markers were absent. No SQL point/range/negative/recovery acceptance is claimed.

The serial shows later ordinary kernel startup, including a generic 4 MiB RAM disk and VFS layer initialization. Those messages do not establish that the protected R3AS FAT32 package mounted or that its SQL fixture was opened. No R3AT-specific FAT/VFS/SQL access markers followed the failure.

## Fault, QMP, and cleanup evidence

No page-fault, protection-fault, panic, or unexpected-fault marker was observed in the serial log. The captured QEMU guest-errors debug log is empty (0 bytes). These observations are limited to the captured logs.

The QMP transport worked: capability negotiation and status queries succeeded, the VM reported running before shutdown, and controlled QMP quit succeeded. However, the separate runtime page-table evidence parser failed with KeyError('cr3_active'): its key matcher omitted digits, so it could not parse the cr3_active field. Consequently no live QMP memory/page-table walk was captured. This host-side evidence defect does not change the serial mapping failure and was not retried.

The QAC-managed run exited with code 0 after the controlled host QMP quit; this is a process/lifecycle result, not a guest qualification PASS. QAC's final inspect showed the reservation Released and the managed child exited. Final coordinator status reported zero QEMU processes (zero foreign), reservations, sessions, queued entries, or blocked entries. The owned process and reservation were absent, and unrelated resources were preserved. Candidate and pinned artifact hashes were rechecked after shutdown and remained unchanged.

## Evidence files

The complete evidence directory is E:\CodexArtifacts\NDB1R4P11R3AT.

- Full serial trace: serial.log, 45,046 bytes, SHA-256 8fc046983ece36a9a032b8bc94ee6f101625d2eb7e4ecdee879abffb82f9fa11
- QMP observations: qmp-observations.jsonl, 2,518 bytes, SHA-256 b1bd1609af7439a8f7a6cf5af3d7a96283cf3441cd0710e682f86531858048ae
- QMP page-walk parser failure: runtime-page-table-walk.json
- QEMU guest-errors log: qemu-debug.log (empty)
- Launch config and candidate identity: guest-launch-config.json, candidate-integrity-preflight.json, candidate-iso-verification.json, candidate-esp-verification.json
- QAC admission and reconciliation: qac-prelaunch-census.json, qac-admission.json, qac-launch-reconciliation.json, qac-postrun.json
- Post-run integrity and raw process summary: artifact-integrity-postrun.json, raw-run-summary.json
- Final analysis and outcome classification: guest-analysis.json, outcome.json

## Recommended next bounded milestone

Open a separate repair phase. Make the trusted page-table sidecar/read path cover every page-table frame needed by the entire RAM-disk alias, while retaining explicit physical-frame provenance and the read-only/supervisor/CR0.WP checks. Add hosted coverage for every 2 MiB leaf-table boundary across the full extent. Correct the QMP evidence parser's field-name handling in the evidence tooling. Then rebuild and verify a new candidate under that repair phase and request a separately authorized guest acceptance. R3AT itself remains closed at the observed failure frontier; no retry is part of this phase.
