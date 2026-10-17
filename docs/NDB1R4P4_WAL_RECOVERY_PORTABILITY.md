# NDB1R4P4: WAL Recovery Portability

## Outcome

**Outcome A — WAL_RECOVERY_PORTABILITY_VERIFIED.** Production WAL recovery now uses a bounded portable record scanner for prefix decoding, payload scratch, cursor advancement, and record CRC validation. SQL transaction replay and recovered page ownership retain their existing behavior. The scanner compiles with the Server AMD64 freestanding configuration, and the full hosted suite passes.

## Starting state and requalification

The SQL checkout started clean on main at
79b107b4543856f2a261a5590d18c6fd6e45bf69.

| Gate | Observed starting value | Result |
|---|---|---|
| SQL branch and HEAD | main, 79b107b4543856f2a261a5590d18c6fd6e45bf69 | Passed |
| SQL worktree | Clean | Passed |
| SQL .phase | SQL9 → SQL10 | Preserved |
| SQL .native-sql.phase | NDB1R4P3 → NDB1R4P4 | Passed |
| Server reference | NATIVE_DB_STORAGE, f29850c93bbac0a0763a31e806d2816310156575, clean | Read only; preserved |

The complete hosted P3 baseline was rerun before the P4 build: **13/13 CTest targets, 113,734 checks, 0 failures**, in 2,152.82 seconds. The focused baselines remained P1 **20,668**, P2 **305**, and P3 **106** checks.

## Scanner conversion and ownership

Before P4, WriteAheadLog::scan read the entire WAL into a hosted std::vector<uint8_t>, up to the 64 MiB WAL limit. It decoded record prefixes from that vector and passed slices to page and header parsers.

The production WriteAheadLog::scan now calls the portable gxWalScanRecords implementation in database/portable/gx_wal_scan.cpp. The scanner:

- Reads each 20-byte record prefix into fixed local storage.
- Uses one reusable GxArray allocated through the configured GxAllocator for the current record payload, bounded by kMaxWalRecordPayload (65,600 bytes). Empty records allocate no payload storage.
- Checks the file limit, record type, reserved prefix fields, payload maximum, prefix-plus-payload size, payload offset, and next cursor using bounded or checked arithmetic before allocation or reading.
- Reads only the validated payload length, computes the existing record CRC over the original prefix with the CRC field zeroed plus that payload, then synchronously calls the SQL record visitor.
- Reports a short prefix or payload at the known file end as an incomplete tail. A failed read for bytes the file reports as present is IoError; an allocator failure is NoSpace; malformed data and CRC mismatches are WalCorrupt.

The SQL visitor preserves BEGIN/PAGE_IMAGE/DB_HEADER/COMMIT transaction ordering, validates transaction IDs, page-image lengths, page CRCs, header structure, and database UUIDs, and keeps the existing committed-versus-incomplete decision. It copies parsed page data into the existing WalTransaction / DatabasePage ownership before the payload scratch is reused. No pointer into the scan scratch escapes the callback.

Allocation failure clears any partially decoded committed transaction list and returns NoSpace; it does not change the WAL file or checkpoint it. Allocation failures while parsing page data or growing transaction vectors are caught and reported through the same resource-failure result. Existing read-only RecoveryRequired, SQL3 replay ordering, and NDB1R3 partial trailing-page recovery remain unchanged.

The conversion is limited to WAL scan bytes and record parsing scratch. The recovered transaction page vectors, transaction-owned page maps, page cache, catalogs, parser trees, relational operators, and prepared-plan ownership remain outside this phase.

## Fault and recovery evidence

The new database_wal_recovery_portable_test passed **200 checks**. It covers:

- A fixed golden BEGIN record and its metadata, all four WAL record types, exact decoded payload equivalence, and two ordered committed transactions.
- PAGE_IMAGE and DB_HEADER validation, unknown and reserved prefix values, oversized and overflowing length fields, bad CRC, and UUID mismatch.
- Truncated prefixes and payloads, committed data followed by a truncated tail, WAL size-query failure, record-prefix and payload read failures.
- Allocation failure on the initial scratch reserve and on scratch growth after a previously committed transaction has decoded.

Both injected scratch allocation failures returned NoSpace, left the WAL bytes unchanged, returned no transactions, freed all scratch, and succeeded on retry. These are the two converted buffer allocation boundaries: initial reserve and growth reserve. The existing P3 writer allocation-fault test still passes all 106 checks; it disables its writer-specific injected failure before its recovery scan so the P4 scan scratch can be allocated independently.

The final complete hosted run passed **14/14 CTest targets, 113,934 checks, 0 failures**, in 2,419.30 seconds. This includes the SQL3 transaction crash matrix, real heap and index changes, incomplete-WAL discard, partial trailing-page recovery, read-only recovery behavior, and the preserved P1/P2/P3 focused targets.

| Suite | Final result |
|---|---:|
| P1 portable runtime | 20,668 checks, 0 failures |
| P2 WAL record portability | 305 checks, 0 failures |
| P3 WAL page-image portability | 106 checks, 0 failures |
| P4 WAL recovery scanner | 200 checks, 0 failures |
| Full hosted SQL CTest | **14/14 targets; 113,934 checks; 0 failures** |

## Freestanding qualification

Command:

    powershell -NoProfile -ExecutionPolicy Bypass -File tools/compile_portable_freestanding.ps1

All nine selected production objects compiled with the flags from the Server kernel/Makefile and kernel/arch/amd64/Makefile.arch, including -std=c++14 -ffreestanding -fno-exceptions -fno-rtti -nostdlib -nostdinc++, the large code model, no red zone, no MMX/SSE, and GXOS_BARE_METAL. The added production object is gx_wal_scan.o.

The WAL scanner object has only the expected portable cross-object references to GxArray operations and the CRC implementation. The checksum object still references __cxa_guard_acquire and __cxa_guard_release for its existing function-local CRC table; the Server kernel C++ runtime provides these symbols. The selected objects have no hosted STL, exception, RTTI, C allocator, or hosted I/O references.

The hosted database_wal.cpp adapter and the broader SQL9 engine remain hosted: they use std::string, std::vector<WalTransaction>, std::vector<DatabasePage>, virtual IDatabaseFile calls, and hosted page and header parsing interfaces. P4 qualifies the real bounded production scanner component used by that adapter; it does not claim the full engine is freestanding.

## Closeout and next phase

The ordinary SQL phase marker remains SQL9 → SQL10. The independent portability marker advances from NDB1R4P3 → NDB1R4P4.

Recommended P5 scope: define and qualify portable ownership for the recovered transaction page collection, including decode-to-owned-page transfer and allocation-failure handling through replay. Keep catalog, parser, relational container, and prepared-plan ownership out of that boundary until their own phases.

The Server checkout's observed branch, HEAD, and clean status were left unchanged. No branch switches, pulls, fetches, merges, rebases, resets, pushes, or other Git topology operations occurred in either repository.
