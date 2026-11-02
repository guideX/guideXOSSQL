# NDB1R4P11R3AL — Native SQL FAT32 Guest Failure Attribution

## Outcome

**Outcome C — `NATIVE_SQL_FAT32_GUEST_PENDING`.** The R3AL instrumentation,
hosted qualification, Server builds, and a new UEFI candidate were prepared and
verified. No R3AL guest was launched. The immediate QAC preflight found an
active, verified `guideXOSServer` acceptance reservation and its QEMU child,
owned by another workstream. No reservation request was submitted, and no
existing guest, reservation, or QAC state was changed. Consequently, R3AL has
no guest serial/QMP trace and does not establish the first FAT32 failure.

The user authorized this bounded diagnostic phase, which allowed at most one
guest boot. The QAC exclusive-admission prerequisite was not available at the
final preflight. The prepared candidate remains preserved for a separately
admitted run.

## Repository identities and preservation

| Repository | Starting identity | Ending identity |
|---|---|---|
| SQL | `main`, `87d17f7639e85ff1de0f7b089b96752a84884534` | `main`, the local commit adding this report; its exact object ID is returned with the phase completion |
| Isolated Server integration | `codex/ndb1r4p11rah-sql-read-integration`, `70ab616acb5901b95e5d6e46b0676d28c8747eb8` | `codex/ndb1r4p11rah-sql-read-integration`, `109eaf4107592864f9de819b103a98a87eaa69a5` |
| Original Server checkout | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575` | Not modified |

The SQL production tree is unchanged; this report is the only SQL source
change. The Server attribution changes were committed locally as
`109eaf4107592864f9de819b103a98a87eaa69a5`. No push or merge was performed.
Existing R3AI, R3AJ/R1, and R3AK output directories remain untouched. The
R3AL artifacts are stored under
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3al`.

## R3AK instrumentation audit and R3AL additions

Reviewed the R3AK changes from Server commit `70ab616acb5901b95e5d6e46b0676d28c8747eb8`:

- `kernel/core/fs_fat.cpp` and `kernel/core/include/kernel/fs_fat.h`: gated
  FAT mount device summary, LBA 0 read status, boot-sector BPB/signature,
  geometry-validation failure codes, partition boot read failure, mount
  result, and a diagnostic-only mount failure code accessor.
- `kernel/core/vfs.cpp`: gated block-device rejection, FAT-driver result,
  read-only mount registration, and failed-mount publication outcome.
- `tests/native_sql_r3ak_mount_regression_test.cpp` and its runner: production
  FAT/VFS qualification against the exact R3AI image and controlled media.

R3AK instrumentation already distinguished a device missing before mount,
LBA 0 block read failure, BPB checks, partition probes, FAT volume-slot
exhaustion, and VFS registration. It did not show computed cluster count,
count-based FAT classification, the two FAT32 BPB rejection branches for zero
FAT size and type-label mismatch, or the returned partition-read status. The
R3AL Server commit added these bounded, compile-time-gated records and more
explicit VFS mount begin/rejection markers. The R3AK behavior is not changed
when the qualification/diagnostic gates are off.

The existing RAM-disk attachment marker reports `wallimg` and its sector
count. R3AL's VFS mount-begin marker reports the selected block index, device
name, generation, capacity, sector size, and read/write callback presence.
Together these identify the attached read-only device and whether VFS can
select it. The production RAM-disk registration function itself does not
currently print the `register_device` return value; a missing or invalid
selected block device is reported by VFS as generation zero / unavailable.
No R3AL guest trace was available to exercise this path.

The FAT implementation has no separate FAT-table cache initialization stage
on the mount path: after validation it fills the static volume descriptor and
marks the volume mounted. R3AL reports that acceptance point and subsequent
VFS publication separately. No production acceptance rule or writable
capability was changed.

## Original evidence and prior failure frontier

The unchanged R3AI candidate remains
`out\ndb1r4p11r3ai\boot-candidate-final-v4\guideXOS-NDB1R4P11R3AI-amd64.iso`,
100,730,880 bytes, SHA-256
`f22e3f3627b2ce6541bf9499934a4d726135574b9b654aa12a20351ed860d497`. Its
RAM-disk SHA-256 remains
`22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78`.
The SQL fixture is 250,880 bytes,
`cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57`; the
trusted `FIXTURE.MAN` is 12,937 bytes,
`43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3`.
The R3AI candidate manifest hash is
`f8dea45ea84bf6b117fb9d0a761499b1e3e2ff0e27de4cbadef896d18a7ff835`.

The R3AJ-R1 serial evidence remains at
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3aj-r1-guest-acceptance-20261101\serial.log`,
40,202 bytes, SHA-256
`71098cfbe8c1d21751ff1ac4c380410ce6cf40c2aba7c24b8c6ea501debaf8a5`.
It proves UEFI and kernel entry, RAM-disk attachment of `0x1D800` 512-byte
sectors, and selection of `wallimg`. Its first production mount failure is
`[VFS] ERROR: Filesystem mount failed`; the SQL diagnostic then emits
`VFS_UNAVAILABLE_OR_WRITABLE`. It contains no FAT attribution records, VFS
mount registration, fixture open, point lookup, or range scan. Thus the R1
failure remains located inside `fs_fat::mount()` via `vfs::mount_type()`, but
its first failing operation is not known.

## Exact package geometry and compatibility assessment

The R3AL candidate uses the exact R3AI RAM-disk bytes; no package contents,
fixture, or trusted manifest were regenerated.

| Field | Packaged value |
|---|---:|
| Volume bytes | 61,865,984 |
| Device-reported capacity in the production RAM-disk registration | 120,832 sectors |
| Device sector size | 512 bytes |
| Volume start offset | LBA 0 (unpartitioned superfloppy; no MBR partition start) |
| Total sectors | 120,832 |
| Sectors per cluster | 16 (8 KiB) |
| Reserved sectors | 32 |
| FAT count / sectors per FAT | 2 / 256 |
| Root cluster | 2 |
| First data sector | `32 + 2 × 256 = 544` |
| Data sectors | `120,832 − 544 = 120,288` |
| Data cluster count | `floor(120,288 / 16) = 7,518` |
| FAT entry capacity | `(256 × 512) / 4 = 32,768` entries |
| FAT32 FSInfo / backup boot sector | 1 / 6 |
| FAT32 flags / version | 0 / 0 |
| Hidden sectors | 0 |
| Boot-sector signature / type label | `55 AA` / `FAT32   ` |

By the conventional cluster-count ranges, 7,518 clusters classify as FAT16,
despite the FAT32 type label. This is a genuine package compatibility concern.
The current production driver does not classify a volume using cluster-count
thresholds: it requires the FAT32 label and validates geometry, volume bounds,
root cluster, and FAT capacity. The exact bytes pass the production hosted
mount. Therefore the label/count mismatch is **not code-supported as the R1
mount rejection branch** in this Server revision. Its broader package
compatibility implications remain a hypothesis for a future packaging phase;
no geometry repair was attempted here.

The hosted test uses the production `ramdisk::create_readonly_at` path,
512-byte sectors, the same 120,832-sector capacity, volume offset zero, a read
callback, and no write callback. The guest path first hands the payload
through UEFI BootInfo memory mapping and calls the same read-only RAM-disk API
during kernel startup. R1 confirms the attached byte size and sector count,
but has no gated report of the guest block sector size or computed geometry.
That guest-versus-hosted memory mapping and startup state remains the
important unmeasured difference. R3AL would report guest device geometry and
each FAT/VFS stage, but its guest did not run.

## R3AL candidate identity and UEFI packaging

The frozen candidate identity is recorded in
`out\ndb1r4p11r3al\accepted-candidate-identity.json`; its ISO SHA-256 is
`698698865d2701c2977ccc3f8bf7efab6662d452b9b6fc5deddaa336408ab87a`.
The identity file binds SQL HEAD `87d17f7639e85ff1de0f7b089b96752a84884534`,
Server HEAD `109eaf4107592864f9de819b103a98a87eaa69a5`, and these artifacts:

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| R3AL UEFI ISO | 100,730,880 | `698698865d2701c2977ccc3f8bf7efab6662d452b9b6fc5deddaa336408ab87a` |
| Embedded UEFI ESP image | 100,663,296 | `d8e4b2844d52774da5d7f8905bafd512644ce8a915216e5b5c52c36bb9380bc8` |
| Kernel PE | 2,848,523 | `84dda104c889bdd8c54e82b4c85680f5eec5ce0d2cb62c3e8a40d74186b90440` |
| Kernel ELF | 2,735,872 | `6c82f6be10e8540ee602431349b6fe8bcdbc7522bc59fd59249fa25b093093e9` |
| RAM disk | 61,865,984 | `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| SQL fixture `MULTI.GDB` | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted `FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| R3AL candidate manifest | 5,564 | `9975a5a8213656794ac8c1744a4791ebd77c4d145c158ab6d09f069f1dfbedca` |

The ISO verifier reports UEFI El Torito platform `0xEF`, bootable, no-emulation,
with boot image LBA 32. The embedded ESP was byte-compared with the staged ESP.
The large EFI image uses the accepted zero boot-load-size sentinel. The R3AI
ISO and its prior evidence were not overwritten. The R3AL candidate was not
rebuilt after its identity was recorded.

## Hosted qualification and builds

The new focused hosted production FAT/VFS regression passed **73/73** checks.
It includes the exact image and computed geometry; read-only callback and
write rejection; hosted SQL fixture, point/range diagnostic, unchanged media,
and handle cleanup; LBA 0 injected timeout with distinct classification;
zero FAT-size and type-label mismatch classification; nonzero partition
offset; and failed-mount nonpublication. The run log is
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3al\hosted-mount-regression.log`,
6,765 bytes, SHA-256
`6ca3f317cf60506a8d1097642924888b29109fd4e5d94bf30c3aefba813967ce`.
Structured qualification is in `out\ndb1r4p11r3al\hosted-qualification.json`.

The prior R3AK FAT32 regression baseline remains 60/60, and the prior R3AI
production-VFS baseline remains 22/22. R3AK's SQL Debug CTest remains 39/39.
SQL production sources are unchanged, so the full long SQL aggregate was not
rerun. The prior freestanding census remains 26/44; it was not rerun.

Fresh ordinary and qualification-enabled AMD64 Server variants both built,
linked to PE, and converted to ELF. Both PE files have zero import DLLs. Each
ELF has the same 21 unique unresolved symbol identities as the R3AK baseline.
The ordinary image has no SQL, FAT, or VFS diagnostic marker strings; the
gated image contains them. Existing unrelated Server compiler warnings were
emitted; neither build had an error. Exact hashes and command lines are in
`out\ndb1r4p11r3al\server-build-qualification.json`.

## QAC admission, guest evidence, and cleanup

Immediately before any launch request, the read-only QAC preflight at
`out\ndb1r4p11r3al\qac-final-preflight.json` reported QAC 2.0.0 online and:

- One QEMU process, zero foreign processes.
- One managed session and one reservation: `0f690ad12b6242cd83eb7f9ea02cc02a`.
- Owner project `guideXOSServer`, mode `acceptance`, state `Running`, owner
  verified as PID 18048, child QEMU PID 22304.
- Zero queued reservations.

This was another workstream's active exclusive acceptance. No R3AL QAC run or
reservation request was submitted; no QEMU was started; no R3AL serial or QMP
file exists. No cleanup of another owner's guest was attempted. The preflight
is not a guest boot result. The preserved original R1 serial evidence remains
the only guest trace relevant to this failure.

Because R3AL acquired no reservation and launched no process, there is no
R3AL reservation or QEMU child to release. The other workstream's QAC process
and reservation were left untouched. QAC cleanup for R3AL is therefore
complete by non-acquisition; admission remains pending.

## Root-cause conclusion and SQL frontier

**Observed R3AL failure:** none; the R3AL candidate did not execute.

**Code-supported findings:** the candidate's type label is FAT32 while its
7,518-cluster count falls in the FAT16 range; the production FAT driver does
not reject based on that count. The exact packaged image mounts and serves the
fixture through production FAT/VFS in hosted execution. R1's first failed
operation remains the un-attributed production FAT32 mount call.

**Hypothesis:** guest UEFI BootInfo mapping, RAM-disk registration/selection,
or guest startup state differs from the passing hosted test. No evidence
selects among these yet. No block read error, BPB rejection, mount allocation
failure, read-only capability conflict, or VFS publication failure is
established for R1. The narrowest next observation is the first R3AL
`[FAT-RO-DIAG]` or `[VFS-RO-DIAG]` outcome after the exact candidate enters the
kernel under an exclusive QAC acceptance reservation.

The SQL diagnostic frontier remains the R1 frontier: no `/system` mount, SQL
fixture open, header/page validation, root/owner validation, point lookup,
range scan, or SQL-provider cleanup was reached. The hosted 73/73 qualification
is not guest SQL execution.

## Recommended bounded next milestone

After the active QAC acceptance workstream releases its reservation, conduct
one separately admitted diagnostic boot of the already-frozen R3AL ISO
(`698698865d2701c2977ccc3f8bf7efab6662d452b9b6fc5deddaa336408ab87a`). Capture
the ordered FAT/VFS mount records and QMP shutdown, then select a targeted
repair only if the first failed production operation is demonstrated. Keep
the RAM disk read-only and preserve the SQL fixture and expected query
results. Do not implement a filesystem behavior repair in this attribution
phase.
