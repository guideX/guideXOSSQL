# NDB1R3: SQL Storage Contract and Partial-Page Recovery

Baseline: SQL repository `main` at
`a4baf7d37c2476840dde20880761c3a70206b51a`; `.phase` remains
`last_completed=SQL9`, `next_expected=SQL10`. No SQL10 language features are
included.

## Storage interface changes

`IDatabaseFile` now has status-bearing `getSize(uint64_t&)` and `close()`;
`IDatabaseFileSystem::exists(path, outExists)` and `remove(path)` also return
`DbResult`. A failed size request cannot look like an empty file, and a failed
lookup cannot look like absence. Host `close()` checks `fclose`; explicit
`flush()` remains the durability boundary. Positional I/O rejects overflow and
short transfers.

`DatabaseEngine` propagates provider lookup errors, rejects null file objects,
and cleans up failed database initialization through the selected filesystem
provider. Cleanup failure returns `RecoveryFailed`. WAL lifecycle and
diagnostics propagate size and close errors rather than using a numeric
sentinel.

`database/database_native_io.h/.cpp` adds a hosted C++ adapter for the
freestanding Server bridge ABI. `NativeDatabaseFile` and
`NativeDatabaseFileSystem` implement the SQL interfaces over an injected
function table, with explicit bridge-to-`DbStatus` translation. It maps
read-only state, size, exact positional I/O, shrink-only truncate, flush, close,
lookup, and removal. The no-STL bridge remains independently buildable in the
kernel. The function table uses the C bridge's integer enum values; NDB1R4
supplies the final runtime glue and links SQL into the kernel.

## WAL recovery ordering

A relational open previously rejected a database ending in a partial page
before scanning its WAL. That made valid committed page images unreachable
when a crash interrupted physical page growth. It also risked discarding the
only repair evidence if an incomplete WAL was checkpointed before the base
extent had been validated.

Relational open now defers physical page validation until after WAL scanning;
low-level database-file opens remain strict. Committed recovery proves that
WAL page images cover every missing page through the final header page count,
replays committed records, flushes, validates the resulting extent and pages,
loads the catalog, and only then checkpoints the WAL. Read-only committed-WAL
open returns `RecoveryRequired` without modifying either file. Incomplete or
clean WALs cannot authorize repair of a partial tail and are not checkpointed
until the base is valid. Corrupt checksums and database-UUID mismatches fail
closed.

A clean WAL is not rewritten on ordinary reopen or close. `Database::flush()`
only checkpoints when the WAL is not already clean or has new bytes since its
last checkpoint. This keeps clean WAL bytes stable across idempotent reopen
while still checkpointing new committed or incomplete records.

The transaction suite covers a committed partial tail and idempotent reopen,
read-only no-modify, no-commit partial append, truncated/corrupt/header-only
WALs, foreign WAL UUID, repair-write fault with byte snapshots preserved, and
heap/index growth recovery. The same test set injects provider size, lookup,
remove, and close failures. A commit is explicitly WAL-flushed before it is
acknowledged; a later close error is separately reported, and reopening still
recovers the already committed transaction.

## Native naming and limits

The bridge maps an 8.3-compatible SQL path ending in `.gxdb` to `.GDB` and the
matching `.gxwal` to `.GWL` in the same directory. The data and sidecar names
do not collide, and each database has its own WAL. Native FAT32 files are
limited to 32-bit sizes. Writes may extend files; `truncateTo(largerSize)` is
unsupported and returns failure. WAL checkpoint shrinking to its 64-byte
header is supported and must be followed by a successful flush.

The Server bridge reaches VFS, FAT32, block flush, and ATA. The provider
contract and a separate QEMU process have been qualified. The adapter and SQL
engine are not linked into the freestanding kernel in this phase. No native
SQL9 execution, native WAL crash recovery, or arbitrary power-loss atomicity is
claimed. FAT32 metadata remains nonjournaled; clean independent-process QEMU
readback only proves the tested shutdown sequence.

Final hosted verification: `cmake --build build --parallel 4` succeeds and
CTest passes **10/10 targets**, totaling **92,655 checks with zero failures**.
The transaction/recovery target contributes 2,553 checks; the new native SQL
adapter target contributes 19.
