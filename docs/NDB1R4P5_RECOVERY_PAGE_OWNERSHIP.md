# NDB1R4P5: Portable Recovered-Page Ownership and WAL Replay Safety

## Qualification result

**Outcome A — RECOVERY_PAGE_OWNERSHIP_VERIFIED.** The production SQL9 recovery
path now retains complete validated page images in a portable collection that
owns bytes independently of the P4 scanner scratch. WAL replay consumes those
owned bytes directly, after coverage checks finish and before any checkpoint.
Page collection allocation failures are explicit and clean up safely. Database
and WAL formats are unchanged.

This phase qualifies the recovered-page collection and its hosted SQL9 replay
adapter. It does not qualify the entire SQL9 engine for freestanding execution.

## Starting repository state

| Gate | Observed value | Result |
|---|---|---|
| SQL path | `D:\dev\guideXOS_SqlDatabase` | Passed |
| SQL branch / HEAD | `main`, `c6227e9260384437e1c65f39284c8a8e47782f2c` | Passed |
| SQL worktree | Clean; branch was already one local commit ahead of `origin/main` | Passed |
| `.phase` | `guideXOSSql`, SQL9 → SQL10 | Preserved |
| `.native-sql.phase` before P5 | `guideXOSSqlNativePortability`, NDB1R4P4 → NDB1R4P5 | Passed |
| Server reference | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575`, clean | Read only |

The Server checkout's AMD64 flags were read from `kernel/Makefile` and
`kernel/arch/amd64/Makefile.arch`. Its `.native-sql.phase` file was absent, as
expected. No Server file or Git state was changed.

## Recovery ownership audit

The production path is:

1. `WriteAheadLog::scan` calls the P4 `gxWalScanRecords` scanner.
2. The scanner lends one reusable payload buffer to its synchronous record
   visitor. It destroys that buffer before `scan` returns.
3. For PAGE_IMAGE, the visitor checks the record is inside the active
   transaction and has exactly the existing page-image header plus one
   database page. The portable page inspector checks the serialized page ID,
   payload bound, and CRC.
4. The visitor copies the complete serialized page into a
   `GxRecoveredPageCollection` owned by the current `WalTransaction`.
5. COMMIT moves that transaction into the existing output
   `std::vector<WalTransaction>`. Only transactions with a validated COMMIT
   enter that vector. An open transaction at end-of-file is destroyed.
6. Writable recovery validates header progression and partial-tail coverage
   before the first database write.
7. Replay applies each committed header image and writes the owned page bytes
   in transaction order, flushes the database, validates the opened pages, and
   returns. The existing caller checkpoints the WAL only after recovery and
   catalog load succeed.

The P4 payload pointer is borrowed only during the visitor call. No scanner
pointer is stored. A collection node owns one allocation containing its
descriptor and a complete serialized page. Nodes are plain-data records and
are never relocated. `next` links are non-owning. Destroy or clear walks the
list and frees each node once through its copied `GxAllocator`.

`WalTransaction` is now move-only. Its `noexcept` move operations transfer the
collection through `gxRecoveredPagesMove`, which zeros the source. This lets
the existing hosted transaction vector grow without duplicating ownership.
Clearing the vector after a scan error releases completed transactions;
uncommitted current pages are released when the scan context unwinds.

## Hosted dependency inventory for this path

| Facility and location | Current role / bound | Failure and replacement decision |
|---|---|---|
| `std::vector<WalTransaction>` in `database_wal.h` / `database_wal.cpp` | Keeps committed transactions in WAL order. Its maximum count is bounded indirectly by the 64 MiB WAL cap; empty BEGIN/COMMIT pairs permit at most 1,677,720 transactions. | Remains hosted by design; this phase does not rewrite the general transaction container. Vector growth exceptions are caught by `scan`, mapped to `NoSpace`, and all retained pages are cleared. `WalTransaction` moves are explicit and ownership-safe. |
| Former `std::vector<DatabasePage>` in `WalTransaction` | Held decoded page payload vectors; a valid SQL3 writer transaction emits at most 4,096 PAGE_IMAGE records. The collection's vector moves transferred nested vectors using hosted allocation. | Replaced by `GxRecoveredPageCollection`, one portable allocator-owned raw page image per distinct page ID. |
| Former `std::vector<uint64_t>` in `Database::validateRecoveryCoverage` | Built a temporary list of all retained page IDs and linearly searched it for every missing tail page. | Replaced by a bitset sized to the missing tail extent, allocated via the same P1 allocator and freed before returning. |
| `std::unique_ptr<IDatabaseFile>` in `WriteAheadLog` | Owns the opened WAL file handle, not page-image memory. | Remains hosted and outside the collection conversion. The file is closed without checkpointing on recovery failure. |
| `std::string` in `WriteAheadLog::scan` / database diagnostics | Stores error and result messages, not page images. | Remains hosted. `std::bad_alloc` escaping the scanner wrapper returns `NoSpace` and clears retained transactions. |

There is no `std::map`, `std::unordered_map`, or `std::shared_ptr` in the
recovered-page owner. The live transaction overlay still uses its SQL3 map and
is outside P5. Query, catalog, parser, and page-cache containers are also
outside P5.

## Portable collection and bounds

`GxRecoveredPageCollection` uses a singly linked list of plain-data nodes.
Each node stores the page ID and a trailing full-page byte image. One
allocator request acquires the whole node, so no descriptor can be published
without its bytes. There are no nontrivial C++ objects in the node, no raw
relocation, no exception unwinding, and no ambiguous second owner.

Collection lookup is linear to preserve stable node addresses and replay
ordering without adding a general-purpose tree. SQL3 caps a transaction at
4,096 PAGE_IMAGE records, so a collection can require at most 4,096 lookups per
record and no more than 16,777,216 page-ID comparisons under the conservative
bound. The P5 test also inserted a full 4,096-page collection and records the
observed time in its test output. Total records and retained bytes remain
bounded by the 64 MiB WAL cap; the smallest supported page is 512 bytes.

Other audited bounds:

- Page IDs accepted by the owner are `1` through `kMaxPageCount - 1`; page 0
  is the database header page and cannot be replayed as a normal page image.
- The transaction page-image record count is checked before retaining image
  4,097. This matches the SQL3 writer's `kMaxTransactionPages` limit of 4,096.
- A full image allocation is at most `kMaxPageSize + node header`, below the
  default P1 allocator's per-allocation maximum.
- The retained image bytes are bounded by the validated WAL bytes. Duplicate
  replacement does not increase the collection count or byte total.
- Page-image allocation size, retained-byte addition, coverage bitmap size,
  page extent multiplication, and page offset calculations use checked bounds.
- Transaction IDs are serialized as `uint64_t`. Recovery now detects the
  exhausted `UINT64_MAX` boundary and prevents a future transaction ID wrap.

## Duplicate page IDs

The SQL3 writer stores a page once in its transaction overlay, so ordinary
commits emit unique page IDs. The scanner still accepts structurally valid
duplicate PAGE_IMAGE records. Their existing effective behavior is
last-image-wins because the old replay loop wrote images in WAL order.

The portable owner validates and allocates a replacement before publishing
it. On success it removes the old node, appends the replacement to the tail,
and frees the old allocation exactly once. This retains the last image and
orders each distinct page by its final occurrence. If validation or allocation
fails, the old node, count, byte total, and links remain intact. Tests cover
multiple replacements, allocation failure, and retry.

## Allocation failures and cleanup

Every collection node and the partial-tail coverage bitset use the P1
`GxAllocator`. The scanner scratch continues to use that same allocator.
Allocation statuses map as follows:

- P1 `OutOfMemory` → `DbStatus::NoSpace`.
- P1 resource or arithmetic limit → `DbStatus::TransactionTooLarge`.
- Invalid serialized page / page CRC → `DbStatus::WalCorrupt`.
- WAL file read/write failure → `DbStatus::IoError`.
- Valid committed redo unavailable for an incomplete base extent →
  `DbStatus::RecoveryRequired`.

Insertion allocates and validates a complete replacement node before changing
the list. A failed insertion leaves old nodes valid and publishes no partial
descriptor. A scan failure clears already committed output transactions and
the current in-progress transaction is destroyed during stack unwinding. A
subsequent scan uses an empty output vector and can retry.

The P5 allocation matrix covers initial scanner allocation failure, later
page-node failure after several retained images, duplicate replacement
failure, collection resource limits, partial construction cleanup, retry,
read-only inspection failure, and failure of the partial-tail coverage bitmap
allocation before replay. The tests check allocator outstanding counts,
invalid frees, and success/free balance. The SQL9 open path receives the same
P1-compatible allocator interface used by the collection.

## Replay, interruption, and retry

All page-image ownership and missing-tail coverage preparation completes
before replay writes begin. The coverage bitset has one bit per missing page
and is released before any page write. Normal replay writes the owned serialized
images through `DatabaseFile::writePageImage`, which revalidates page ID and
CRC and writes the exact image without constructing a `DatabasePage` or
temporary hosted vector.

After replay starts, page I/O, database flush, and full-page validation can
still fail. Such a failure returns an error, closes the database handle, and
does not checkpoint or truncate the committed WAL. Some base pages may already
have been written; the WAL remains the redo source. Reopening replays the same
complete page images, so retry is idempotent. The existing SQL3 crash matrix
continues to inject failures around commit/recovery boundaries.

The final disk flush and full-page validation still use the existing hosted
database-file implementation. `std::bad_alloc` there is caught at the recovery
boundary and reported as `NoSpace`; the WAL is not checkpointed. This is a
remaining hosted adapter dependency, not an allocation inside the portable
page collection.

## Read-only and partial-tail behavior

A read-only open that finds committed WAL returns `RecoveryRequired` before
replay. It does not write the database or WAL. P5 tests compare both files
byte-for-byte for valid committed, incomplete, and corrupt WAL cases. A
read-only scan allocation failure returns `NoSpace`; the database and WAL
remain unchanged. The test also fails the final coverage bitmap allocation
after committed images have been collected and verifies `NoSpace` with both
files unchanged.

For a partial trailing page, recovery computes the final logical page count
from committed header images and verifies every page ID from the physical
first-missing-page index through the logical end. A bounded bitset avoids
duplicate IDs hiding missing coverage. Missing or corrupt images do not
authorize truncation. Only complete committed images can reconstruct the tail;
WAL evidence stays until redo, flush, page validation, and catalog load finish.

## Relational proof

The dedicated P5 test uses the real SQL9 engine and `HostFileSystem`. Its
fixture has a parent table, child table, primary keys, a unique index, a
secondary index, a foreign key, and multiple child heap pages. A committed
transaction inserts 24 child rows and produces 59 PAGE_IMAGE records at the
512-byte page size.

The test restores the pre-transaction database plus a partial final extent and
the committed WAL. It verifies:

- An uncommitted transaction tail leaves the original single child row visible.
- Corrupt WAL is rejected without rewriting either file.
- Read-only recovery returns `RecoveryRequired` for committed or incomplete
  recovery and returns `WalCorrupt` for corrupt WAL, preserving both files in
  each case.
- A committed WAL missing a page image needed for its partial tail returns
  `RecoveryRequired` and preserves both files.
- Allocation failure after several retained page images returns `NoSpace`,
  releases all P1 allocations, and preserves the partial database and WAL.
- Read-only scan allocation failure and coverage-bitmap allocation failure
  return `NoSpace` before replay and preserve both files exactly.
- Retry reconstructs the partial extent and exposes exactly the 24 committed
  rows, with expected parent IDs, values, and payloads.
- Primary and secondary indexes validate; foreign-key integrity and catalog
  metadata validate; primary-key index lookup returns a locator that fetches
  the exact expected row.
- A second open returns the same rows and index/FK state after checkpoint.

## Freestanding compilation

The actual Server AMD64 compile configuration remains
`-std=c++14 -ffreestanding -fno-exceptions -fno-rtti -nostdlib -nostdinc++`
plus the Server's large code model, no-red-zone, no-SIMD, and no-PIC flags.
The compile script now builds the real recovered-page object in addition to
the accepted P1–P4 objects.

The selected production objects are `gx_allocator`, `gx_array`,
`gx_recovered_pages`, `gx_owner`, `gx_string`, `database_checksum`,
`gx_page_codec`, `gx_wal_record`, `gx_wal_scan`, and `gx_wal_page_image`.
`gx_recovered_pages.o` references only the portable allocator and page
inspector objects. The checksum object retains the expected
`__cxa_guard_acquire` and `__cxa_guard_release` references for its existing
function-local CRC table; the Server runtime provides them.

`database_wal.cpp`, `database_relational.cpp`, `database_file.cpp`, and the
wider SQL9 adapter remain hosted. In particular,
`std::vector<WalTransaction>`, `std::string` diagnostics, file-provider
virtual calls, catalog loading, and full database validation are not claimed
to compile freestanding.

## Compatibility, regression, and closeout

No `.gxdb` or `.gxwal` version, page encoding, record encoding, CRC algorithm,
catalog layout, heap layout, B+ tree format, or transaction-ID format changed.
Recovered pages are written as the exact serialized bytes stored in the WAL.
The ordinary SQL marker remains SQL9 → SQL10. The Server checkout was not
modified. No Git topology operation or push was performed.

The final P5 target reports **4,594 checks, 0 failures**. In the final full
CTest run, its 4,096-page collection completed in 100.45 ms.

| Suite | Result |
|---|---:|
| P1 portable runtime | 20,668 checks, 0 failures |
| P2 WAL encoding | 305 checks, 0 failures |
| P3 PAGE_IMAGE encoding | 106 checks, 0 failures |
| P4 WAL recovery scanner | 204 checks, 0 failures |
| P5 ownership and SQL9 recovery | 4,594 checks, 0 failures |
| Full SQL hosted CTest suite | 15/15 targets, 118,532 checks, 0 failures |

The full run was `ctest --test-dir build --output-on-failure` and took
1,561.34 seconds. Results by CTest target:

| CTest target | Checks | Result |
|---|---:|---|
| `database_storage_tests` | 172 | Passed |
| `database_relational_test` | 10,398 | Passed |
| `database_transaction_test` | 2,553 | Passed |
| `database_sql_test` | 1,779 | Passed |
| `database_index_test` | 2,752 | Passed |
| `database_sql7_test` | 1,307 | Passed |
| `database_sql8_test` | 4,503 | Passed |
| `database_sql9_test` | 15,274 | Passed |
| `database_sql9r_test` | 53,898 | Passed |
| `database_native_io_test` | 19 | Passed |
| `database_portable_runtime_test` | 20,668 | Passed |
| `database_wal_portable_test` | 305 | Passed |
| `database_wal_page_portable_test` | 106 | Passed |
| `database_wal_recovery_portable_test` | 204 | Passed |
| `database_ndb1r4p5_test` | 4,594 | Passed |

## Remaining hosted dependencies and recommended P6 scope

The transaction vector, SQL diagnostics and all normal relational containers
remain hosted. `DatabaseFile::validateOpenedPages` and the catalog load still
allocate hosted vectors after redo; failures are returned as `NoSpace` and the
WAL remains available for another open. P6 should separately audit portable
page-cache entry ownership, buffer lifetimes during eviction, and storage
object destruction. It should not begin until this dependency inventory is
reviewed.

## Final closeout

The full hosted suite passed 15/15 targets with 118,532 checks and no failures.
The real recovered-page production object and accepted P1–P4 objects compiled
under the Server AMD64 freestanding flags; the symbol audit found only the
expected CRC guard references among the reviewed runtime symbols. No database
or WAL format changed.

At closeout, the SQL branch remains `main`, the ordinary `.phase` remains
SQL9 → SQL10, and `.native-sql.phase` advances from NDB1R4P4 → NDB1R4P5. The
Server reference remains on `NATIVE_DB_STORAGE` at
`f29850c93bbac0a0763a31e806d2816310156575`; its worktree is clean and it has
no `.native-sql.phase` file. The reviewed P5 changes and portability marker
are committed locally on `main`; no push was performed.
