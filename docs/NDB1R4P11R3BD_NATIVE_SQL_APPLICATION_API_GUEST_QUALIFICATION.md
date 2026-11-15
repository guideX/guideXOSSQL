# NDB1R4P11R3BD — Native SQL Application API Guest Qualification

**Outcome: `NATIVE_SQL_APPLICATION_API_HOSTED_READY_GUEST_PENDING`**

The genuine SDK application, production SQL callbacks, hosted checks, two AMD64 kernel builds, and a new UEFI candidate all passed their available preflight checks. A fresh QAC census found another team's managed acceptance session already running. No R3BD reservation was requested and no guest was launched. The candidate is preserved for the one permitted guest attempt when QAC is available.

## Source and baseline

| Repository | Starting commit | Candidate source commit | Branch |
|---|---|---|---|
| SQL | `ea787c67e7089caf63f3b5271138168ff26d127e` | `322fdc5959458ad4f1278ad65559d5085aeef256` | `main` |
| Server | `7a6726049ef48e2549dd5571c7c5d28c09507a53` | `bf51863a3801958c3af960a82cad05f84f45449a` | `codex/ndb1r4p11rah-sql-read-integration` |

The Server commit strengthens the R3BC SDK sample and its hosted harness. The SQL commit adds fresh-owner guarded QMP pointer input while preserving the existing two-second ownership snapshot rules. This report is a separate local SQL documentation commit. No reset, stash, rebase, clean, push, or merge was performed. Existing SQL `__pycache__` directories and Server untracked outputs were preserved.

The R3BC historical SQL Debug campaign remains **38 passed in the original full run**, followed by a successful targeted rerun of the native-link test after its missing baseline artifact was produced. It is not represented as one uninterrupted 39/39 run. R3BC itself was hosted-only and did not run QEMU.

## Application, ABI, and production call path

The packaged application is the Server SDK sample `sdk/samples/sqlread/main.cpp`. Its manifest is `com.guidexos.sqlread`, display name **SQL Read Demo**, architecture `amd64`, entry `gx_main`, ABI `guidexos-c-abi-v1`, runtime `native-elf`, and permission `database.read`.

The sample statically checks the AMD64 public layout: `sizeof(gx_app_context) == 24`; the SQL open, close, point, and range slots are at offsets 256, 264, 272, and 280; and `sizeof(gx_host_calls) == 288`. At entry it checks context and table versions, table size through the final SQL callback, `get_api_version`, and every required callback pointer. The four callbacks are appended to the public table after the pre-existing entries.

The intended normal launch is **Start → All Programs → SQL Read Demo**. The package registry supplies the manifest-backed app; `desktop.cpp` resolves the selection and calls `native_elf::launch`. The loader creates the real runtime context and host table, installs the SQL callbacks, attaches an owner identity with the manifest permission and buffer validator, and enters `gx_main`. Those callback implementations validate the context, permission, handle, owner, request, and caller spans before dispatching through `gxNativeSqlValidateDatabase` / `gxNativeSqlExecuteRead` to the production SQL reader and read-only VFS. On `gx_main` return the runtime calls `native_sql_application::detach` before freeing the application stack and image.

This invocation path and the code that will populate the Start Menu were reviewed statically. Because there was no R3BD guest launch, no claim is made that the guest displayed or launched this app.

## Application checks exercised by the hosted harness

The hosted harness invokes the SDK sample entry point with the real production SQL service installed through `installHostCalls`, the production VFS and approved R3AH fixture, and a harness log callback. It is useful hosted evidence, not guest evidence.

The sample passes `databaseIdentity=GX_SQL_DATABASE_R3AH_MULTILEVEL` and `GX_SQL_OPEN_READ_ONLY`, checks a nonzero opaque handle, then supplies an Int64 point key of `7` to the K64 callback. It checks capacity failure without row publication, then all 73 returned locators by count, boundary values, and an ordered-sequence FNV-1a fingerprint (`0x61532064D1753604`). It supplies inclusive Int32 bounds `[1,2]` to the K32 callback and checks all 32 results, boundaries, and the ordered-sequence fingerprint (`0xD4841B43ACFFF3A6`). An exclusive `[1,2]` request produces `NO_MATCH` with sentinel rows unchanged.

The sample then checks no-match for application-supplied key `999`, rejects key type `99` with the output array unchanged, succeeds on another valid point request in the same session, closes the database, and verifies that use of the closed handle returns `INVALID_HANDLE`. The hosted service checks additionally cover owner isolation, generation-tagged handles, provider/VFS failures, buffer bounds, and detach cleanup with a deliberately outstanding hosted session.

The fresh R3BD hosted run used the candidate R3AQ RAM disk, the existing missing-fixture RAM disk, and the trusted SQL fixture. It logged all sample checks above, ended with `nativeSqlR3aqServiceChecks=50/50`, and exited successfully. The complete captured output is under `E:\CodexArtifacts\NDB1R4P11R3BD\hosted\run-output.txt`.

Application-exit cleanup has **not** been demonstrated in a guest. The production runtime does call `detach` at application return, and the hosted harness verified cleanup of a live session, but the SDK sample explicitly closes its only session and the current runtime does not emit a guest-side count of sessions reclaimed by `detach`. This report therefore keeps that guest criterion unverified.

## Builds and candidate identity

The SDK was built with `sdk/build-samples.ps1 -SampleName SQLReadDemo -OutputRoot E:\CodexArtifacts\NDB1R4P11R3BD\sdk-apps -Verbose`. `llvm-readobj` identifies the app as an ELF64 little-endian x86-64 executable; `llvm-nm` shows a defined `gx_main` and no undefined symbols.

Both fresh kernel builds exited with code 0. The ordinary build enabled the public Native SQL API with qualification and diagnostic flags disabled. The qualification build enabled the API, R3BD qualification, SQL diagnostic, provenance trace, mapping trace, and R3AS package. Compiler output contains warnings in unrelated kernel/filesystem code, but no build error; the resulting PE and ELF files are included below.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| SDK `SQLReadDemo/app.json` | 756 | `0951fccc0cb92ebe847dcaf17da5daa7eea53cecf955b33ce506678b1355ce68` |
| SDK `sqlread.elf` | 7,104 | `dca165b48c4db11dc5e765f247e28273d5dcfe92d5e4f9c653c027cfc5b1fed0` |
| Ordinary `kernel.elf` | 2,719,720 | `a3a3a691b4524f78d5e13e0ac842e796f4f499a43a7cf9ffd08afc60583a4ce9` |
| Ordinary `kernel.pe` | 2,834,458 | `cbec332cecdbb4353b090cd90fa52a667eab61c8a70ebcfe8cabc433a9bfb930` |
| Qualification `kernel.elf` | 2,750,776 | `355effb0a94e01b932904fa209170108085994968b8bb5a1fe1e9ac7810af1fe` |
| Qualification `kernel.pe` | 2,863,727 | `a67586820f7e0b747a28674ced55a7517a7e949a8e8beaf4f705cba5e2e443f9` |
| Reused R3AU UEFI loader | 83,968 | `f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77` |
| R3BD FAT32 RAM disk | 61,865,984 | `f9bc166ea9bf81c36fa8d3eb6e774b9910a8762424d8c0ad737d91cfc40d667c` |
| R3BD ESP image | 100,663,296 | `44786c3a00b4be43ddca92da835a9b142d81fe485256a4b4a86e2ef40438ea7c` |
| R3BD UEFI ISO | 100,730,880 | `df2333d26e8b4b35c19e051440179c3fce65c1b011b6d3887f81a16bef6722e5` |

The RAM disk was made in the dedicated R3BD artifact tree from the accepted R3AR runtime files plus the compiled sample at `/Apps/SQLReadDemo`. Independent raw and PyFatFS validation passed: FAT32, 512-byte sectors, one sector per cluster, two FATs, 118,926 data clusters, 89 files, and nine directories. The trusted fixture remains 250,880 bytes with SHA-256 `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57`; `FIXTURE.MAN` remains 12,937 bytes with SHA-256 `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3`.

The repository ESP verifier passed. The ISO verifier confirmed a UEFI El Torito, no-emulation image. I extracted the boot image from the ISO and independently verified it as an ESP, then extracted the embedded RAM disk and checked the app manifest, app ELF, database fixture, and trusted manifest against their expected byte lengths and hashes. The embedded app ELF and manifest match the SDK build outputs byte-for-byte. Reports and the candidate manifests are in `E:\CodexArtifacts\NDB1R4P11R3BD\candidate`; `candidate-manifest-v3.json` records the final prelaunch-blocked outcome while preserving the earlier v2 snapshot.

The affected QMP ownership helper tests had an earlier 20/20 pass during implementation. A later fresh run had 19 passing tests and one timeout in the synthetic Windows process/listener attribution test. A targeted rerun of that integration test passed 1/1 in 37.3 seconds. This Windows timing-sensitive helper result does not affect the built ISO; the existing two-second freshness and ownership correlation checks were not weakened.

## QAC admission and guest frontier

The first fresh authoritative QAC `status`, `list`, and `queue` census was captured at `2026-11-14T23:32:34Z` in `E:\CodexArtifacts\NDB1R4P11R3BD\candidate\qac-census-20261114T233234Z.txt`; it found another team's managed acceptance. A later census at `2026-11-14T23:46:08Z`, `E:\CodexArtifacts\NDB1R4P11R3BD\candidate\qac-final-census-20261114T234608Z.txt`, found QAC clear. Candidate prelaunch checks then verified the immutable OVMF and ISO identities and a free local QMP port.

The required fresh admission census immediately before submission was captured by the runner at `2026-11-15T00:03:01.401Z`. Its full `status`, `list`, and `queue` responses are preserved in `E:\CodexArtifacts\NDB1R4P11R3BD\attempt-20261114-single-launch\qac-pre-admission-census.json`. QAC was online, with one managed QEMU session and no queued or blocked reservations. `list` identified the active owner as `guideXOSUEFI-FONTS8R2` acceptance (reservation `2ce27cb50c5d4078a8c1ae2b9f463778`, QEMU PID 30884). The single-launch runner wrote `prelaunch-blocked.json` and stopped before invoking QAC's `run` command. Thus no R3BD reservation was submitted, no R3BD QEMU process was started, and no guest serial/QMP evidence exists. The change from the earlier clear census is an admission-time race with another managed run; that run was left undisturbed. The final blocked census is also recorded in `timeline.jsonl` in the attempt directory.

| Required guest observation | R3BD status |
|---|---|
| Normal Server boot and App Model initialization | Not observed; no guest launch |
| Start Menu package discovery and Native ELF load | Statically checked; guest not observed |
| Real `gx_main` and public SQL callback dispatch | Hosted through production service only; guest not observed |
| Production VFS page reads and guest point/range/no-match results | Hosted checks pass; guest not observed |
| Guest invalid-input recovery and close/stale-handle behavior | Hosted checks pass; guest not observed |
| Cleanup of an outstanding session on guest app exit | Not observed; hosted detach cleanup passes |
| Guest fault-free completion, shutdown, and QAC release | Not observed; no R3BD reservation |

The classification is Outcome B because the pre-execution candidate and hosted/build checks passed, while the fresh admission guard found QAC occupied and stopped before guest execution. It is not Outcome A. The exact candidate remains preserved for a separately authorized acceptance lifecycle when QAC is available; this blocked lifecycle did not start a guest and produced no guest evidence.

## Security boundary and separate R3BB issue

Native ELF executes in kernel address space. This work does not establish user/kernel isolation, signed application identity, a complete privilege boundary, or protection from malicious native code. The actual service contracts remain opaque generation-tagged handles, context/owner association, manifest `database.read` permission, typed-request validation, caller buffer bounds/capacity checks, read-only provider dispatch, close, and lifecycle detach. The manifest is package metadata, not a signed publisher identity.

R3BB Gate 9 remains **UNVERIFIED**. Its independent QMP walker stopped at the writable sidecar mapping at `0x3DD3F003` before completing the RAM-disk page-table/boundary witness. R3BD did not run that walker and does not change the R3BB evidence or close Gate 9. Track the witness separately from application API acceptance.

After the R3BD guest acceptance is complete, the next bounded SQL functionality milestone should stay within immutable, manifest-approved reads: exercise a second approved fixture or caller-selected index case through the same public API and ownership limits. Do not expand this milestone into database writes or a general SQL parser.
