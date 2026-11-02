# NDB1R4P11R3AL-R1 — Native SQL FAT32 Guest Attribution Retry

## Outcome

**Outcome C — `NATIVE_SQL_FAT32_GUEST_PENDING`.** The R3AL candidate passed
identity checks, but the runner's final QAC preflight found an active exclusive
acceptance reservation belonging to another QAC client. It refused before
requesting a reservation or starting QEMU. No R3AL guest boot occurred, so this
retry provides no new FAT32 failure attribution and no SQL guest evidence.

The pasted task explicitly authorized at most one diagnostic guest boot. The
required exclusive QAC admission was unavailable at the runner's final
preflight. No QAC request, reservation, QEMU process, serial file, or QMP
connection was created by this run. The single-use launch-attempt marker is
absent.

## Repository identities and preservation

| Repository | Starting identity | Ending identity at diagnostic closeout |
|---|---|---|
| SQL | `main`, `3a4a56f46baa2e5fe9f81b33242f64ae63e39d51` | Same before this report was committed locally; this report is the only new SQL file. |
| Isolated Server integration | `codex/ndb1r4p11rah-sql-read-integration`, `109eaf4107592864f9de819b103a98a87eaa69a5` | Same; tracked source remained clean. |
| QAC | `main`, `1ea193258690f9e7b39390b101a117166ef8bbdd` | Read-only; no source changes. |
| Original Server checkout (`D:\dev\guideXOSServer_NATIVE_DB_STORAGE`) | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575` | Same; tracked tree clean and not modified. |

The candidate manifest records SQL build commit
`87d17f7639e85ff1de0f7b089b96752a84884534` and Server commit
`109eaf4107592864f9de819b103a98a87eaa69a5`. The SQL build commit is an
ancestor of the requested current SQL HEAD. Its only intervening file is the
R3AL attribution report, a documentation-only receipt; there is no production
SQL source change. The runner copy records and enforces this exact source
delta. Existing untracked Server output directories were preserved.

## Frozen candidate verification

The exact R3AL ISO was verified at:

`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3al\boot-candidate\guideXOS-NDB1R4P11R3AL-amd64.iso`

It is **100,730,880 bytes**, SHA-256
`698698865d2701c2977ccc3f8bf7efab6662d452b9b6fc5deddaa336408ab87a`.
This is not the original R3AI ISO (SHA-256
`f22e3f3627b2ce6541bf9499934a4d726135574b9b654aa12a20351ed860d497`).

The candidate manifest is 5,564 bytes, SHA-256
`9975a5a8213656794ac8c1744a4791ebd77c4d145c158ab6d09f069f1dfbedca`.
Its SQL and Server source commits match the candidate provenance above. The
accepted identity record and manifest match the ISO, PE/ELF kernel, embedded
ESP, bootloader, RAM disk, fixture, and trusted fixture manifest hashes. All
nine manifest-pinned artifacts were rehashed after the QAC refusal; all nine
still match. The post-refusal check confirms the source ISO and fixture were
unchanged.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| Kernel ELF | 2,735,872 | `6c82f6be10e8540ee602431349b6fe8bcdbc7522bc59fd59249fa25b093093e9` |
| Kernel PE | 2,848,523 | `84dda104c889bdd8c54e82b4c85680f5eec5ce0d2cb62c3e8a40d74186b90440` |
| Embedded RAM disk | 61,865,984 | `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| Embedded ESP | 100,663,296 | `d8e4b2844d52774da5d7f8905bafd512644ce8a915216e5b5c52c36bb9380bc8` |
| SQL fixture `MULTI.GDB` | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted `FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |

The ISO verification report identifies a bootable UEFI El Torito image
(platform `0xEF`, no emulation, boot image LBA 32). The embedded ESP hash and
its kernel, bootloader, and RAM disk hashes match the accepted identity.

The build provenance is recorded in
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3al\server-build-qualification.json`:
AMD64 with `GX_NATIVE_SQL_READ_QUALIFICATION=1` and
`GX_NATIVE_SQL_READ_DIAGNOSTIC=1`. Its diagnostic kernel ELF and PE hashes
match the candidate identity. The fixture is packaged as
`/Apps/R3AI/MULTI.GDB` and is addressed by the SQL diagnostic at
`/system/Apps/R3AI/MULTI.gxdb`.

The manifest's SQL diagnostic contract is:

- `SQL_NATIVE_R3AI BEGIN fixture=NDB1R4P11R3AH_MULTILEVEL`
- `SQL_NATIVE_R3AI FIXTURE_OK length=250880 page_size=512 page_count=490 catalog_root=1`
- `SQL_NATIVE_R3AI POINT_PASS index=IX_Rows_K64 root=234 owner=1 count=73`
- `SQL_NATIVE_R3AI RANGE_PASS index=IX_Rows_K32 root=148 owner=2 lower=1 upper=2 count=32`
- `SQL_NATIVE_R3AI END PASS`

These are expected markers only. None was observed in this retry because the
guest did not run. The package geometry remains the previously audited
7,518-cluster volume; this retry supplies no evidence that geometry caused the
earlier failure.

The QEMU executable identity is SHA-256
`a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02`.
Firmware `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\OVMF.fd` is SHA-256
`2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792`.
The prepared runner's configuration uses read-only OVMF and ISO media,
`pc,usb=off`, 1024 MiB RAM, no display, serial-to-file output, loopback QMP,
and `-no-reboot`. No QEMU command was issued, no QMP port was allocated, and
none of those settings was exercised.

## QAC authorization and admission

The QAC README and Phase 4R2 coordination report were reviewed. The current
QAC CLI/coordinator requires a reliable empty census for exclusive acceptance
and manages QEMU as the direct child of `qac run`. A foreign or already-managed
QEMU blocks admission; QAC does not terminate or reclaim another owner's
process. The pasted task supplied user authorization for this bounded run.

An earlier read-only QAC snapshot showed zero QEMU processes, reservations,
managed sessions, and queued requests. Before the runner could request
admission, its own status/list/queue preflight detected that the census had
changed. The runner stopped with `QAC is not exclusively empty; guest launch is
refused`. A subsequent read-only census identified:

- Reservation `2a88ba01d1a64dbba8e3add8ae6a045b`, project `guideXOSServer`, mode
  `acceptance`, state `Running`.
- Verified reservation owner PID `30508`; direct QEMU child PID `8172`, started
  at `2026-11-02T14:37:05.0305996Z`.
- One managed QEMU process, zero foreign QEMU processes, and no queued request.

This was a separate active QAC client and was not acquired by this retry. No
reservation request was submitted. The reservation was not released, its
process was not stopped, and its QMP or serial resources were not inspected or
reused. The exact status/list/queue capture is preserved in
`qac-admission-refused.json`; the runner's refusal is in `runner-error.json`.

## Guest result and FAT32 attribution

| Stage | R3AL-R1 result |
|---|---|
| UEFI entry | Not run |
| Kernel entry | Not run |
| RAM disk registration / block device identification | Not run |
| VFS initialization and FAT32 mount | Not run |
| Instrumented FAT32 validation stages | Not run |
| VFS mount registration | Not run |
| SQL fixture opening or metadata validation | Not run |
| Root/owner validation, point lookup, range scan | Not run |

There is no observed first failing production operation in R3AL-R1. The
R3AJ-R1 trace remains the only guest failure evidence: UEFI and kernel entry
succeeded, the RAM disk attached as `wallimg`, and the generic production FAT32
mount failed before SQL fixture access. That 40,202-byte serial trace has SHA-256
`71098cfbe8c1d21751ff1ac4c380410ce6cf40c2aba7c24b8c6ea501debaf8a5` and is
preserved at
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3aj-r1-guest-acceptance-20261101\serial.log`.

R3AK's static audit and hosted tests, and R3AL's earlier 73/73 hosted
qualification, do not identify the guest's first failing operation. The
previous audit found no production cluster-count threshold rejection, and the
exact package mounted in the hosted production path. With no R3AL guest trace,
block-device access, boot-sector read/validation, geometry checks, FAT
acceptance, memory allocation, read-only capability handling, and VFS
publication remain unobserved for this retry. The 7,518-cluster geometry stays
a compatibility concern, not an attributed root cause.

The execution frontier is unchanged from R3AJ-R1: no `/system` mount, fixture
open, database metadata/root/owner validation, point lookup, range scan, or SQL
provider cleanup. No actual locator sequence or SQL result was produced. The
trusted manifest's expected arrays (73 point locators for root 234/owner 1 and
32 range locators for root 148/owner 2) are not guest evidence; no SQL guest
acceptance is claimed.

## Evidence and cleanup

The new evidence directory is:

`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3al-r1-guest-attribution-20261102`

It contains the copied accepted identity and candidate manifest,
`candidate-preflight.json`, the evidence-scoped runner,
`qac-admission-refused.json`, `runner-error.json`, and
`post-preflight-integrity.json`. The latter confirms all nine manifest-pinned
artifacts still match. There is no `launch-attempt.json`, `serial.log`, QMP
observation file, or QAC-owned R3AL process/reservation.

No guest was started, so no shutdown or R3AL reservation release was needed.
The other active QAC reservation remained untouched at the captured census.
No source ISO, fixture, original Server checkout, or prior evidence directory
was changed. The Server repository remains at
`109eaf4107592864f9de819b103a98a87eaa69a5`; its existing and new untracked
`out/` artifacts were preserved.

## Recommended next bounded milestone

After the active QAC acceptance owner releases its reservation, take a fresh
single QAC preflight and run the already-frozen R3AL ISO once under an exclusive
`guideXOSServer` acceptance reservation. Preserve the first ordered FAT/VFS
diagnostic record and QMP shutdown. Continue to distinguish guest-observed
failures from hosted or static evidence; do not infer a cluster-geometry cause
without the actual failing branch and values.
