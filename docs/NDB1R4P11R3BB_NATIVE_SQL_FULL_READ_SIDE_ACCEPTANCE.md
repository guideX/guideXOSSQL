# NDB1R4P11R3BB Native SQL Full Read-Side Acceptance

**Outcome:** `NATIVE_SQL_R3BB_GUEST_PARTIAL_OR_FAILED` (Outcome B)
**Run date:** 2026-11-14
**Disposition:** One QEMU launch; guest resumed and completed its production SQL qualification. Nineteen gates passed. Gate 9 remains unverified because the independent QMP page-table walk stopped while checking the sidecar mapping, before the RAM-disk boundary walk. No second launch was made.

## Acceptance summary

The R3AW ISO passed preflight and post-run integrity checks. Fresh QAC ownership evidence stayed within the unchanged 2.0-second limit at each QMP-sensitive checkpoint. The guest resumed, reported the production mapping and package diagnostics as PASS, read the complete manifest through the guest VFS, and passed the point, range, invalid-request, and recovery SQL checks. QMP `quit` shut down the owned guest and QAC released the reservation; the final QAC census was clear.

The first unsatisfied gate was Gate 9. The external QMP walker stopped after reading the active registers and attempting to walk the sidecar's identity mapping. Its helper rejected the sidecar leaf because it was writable (`0x3DD3F003`). It did not reach RAM-disk boundary translation or independent RAM-disk permission sampling. This is an incomplete independent witness; it does not establish a RAM-disk mapping defect. The production guest validator separately completed its full mapping diagnostic successfully.

The highest successful contiguous frontier is Gate 8. Gates 10–20 passed on their own evidence, but they do not turn the incomplete Gate 9 into an acceptance pass. R3AZ and R3AZ-R1 historical outcomes are unchanged.

## Repository state

| Repository | State at launch | State after reporting |
|---|---|---|
| SQL `D:\dev\guideXOS_SqlDatabase` | `main`, `c0fc999823f99dd960ecb0b5e84393d4c88363e7` | Report committed locally; final HEAD recorded in the completion message. Pre-existing untracked `tests/__pycache__/` and `tools/__pycache__/` were preserved. |
| Server `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH` | `codex/ndb1r4p11rah-sql-read-integration`, `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd` | Unchanged. |
| QAC `D:\dev\guideXOS_QAC` | `main`, `1ea193258690f9e7b39390b101a117166ef8bbdd` | Unchanged. |

No production source, ISO, or ownership helper was changed. No push or merge occurred. The only SQL repository change is this report.

## Frozen candidate and admission

- Candidate: `E:\CodexArtifacts\NDB1R4P11R3AW\candidate\guideXOS-NDB1R4P11R3AW-amd64.iso`
- ISO: 100,730,880 bytes; SHA-256 `60ffbbbd16ff7f3c66a99ebf1f48389f807811afed62fc14e9d3e6c11d93a207`
- All 20 manifest-pinned paths were unique and matched both before and after the guest run. The packaged ESP matched the pinned ESP byte-for-byte (100,663,296 bytes; SHA-256 `6e89f79d4e84ab328e8b756c4a90d14047460f130ad1e644547b4bac1e8454db`).
- Known guest artifacts: RAM disk SHA-256 `50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c`; SQL fixture SHA-256 `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57`; trusted manifest SHA-256 `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3`.
- Fresh pre-admission QAC census: coordinator online; 0 QEMU processes (0 foreign), 0 reservations, 0 managed sessions, 0 queued, 0 blocked. Exclusive acceptance reservation `785c41ac68d447b0bdb21070f5414e28` was granted.
- One QAC-managed QEMU child launched: owner PID `23288`, child PID `22244`, QEMU start `2026-11-14T19:41:44.8439169Z`, executable `C:\Program Files\qemu\qemu-system-x86_64.exe`.
- The launch used `-S`, read-only OVMF and ISO media, no writable guest storage, and the approved QMP endpoint `127.0.0.1:62526`. QEMU SHA-256 was `a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02`; OVMF SHA-256 was `2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792`.

## Fresh ownership and QMP

The R3BA helper was used without changing the 2.0-second limit. Windows process/command-line/listener observations were followed by new authoritative QAC snapshots and correlation. PID, executable, parent, start identity, endpoint listener, and accepted peer agreed. The accepted peer was `127.0.0.1:62536` to listener `127.0.0.1:62526`, owned by QEMU PID `22244`.

| Checkpoint | Fresh QAC observation (UTC) | QAC age | Result |
|---|---:|---:|---|
| After QMP greeting | `19:42:22.581087` | 0.0 s | Verified |
| Before resume ownership check | `19:42:25.146607` | 0.0 s | Verified |
| Immediately before `cont` | `19:42:27.799799` | 0.0 s | Verified |
| Before live page-table walk | `19:42:40.602671` | 0.0 s | Verified |
| Before QMP shutdown/status check | `19:42:45.550016` | 0.0 s | Verified |
| Immediately before QMP `quit` | `19:42:48.084491` | 0.0 s | Verified |

QMP capabilities and paused status were correlated. `query-status` returned `prelaunch, running:false`; `cont` was transmitted and returned `{}`; the following status returned `running:true`. The guest CPU ran and completed its qualification. Raw correlated frames are retained.

Live registers from QMP `info registers` at `19:42:40.602671Z`:

- `CR0=0x80010033`; WP bit set.
- `CR3=0x000000003DD3C000`; active root `0x3DD3C000`.
- `CR4=0x00000668`; LA57 bit clear, four-level paging.

The independent walker first requested the sidecar VA `0x3DD3F254`, then read the four entries for that translation. The leaf at physical entry address `0x3DBC79F8` was `0x3DD3F003`. The runner stopped with `Sidecar translation/permissions invalid`; it did not sample the RAM-disk page table at `0xFFFF900000200000`, complete the independent RAM-disk walk, or produce an independent boundary result. Gate 9 is therefore marked UNVERIFIED.

## Twenty-gate ledger

| # | Status | Evidence and result |
|---:|---|---|
| 1 | **PASS** | Frozen ISO, embedded ESP, and all 20 unique manifest pins matched before and after execution. See `candidate-integrity-preflight.json`, `candidate-iso-verification.json`, `candidate-esp-verification.json`, and `candidate-integrity-postrun.json`. |
| 2 | **PASS** | Fresh QAC census was clear and exclusive acceptance reservation was granted. See `qac-pre-admission-census.json` and `qac-admission.json`. |
| 3 | **PASS** | Reservation owner PID `23288`, managed QEMU child PID `22244`, executable, parent, process start identity, and running reservation matched. See `qac-admission.json` and ownership snapshots. |
| 4 | **PASS** | Exactly one approved paused `-S` launch; OVMF and ISO read-only; no writable guest storage. See `launch-attempt.json`. |
| 5 | **PASS** | Fresh QAC/Windows/listener/accepted-peer correlation, QMP capabilities, paused status, and `cont`→running transition all passed. Every recorded sensitive snapshot age was 0.0 s (maximum allowed 2.0 s). See `qmp-preconnect-ownership.json`, `pre-resume-ownership-evidence.json`, `ownership-revalidation.jsonl`, and `qmp-resume.json`. |
| 6 | **PASS** | Live QMP registers showed CR0.WP set and CR4.LA57 clear, with active CR3 root `0x3DD3C000`. See `qmp-parsed-frames.jsonl` and `qmp-raw-io.jsonl`. |
| 7 | **PASS** | Guest emitted `SQL_R3AU_TABLE_LIST available=1` with sidecar count `0x21`, capacity `0x1B1`, 30 range samples, and matching root. Production then emitted `SQL_R3AU_MAP_RESULT=PASS`. The executed `ValidateTablePageSidecar` validates version/header/capacity, active root membership, nonzero aligned addresses, uniqueness, physical-address constraints, and page checksum. The active contract marker identifies BootInfo v3. |
| 8 | **PASS** | Production guest emitted `SQL_R3AU_MAP_RESULT=PASS pages_checked=0000000000003B01` (15,105 checks: 15,104 page bases plus one distinct final-byte probe). `ValidateMapping` checks physical progression and RAM-disk permissions for each page and the final byte. |
| 9 | **UNVERIFIED** | Independent QMP page-table walk stopped at the sidecar mapping check before the RAM-disk samples and 2 MiB boundary translation. No independent live boundary result exists. See `runtime-page-table-walk-failure.json` and the raw/parsed QMP frames. This is the first unsatisfied gate. |
| 10 | **PASS** | The production active-CR3 diagnostic emitted `SQL_R3AU_MAP_RESULT=PASS`; its executed validator checks supervisor-only/read-only RAM-disk mappings, CR0.WP, BootInfo read-only mapping, and no physical overlap with the kernel image. Live QMP registers also confirm WP. Gate 9's external independent witness is separately unverified. |
| 11 | **PASS** | Guest read LBA 0; package boot-sector marker reported 512-byte sectors, one sector per cluster, signature `55AA`; FAT geometry reported 118,926 data clusters and `cluster_class=FAT32`; mount registered read-only. Guest `/system/...` opens later succeeded. See complete serial log. |
| 12 | **PASS** | Guest VFS opened `/system/Apps/R3AI/FIXTURE.MAN` read-only at 12,937 bytes, read all bytes in 14 calls, observed EOF, closed successfully, then emitted the required manifest READ PASS before SQL. Production source performs `stat` and validates regular-file/size before emitting OPENED. Marker explicitly says `content_identity=NOT_CHECKED`; this is not a runtime cryptographic-authentication claim. |
| 13 | **PASS** | Guest opened `MULTI.GDB`; fixture length 250,880 bytes, page size 512, page count 490. |
| 14 | **PASS** | Production point request used `IX_Rows_K64`, root 234, owner 1, and emitted 73 ordered PASS locators. The production comparator checks every page ID and slot against the expected vector. |
| 15 | **PASS** | Production range request used `IX_Rows_K32`, root 148, owner 2, and emitted 32 ordered PASS locators. The production comparator checks every page ID and slot against the expected vector. |
| 16 | **PASS** | Invalid key returned `INVALID_REQUEST`, count 0, output unchanged, cleanup PASS. |
| 17 | **PASS** | A valid point request after rejection returned 73 ordered locators with cleanup PASS. |
| 18 | **PASS** | Complete post-shutdown serial log is 50,272 bytes, SHA-256 `fb086268ca5011a4740caa9fcc4b87bb868fe4b282c30ba850451f5ce95920c6`. Required completion markers are present and ordered; no mandatory failure, panic, or fault indicators were found. The manifest BEGIN line has an IRQ diagnostic interleave, but OPENED, BYTES_READ, READ PASS, and subsequent SQL completion markers are intact and ordered. |
| 19 | **PASS** | Required production mapping, package, manifest, and SQL markers are all present and PASS, including `SQL_NATIVE_R3AQ END PASS` and `SQL_NATIVE_R3AU QUALIFICATION END PASS`. This serial result does not override Gate 9. |
| 20 | **PASS** | Fresh ownership check immediately before `quit` was 0.0 s old. Raw QMP records show the exact `quit` transmission, `SHUTDOWN` event (`host-qmp-quit`), and `{}` reply. QAC recorded child exit code 0 and reservation release. Final census showed zero QEMU processes, reservations, sessions, queued, or blocked reservations; a post-run PID check found owner and child absent. No fallback was used. All candidate pins remained unchanged. |

### Production mapping and package observations

The guest reported:

- Ramdisk virtual base `0xFFFF900000000000`, physical base `0x155BC000`, length `0x03B00000`.
- `CR0=0x80010033`, `cr0_wp=1`, `BootInfo_v3`, `UEFI_EfiLoaderData`, `readonly=1`.
- Mapping result PASS with `0x3B01` (15,105) checks.
- Package and RAM-disk diagnostic PASS.
- FAT32 geometry: 512 bytes/sector, 1 sector/cluster, 2 FATs, 118,926 data clusters.

The production source at `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\kernel\core\include\kernel\native_ramdisk_mapping_diagnostic.h` shows that `ValidateMapping` checks BootInfo v3, WP, page-by-page physical progression, read-only supervisor permissions, the last byte, and overlap with the kernel image. `guideXOSBootLoader\ramdisk_pte_provenance.h` contains the sidecar validator. `kernel\core\native_sql_read_diagnostic.cpp` contains the ordered-vector comparator and the runtime manifest-read sequence.

### Shutdown and evidence reconciliation

QMP `quit` was sent at `2026-11-14T19:42:48.084491Z`; QMP reported a `SHUTDOWN` event with reason `host-qmp-quit`, then the `{}` response. QAC reported QEMU exit code 0 at `19:42:48.1561214Z` and released reservation `785c41ac68d447b0bdb21070f5414e28`. The authoritative final census completed at `19:43:37.917660Z` and showed the coordinator online with no QEMU process, reservation, managed session, queued reservation, or blocked reservation. The owner and child PID check at `19:52:19.6991384Z` found neither process.

After shutdown, the runner hit an `UnboundLocalError` while assembling its cleanup result dictionary because that dictionary referenced its own not-yet-created `cleanup` value. QEMU had already exited, QAC had released the reservation, and the runner had already captured a post-run census. The original error is preserved as `runner-error.json`. The cleanup and acceptance records were reconciled from the raw QMP log, QAC output, final census, process check, and final integrity record; the guest was not relaunched.

The evidence root is `E:\CodexArtifacts\NDB1R4P11R3BB`; the attempt is `E:\CodexArtifacts\NDB1R4P11R3BB\attempt-20261114-acceptance`. Key reconciled records are `acceptance-result.json`, `shutdown-qac-cleanup.json`, `serial-final-reconciliation.json`, `qac-final-reconciliation-after-runner-error.json`, and `postrun-owner-child-process-check.json`. Full raw serial and QMP transcripts remain in `serial.log`, `qmp-raw-io.jsonl`, and `qmp-parsed-frames.jsonl`.

## Final classification

`NATIVE_SQL_R3BB_GUEST_PARTIAL_OR_FAILED` is the contract-compliant result. The guest resumed and completed the native SQL read-side checks, but Gate 9 lacks its mandatory independent QMP page-table and boundary witness. There was one launch, no retry, no confirmed RAM-disk defect, no production source change, and no QAC resource left behind.
