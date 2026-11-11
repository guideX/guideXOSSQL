# NDB1R4P11R3AW — Native SQL Manifest VFS Read Qualification

**Outcome:** `NATIVE_SQL_MANIFEST_VFS_READ_HOSTED_VERIFIED` (Outcome A)<br>
**Phase:** `NATIVE_SQL_TRUSTED_MANIFEST_GUEST_READ_PREPARATION`<br>
**Date:** 2026-11-11 (America/Los_Angeles)

R3AW closes the hosted/build prerequisite missing from R3AV: the qualification kernel now performs an actual bounded read of the packaged SQL manifest through the production read-only VFS before issuing SQL fixture requests. The new ISO is frozen for R3AX. No QEMU launch, QAC admission, or guest execution occurred in R3AW; guest access is not yet established.

## R3AV gap and repository provenance

R3AV Outcome B had one unverified acceptance gate: the guest had opened `/system/Apps/R3AI/MULTI.GDB`, but its evidence did not show an open and read of `/system/Apps/R3AI/FIXTURE.MAN`. R3AW adds that manifest access gate before the existing SQL point/range/negative/recovery sequence. It does not change the SQL query algorithms, FAT32 driver, mapping implementation, database format, or SQL production source.

| Repository | Starting branch and HEAD | Ending branch and HEAD |
|---|---|---|
| SQL | `main`, `ecf93148d06b4ccf77e59dce35f32c478ae56b86` (tracked tree clean; branch was ahead of origin by two existing commits) | `main`; this report is the final SQL commit. Its exact resulting HEAD is recorded in the R3AW closeout message. |
| Isolated Server | `codex/ndb1r4p11rah-sql-read-integration`, `e1c8c4a71c1a01fec7d7a6ddce095fd5c4986082` | `codex/ndb1r4p11rah-sql-read-integration`, `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd` |

The Server source was committed in `99f6ddead0ba89bbcb0124d19058a6557f689c73`; a second commit corrected a transcribed R3AR validation-report hash before packaging. The SQL source tree was not changed. SQL portability/database phase markers remain at their existing P11/SQL10 state. Existing untracked Server qualification outputs were preserved; no reset, rebase, stash, clean, merge, or push was performed. The unrelated Server checkout was not modified.

## Manifest and package identity

The authoritative file is exactly 12,937 bytes, SHA-256 `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3`.

| View | Exact path |
|---|---|
| FAT32 package | `/Apps/R3AI/FIXTURE.MAN` |
| Guest production VFS | `/system/Apps/R3AI/FIXTURE.MAN` |
| SQL source fixture | `tests/fixtures/NDB1R4P11R3AH/fixture.manifest` |

Its format is a versioned UTF-8 text key/value manifest beginning with `GUIDEXOS_SQL_FIXTURE_MANIFEST_V1`; it is not JSON. It identifies fixture `NDB1R4P11R3AH_NATIVE_SQL_READ`, records two database images and 17 query cases, and includes image lengths, SHA-256 values, format/catalog metadata, table/row counts, and index metadata. Its `image.1` entry identifies `multilevel.gxdb`, 250,880 bytes, SHA-256 `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57`, 512-byte pages, 490 pages, and 80 rows. The packaged database is `/Apps/R3AI/MULTI.GDB`; the guest mount prefix `/system` exposes it as `/system/Apps/R3AI/MULTI.GDB`.

R3AR package manifests and provenance were checked against the actual files before the hosted build and again before candidate creation. The ISO verifier later extracted the ESP from the ISO, extracted `ramdisk.img` from that ESP, mounted the extracted RAM disk as FAT, and independently re-read both SQL files. The embedded manifest and fixture matched their pinned lengths and SHA-256 identities. These host checks establish package identity; they do not substitute for the R3AX guest VFS read.

## Production VFS read and resource lifetime

The qualification-only implementation in `kernel/core/native_sql_read_diagnostic.cpp` uses the Server's existing VFS and block-device interfaces:

1. Confirm `/system` is an active FAT32 read-only mount backed by a RAM disk with a read callback and no write callback.
2. Call `vfs::stat` on the fixed, case-sensitive manifest path; require a regular file of exactly 12,937 bytes.
3. Call `vfs::open(path, vfs::OPEN_READ)`. Obtain the VFS handle, require it to be open, regular, read-only, attached to the mount selected for the fixed path, and to report the expected size. Confirm `vfs::file_size` agrees.
4. Stream the file through `vfs::read` using a fixed 1,024-byte stack buffer. Partial positive reads are accumulated. A zero count before all 12,937 bytes, a negative result, or a count larger than requested fails the gate.
5. After exactly 12,937 bytes, issue a one-byte read. Zero confirms EOF; a byte beyond the expected file or an I/O error fails.
6. Call `vfs::close` on every successfully opened handle, including paths that subsequently fail handle, size, read, or EOF checks. A close failure is itself a failure. No operation accesses the handle after close.

The path and expected size are compile-time constants. No user-supplied path, whole-file allocation, filesystem write, STL, exception, or RTTI is used by the native path. The file contents are consumed into the bounded buffer but are not printed. The ordinary Server build does not compile in this diagnostic.

### Access and identity scope

The guest diagnostic establishes access only: a read-only production VFS handle returned the full expected number of bytes and reached EOF before closing. It does not compare the bytes with a trusted digest, parse the manifest, or establish runtime content authenticity/provenance. The kernel's existing Mbed TLS SHA-256 implementation is coupled to its TLS profile; reusing it here would add a runtime content-integrity gate and coupling that R3AV does not require. No hash subsystem or alternate manifest representation was added. The controlled package provenance, exact offline hashes, and R3AX candidate-integrity gate remain responsible for identity.

Hosted package checks reject a same-length altered manifest by comparing the full byte sequence with the trusted source, and packaging scripts pin SHA-256. This host-side negative case is explicitly not a runtime identity claim. A different same-length file could pass the guest's access-only reader; R3AX must first verify the exact frozen ISO and then require the guest access markers below.

## Serial sequencing and fail-closed behavior

The read is inserted at the start of `run_after_vfs_mount`, after the existing mapping, package, and VFS initialization prerequisites and before SQL fixture operations. A successful run emits, in order:

```text
SQL_NATIVE_R3AW MANIFEST READ BEGIN path=/system/Apps/R3AI/FIXTURE.MAN expected_bytes=12937 access_only=1
SQL_NATIVE_R3AW MANIFEST OPENED path=/system/Apps/R3AI/FIXTURE.MAN bytes=12937 mode=READ
SQL_NATIVE_R3AW MANIFEST BYTES_READ path=/system/Apps/R3AI/FIXTURE.MAN bytes=12937 read_calls=14 eof=1 close=PASS
SQL_NATIVE_R3AW MANIFEST READ PASS path=/system/Apps/R3AI/FIXTURE.MAN bytes=12937 eof=1 close=PASS content_identity=NOT_CHECKED
```

The production FAT read takes 13 bounded data reads plus one EOF probe. No manifest content is sent to serial. The explicit `content_identity=NOT_CHECKED` field distinguishes file access from authenticity.

Stat, type, size, open, handle-contract, file-size, premature EOF, read, extra-byte, EOF-probe, and close errors report a specific manifest failure code. The diagnostic then emits `SQL_NATIVE_R3AQ BLOCKED reason=MANIFEST_READ_FAILURE`, the existing final `SQL_NATIVE_R3AQ END FAIL`, and returns `Failed` without issuing dependent SQL requests. This failure contributes to the existing overall qualification result, so later unrelated success cannot turn the combined gate into PASS.

## Hosted tests and regressions

The R3AW hosted suite mounts the actual R3AR image with the production read-only RAM disk, FAT32 driver, VFS, and SQL service. It compares the VFS-returned manifest and database bytes with the trusted source, exercises the reader twice, verifies read-call count/EOF/cleanup, and confirms the backing image remains byte-identical. Injected read-operation faults exercise failure propagation and resource cleanup; the real SQL qualification succeeds after the controlled failures.

| Fresh run | Result |
|---|---:|
| R3AW manifest VFS read suite, using RAM disk extracted from the final R3AW ISO | **99/99** |
| R3AQ read-only SQL service | **50/50** |
| R3AR production FAT32/VFS | **17/17** |
| R3AI production VFS | **22/22** |
| R3AU page-table sidecar | **28/28** |
| R3AS mapping/package diagnostics | **31/31** |
| R3AO boot-payload mapping | **36/36** |
| VFS file resize/lifetime regression | **171/171** |
| Hosted VFS read regression | **4/4** |

The historical SQL Debug CTest aggregate remains **39/39 (historical)**; it was not rerun because no SQL production dependency changed, as directed. The older counts above are recorded here as fresh R3AW executions, not carried forward from R3AV.

## Builds and linkage

Both AMD64 builds succeeded using the existing freestanding build restrictions and settings.

| Build | Result |
|---|---|
| Ordinary Server | Qualification and manifest diagnostic flags absent; fixture not required; no R3AW serial markers or PE import directory. |
| R3AW qualification Server | Existing BootInfo v3, version-2 sidecar, full-extent mapping, FAT32 package, and SQL read-only qualification retained; R3AW manifest gate enabled before SQL. |

PE output is PE32+ x86-64; ELF output is ELF64 x86-64 with entry `0x100000`. R3AW PE images have zero imports. Undefined-symbol identity sets match R3AU exactly: 21 ordinary symbols and 22 qualification symbols, with no additions or removals. The qualification kernel and Server source are local-only; nothing was pushed or merged.

| Image | Bytes | SHA-256 |
|---|---:|---|
| Ordinary `kernel.pe` | 2,774,807 | `680a74cf3ff717d6b2f0c0d8ae1eef48da528b44eef1d3943801f3883c2d0ea4` |
| Ordinary `kernel.elf` | 2,670,368 | `43db7160b3620107a9d5af8de68c2f154f20e20dcec685e3919242f809214561` |
| Qualification `kernel.pe` | 2,886,019 | `a39a71495b82c4f404c7158479df153c929565425c53aace2f0480175ba48312` |
| Qualification `kernel.elf` | 2,773,768 | `d6d8632684af6dded22c2a3f795461e5599a461ba38009d29062cd052c47e152` |

The unchanged R3AU UEFI loader was reused byte-for-byte; it is 83,968 bytes with SHA-256 `f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77`.

## Frozen R3AW candidate and preserved R3AU baseline

Candidate directory: `E:\CodexArtifacts\NDB1R4P11R3AW\candidate`<br>
ISO: `E:\CodexArtifacts\NDB1R4P11R3AW\candidate\guideXOS-NDB1R4P11R3AW-amd64.iso`

| Candidate artifact | Bytes | SHA-256 |
|---|---:|---|
| UEFI ISO | 100,730,880 | `60ffbbbd16ff7f3c66a99ebf1f48389f807811afed62fc14e9d3e6c11d93a207` |
| ESP image (`EFI-BOOT.IMG`) | 100,663,296 | `6e89f79d4e84ab328e8b756c4a90d14047460f130ad1e644547b4bac1e8454db` |
| R3AR RAM disk | 61,865,984 | `50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c` |
| SQL fixture `MULTI.GDB` | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted SQL manifest `FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |

The ISO verification reports a bootable UEFI El Torito entry (`0xEF`, no-emulation, `UEFI_BOOT.IMG`). The independent content verifier extracted and verified the actual ESP and RAM disk inside the ISO, then verified both `/Apps/R3AI` SQL files. The candidate manifest is `E:\CodexArtifacts\NDB1R4P11R3AW\candidate\r3aw-boot-candidate-manifest.json`; it pins the repository commits, builds, loader, inputs, ESP, ISO, expected serial markers, and the R3AX-only execution boundary.

The R3AU ISO was not modified and still hashes to `749f909c38d20c9c6e61426ceec85bf397fea7112f8c2ec36aca22b45273194e`. R3AV evidence under `E:\CodexArtifacts\NDB1R4P11R3AV\attempt-20261111-acceptance` was preserved.

## QMP parser checkpoint

R3AV's correlated raw QMP `info registers` response represented CR0 as `80010033` and CR4 as `00000668`, both valid eight-digit hexadecimal values. The qualification helper in `scripts/r3au_qmp_evidence.py` requires exactly 16 hexadecimal digits for those controls and rejected these valid representations. The raw response and parser finding remain at `E:\CodexArtifacts\NDB1R4P11R3AV\attempt-20261111-acceptance\qmp-control-register-parser-findings.json`; raw correlated QMP recovery evidence remains in that attempt directory (`qmp-observations-recovery.jsonl` and recovery result files). No synthetic data was used.

This host evidence-tool defect was kept separate from R3AW and was not changed. For R3AX, retain the raw correlated QMP frames and interpret the observed 8- or 16-digit control-register value without rewriting or replacing the captured response. Require live CR3 agreement with the MAP-E record and `CR4.LA57=0`; do not allow the parser's width assumption to invalidate otherwise correlated raw evidence.

## R3AX guest acceptance procedure

Use the exact frozen R3AW ISO and preserve all R3AV memory, storage, SQL, and cleanup requirements. Do not accept host verification as a substitute for guest manifest access. Run one QAC-exclusive guest execution and require every gate:

1. Recompute the frozen R3AW ISO's exact length and SHA-256 and verify its candidate manifest and embedded ESP before admission.
2. Obtain exclusive QAC acceptance admission. Reconcile reservation, owner PID, managed QEMU child, and executable identity before opening QMP; do not launch QEMU outside QAC.
3. Require the existing UEFI BootInfo v3 handoff validation.
4. Require version-2 page-table sidecar verification, including the active root, exact frame set/capacity, bounds, uniqueness, checksum, and agreement with live walks.
5. Require all 15,104 RAM-disk page bases and a separate final-byte probe, including complete extent bounds.
6. Require the previously failing 2 MiB boundary inspection and correct PD transition.
7. Correlate active CR3 and CR4 from the guest marker and live QMP response; preserve raw request/response frames and require the expected four-level paging mode (`CR4.LA57=0`).
8. For every inspected RAM-disk PTE, require supervisor-only and read-only effective permissions, correct physical progression, no kernel-image overlap, and the established read-only alias contract.
9. Require active `CR0.WP=1`.
10. Validate the full 512-byte RAM-disk boot sector against the pinned R3AR package expectation.
11. Require FAT32 classification and successful production read-only `/system` VFS mount backed by a read callback and no write callback.
12. Require the production VFS to open `/system/Apps/R3AI/FIXTURE.MAN` read-only, retrieve exactly 12,937 bytes using bounded reads, observe EOF, close successfully, and emit `SQL_NATIVE_R3AW MANIFEST READ PASS` before any final SQL acceptance marker. Any manifest failure requires overall FAIL.
13. Require production VFS access to `/system/Apps/R3AI/MULTI.GDB` and the pinned fixture length.
14. Require the exact ordered 73-locator point lookup and cleanup.
15. Require the exact ordered 32-locator range scan and cleanup.
16. Require invalid-request rejection with zero results and unchanged caller output.
17. Require the valid recovery request after the negative request to succeed with the expected ordered result.
18. Scan the complete serial output for mandatory FAIL/BLOCKED, panic, unexpected fault, and memory-fault markers; any such marker prevents acceptance.
19. Require the final combined mapping, package, RAM-disk, manifest, and SQL qualification PASS markers, with manifest PASS preceding final SQL acceptance.
20. Perform controlled QMP shutdown; verify the owned guest exits, QAC releases the same reservation, unrelated QAC resources remain unchanged, and the frozen ISO hash still matches.

For the complete mapping proof, retain R3AV's required live walks at the alias base, former 2 MiB boundary, final page, and final byte, including effective writable/user bits, frame ownership, and boundary checks. Preserve every raw QMP JSONL frame. Do not soften any R3AV mapping, FAT32, SQL, negative/recovery, contradictory-marker, shutdown, or QAC-release condition to accommodate the new gate.

## Commits

- Server source, tests, and packaging support: `99f6ddead0ba89bbcb0124d19058a6557f689c73`.
- Server candidate-input hash correction: `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd` (final Server HEAD).
- SQL: this committed report is the only R3AW SQL repository change; the resulting SQL HEAD is recorded in the R3AW completion response. No push or merge was performed.
