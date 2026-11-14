# NDB1R4P11R3BC — Application-Facing Native SQL Read API

**Outcome:** `NATIVE_SQL_APPLICATION_API_HOSTED_VERIFIED` (Outcome A)

This is an implementation and hosted/build qualification milestone. It adds a
read-only SQL API to the existing Native ELF host ABI and exercises it from a
real SDK application entry point. No QEMU boot or QAC run was performed in
R3BC.

## Repository baseline and final revisions

| Repository | Branch | Starting HEAD | R3BC implementation HEAD |
|---|---|---|---|
| SQL | `main` | `5e0acd49c263caa79bc7b22f3ae3bf6b3d316713` | `d61cf3bfb02abc75bec2dfabd44d6686a3a1694d` (implementation; report commit follows) |
| Server | `codex/ndb1r4p11rah-sql-read-integration` | `d40557a1710f8c21fe34fdf5be8e9c12f7f592dd` | `7a6726049ef48e2549dd5571c7c5d28c09507a53` |

The SQL implementation commit and Server commit IDs, plus the final SQL report
commit ID, are included in the R3BC closeout. The report cannot contain its own
Git object ID; it is the final SQL HEAD after the implementation commit.

No reset, rebase, stash, clean, push, or merge was performed. Existing unrelated
worktrees and generated qualification/build outputs were preserved. The frozen
R3AW image was not modified. Initial untracked Python cache directories in the
SQL checkout were left untouched.

## Existing App Model and chosen call path

Native ELF applications receive a `gx_app_context` and an append-only
`gx_host_calls` table. The table already provides the App Model's supported
kernel-service entry points; this milestone appends four SQL callbacks instead
of adding a second application framework, a separate SQL engine, or a boot
diagnostic interface.

The production call path is:

1. Native ELF package metadata is parsed by `native_elf_baremetal.cpp`; the
   existing manifest permission parser records `database.read`.
2. The runtime builds its normal host-call table and installs the SQL
   callbacks. It attaches the exact kernel-owned app context to a bounded SQL
   principal for the duration of the application entry call.
3. `gx_main` calls the public `gx_host_calls` SQL slots declared in the SDK.
4. The kernel service checks the exact active context, manifest permission,
   caller buffer spans, operation arguments, and opaque session ownership.
5. The service resolves fixed database and index identities internally and
   invokes `gxNativeSqlValidateDatabase` or `gxNativeSqlExecuteRead`.
6. The production SQL page provider uses the Server native file bridge and
   read-only VFS/block path. Provider handles are opened and closed for each
   request; SQL/VFS pointers are not retained in the app session.
7. On return from `gx_main`, the runtime detaches the principal, consumes its
   handles, and frees the runtime image and stack.

The older boot-time diagnostic remains available in qualification builds as a
regression path. The new application callbacks do not use diagnostic markers
or predefined diagnostic vectors to produce results.

The hosted App Model harness calls the compiled sample's real `gx_main` entry
using the same public ABI table and the production callback installation,
principal/session service, SQL reader, file bridge, and VFS. The harness does
not exercise the guest ELF loader or boot an OS image; guest execution is the
next R3BD qualification step.

Alternatives considered: direct app calls into the boot diagnostic would not
be application-facing; a second app API or SQL parser would duplicate current
interfaces and add unsupported capability; a retained VFS/provider object
would extend lifetimes beyond the existing per-request SQL resource model.

## Public API contract

The public declarations are in `sdk/include/guidexos/sql.h` and the four
append-only function slots are at the end of `gx_host_calls`:

- `sql_open_read_only_database(ctx, databaseIdentity, flags, outHandle)`
- `sql_close_database(ctx, handle)`
- `sql_lookup_index(ctx, handle, indexIdentity, typedKey, flags, results, capacity, outCount)`
- `sql_scan_index_range(ctx, handle, indexIdentity, typedRange, flags, results, capacity, outCount)`

The ABI table grows from 248 to 288 bytes. SQL API slots are at offsets 256,
264, 272, and 280 bytes respectively. Existing slots retain their offsets.

The first supported database identity is
`GX_SQL_DATABASE_R3AH_MULTILEVEL`. The service accepts only
`GX_SQL_OPEN_READ_ONLY`; it rejects other flags and unknown database IDs. The
service validates the approved image using the production read-only provider,
image header validation, known internal index roots, and mount/block write
denial before issuing an application handle.

The public index identities are `GX_SQL_INDEX_ROWS_K64` for Int64 point lookup
and `GX_SQL_INDEX_ROWS_K32` for Int32 range scan. Keys use an explicit
`GX_SQL_KEY_INT32` or `GX_SQL_KEY_INT64` tag and reject null/unsupported
encodings. Ranges have optional lower and upper bounds and explicit inclusive
bits; reversed bounds are rejected. The caller supplies
`GX_SQL_QUERY_ORDERED_LOCATORS`, a result buffer, and capacity. The current
limit is 73 locators, matching the production reader's bounded result contract.
The service returns an explicit `gx_sql_status`, count, and actual ordered
`gx_sql_locator { page_id, slot }` values.

Statuses distinguish success, no match, invalid argument/database/index/
handle, permission denial, capacity exhaustion, storage failure, corrupt
index, unsupported operation, and bounded-resource exhaustion. There is no
general SQL text execution or write API.

## Ownership, authorization, and safety

The kernel keeps up to 8 principal contexts and 16 global database sessions.
Application handles contain only a slot and generation; they do not encode a
kernel pointer. Each session is associated with an internal runtime owner ID.
Duplicate active owner IDs are refused; invalid and stale generations are
rejected; cross-owner query and close attempts fail. Explicit close consumes
the handle. Detaching an app closes all its sessions. No SQL provider or VFS
handle remains open between calls.

Before invoking SQL, the callbacks check operation flags, identities, typed
key shape, range order, capacity (1–73), required pointers, overflow-safe span
arithmetic, and handle ownership. The App Model buffer callback accepts only
spans within the active image or runtime stack. Results are staged in a
bounded kernel buffer and copied only after full successful execution, so
invalid requests, storage failures, and insufficient capacity do not publish
partial locators. Empty results return `NO_MATCH` without altering locator
storage. The output count is initialized before dispatch.

The app manifest's `database.read` bit is enforced at open/query time using the
existing manifest mechanism. Database IDs and catalog roots are resolved by
the service; callers cannot supply paths, physical addresses, VFS handles, or
index roots. The approved image remains the only supported database.

Security boundary: Native ELF applications execute in the kernel address
space. The manifest is self-declared package metadata, not a signed publisher
identity or administrator-issued authorization. Runtime owner IDs and
image/stack span checks are useful API ownership and bounds checks, but they do
not provide ring separation, page-level isolation, or protection from a
malicious kernel-mode application. The implementation does not claim otherwise
or add a new user-management/trust system.

## Demonstration application

`sdk/samples/sqlread/` adds the Native ELF `SQLReadDemo`, including an
`app.json` with the `database.read` permission. Its `gx_main` calls only the
public SDK host slots. It opens the approved read-only fixture, supplies an
Int64 point key and Int32 inclusive range, examines actual first/last returned
locators, checks a no-match key, verifies invalid key rejection leaves the
result buffer unchanged, succeeds with a subsequent valid request, closes the
database, and verifies that the closed handle is rejected.

The sample is built with the existing SDK sample toolchain and staged at:

`out/ndb1r4p11r3bc/sdk-apps/SQLReadDemo/bin/amd64/sqlread.elf`

It was inspected as ELF64 AMD64 `ET_EXEC`; `gx_main` is defined and `nm -u`
reported no undefined symbols. ABI layout assertions verify that existing
fields remain fixed and the appended slots occupy the expected offsets.

## Hosted and build qualification

The dedicated hosted test is `tests/native_sql_r3bc_application_api_test.cpp`,
run by `scripts/run-native-sql-r3bc-application-api-test.ps1`. It links the
production SQL reader, index provider, Server file bridge, production FAT VFS,
and the real SDK sample entry. It covers:

- Actual SDK app call through the public host table; point, ordered range,
  no-match, invalid request with unchanged result buffer, valid recovery,
  explicit close, and stale/closed-handle rejection.
- Different supported keys and singleton/reversed bounds; unknown database
  and index IDs; missing permission; null key; result-capacity exhaustion;
  integer wraparound; duplicate principal and bounded handle table behavior.
- Cross-owner query/close rejection, generation invalidation, repeated
  open/close, app-detach cleanup, VFS read and close failures, recovery after
  injected failures, and no leaked VFS handles.
- Exact fixture/package byte preservation.
- The existing R3AQ service regression: exact 73-point and 32-range locator
  sequences, invalid-request immutability, subsequent recovery, fail-closed
  writable mount, missing/corrupt fixture rejection, cleanup, and byte
  preservation (`nativeSqlR3aqServiceChecks=50/50`).

The complete SQL Debug suite was freshly configured and built in
`build/ndb1r4p11r3bc-debug`. The first full CTest run passed 38/39 tests. Its
only failure was `database_ndb1r4p11r3ag_native_link`, which initially lacked
the expected `kernel/build/amd64/bin/kernel.pe` baseline in the isolated
Server checkout. After producing that new API-disabled baseline, the legacy
test refused attribution because the original checkout has preserved
pre-existing untracked qualification artifacts. I kept those artifacts and
used a clean detached worktree at the final Server commit under the SQL Debug
build directory, with the locally installed ignored Mbed TLS dependency
exposed through a junction. I built that worktree's API-disabled baseline,
reconfigured the R3BC CTest build to the clean source snapshot, and reran test
23; it passed. Together the full run and the targeted CTest rerun cover all
39/39 test cases. The native-link report records 17 freestanding source
compiles, matching Server bridge exports, a PE and ELF link, and zero added
undefined imports. No existing Server checkout or qualification output was
cleaned or overwritten.

The Server ABI layout test passed. Both ordinary AMD64 and qualification AMD64
kernel builds completed and emitted `kernel.elf` in separate R3BC build
directories. The qualification build included the existing diagnostic path
for regression; no guest was launched. No database write route was added.

Commands used:

```powershell
cmake -S . -B build/ndb1r4p11r3bc-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DGX_GUIDEXOS_SERVER_SOURCE_DIR=D:/dev/guideXOSServer_SQL_READ_RC_NDB1R4P11RAH
cmake --build build/ndb1r4p11r3bc-debug --parallel 6
ctest --test-dir build/ndb1r4p11r3bc-debug --output-on-failure
cmake -S . -B build/ndb1r4p11r3bc-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DGX_GUIDEXOS_SERVER_SOURCE_DIR=D:/dev/guideXOS_SqlDatabase/build/ndb1r4p11r3bc-debug/r3ag-server-clean
ctest --test-dir build/ndb1r4p11r3bc-debug `
  -R '^database_ndb1r4p11r3ag_native_link$' --output-on-failure

powershell -NoProfile -ExecutionPolicy Bypass -File `
  sdk/build-samples.ps1 -SampleName SQLReadDemo `
  -OutputRoot out/ndb1r4p11r3bc/sdk-apps -Verbose

powershell -NoProfile -ExecutionPolicy Bypass -File `
  scripts/run-native-sql-r3bc-application-api-test.ps1 `
  -CandidateRamdisk out/ndb1r4p11r3aq/candidate/esp-source/ramdisk.img `
  -MissingFixtureRamdisk out/ndb1r4p11r3ai/missing-test/ramdisk.img `
  -SourceFixture D:/dev/guideXOS_SqlDatabase/tests/fixtures/NDB1R4P11R3AH/multilevel.gxdb

mingw32-make ARCH=amd64 BUILD_DIR=build/amd64-r3bc-ordinary `
  GX_SQL_DATABASE_ROOT=D:/dev/guideXOS_SqlDatabase
mingw32-make ARCH=amd64 BUILD_DIR=build/amd64-r3bc-qualification `
  GX_SQL_DATABASE_ROOT=D:/dev/guideXOS_SqlDatabase `
  GX_NATIVE_SQL_READ_QUALIFICATION=1 GX_NATIVE_SQL_READ_DIAGNOSTIC=1

# Legacy native-link regression baseline, in the clean detached Server worktree:
mingw32-make ARCH=amd64 BUILD_DIR=build/amd64 `
  GX_SQL_DATABASE_ROOT=D:/dev/guideXOS_SqlDatabase GX_NATIVE_SQL_READ_API=0
```

## Existing qualification and remaining work

R3BB remains Outcome B because independent QMP Gate 9 was unverified after a
writable sidecar mapping was encountered. The independent-walker investigation
remains a separate qualification item. R3BC does not change or close that item,
and does not represent R3BB as full 20/20 acceptance.

R3BD guest acceptance should use a new phase-specific candidate and preserve
the frozen R3AW image. Its procedure should:

1. Package this SDK ELF and the approved immutable fixture into a separately
   identified candidate without changing BootInfo v3, RAM disk mapping,
   FAT32 geometry, or the accepted sidecar arrangement.
2. Boot that candidate in the planned guest environment and launch
   `com.guidexos.samples.sqlread` through the real Native ELF loader.
3. Capture evidence that the app's own public host-call requests enter the
   production SQL service and that the app receives and checks live point and
   range locators, no-match, invalid-request immutability, recovery, close, and
   stale-handle results.
4. Verify application-exit cleanup and fixture byte preservation in the
   guest, while keeping Gate 9's independent walker result separately visible.

Remaining prerequisites are guest execution evidence in R3BD and the separate
R3BB Gate 9 investigation. General databases, SQL statements, writes, user
identity, signed app trust, and process isolation remain outside this API.

## Commits

- Server implementation: `7a6726049ef48e2549dd5571c7c5d28c09507a53` on
  `codex/ndb1r4p11rah-sql-read-integration`.
- SQL native-service implementation: `d61cf3bfb02abc75bec2dfabd44d6686a3a1694d`
  on `main`.
- This report is committed after the SQL implementation and is the final SQL
  HEAD. Its own Git object ID is provided in the R3BC closeout because a commit
  cannot include its own ID in its content.

All changes are committed locally. Nothing was pushed or merged.
