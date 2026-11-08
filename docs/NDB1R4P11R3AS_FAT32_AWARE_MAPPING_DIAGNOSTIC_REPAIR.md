# NDB1R4P11R3AS FAT32-Aware Mapping Diagnostic Repair

**Outcome:** `NATIVE_SQL_FAT32_DIAGNOSTIC_REPAIR_HOSTED_VERIFIED`

**Guest acceptance:** Pending R3AT. No QEMU or QAC execution occurred during R3AS.

## Repository boundary and commits

| Repository | Starting branch / HEAD | Result |
|---|---|---|
| SQL database (`D:\dev\guideXOS_SqlDatabase`) | `main` / `30b2cf33043f243fbe80808361a0cfd29c008b0f` | SQL production source unchanged; this report is committed locally on `main` |
| Isolated Server integration (`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH`) | `codex/ndb1r4p11rah-sql-read-integration` / `5a95548056f108156bd9fec2bd8793d9ba98ec8c` | `12df41bdc29d6608055d0dbbe16d997c661a219d` |

The separate active Server checkout was not modified. Existing untracked Server output directories were preserved. No reset, rebase, stash, clean, push, or merge was performed.

The R3AS candidate was built from Server commit `12df41bdc29d6608055d0dbbe16d997c661a219d` and SQL source commit `30b2cf33043f243fbe80808361a0cfd29c008b0f`. The final SQL report commit is the commit containing this file; its ID is recorded in the repository's Git history and in the completion summary.

## R3AR failure and stale expectation

R3AR remains `NATIVE_SQL_FAT32_GUEST_FAILED`. Its captured serial log contains `SQL_R3AO_MAP_RESULT=FAIL`, followed by successful FAT/VFS and SQL checks. That result is not changed by this repair.

The stale comparison in the pre-repair `kernel/core/main.cpp` was a hardcoded 16-byte BPB/OEM fingerprint:

```text
EB 58 90 47 55 49 44 45 58 4F 53 00 02 10 20 00
```

It was copied from the R3AQ package. The `0x10` byte at offset 13 encodes 16 sectors per cluster. The independently verified R3AR volume uses one sector per cluster and a different OEM field. Its exact first 16 bytes are:

```text
EB 58 90 4D 53 57 49 4E 34 2E 31 00 02 01 20 00
```

The old function mixed two separate claims: whether the alias translated to the expected physical RAM disk and whether the first bytes matched the old FAT package. It sampled the BootInfo pointer and a small set of RAM-disk addresses for mapping/permissions, then made the mapping result depend on the R3AQ byte fingerprint. It did not prove every page of the RAM-disk extent, and the function itself did not read CR0.WP. R3AR's guest evidence separately showed CR0.WP enabled, correct read-only supervisor samples, FAT32 classification, and successful SQL operations.

R3AR package facts reverified from the exact RAM-disk image and trusted package reports:

| Field | Verified value |
|---|---:|
| RAM disk | 61,865,984 bytes; SHA-256 `50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c` |
| Boot sector | 512 bytes; SHA-256 `369abefec78107f9a884d3ae2e69029c3509d7273d5ddb2cad843241825e5c62` |
| Bytes per sector | 512 |
| Sectors per cluster | 1 |
| Reserved sectors / FAT copies | 32 / 2 |
| Sectors per FAT / total sectors | 937 / 120,832 |
| First data sector / data clusters | 1,906 / 118,926 (FAT32) |
| Root cluster / FSInfo / backup boot sector | 2 / 1 / 6 |
| Volume ID | `0x47585750` |

The package manifest, packaging provenance, and independent validation report were also checked against their pinned SHA-256 values. The SQL fixture and trusted manifest match the previously reviewed source hashes.

## Corrected expectation and mapping contract

The package-specific expectation is committed at `kernel/core/include/kernel/native_ramdisk_package_expectation_r3as.h`. `scripts/generate-r3as-native-ramdisk-expectation.py` reads the exact R3AR RAM disk and package reports, verifies their fixed hashes and phase identity, validates BPB geometry, and emits the complete 512-byte expected sector. Its normal verification mode fails if the committed header differs from that output. It does not add a general-purpose manifest interpreter or kernel cryptographic subsystem.

At runtime, the diagnostic requires the explicit `GX_NATIVE_RAMDISK_PACKAGE_R3AS=1` build selection. It validates the package identity and expected length, parses and compares all listed FAT geometry, and compares every byte of the 512-byte boot sector. R3AQ compatibility is exercised only when the R3AQ expectation is explicitly selected in hosted tests. The R3AQ expectation cannot satisfy R3AS checks by accident.

The runtime does not hash all 61,865,984 RAM-disk bytes. Full-image identity is established during hosted package verification and again for the packaged ESP file; the kernel runtime check is limited to mapping plus exact boot-sector/package metadata. This limitation is recorded in the expectation report.

The new shared, freestanding validator keeps mapping evidence separate from package evidence. It checks:

- BootInfo magic, v3 fields, mapping contract, RAM-disk flag, and expected size.
- The active CR3 and the BootInfo page-table sidecar identity before page walks.
- The BootInfo pointer's translated physical backing, supervisor access, and read-only PTE.
- CR0.WP enabled.
- Canonical, non-overflowing virtual range and non-overflowing physical range.
- Exact expected physical backing for each 4 KiB boundary in the RAM-disk extent and the final payload byte.
- Read-only supervisor PTE permissions at every walked RAM-disk address.
- No physical overlap between the RAM-disk backing and the physical kernel-image range. The hosted suite also points an incorrect translation into the kernel-image physical range and requires rejection.
- Exact package length, selected package identity, expected geometry, boot-sector signature, and all 512 expected bytes.

Mapping results and package results have separate failure names and serial markers. A readable sector cannot make a wrong mapping pass; a correct mapping cannot make a wrong package pass.

## Failure propagation

For the R3AS qualification build, the mandatory mapping/package result gates the SQL service diagnostic. On a failed check, the kernel reports the mapping/package reason, reports the RAM-disk diagnostic failure, skips later RAM-disk alias consumers (including provenance reads and wallpaper/config/certificate handoff), skips the SQL qualification operation, emits a blocked marker, and emits `SQL_NATIVE_R3AS QUALIFICATION END FAIL`. A later SQL result cannot override a failed gate. The kernel is not deliberately panicked to report the expected failure.

The existing BootInfo v3 contract check still runs before the R3AS diagnostic. An invalid top-level BootInfo contract follows the pre-existing early halt path; it cannot reach a successful SQL qualification result. Ordinary startup is compiled without R3AS mapping/package tracing and keeps its existing startup path.

## Source changes

Server changes committed in `12df41bdc29d6608055d0dbbe16d997c661a219d`:

- `kernel/core/include/kernel/native_ramdisk_mapping_diagnostic.h` — package and complete mapping validation.
- `kernel/core/include/kernel/native_ramdisk_package_expectation_r3as.h` — generated R3AR boot-sector expectation.
- `kernel/core/include/kernel/native_sql_qualification_gate.h` — qualification result gate.
- `kernel/core/main.cpp` and `kernel/Makefile` — opt-in diagnostic wiring, result reporting, and failure gating.
- `scripts/generate-r3as-native-ramdisk-expectation.py` — fixed-input expectation generation/verification.
- `scripts/prepare-native-sql-r3as-boot-candidate.ps1` — R3AS-only candidate assembly and manifest.
- `tests/native_ramdisk_mapping_diagnostic_test.cpp` and `scripts/run-native-ramdisk-mapping-diagnostic-test.ps1` — production page-walker/validator hosted tests.
- `tests/native_sql_r3am_ramdisk_provenance_test.cpp` — phase test's expected first bytes updated to the R3AR RAM disk used by this fresh run. The focused compatibility suite retains explicit R3AQ selection.

No FAT driver, SQL production source, SQL data, or SQL index was changed.

## Hosted qualification

The final focused suite compiled the production page-table walker and validator and passed **31/31**. It covers exact R3AR bytes and geometry, explicit historical R3AQ compatibility, package switching, wrong cluster geometry, complete sector corruption, invalid length and expectation metadata, BootInfo v3, every page boundary plus the endpoint, wrong backing, kernel overlap, missing mappings, writable/user mappings, CR0.WP disabled, range overflow, and failure/success of the SQL qualification gate.

Fresh hosted regressions in this phase:

| Regression | Result |
|---|---:|
| R3AO mapping | 36/36 |
| R3AN PTE provenance | 34/34 |
| R3AM byte provenance | 34/34 |
| R3AR production VFS with the exact R3AR package | 17/17 |
| R3AQ SQL read-only query service with the R3AR package | 50/50 |
| R3AI VFS regression with pinned R3AQ image/missing-fixture case | 22/22 |
| R3AK FAT mount/read-only regression | 73/73 |

The first R3AM attempt still expected R3AQ's first bytes and failed its five package-byte assertions. The R3AM test expectation was corrected to match the R3AR image selected for this phase, then the complete 34/34 suite passed. R3AQ compatibility remains independently tested in the new 31-case suite. SQL full Debug CTest **39/39 is historical** and was not rerun: SQL production code and shared SQL dependencies were unchanged.

## Build qualification

Both AMD64 kernel builds succeeded with the existing freestanding and linker configuration. Both UEFI loader builds succeeded in separate output directories.

| Build artifact | Bytes | SHA-256 |
|---|---:|---|
| Ordinary kernel PE | 2,774,807 | `f7314102fd189a8919fe37bb6e0d6afcef3d85fb6ed87e622389d106b6edc584` |
| Ordinary kernel ELF | 2,670,368 | `43db7160b3620107a9d5af8de68c2f154f20e20dcec685e3919242f809214561` |
| Ordinary UEFI loader | 65,024 | `996f473660307a5025b4d606f55f1f71ac254508fd570c95e9227f081ac3fa91` |
| R3AS qualification kernel PE | 2,878,167 | `50621cdfb408ded4552d907f181d6c621f0cb410efff80aa12c779c41997dec5` |
| R3AS qualification kernel ELF | 2,764,480 | `64de3f113a900597e344cea86592c63c93fa9d7fd75b2642323834915d4ff839` |
| R3AS qualification UEFI loader | 81,920 | `f5695f68cd6210f3cccc78598697e6b31187607e861f22da263766cbf6d5d50b` |

The ordinary ELF hash matches the R3AR ordinary baseline. Undefined-symbol identities match the R3AR baseline (21 ordinary, 22 qualification), with no added imports. The ordinary PE contains no SQL/R3AS/mapping diagnostic strings. Both PE import directories have a zero RVA and size. Qualification strings include the R3AS mapping/package/final-result markers.

## Frozen R3AS candidate

Candidate directory: `E:\CodexArtifacts\NDB1R4P11R3AS\candidate`

ISO: `guideXOS-NDB1R4P11R3AS-amd64.iso`

Status: hosted verified, R3AT guest acceptance pending. Candidate files were marked read-only after verification.

| Candidate component | Bytes | SHA-256 |
|---|---:|---|
| R3AS ISO | 100,730,880 | `1388b65415bd7501e4bf3a3dd00d90d2799ea5ef822d46d18b600a82789050f9` |
| ESP image | 100,663,296 | `0435b8961fc2bede92c1cf08b0ef61977922d2d861f8308423cf3d3c12f9c022` |
| Kernel PE | 2,878,167 | `50621cdfb408ded4552d907f181d6c621f0cb410efff80aa12c779c41997dec5` |
| Kernel ELF | 2,764,480 | `64de3f113a900597e344cea86592c63c93fa9d7fd75b2642323834915d4ff839` |
| UEFI loader | 81,920 | `f5695f68cd6210f3cccc78598697e6b31187607e861f22da263766cbf6d5d50b` |
| R3AR RAM disk | 61,865,984 | `50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c` |
| SQL fixture | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted SQL manifest | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| Diagnostic expectation header | 3,895 | `17c2976f34a1f679bac22d8dd1c8c5609bab7eaf6b0789db17d3bfdd45c773ec` |
| Diagnostic expectation report | 1,762 | `09c2c50fbb12036404c566529dccb9ac3369c71ec14d8ac9ac5ff8b91aa5585d` |

The ESP verifier reopened the generated FAT32 ESP and confirmed `BOOTX64.EFI`, `kernel.elf`, and `ramdisk.img`; the embedded RAM disk has the exact R3AR source size and hash above. The ISO verifier confirmed a bootable UEFI El Torito image and the exact ESP size/hash. The final verification record and build/package manifests are in the same read-only candidate directory.

Prior identities rechecked after candidate creation:

| Preserved artifact | Bytes | SHA-256 |
|---|---:|---|
| Frozen failed R3AR ISO | 100,730,880 | `75ead34a8f22888c1d7856a1d5e0afacf4ae44baa0b2c786e84872c5a10acf1b` |
| Original R3AR serial evidence | 50,991 | `2f6b3726b5cf303b70068739ef4a34b3b4170ba674f84255ccf30622ce2f0fec` |
| Accepted R3AQ ISO | 100,730,880 | `32cf00799e5c5677eaf507456bf54380247485febf15e08cdc10537bd8b5beaf` |

The R3AO output directories and all other earlier reports, fixtures, and output trees were left untouched. The R3AS ISO has its own phase name, directory, and hash.

## Remaining runtime uncertainty

Hosted tests validate page-table and package logic against synthetic mappings; they do not establish that UEFI will provide the same live mappings or that a native guest will complete without hardware or memory faults. Runtime walks about 15,104 RAM-disk page starts plus the final byte and should be checked for acceptable boot-time cost. The kernel does not hash the full RAM disk. The R3AR guest's diagnostic failure remains recorded as failed; successful downstream R3AR queries do not count as R3AS guest acceptance.

## R3AT guest acceptance procedure

Use only the frozen R3AS ISO above, after confirming its SHA-256 is `1388b65415bd7501e4bf3a3dd00d90d2799ea5ef822d46d18b600a82789050f9`. Follow the approved QAC admission and cleanup procedure for a single guest. Capture the full serial output and QAC pre/post-run inventory. Do not substitute the R3AR or R3AQ ISO.

Accept only if every item below passes in the same guest run:

1. UEFI boots the candidate and the kernel validates BootInfo v3.
2. The serial evidence identifies the active CR3 and the page-table walk uses it.
3. BootInfo RAM-disk virtual base translates to its declared physical backing at each checked page and at the final byte.
4. BootInfo and RAM-disk mappings are supervisor-only and read-only; CR0.WP is enabled.
5. The observed first 16 bytes are `EB58904D5357494E342E310002012000`; all 512 boot-sector bytes and R3AR geometry pass the selected package check.
6. `SQL_R3AS_MAP_RESULT=PASS`, `SQL_R3AS_PACKAGE_RESULT=PASS`, and `SQL_R3AS_RAMDISK_DIAGNOSTIC=PASS` appear.
7. The production FAT path classifies the image as FAT32 and mounts the boot volume through VFS.
8. The SQL fixture opens with the expected 250,880-byte identity and its trusted manifest.
9. The point lookup returns the complete 73-locator vector in order, matching `fixture.pointIndex.expectedLocators` in the frozen R3AS candidate manifest.
10. The range query returns the complete 32-locator vector in order, matching `fixture.rangeIndex.expectedLocators` in that manifest.
11. Invalid input is rejected with count zero and unchanged caller output.
12. A valid query after the rejected request returns the complete expected point result.
13. The service emits `SQL_NATIVE_R3AQ END PASS` and the wrapper emits `SQL_NATIVE_R3AS QUALIFICATION END PASS`.
14. No mapping, package, VFS, SQL, or memory-fault failure marker appears.
15. The guest exits through the controlled completion path and QAC cleanup leaves no owned guest/process residue.

Any failed or missing check leaves R3AS guest acceptance pending or failed; hosted PASS alone does not promote it.

## Final commits

- Server implementation and hosted test commit: `12df41bdc29d6608055d0dbbe16d997c661a219d`.
- SQL repository: this report is committed locally on `main`; no SQL production source changed. The report commit is the commit containing this file.
- Neither commit was pushed or merged.
