# NDB1R4P11R3AR — Standards-Conforming FAT32 and Native SQL Qualification

**Outcome D — NATIVE_SQL_FAT32_GUEST_FAILED**

The R3AR RAM disk has independently checked FAT32 geometry, preserves the trusted SQL fixture and manifest, and passes the production hosted VFS and SQL service checks. The single QAC-managed guest mounted FAT32 and completed the SQL point, range, rejection, and recovery checks. Guest acceptance nevertheless fails because the required BootInfo mapping diagnostic emitted SQL_R3AO_MAP_RESULT=FAIL. The read-only alias mappings themselves match their backing; the diagnostic's fixed first-sector byte check still expects the previous R3AQ boot sector.

No guest retry, rebuild, repackaging, or production source repair was performed after that failure.

## Repository identities and scope

| Repository | Starting HEAD | Ending HEAD |
| --- | --- | --- |
| SQL main | 826737f39b067df713f6017d331675bf12257b96 | This report's local commit on main (full object ID is in the completion record) |
| Server integration codex/ndb1r4p11rah-sql-read-integration | 8e4f797aed180ecbbe595a7e77bfd3f8167dfabe | 5a95548056f108156bd9fec2bd8793d9ba98ec8c |

The SQL repository's only change is this report. The fixture, manifest, SQL production source, and SQL commit used by the candidate are unchanged. Server commit 5a955480… adds only the R3AR packaging/validation scripts and focused hosted test. It does not modify the production SQL service, FAT driver, VFS, BootInfo mapping, or bootloader. Existing untracked Server out/ artifacts were preserved.

Accepted ISO files were not modified:

- R3AQ: 100,730,880 bytes, SHA-256 32cf00799e5c5677eaf507456bf54380247485febf15e08cdc10537bd8b5beaf.
- R3AO accepted candidate-r3: 100,730,880 bytes, SHA-256 305b56a6001597afa049340501c4a4b30fa47ec6fe63ead8a8accc9185c750f0.

## FAT type rule and geometry

Microsoft determines FAT type from the calculated data-cluster count, not the BPB label. The calculation is:

DataSec = TotSec - (RsvdSecCnt + NumFATs × FATSz + RootDirSectors)

CountOfClusters = floor(DataSec / SecPerClus)

A count below 4,085 is FAT12; below 65,525 is FAT16; 65,525 or more is FAT32. The FAT label alone does not select a variant. See the [Microsoft FAT overview](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf) and the [Microsoft FAT32 specification](https://jyywiki.cn/pages/OS/manuals/MSFAT-spec.pdf).

| Field | Accepted R3AQ media | R3AR package |
| --- | ---: | ---: |
| Partitioning / volume offset | Unpartitioned, volume starts at LBA 0 | Unpartitioned, volume starts at LBA 0 |
| Image size | 61,865,984 bytes (59 MiB) | 61,865,984 bytes (59 MiB) |
| Bytes per sector | 512 | 512 |
| Sectors per cluster | 16 | 1 |
| Reserved sectors | 32 | 32 |
| FAT copies | 2 | 2 |
| Sectors per FAT | 256 | 937 |
| Total sectors | 120,832 | 120,832 |
| First data sector | 32 + 2×256 = 544 | 32 + 2×937 = 1,906 |
| Data sectors | 120,832 - 544 = 120,288 | 120,832 - 1,906 = 118,926 |
| Data clusters | floor(120,288 / 16) = 7,518 | floor(118,926 / 1) = 118,926 |
| Standard cluster class | FAT16, despite FAT32 BPB fields/label | FAT32 |
| FAT32 root cluster | 2 | 2 |
| FSInfo / backup boot | Sectors 1 / 6 | Sectors 1 / 6 |

The corrected image exceeds the 65,525 FAT32 threshold by 53,401 clusters. Each FAT has capacity 937 × 512 / 4 = 119,936 entries; 118,926 + 2 = 118,928 entries are required for the data clusters and reserved entries, leaving 1,008 spare entries. FAT32 BPB fields are used (RootEntCnt=0, FATSz16=0, FATSz32=937, TotSec16=0), hidden sectors are zero, the primary boot signature is 55 AA, and the root cluster points into the data region.

The formatter output has valid primary and backup boot records; the backup record at sector 6 contains sectors 0–2, with the backup FSInfo at sector 7. Primary and backup FSInfo signatures and values agree. The structural validator found 111,744 allocated and 7,182 free clusters, matching the 118,926-cluster data region. It verified both FAT copies, the root chain, every directory/file chain, bounds, and absence of orphaned allocations.

The existing 59 MiB capacity is retained. The image remains 15,104 4 KiB pages, so BootInfo v3 allocation and alias-window dimensions do not change. The accepted 4 GiB alias window, EfiLoaderData backing, read-only supervisor PTEs, and CR0.WP design were not changed.

## Packaging and fixture provenance

The package was built with the existing runtime staging script and the installed pyfatfs.PyFat.mkfs formatter (version 1.1.0). No filesystem stack was installed. The exact command was:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\package-native-sql-r3ar-fat32.ps1

The script staged the existing runtime assets and the already closed R3AH database, formatted a 59 MiB FAT32 volume with 512-byte sectors, one sector per cluster, two FATs, volume label GXWALLPAPER, and volume ID 0x47585750, then copied the staged files. It writes valid FSInfo and backup records and refuses to replace existing output.

The trusted SQL source identities were verified before and after qualification:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| tests/fixtures/NDB1R4P11R3AH/multilevel.gxdb | 250,880 | cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57 |
| tests/fixtures/NDB1R4P11R3AH/fixture.manifest | 12,937 | 43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3 |

The package preserves the trusted manifest's production-writer provenance and all recorded database/catalog/index identities. No database rows or indexes were regenerated. The packaged paths are /Apps/R3AI/MULTI.GDB and /Apps/R3AI/FIXTURE.MAN; the existing VFS alias exposes the fixture at /system/Apps/R3AI/MULTI.gxdb. The new FAT directory entries and cluster chains are packaging metadata only.

The final RAM disk is 61,865,984 bytes with SHA-256 50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c. The fixture occupies 490 clusters (250,880 / 512); the manifest occupies 26 clusters.

### Independent validation

The focused raw-sector validator passed boot and backup signatures, BPB and total-sector accounting, FAT capacity, FAT copy equality, root and file chains, allocation bounds, no orphan clusters, FSInfo values, every staged path, and byte-for-byte fixture/manifest content checks.

A separate read-only pyfatfs.PyFatFS parse (1.1.0) classified the volume as FAT32 and read all 87 files and 6 directories with exact source-byte comparison. PyFatFS is part of the same package suite as the formatter, so the report does not present it as an unrelated implementation. A separate raw structure/chain validator was used alongside it. No fsck.fat, dosfsck, or mtools FAT validator was installed in the existing environment; no unrelated tool stack was added.

Reports:

- Package manifest: out/ndb1r4p11r3ar/fixture-package-r3/r3ar-fixture-package-manifest.json, SHA-256 9f70ced501009c2e80bc625e6f60f2d2d0d0d54921553f8c3637cd259fc53d6f.
- Packaging provenance: .../r3ar-fat32-packaging-provenance.json, SHA-256 668bb7e1c1dc9d0e9acbe1e144fae4ed51e64558a18ac844b400b55644025a30.
- Independent validation: .../r3ar-fat32-independent-validation.json, SHA-256 965627ca86ae299ae254db8e58eb0643d2598ecbbb86ab3c132364a4f0564711.

## Hosted and regression results

| Qualification | Result | Image/scope |
| --- | ---: | --- |
| R3AR FAT32 package raw geometry and chain validation | PASS | Final R3AR package, SHA-256 50ce3433… |
| PyFatFS read-only FAT32 parse | PASS | 87 files, 6 directories; exact content comparison |
| R3AR production FAT/VFS qualification | 17/17 PASS | Final R3AR package |
| R3AQ production SQL query-service harness | 50/50 PASS | Final R3AR package |
| R3AI production VFS regression | 22/22 PASS | Existing pinned R3AI baseline image |
| R3AK FAT mount regression | 73/73 PASS | Existing pinned old-geometry baseline image |
| SQL Debug CTest | 39/39 historical PASS | Existing accepted SQL qualification; not rerun because SQL production source was unchanged |

The final-package VFS test used the real RAM disk block layer, production FAT driver, VFS, native SQL file bridge/provider, and query service. It verified FAT32 classification, a read-only mount with no block write callback, exact fixture/manifest reads, rejected unlink and block write, successful point/range diagnostic, handle cleanup, and unchanged image bytes. It reports:

    R3AR_GEOMETRY bps=512 spc=1 reserved=32 fats=2 fat_sectors=937 total=120832 first_data=1906 data_sectors=118926 clusters=118926 cluster_class=FAT32 root=2

The final-package SQL harness compared the complete ordered point and range vectors, tested invalid encodings, wrong fixture/index/root/owner identities, unsupported live access, insufficient output capacity, controlled missing/corrupt fixture images, injected read/close failures, and valid recovery queries. It reported nativeSqlR3aqServiceChecks=50/50. The existing diagnostic compares every expected locator before emitting ordered=PASS; these results are not count-only checks.

A temporary exploratory hosted OPEN_WRITE-then-close probe crashed the test process before the formal 17/17 run. The exact call site was not isolated. The formal checks therefore cover rejected VFS unlink, rejected block writes, read-only registration, and the service's read-only paths; production VFS code was not changed. This probe remains an out-of-scope read-only VFS limitation to track separately.

## Server builds and candidate artifacts

Two ordinary production-rule AMD64 builds completed:

| Build | Flags | Result |
| --- | --- | --- |
| Ordinary | ARCH=amd64 BUILD_DIR=build/amd64-r3ar-ordinary | Build PASS; no qualification flags, R3AR SQL diagnostic/service symbols, or strings |
| Qualification | ARCH=amd64 BUILD_DIR=build/amd64-r3ar-qualification GX_NATIVE_SQL_READ_QUALIFICATION=1 GX_NATIVE_SQL_READ_DIAGNOSTIC=1 GX_NATIVE_RAMDISK_PROVENANCE_TRACE=1 GX_NATIVE_RAMDISK_MAPPING_TRACE=1 GX_SQL_DATABASE_ROOT=D:/dev/guideXOS_SqlDatabase | PASS; gated read-only service and diagnostics linked |

nm found the qualification SQL entry points defined and absent from the ordinary build. objdump -p showed no PE import directory, and objdump -r showed no PE relocations. The 47 undefined COFF symbol records in the qualification PE are identical to the accepted R3AQ qualification baseline; no new unresolved symbol identities were introduced. This reports the inspected symbol table and PE directories rather than claiming that nm -u is empty.

The existing R3AQ mapping loader was reused unchanged. The ESP was separately reverified with release_esp.py verify; the ISO was separately reverified with the standard-library ISO/El Torito verifier. The ISO report's boot-image SHA-256 matches the ESP SHA-256, and the ESP file report matches the final kernel, loader, and FAT32 RAM disk hashes, proving that the frozen ISO contains this package.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| R3AR UEFI ISO | 100,730,880 | 75ead34a8f22888c1d7856a1d5e0afacf4ae44baa0b2c786e84872c5a10acf1b |
| ESP image | 100,663,296 | 3fe264b7ed32ff88e62b5a05a14cfed53b2642353d998e28b17cc51e1ff339bf |
| Qualification kernel PE | 2,872,895 | 77a263b9e9afb99fdcf8a45f7aed9f780b864f576cc50ad23acb6c23d51a3c69 |
| Qualification kernel ELF | 2,763,264 | a74532deea4bd89425376b14b4c084edd585aba097fb5df98f8b5e36f43b75f2 |
| Ordinary kernel PE | 2,774,807 | df40840c613ff6c43240924ffd93a7d5b084621bc5cd511b7cb26ad72f98b717 |
| Ordinary kernel ELF | 2,670,368 | 43db7160b3620107a9d5af8de68c2f154f20e20dcec685e3919242f809214561 |
| Existing UEFI loader | 81,920 | 65225cb759294360f8c6cf1b17165d53d7320ce99b96b80dda9293da8af7845e |
| FAT32 RAM disk | 61,865,984 | 50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c |
| SQL fixture | 250,880 | cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57 |
| Trusted SQL manifest | 12,937 | 43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3 |
| Candidate manifest | 17,695 | 1e36eb7d40743257a60cc4d8efbd7f124295b4f24a1e228dda9c98411851979d |
| Packaging provenance | 15,771 | 668bb7e1c1dc9d0e9acbe1e144fae4ed51e64558a18ac844b400b55644025a30 |

The ordinary PE/ELF sizes and hashes, package manifest, independent validation report, ESP report, ISO report, and candidate manifest are retained under kernel/build/amd64-r3ar-*, out/ndb1r4p11r3ar/fixture-package-r3/, and E:\CodexArtifacts\NDB1R4P11R3AR\candidate\.

## Single guest run and failure frontier

Immediately before launch, QAC 2.0 was online with zero QEMU processes, reservations, managed sessions, queued reservations, or blocked sessions. QMP framing passed a local socket preflight using UTF-8 without BOM and CRLF-delimited JSON. The frozen ISO hash was checked again. The accepted R3AQ pflash configuration was used: OVMF.fd and edk2-i386-vars.fd were read-only pflash drives, and the ISO was read-only. No EDK2 image was passed as a legacy BIOS image.

QAC granted reservation 004ad49c8323474596da38a2d6b16d09 and launched the single QEMU child (PID 18592). The live QMP session successfully exchanged qmp_capabilities and query-status; the raw transcript contains UTF-8 JSON/CRLF frames with no BOM or parse error.

Observed guest evidence:

- UEFI loaded the kernel and full 61,865,984-byte RAM disk; BootInfo v3 carried the virtual alias and original EfiLoaderData physical backing.
- MAP-E PTE samples at the alias start, next page, and final page mapped the intended physical frames with writable=0 and user=0. The BootInfo pointer mapping also matched its backing and was read-only. CR0=0x0000000080010033 has WP bit 16 set.
- The mount driver's successful LBA 0 read reported 512-byte sectors. Runtime geometry reported clusters=0001D08E, cluster_class=FAT32, a successful FAT mount, and VFS read_only=1.
- The mapping diagnostic emitted SQL_R3AO_MAP_RESULT=FAIL.
- The existing SQL diagnostic subsequently emitted point 73 ordered=PASS, range 32 ordered=PASS, invalid-key rejection with unchanged output, a valid 73-locator request after rejection, and SQL_NATIVE_R3AQ END PASS. Its implementation compares every locator against the trusted ordered vectors.
- The guest kernel does not halt when this mapping trace fails, and the host controller evaluated the completed serial trace at the SQL service marker. The same run therefore continued through FAT/VFS and SQL output before the host check recorded the failed mapping gate and sent QMP quit. This stop was later than the observed failure frontier; the deviation is recorded here. No second guest was launched.

### Cause of the required mapping-gate failure

The mapping trace in Server kernel/core/main.cpp compares the first 16 bytes at the mapped RAM-disk alias to a hard-coded R3AQ fingerprint (expectedBytes, lines 888–898). That fingerprint is:

    EB58904755494445584F530002102000

It encodes the previous OEM bytes GUIDEXOS, 512-byte sectors, and 16 sectors per cluster. R3AR's valid formatted sector begins:

    EB58904D5357494E342E310002012000

It has the PyFat OEM bytes MSWIN4.1, 512-byte sectors, and the required one sector per cluster. The exact byte comparison sets mappingEvidencePassed=false even though the independently logged MAP-E translations, read-only permissions, physical backing, and CR0.WP evidence pass. The guest result is therefore a mandatory qualification failure, not an indication that the R3AR volume was classified as FAT16 or that the alias PTEs were writable.

The unmodified mapping checker cannot accept the required 512-byte cluster geometry because its expected byte vector pins the old SPC=16 BPB byte. Do not increase the disk to preserve that old geometry. The next bounded repair is to decouple this mapping diagnostic from the legacy BPB fingerprint: keep checking alias-to-backing translations and permissions there, and validate the actual BPB fields through the FAT qualification. Then create a new candidate and a new one-boot acceptance lifecycle.

## Cleanup, retained evidence, and next milestone

The QAC-owned child exited with code 0 after QMP quit; QAC released its reservation. The post-run census showed zero QEMU processes, zero reservations, and zero managed, queued, or blocked sessions. No other QEMU process was present. The tested ISO remained at SHA-256 75ead34a…; the source fixture and trusted manifest retained their expected hashes.

Raw evidence is retained under E:\CodexArtifacts\NDB1R4P11R3AR\evidence\:

- serial-r3ar.log — 50,991 bytes, SHA-256 2f6b3726b5cf303b70068739ef4a34b3b4170ba674f84255ccf30622ce2f0fec.
- qmp-r3ar.jsonl — 1,652 bytes, SHA-256 cbac27a7fa4fd91eec3684b1ee95ab71155719403f8f223e783596075ab5d02e; includes greeting, exact QMP frames/replies, and controlled quit.
- qac-run-r3ar-transcript.txt — reservation, child PID, exit, and release; SHA-256 a7919a88c6ea3461e5a3daffb2e03b5d75170377b2c0957af85ae6a2405ae885.
- qac-pre-run-* and qac-post-run-* — authoritative QAC censuses.
- guest-result.json and guest-reconciliation.json — result and failure-cause reconciliation.
- qmp_r3ar_control.py and launch_r3ar_qac_guest.ps1 — the one-run QMP controller and QAC launch configuration.

Recommended next milestone: fix the stale mapping-trace fingerprint in a separate Server qualification change, rerun hosted package/mapping checks, and perform one new QAC-admitted guest acceptance. Once that FAT32/mapping gate is closed, resume the next native SQL read-only feature from the project plan. No SQL functionality was added in R3AR.