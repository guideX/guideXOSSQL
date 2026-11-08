# NDB1R4P11R3AP — Native RAM Disk Mapping Guest Acceptance Report

## Outcome

**Outcome A — `NATIVE_SQL_GUEST_READ_ACCEPTED`.** One QAC-managed guest boot
passed the repaired BootInfo v3 mapping, production RAM-disk/FAT32/VFS, and
native SQL diagnostic gates. The 73 point locators and 32 range locators were
compared in order by the production diagnostic against the candidate manifest
expectations. The owned QAC reservation was released and the frozen ISO hash
was unchanged after the run.

This was acceptance-only: no production source changed, no candidate was
rebuilt or repackaged, and nothing was pushed. The only guest boot was run
through QAC. The raw runner's provisional pending records remain preserved;
final classification is recorded separately in `final-outcome.json` and
`guest-analysis.json` under the evidence directory.

## Repository and provenance checks

| Checkout | Starting identity | Starting state and disposition |
|---|---|---|
| SQL | `main`, `d7ade78ed8b47fb0a9f31e0d4acc7bd5c5077f76` | Clean; one local commit ahead of `origin/main`. Candidate SQL source commit `6d2d52ab812ae8c89a14da8dc6908499bdec9a77` is an ancestor; no SQL source changes since it. |
| Isolated Server | `codex/ndb1r4p11rah-sql-read-integration`, `04bc1900398debffa44611e89af4024edd68cf54` | Tracked files clean. Pre-existing untracked build/evidence output directories were left in place. |
| QAC | `main`, `1ea193258690f9e7b39390b101a117166ef8bbdd` | Clean; used for the authoritative reservation and process census. |
| Original Server checkout | Not operated on | Left untouched, including its local assets. |

## Frozen candidate integrity

The candidate manifest SHA-256 is
`e89385508bb82863c4cb3e1fc66d9947156947fa59b073b427f9121115cd730d`. All
applicable pinned artifacts were checked against the manifest before launch;
the ISO was rechecked after shutdown. The candidate was the R3AO candidate
from Server commit `04bc1900398debffa44611e89af4024edd68cf54`, based on SQL
source commit `6d2d52ab812ae8c89a14da8dc6908499bdec9a77`.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| Candidate ISO (`GUIDEXOSR3AO`) | 100,730,880 | `305b56a6001597afa049340501c4a4b30fa47ec6fe63ead8a8accc9185c750f0` |
| Embedded ESP image | 100,663,296 | `df241b901f3b8d8a0722a974538bfd97f0792e56c3debb4dbc4633b32345cff3` |
| Qualification kernel ELF | 2,758,776 | `3d79f4fa99be3ff282ea157602659851bdb529e16cf0b2ce6a018a7005f51b80` |
| Qualification kernel PE | 2,868,832 | `1b583fc394a0bdbdd623211005797180a956c6eca2e28e45e87ef8ff194f484f` |
| Qualification UEFI loader | 81,920 | `db8789dfbf08b21f3959d32fab01354af3f96cd8c1eb715466f21edf40214351` |
| Read-only RAM disk | 61,865,984 | `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| SQL fixture `MULTI.GDB` | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted `FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| ESP file manifest | 489 | `14751864ab22826596becf50ffc69933079a5568d2cd93b9d573aca12c35ca2f` |
| ESP packaging report | 576 | `d78384326a55361456caec9bfc6c8ff0df99012d90b57a40448303aec5711e2d` |
| ISO file manifest | 354 | `e659c68fd50ec6a62428725db886537e152b02285a0fbfd56c35d68cd5428106` |
| ISO packaging report | 677 | `847c41f750b61695ce2a149c5d2559a0ccd7179ca6a43b5dfa582f6c8ade7f1f` |
| ISO README | 519 | `d9b844ac261d4c175d49e3994410012f741bef1880a5f2dbe7c6098fe6a1f3d9` |
| Candidate manifest | 6,511 | `e89385508bb82863c4cb3e1fc66d9947156947fa59b073b427f9121115cd730d` |

The evidence `candidate-preflight.json` and
`prelaunch-integrity-recheck.json` contain the full prelaunch records. The
post-run QAC record confirms the ISO still hashes to the frozen value above.

## QAC admission and ownership

A fresh immediate prelaunch census at `2026-11-08T04:29:04.800038Z` reported
QAC 2.0.0 online, zero QEMU processes, zero reservations, zero managed
sessions, zero queued or blocked requests. The acceptance reservation
`52fa89bfd8e84f71939da1094dae2db9` was granted at
`2026-11-08T04:29:05.0123971Z` with no queue wait. QAC identified project
`guideXOSServer`, mode `acceptance`, verified owner PID 16348, and managed
QEMU child PID 21108. The frozen ISO was mounted read-only and no writable
disk was attached.

The runner did not surface its launch notification and emitted
`admission-unresolved.json`. Before interpreting the guest or shutting it down,
ownership was reconciled from QAC status/list/queue/inspect, the Windows
owner-child process records, and the matching QMP endpoint. No second launch
was attempted. The raw `launch-attempt.json` also contains a stale R3AM phase
label in its generic authorization string; it is retained unmodified. The
current user request explicitly authorized this R3AP one-boot acceptance, as
recorded in `authorization-record.json`.

## Guest observations

### Boot and memory mapping

UEFI booted the QEMU DVD-ROM, loaded the guideXOS UEFI bootloader, reached the
post-`ExitBootServices` handoff, and entered `kernel_main` using UEFI BootInfo.
The kernel accepted the BootInfo v3 contract:

```text
SQL_R3AO_KERNEL_HANDOFF valid=1 status=BOOTINFO_VALID
  bootinfo=FFFF900003B10000 image_virtual=FFFF900000000000
  image_physical=00000000155BF000 image_length=0000000003B00000
  address_mode=kernel_virtual_alias mapping=read_only
  owner=UEFI_EfiLoaderData bytes16=EB58904755494445584F530002102000 bps=0200
```

MAP-A through MAP-D and MAP-E were present, followed by
`SQL_R3AO_MAP_RESULT=PASS`. MAP-E recorded active CR3 `0x3DD3C000`, RAM-disk
virtual alias `0xFFFF900000000000`, physical backing `0x155BF000`, and complete
length `0x03B00000`. The BootInfo alias `0xFFFF900003B10000` mapped to physical
`0x3DD3F000`.

The mapping diagnostics also expose the original low numeric RAM-disk address
overlapping the kernel virtual span: MAP-B resolves that old address to kernel
backing and marks `WRONG_BACKING`. The corrected high alias in MAP-C, MAP-D,
and active MAP-E has no virtual-range overlap and is the alias consumed by the
BootInfo and production storage paths. This is the repaired collision being
qualified; no unexpected collision, fault, or panic followed in the active
mapping or production path.

QMP independently read the first 16 bytes at the physical backing as
`EB58904755494445584F530002102000`, matching the MAP-E alias dereference.
Actual QMP page-table walks for the RAM-disk first, interior, and last pages
all resolved to the expected physical frames with present, supervisor,
read-only leaf entries. The sample PTE values were `0x155BF001`,
`0x155C0001`, and `0x190BE001`. The BootInfo mapping was also present,
supervisor, and read-only.

QMP observed CR0 `0x80010033` with WP set, and CR3 matched the MAP-E serial
record. The handoff stack alias at `0xFFFF900003B00000` mapped to physical
`0x00200000`, length `0x00010000`; QMP verified its first and last pages as
present, supervisor, and writable. At QMP capture the current RSP was
`0x22BB2BD8`, outside that handoff alias; the kernel image's first mapping
remained present at VA `0x00100000` to PA `0x190BF000`.

### RAM disk, FAT32, and VFS

The production attachment registered the borrowed RAM disk read-only without
copying it. The LBA 0 source, destination, and FAT read all returned status
`00` and bytes `EB58904755494445584F530002102000`; bytes per sector was `0200`
(512). The FAT diagnostic recorded BPB `validation=ACCEPTED`, then
`mount_success filesystem=FAT volume=00 ro_block=1`. The subsequent VFS marker
registered `fs_type=FAT32 read_only=1`. The volume diagnostic also reports
`cluster_class=FAT16`; this exact diagnostic is preserved here alongside its
FAT32 label and successful BPB/mount markers.

### Native SQL diagnostic

The production diagnostic ran against
`/system/Apps/R3AI/MULTI.gxdb` through the read-only VFS/provider. It emitted,
in order:

```text
SQL_NATIVE_R3AI BEGIN fixture=NDB1R4P11R3AH_MULTILEVEL
SQL_NATIVE_R3AI FIXTURE_OK length=250880 page_size=512 page_count=490 catalog_root=1
SQL_NATIVE_R3AI POINT_PASS index=IX_Rows_K64 root=234 owner=1 count=73
SQL_NATIVE_R3AI RANGE_PASS index=IX_Rows_K32 root=148 owner=2 lower=1 upper=2 count=32
SQL_NATIVE_R3AI END PASS
```

The production point and range checks compare each returned page/slot locator
in order, not just the counts. The source's expected arrays were cross-checked
against the frozen candidate manifest (`manifest-locator-source-crosscheck.json`);
the complete 73- and 32-entry arrays are retained in
`expected-locator-arrays.json`. The actual guest's two `*_PASS` markers and
final `END PASS` show the production reader/VFS execution completed those
comparisons and cleanup. There was no SQL `FAIL`, panic, page fault, or triple
fault marker.

## Acceptance gates

All 13 gates in the procedure passed in this single run: UEFI boot; kernel
entry and BootInfo-v3 validation; MAP-A through MAP-E and active-CR3
translation; physical bytes; successful LBA 0; FAT32 BPB acceptance and mount;
read-only VFS registration; production fixture access and resource cleanup;
fixture metadata/root/owner validation; ordered point lookup; ordered range
scan; final SQL diagnostic PASS; and owned-guest shutdown/release.

## Shutdown, release, and final census

After ownership was established, QMP `quit` was sent only to QAC-owned QEMU
PID 21108 at `2026-11-08T04:33:11Z`. QAC inspect then reported reservation
`52fa89bfd8e84f71939da1094dae2db9` as `Released` because its managed child
exited; QEMU PID 21108 and owner PID 16348 were absent. The candidate ISO
post-run SHA-256 matched its frozen prelaunch value.

The final QAC census at `2026-11-08T04:33:51.7653985Z` showed one other active
acceptance reservation, `9e93be070c0b4f689e9827efe7027bc6`, owned by PID 24796
with QEMU PID 21316. It was a separate workstream and was left untouched. The
final census therefore does not claim that QAC was globally idle; it confirms
that this R3AP reservation and its processes were gone while the other
reservation remained managed and active.

## Evidence and next milestone

Evidence directory: `E:\CodexArtifacts\NDB1R4P11R3AP`.

| Evidence | Size / identity |
|---|---|
| `serial.log` | 64,405 bytes; SHA-256 `45c385a9c9abc3fa5bc8b5cc34a4c9231169c1e23e841e19789f64624d04b04d` |
| `qmp-observations.jsonl` | 9,951 bytes; SHA-256 `42bc5a4d4e16e76bf91a57a48303e6d75fc91cf786097bd5d40cc18df02a3f72` |
| `runtime-page-table-walk.json` | QMP CR3/PTE walks, physical read, CR0/WP, stack and kernel mapping observations |
| `candidate-preflight.json`, `prelaunch-integrity-recheck.json` | Candidate artifact and hash checks |
| `qac-live-owner-reconciliation.json`, `qac-owner-before-shutdown.json`, `qac-postrun.json` | Owner proof, pre-shutdown state, release, process exit, post-run ISO identity |
| `manifest-locator-source-crosscheck.json`, `expected-locator-arrays.json` | Production expected-array cross-check and complete trusted locator arrays |
| `guest-analysis.json`, `admission-reconciliation.json`, `final-outcome.json` | Gate-by-gate result, QAC reconciliation, final classification |
| `outcome.json`, `admission-unresolved.json`, `launch-attempt.json` | Original runner records retained unchanged; provisional outcome superseded by the reconciled final records |

No R3AP repair run is indicated. The next milestone should be the next planned,
bounded native SQL guest phase, using this accepted frozen-candidate result as
its baseline and preserving R3AP evidence unchanged.
