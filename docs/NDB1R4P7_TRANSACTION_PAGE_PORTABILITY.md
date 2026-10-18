# NDB1R4P7: Portable Live Transaction Page Ownership

## Qualification result

**Outcome A — TRANSACTION_PAGE_PORTABILITY_VERIFIED.** The SQL3 live
transaction overlay now owns bounded portable serialized page images. The
hosted page-number map remains as an index into that owner. Focused ownership,
allocation-fault, statement-rollback, WAL-encoding, and SQL9 workload checks
pass, as does the complete 17-target hosted regression.

This phase qualifies only transaction-owned page bytes and their transition
into the existing WAL path. It does not qualify the whole SQL9 transaction
manager for freestanding execution.

## Starting gates and P6 independent verification

| Gate | Starting value | Result |
|---|---|---|
| SQL checkout | `D:\dev\guideXOS_SqlDatabase` | Passed |
| SQL branch / HEAD | `main`, `d4efe1995c320691a7ebac785578273dcf56103c` | Passed |
| SQL worktree | Clean before P7 edits | Passed |
| SQL `.phase` | `guideXOSSql`, SQL9 → SQL10 | Preserved |
| `.native-sql.phase` | `guideXOSSqlNativePortability`, NDB1R4P6 → NDB1R4P7 | Passed |
| Server reference | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575` | Inspected read-only |

The latest SQL commit is the accepted P6 commit, `SQL: qualify portable
page-cache ownership (NDB1R4P6)`. The committed
`docs/NDB1R4P6_PAGE_CACHE_PORTABILITY.md` was reviewed. It documents a
production `BufferManager` conversion to P1-owned serialized cache images,
stable descriptor and view lifetimes, candidate-first eviction, explicit
allocation errors, and the `recoveryRequired` state after durable-WAL cache
publication failure. It records unchanged `.gxdb` and `.gxwal` formats,
freestanding qualification of the selected cache object, a 612-check P6
focused suite, and the 16-target / 119,157-check full regression result.

The P6 focused target was independently rerun and passed **612 checks, 0
failures**. Its accepted report records **16/16 CTest targets, 119,157 checks,
0 failures**. No P6 change is repeated here.

The Server reference was checked during P7: branch `NATIVE_DB_STORAGE`, HEAD
`f29850c93bbac0a0763a31e806d2816310156575`, clean worktree. The AMD64 flags
were read from `kernel/Makefile` and `kernel/arch/amd64/Makefile.arch`. No
Server file or Git state was modified.

## Existing transaction page lifecycle

Before P7, `Transaction` owned `std::map<uint64_t, DatabasePage> _pages` and a
`std::vector<uint64_t> _order`. The writer limit is 4,096 modified pages.
Each map node owned a `DatabasePage`; its payload was a hosted
`std::vector<uint8_t>` bounded by the selected page size minus the 32-byte
page header. The map gave O(log n) lookup. `_order` recorded first-stage
order, and replacing a page updated the map value without moving its order
entry. Commit iterated `_order`.

The transaction flow is:

1. `Database::beginTransaction` ensures the WAL is open and checkpoints first
   if required. It then constructs a transaction and initializes its page
   owner before publishing `_activeTx`.
2. A read checks the transaction page-number index first. On a hit, it parses
   the owned serialized image into a `DatabasePage`; otherwise it reads from
   the committed `Database` when the ID is below the base page count.
3. A write serializes the supplied page into a new owner image. The page ID
   index is inserted only after the image is complete. Repeated writes replace
   the image at the existing descriptor and preserve first-stage order.
4. A statement savepoint deep-copies the owner images and hosted lookup map,
   then copies the catalog and next-page state. A failed snapshot returns
   before the SQL statement mutates anything. Statement rollback transfers
   the saved image set and swaps the map and catalog without allocation.
5. Transaction rollback swaps the original catalog snapshot back, clears all
   staged images and the lookup map, and leaves committed cache and file pages
   untouched.
6. Commit serializes a changed catalog into the overlay, appends `BEGIN`,
   every `PAGE_IMAGE`, an optional `DB_HEADER`, then `COMMIT`, and calls
   `wal.sync()`. That WAL flush remains the durability point. Only after it
   succeeds does commit set the page count and publish decoded pages into the
   committed buffer manager.
7. Any pre-durability failure rolls back and returns failure. If page-count
   or cache publication fails after durable COMMIT, the database is marked
   `recoveryRequired`; committed reads and checkpointing stay blocked and
   reopen replays the retained WAL.

The P6 isolation rule is preserved: `Database::readPage` sees the committed
cache only, while `Transaction::readPage` sees its overlay first. No
uncommitted image is installed in the committed cache.

## Selected production conversion and remaining hosted dependencies

P7 replaces the `DatabasePage` payload-vector ownership and the order vector
with `GxTransactionPages`. A fixed descriptor table contains plain
`{pageId, image}` records. It is allocated once at transaction initialization
for 4,096 entries and is never relocated. Each image is one allocator-owned,
complete serialized page of exactly `pageSize` bytes. The P1 allocator
contract limits alignment to the Server's 8-byte convention and reports
allocation failure explicitly.

At the largest supported page size, steady-state owner image bytes are at
most `4096 * 65,536` (256 MiB); one replacement may temporarily add one page
(64 KiB). At 512-byte pages the image bound is 2 MiB. The descriptor table is
bounded separately (65,536 bytes on the AMD64 ABI). Allocator metadata,
savepoint copies, the hosted lookup map, catalog copies, and decoded
`DatabasePage` vectors are outside the image-byte statistic.

| Facility in the live transaction path | P7 behavior |
|---|---|
| Former `std::map<uint64_t, DatabasePage>` | Replaced as page-byte owner by fixed portable descriptors and separately allocated serialized images. |
| Former `std::vector<uint64_t> _order` | Removed; descriptor append order is the original first-stage order. |
| `std::map<uint64_t, size_t> _pageIndexes` | Retained as a hosted lookup-only index, bounded by 4,096 nodes, O(log n) lookup, and mapping page ID to stable descriptor index. |
| `TransactionSavepoint` map/catalog snapshots | Remain hosted and deep-copy before statement execution. Page images use the explicit allocator; rollback restores through ownership transfer and swaps. |
| `DatabasePage` and its payload vector | Remains the hosted relational API/view. Reads decode the owned image; commit decodes only after WAL durability before cache publication. |
| `std::unique_ptr<Transaction>`, `std::string` results, catalog vectors/maps, SQL plan/query containers | Remain hosted and outside P7. |

The owner contains no nontrivial C++ objects and does not move or byte-copy
constructed objects. Explicit init, destroy, clone, move, append, replacement,
pop, and clear operations use allocator callbacks and return `GxStatus`.

## Replacement, duplicate-page, and rollback guarantees

For a replacement, P7 allocates and fully encodes candidate B while A remains
owned. Only after encoding succeeds does it publish B's pointer and free A.
An allocation or encoding error leaves A, the descriptor count, and current
owned bytes intact. The lookup index points to the same descriptor before and
after replacement, so the page retains its initial commit-order position.

The SQL3 writer stores one owner per page ID. Duplicate writes therefore
replace the existing image and retain first-stage order, matching the old
map-plus-order behavior. A map-node allocation failure after first-image
construction pops and frees only the just-appended final descriptor.

Transaction rollback clears all owner images and the lookup index. The fixed
descriptor allocation is released when the transaction object is destroyed.
Statement rollback restores a deep snapshot; the portable page-set move
destroys the failed statement's set and transfers the saved set, while map and
catalog swaps require no new allocation. Transaction and statement rollback
do not publish any bytes into the committed page cache.

## Allocation failure and focused evidence

`database_ndb1r4p7_test` reports **307 checks, 0 failures**. Its failure
injector covers:

- Owner descriptor allocation failure during `BEGIN`, returning `NoSpace`
  without publishing an active transaction, followed by a successful retry.
- First staged-image failure, later page-image failure after a staged page,
  and successful retries. Failed allocation does not increment modified page
  count or transaction page count.
- Replacement failure while A remains readable, then successful replacement.
- Partial savepoint clone failure after its descriptor allocation; the clone
  releases partial storage and leaves the live transaction unchanged.
- Rollback while allocator failure remains armed, demonstrating that
  statement restoration performs no allocation.
- Exact owner allocation/free balance after owner, transaction, savepoint,
  rollback, database close, and reopen.
- Side-by-side old-field and new-raw-image PAGE_IMAGE encoding. The framed
  record bytes compare equal for the same transaction and page.

The direct production-overlay fault diagnostic observed two staged pages,
five image allocations, three replacements, four injected owner allocation
failures, and 1,536 peak image bytes. The 512-byte page workload's peak
includes the prior two images and one temporary replacement candidate.
Rollback returned current owned image bytes to zero, committed page 1 stayed
byte-identical, the allocated page was absent, and reopen retained the
pre-transaction page count and bytes.

The SQL9 production workload observed nine staged pages, 88 logical overlay
image allocations, 79 replacements, zero injected failures, and 5,120 peak
image bytes at 512-byte page size. `Transaction::pageStats()` measures the
active overlay's image history; it excludes deep savepoint scratch images,
the fixed descriptor table, allocator metadata, and hosted decoded views.

## SQL9 relational workload and isolation

The focused SQL workload uses the production `Database`, `Transaction`, and
`SqlEngine` with the hosted file provider and a custom transaction-page
allocator. It creates parent and child tables with primary keys, an FK,
multiple child heap pages, and a secondary index. Prepared statements insert
12 parent/child pairs in an explicit transaction. It then performs a long
variable-width prepared update, confirms the primary-key row locator moved,
executes a failing FK update, successfully inserts another row, deletes a
row, and commits. A prepared read during the transaction observes the updated
payload.

The workload validates the FK/catalog integrity and every index. It runs a
prepared update followed by rollback, closes and reopens the database twice,
and verifies the exact 12 child IDs (`1..11` and `13`), the 350-character
updated payload, and the absence of deleted ID 12. The direct overlay test
also compares a transaction's staged page with the committed page read from
`Database` and proves the latter remains unchanged until commit.

## WAL, recovery, and persistent compatibility

`WriteAheadLog::appendPageImageImage` accepts a borrowed owner image only for
the synchronous call. The P3 PAGE_IMAGE framing remains unchanged: the
existing record type and 12-byte page-image header wrap a complete database
page. The raw-image encoder validates page identity and CRC before copying
bytes into the framed record. The P7 focused test compares that output byte
for byte with the established field-based encoder.

The complete existing `database_transaction_test` and SQL crash targets
requalify pre-COMMIT interruption, committed-WAL replay, multi-page redo,
partial-tail recovery, interrupted checkpoint, replay failure/retry, and
repeated recovery. P5 additionally requalifies partial-page recovery coverage
and retry. P6 requalifies a durable COMMIT whose committed cache publication
allocation fails: reads and flush are blocked, WAL remains, and reopen replays
the committed rows. No WAL record type or transaction framing changed.

No `.gxdb` or `.gxwal` version, page layout, checksum, catalog version, heap
layout, B+ tree layout, record framing, or transaction-ID encoding changed.

## Freestanding compatibility and remaining boundary

`tools/compile_portable_freestanding.ps1` now compiles
`database/portable/gx_transaction_pages.cpp` with the current Server AMD64
configuration: C++14, `-ffreestanding`, `-fno-exceptions`, `-fno-rtti`,
`-nostdlib`, `-nostdinc++`, `-fno-builtin`, large code model, no red zone,
no SIMD, and no PIC. The new object references only P1 allocation functions
and the portable page encoder/inspector. Symbol inspection found no hosted
STL, exception, RTTI, or C runtime dependencies in it. The accepted checksum
object retains only its prior `__cxa_guard_acquire` / `__cxa_guard_release`
references.

The broader `Transaction`, `Database`, WAL adapter, lookup map, statement
snapshot, catalog, SQL executor, error strings, and file provider remain
hosted. The converted owner is genuinely freestanding-compilable; the full
SQL9 transaction manager is not claimed as freestanding.

## Full hosted regression

The serial `ctest --test-dir build --output-on-failure` run completed with
**17/17 targets passed, 119,464 checks, 0 failures**, in **1,146.35 seconds**
(19 minutes, 6.35 seconds). It included every existing SQL1–SQL9, SQL9R,
NDB1R3, P1–P6 target plus the new P7 target. The check total is the accepted
P6 full-suite count of 119,157 plus the P7 target's 307 checks.

P6's accepted full-suite baseline is **16/16 targets, 119,157 checks, 0
failures**. The new P7 target adds 307 checks.

## Closeout

Starting SQL HEAD: `d4efe1995c320691a7ebac785578273dcf56103c` on `main`.
The accepted P7 files and marker are committed locally on `main`; the commit
SHA is included in the final task closeout. The ordinary SQL `.phase` remains
SQL9 → SQL10.
The Server reference remains on `NATIVE_DB_STORAGE` at
`f29850c93bbac0a0763a31e806d2816310156575` with a clean worktree; it was
inspected read-only. No branch switching, pull, merge, reset, or push was
performed.

Recommended P8 boundary: measure and, if needed, replace the remaining
bounded `std::map<uint64_t, size_t>` page lookup with a portable fixed-capacity
page-ID index while keeping the owner descriptor order stable. Keep catalog,
parser, prepared-plan, and query-container conversion out of that slice.
