# SQL1 Hosted Test Report

Toolchain: MinGW-w64 g++ 15.2.0, CMake 3.x, Ninja, Windows x64.
Command: `ctest --test-dir build --output-on-failure` (or run
`build/database_storage_tests.exe` directly).

Result: **172 checks passed, 0 failed** across 14 test groups.

| Group | What it proves |
|-------|----------------|
| checksum | CRC32 matches the canonical `"123456789"` vector (`0xCBF43926`) |
| creation/reopen | create, durable size/alignment, close, reopen, identity persists |
| creation overwrite guard | existing DB is not clobbered unless explicitly allowed |
| creation invalid arguments | empty path and non-power-of-two / out-of-range page sizes rejected |
| page size boundaries | 512 / 4096 / 8192 / 65536 accepted and sized correctly |
| page I/O persistence | allocate, deterministic payload, flush, close, reopen, nonsequential reads |
| header corruption | magic, version, CRC, page size, root page, page count mutations fail safely |
| page corruption | corrupt root page fails on open; corrupt data page fails on read and full validation |
| truncation | inside header, after header, mid-page, before referenced root page |
| bounds and overflow | absurd page ids, `UINT64_MAX`, one-past-end writes, oversized payloads, closed/read-only ops |
| deterministic identity | caller-supplied UUID and creation time persist exactly |
| repeatability | 40 create/open/write/flush/close/open/verify cycles |
| multiple databases | two databases with distinct identities and payloads, no shared-state contamination |
| diagnostics | inspect reports fields on success and a failure description on corruption |

Notes:

- Tests write real files under the OS temp directory (`%TEMP%/gxos_gxdb_tests`),
  exercising the full on-disk format through `HostDatabaseFile`.
- Corruption/truncation cases assert the specific `DbStatus` failure class.
- The repeatability group runs 40 full lifecycle cycles to surface
  lifetime/state leaks while keeping the suite fast (~4 s).

Verification tiers:

- **Hosted proof: complete** (this report).
- **QEMU proof: not performed.**
- **Bare-metal proof: not performed.**
