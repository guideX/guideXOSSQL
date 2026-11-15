# NDB1R4P11R3BE — Native SQL Application Lifecycle Qualification

**Outcome: NATIVE_SQL_APP_LIFECYCLE_HOSTED_VERIFIED (Outcome A)**

The production SQL host callbacks now emit qualification-only dispatch evidence, and the SDK qualification sample returns normally with one SQL session still open. Hosted tests call the real SQL service and its production detach function, verify that detach releases the outstanding owner session, and reject the released handle generation. The ordinary and qualification AMD64 kernels and the Native ELF sample build successfully. A separately packaged R3BE ISO is independently verified and ready for a separately authorized R3BF guest run.

R3BE was implementation, hosted testing, and candidate preparation only. No QAC admission was requested and no QEMU guest was launched.

## Source identities

| Repository | Branch | Starting HEAD | Ending revision |
|---|---|---|---|
| SQL | main | 7007d1940a66c51e46e89524a9c4d9aebbdc9a01 | This documentation-only local commit; its exact ID is recorded in candidate-manifest.json |
| Server | codex/ndb1r4p11rah-sql-read-integration | bf51863a3801958c3af960a82cad05f84f45449a | af0e1706d0ebac475a2b205938b14fa6471dc8af |

The SQL repository has no implementation changes in this phase. The final SQL HEAD is the local commit that adds this report. A commit cannot contain its own Git object ID, so candidate-manifest.json records that exact final SQL HEAD. Both changes are local; nothing was pushed or merged.

## R3BD baseline and guest boundary

R3BD ended as NATIVE_SQL_APPLICATION_API_HOSTED_READY_GUEST_PENDING. Its SDK app, service checks, builds, and candidate packaging passed. The final admission census found another managed acceptance guest, guideXOSUEFI-FONTS8R2. The R3BD runner stopped before QAC submission; no R3BD guest launched, and its candidate remains preserved at E:\CodexArtifacts\NDB1R4P11R3BD.

R3BE did not request QAC admission or start QEMU. The earlier R3BD admission allowance does not authorize an R3BE or R3BF launch. The new candidate is for a separate R3BF acceptance with fresh authorization and exclusive admission.

## Production application and teardown path

The supported route is Start → All Programs → SQL Read Demo. The bare-metal desktop refreshes the package-backed Start Menu list, maps the selected visible row to the app, and routes it through the shared App Model resolver. A NativeElfApp target reaches native_elf::launch in kernel/core/native_elf_baremetal.cpp. The packaged app manifest identifies com.guidexos.sqlread, display name SQL Read Demo, architecture amd64, entry gx_main, ABI guidexos-c-abi-v1, runtime native-elf, and permission database.read.

The Native ELF runtime creates its normal context and host table, installs the public SQL callbacks, attaches the exact app context to a bounded principal using the runtime sequence as owner identity, and invokes gx_main on the runtime stack. When gx_main returns normally, run_package records the return, performs window cleanup, calls native_sql_application::detach on the production context, checks the returned cleanup result, and only then frees the app stack and image. A failed app return or failed detach cannot produce lifecycle PASS.

The R3BE guest launch procedure is deliberately interactive: boot the frozen ISO, click Start, choose All Programs if that view is not already open, select SQL Read Demo, and click it once. The demo is not auto-started. The hosted tests do not execute the desktop or the Native ELF loader; the actual guest launch remains for R3BF.

## Qualification-only SQL dispatch evidence

The production application-facing SQL service now emits bounded markers through the app’s existing host log callback. In the bare-metal runtime that callback reaches the normal serial log. The dispatch sequence is a nonzero 32-bit counter. The owner field is the existing runtime sequence, not a kernel pointer. No private address, handle value, or database contents are emitted.

The callback markers distinguish the public operations:

- OpenReadOnlyDatabase: ENTER, service target SQL_VALIDATE_DATABASE, then RETURN with status.
- CloseDatabase: ENTER, service target OWNER_SESSION_RELEASE, then RETURN with status.
- LookupIndex: ENTER, service target SQL_EXECUTE_READ with INT64 and ROWS_K64 typing when the request reaches the reader, then RETURN with status and count.
- ScanIndexRange: ENTER, service target SQL_EXECUTE_READ with INT32 and ROWS_K32 typing when the request reaches the reader, then RETURN with status and count.

Invalid requests that are rejected before production dispatch still emit the actual callback return status and zero result count. They do not emit a SQL_EXECUTE_READ service marker. Markers are written at the real dispatch and return sites; they are not sample-generated PASS messages.

At normal return, the production service emits SQL_APP_LIFECYCLE DETACH_BEGIN with owner_sessions_before. It then performs the existing owner-session table cleanup and emits DETACH_END with cleanup=PRODUCTION, sessions_released, owner_sessions_after, provider_refs_retained, and status. The Native ELF caller emits GX_MAIN_RETURN_DETECTED before detach and APP_DETACH_COMPLETE after it. A PASS status comes from the actual app result and DetachResult, not from a constant marker.

The kernel Makefile now passes GX_NATIVE_SQL_READ_QUALIFICATION to the kernel callback and loader objects only in qualification builds. The ordinary AMD64 image contains none of SQL_APP_API, SQL_APP_LIFECYCLE, GX_MAIN_RETURN_DETECTED, or APP_DETACH_COMPLETE. The qualification kernel contains those markers and the four operation labels.

## Public ABI and service limits

The SDK app statically checks the existing layout:

| ABI element | Verified layout |
|---|---:|
| gx_app_context | 24 bytes |
| sql_open_read_only_database | offset 256 |
| sql_close_database | offset 264 |
| sql_lookup_index | offset 272 |
| sql_scan_index_range | offset 280 |
| gx_host_calls | 288 bytes |

The host table prefix and callback signatures remain unchanged. The internal detach result type does not alter the public SDK ABI or API version.

The production service continues to allow only the approved immutable R3AH database, read-only opening, Int64 point lookups on K64, and Int32 range scans on K32. It retains the existing opaque generation-tagged handle representation, owner checks, result-capacity rules, explicit error statuses, 16-session limit, and 73-locator result limit. It does not add SQL text parsing, writes, WAL mutation, recovery, or new data types. The file/page provider and VFS handles are opened and closed per request; app sessions retain no provider or VFS reference.

## SDK sample and lifecycle scenario

The ordinary SDK sample keeps the successful demonstration flow and closes its first database session. The opt-in build flag -SqlLifecycleQualification is accepted only with SQLReadDemo. In that qualification build, after the existing capacity, point, range, no-match, invalid-request, recovery, explicit-close, and closed-handle checks, the sample opens a second approved database session, verifies the returned handle, leaves it open, logs that it is returning normally with one session outstanding, and returns GX_OK. It does not call detach or any cleanup helper.

The sample checks the ordered 73-locator point reference using FNV-1a digest 0x61532064D1753604 and the inclusive [1,2] range reference using digest 0xD4841B43ACFFF3A6. It also checks the empty result, invalid key rejection without result mutation, successful recovery, explicit close, and stale closed-handle rejection.

## Hosted qualification and regression results

The R3BE harness compiles the SDK sample entry point with the real production Native SQL service, production read-only SQL provider, production VFS, and the approved fixture. It passed the callback ordering and actual result checks, including open, capacity rejection, 73-row point lookup, 32-row range scan, empty result, invalid request, recovery, explicit close, closed-handle rejection, and the second open left outstanding at return.

After gx_main returns, the hosted harness explicitly calls the production detach function and checks the production session table: one session before cleanup, one released, zero afterward, and no provider reference retained. It then reattaches and verifies the released handle generation remains invalid. Three repeated hosted app-return/detach cycles also release exactly one session each and reject stale generations. Missing callbacks, invalid owners, cross-owner close, session exhaustion, partial open failure, and unexpected detach failure cannot produce an application PASS.

This is hosted proof of the production detach implementation after a normal sample return, not execution of the bare-metal loader call site. The loader’s return → detach → completion sequence was source-audited and the qualification kernel contains the caller markers. R3BF must observe that sequence during a real Start Menu launch.

| Check | Fresh result |
|---|---|
| R3BE application lifecycle and embedded R3AQ production-VFS service regression | PASS; nativeSqlR3aqServiceChecks=50/50 |
| R3BC hosted application API regression | PASS; nativeSqlR3aqServiceChecks=50/50 |
| Standalone R3AQ production-VFS query-service regression | PASS; nativeSqlR3aqServiceChecks=50/50 |
| App Model application actions | PASS; 55/55 |
| App Model identity | PASS; 25/25 |
| Hosted VFS read | PASS; 4/4 |
| Native database file bridge | PASS; 53 checks, zero failures |
| Native filesystem contract | PASS |
| Native ABI layout | PASS |

The Native filesystem contract runner had omitted PSA built-in include directories and the winmm library present in build-native-experimental.bat. Its flags now match those required paths and library, and the full Native ELF filesystem contract build and test passes.

No SQL database source changed, so the full SQL Debug CTest aggregate was not rerun. The accepted R3AQ SQL Debug result remains 39/39 historical; it is not a fresh R3BE run. The earlier R3BC campaign recorded 38 passes in its original full run plus a targeted 1/1 rerun, not one uninterrupted 39/39 run. Fresh R3AQ production-VFS service coverage is listed above.

## AMD64 builds and ABI verification

The SDK qualification build succeeded with sdk/build-samples.ps1 -SampleName SQLReadDemo -SqlLifecycleQualification. llvm-readobj identifies the app as ELF64 x86-64, gx_main is defined, and llvm-nm reports no undefined app symbols. The staged app manifest has the expected amd64 NativeElf entry and database.read permission.

Both final kernel builds used fresh output directories and the explicit all target. The ordinary build used GX_NATIVE_SQL_READ_API=1 with qualification and diagnostic flags disabled. The qualification build enabled the SQL API, SQL qualification diagnostics, RAM-disk provenance and mapping traces, and R3AS packaging. Both builds exited with code 0.

The qualification kernel contains gxNativeSqlValidateDatabase, gxNativeSqlExecuteRead, the production Native SQL host callback functions, and the callback/lifecycle markers. Its PE import table is empty. The converted kernel ELF symbol inventory has the same baseline unresolved-reference entries in both builds; the qualification-only serial formatting reference resolves to an in-image definition. No new unresolved external dependency was introduced. The SDK app has no undefined symbols.

## Frozen R3BE candidate

The final candidate is E:\CodexArtifacts\NDB1R4P11R3BE\candidate-final. It was built from a separate copy of the R3BD runtime source tree, with only the SQLReadDemo package refreshed and the qualification kernel embedded. The R3BD candidate was not modified.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| R3BE ISO, guideXOS-NDB1R4P11R3BE-amd64.iso | 100,730,880 | 10d4a361c612c496ac2f39cb2cc9f272cd61d7166920c7458ad2bb6e8982e197 |
| ESP image | 100,663,296 | ee66d90fc5c88e552eca44bdfb69ee93813d2ed6865dfeb7d308729ab2db7988 |
| UEFI loader | 83,968 | f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77 |
| Ordinary kernel PE | 2,840,067 | 9190c882d5e0f2632d044ca143697d210c1a85fab950d4eacb383dfb58f65423 |
| Ordinary kernel ELF | 2,725,208 | 0785124fd68ec3d46e66d0437ca07451c05ef00d285eeb119f0eafebf6d6118f |
| Qualification kernel PE | 2,901,245 | 194421af83440a8a8498fbb684f59e61939a4e0e64e5656eec7f15a1f8527361 |
| Qualification kernel ELF, embedded in ESP | 2,785,000 | 6e77de41b3278178483040afe9e6a15b82bb2ccdf2df4de22f863f6937530802 |
| SDK SQLReadDemo ELF | 7,312 | 96812b627bda58bc6e176271c94fa4fb92d352fa07fae1a301bb41d29584c13e |
| RAM disk | 61,865,984 | 902387311c6fb014f83fcb4f02541834f2fe6a6d2da5363748d99995aee7c237 |
| Database fixture MULTI.GDB | 250,880 | cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57 |
| Trusted manifest FIXTURE.MAN | 12,937 | 43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3 |
| RAM-disk packaging report | 16,038 | dd71a7ef859e9f2eebd2f46482e647e7941aa846940cc254c393fd5b8cad297e |
| Embedded content verification report | 2,100 | 202d0933b9c75bd48283abe36f90304fad56685cf5422c17bdaa068285101070 |

The candidate has the app at /Apps/SQLReadDemo/app.json and /Apps/SQLReadDemo/bin/amd64/sqlread.elf. The app manifest is 756 bytes with SHA-256 0951fccc0cb92ebe847dcaf17da5daa7eea53cecf955b33ce506678b1355ce68. The RAM-disk report independently reopened all 89 files as FAT32 with 512-byte sectors, one sector per cluster, two FATs, and 118,926 data clusters. The database fixture and trusted manifest match the prior accepted byte identities.

The ESP verifier and ISO verifier passed. The ISO is UEFI El Torito, no-emulation, with boot image UEFI_BOOT.IMG. The independent embedded verifier extracted the ESP from the ISO, verified the embedded loader, qualification kernel and RAM disk, reopened the RAM disk, and verified the app manifest, exact SDK ELF, fixture and trusted SQL manifest against their source identities. candidate-manifest.json records the full candidate, reports, source commits, and SHA-256 values.

## R3BF guest acceptance procedure

R3BF is a separate phase and requires fresh explicit authorization plus exclusive QAC admission. Use the current qualified QAC ownership helper and the established QMP lifecycle. Verify the QAC reservation, managed QEMU child PID, and QMP endpoint belong to the same R3BF owner before sending input. Start at most one guest. Do not launch QEMU directly or retry if input or evidence is uncertain.

Before that single launch, verify candidate-manifest.json and the frozen ISO hash above, verify the accepted OVMF identity, capture current QAC status/list/queue evidence, and confirm no other managed guest is active. Boot the frozen ISO, confirm UEFI and Server startup, then use guest mouse input to follow Start → All Programs → SQL Read Demo. Select the row once.

Require the SDK-side SQLReadDemo pass messages and the production SQL_APP_API serial markers for the same runtime owner and ordered operation sequence. Check the 73-locator K64 point case and its expected digest, inclusive Int32 [1,2] range with 32 locators and its expected digest, no-match, invalid request without output mutation, successful recovery, explicit close, and stale-handle rejection. Require the second open and the sample’s normal return message.

Then require GX_MAIN_RETURN_DETECTED, DETACH_BEGIN with one owner session present, DETACH_END with one session released, zero owner sessions afterward and zero retained provider references, followed by APP_DETACH_COMPLETE status=PASS and the ordinary application lifecycle PASS. Preserve serial and QMP evidence. Use QMP-controlled shutdown, verify the QEMU child exits, and verify QAC releases the same reservation. If any launch or evidence step is uncertain, stop; do not spend a second launch.

## Separate issues and security limits

R3BB Gate 9 remains UNVERIFIED. Its independent QMP walker stopped at writable sidecar mapping 0x3DD3F003 before the RAM-disk page-table/boundary witness. R3BE did not run that walker and does not close Gate 9. R3BF application success must not be used to mark it resolved.

Native ELF code still runs in kernel address space. This work establishes neither user/kernel memory isolation nor signed application identity. The manifest permission is package metadata, not publisher authentication. The current API remains an approved read-only service with fixed identities and bounded typed operations.

Final local source commits are recorded in candidate-manifest.json and the R3BE completion record. No push or merge was performed.
