# NDB1R4P8: Portable Transaction Page Index and Overlay

## Qualification status

**Outcome A — TRANSACTION_PAGE_INDEX_VERIFIED.** P8 replaces the live SQL9
transaction overlay's hosted page-number map with a bounded, portable
open-addressed index. `GxTransactionPages` remains the sole page-image owner
and commit-order source. The transaction manager, WAL format, database format,
and ordinary SQL phase are unchanged.

P8 verifies portable live page lookup and its transaction integration. It does
not claim that the full SQL9 transaction manager is freestanding.

## Starting state and gates

| Gate | Starting observation | Result |
|---|---|---|
| SQL repository | `D:\dev\guideXOS_SqlDatabase` | Passed |
| Branch / HEAD | `main`, `bbb396e6caa528958ee0b6a667d13b7a4015eb9b` | Passed |
| Worktree | Clean before P8 edits | Passed |
| `.phase` | `guideXOSSql`, SQL9 → SQL10 | Passed; unchanged |
| `.native-sql.phase` | `guideXOSSqlNativePortability`, NDB1R4P7 → NDB1R4P8 | Passed at start |
| P6 evidence | `docs/NDB1R4P6_PAGE_CACHE_PORTABILITY.md` | Read; 612 focused checks and 16-target accepted regression documented |
| P7 evidence | `docs/NDB1R4P7_TRANSACTION_PAGE_PORTABILITY.md` | Read; 307 focused checks and 17-target accepted regression documented |
| Server reference | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575` | Inspected read-only |

The requested P6 report name in the phase prompt did not match the repository;
the accepted file is `NDB1R4P6_PAGE_CACHE_PORTABILITY.md`. P7 confirms the prior
hosted lookup was `std::map<uint64_t, size_t>`, keyed by page ID and containing
stable indices into the fixed P7 descriptor array. It was bounded to 4,096
entries, used O(log n) map lookup, and owned no payload bytes. The owner array
held page bytes and first-insertion order; the map provided lookup only.

No P8 implementation was present at the starting HEAD. No concurrent change
was observed before editing. SQL branch and HEAD remained on the required
baseline during implementation. Server remains unmodified.

## Chosen index and ownership

The production overlay is `GxTransactionPageOverlay`, implemented in
`database/portable/gx_transaction_page_overlay.{h,cpp}`. It contains the P7
`GxTransactionPages` owner and the new
`GxTransactionPageIndex`. `Transaction` and `TransactionSavepoint` now own
that paired overlay rather than a page owner plus `std::map`.

The index is allocated once at initialization and never relocates. It uses
power-of-two linear probing and a SplitMix64 finalizer to mix the 64-bit page
ID before masking. Page ID zero is reserved as the empty-slot sentinel by the
database format, so the table needs no tombstones: entries are removed only by
clearing or destroying the whole transaction index. Duplicate keys are
rejected. Owner references are bounded numeric descriptor indices, never raw
pointers. Each hit also verifies that the referenced owner descriptor contains
the requested page ID and a non-null image.

The 4,096-page transaction bound requires 8,192 slots, for a maximum 50% load
factor. The current AMD64 slot is 16 bytes, so one live index occupies 131,072
bytes per transaction. Checked arithmetic covers doubling, power-of-two
capacity rounding, slot-byte multiplication, and slot addressing. Lookup and
insert probe at most the fixed capacity; expected lookup is O(1), while the
worst case is bounded O(8,192). The near-limit diagnostic observed 1.52 probes
per successful lookup and 2.54 probes per miss over its fixed 4,096-key
fixture, with a maximum observed probe run of 27. Insertions and lookups do not
allocate after initialization.

Index and owner initialization are paired. If index allocation fails after
the P7 descriptor table succeeds, the owner is destroyed before the failed
overlay is returned. For first insertion, the page is fully encoded and
published to the owner, then its owner index is inserted into the preallocated
table. If index insertion rejects it, the just-appended owner is popped and
freed. Duplicate replacement allocates and encodes the candidate image before
replacing the old one; the key and owner index remain unchanged. The index
never owns page bytes.

At the maximum 65,536-byte page size, steady-state owner image storage remains
bounded by 4,096 pages (256 MiB), with one additional page allowed transiently
for strong replacement. The P7 descriptor table is 65,536 bytes on AMD64. The
new index adds 131,072 bytes. A statement savepoint clones the fixed owner
descriptor allocation, owned images, and index; scratch allocation therefore
adds at most one bounded overlay image set plus a 131,072-byte index and its
descriptor table. Allocator metadata and hosted decoded views are not included.

## Transaction semantics and savepoints

The overlay owner remains authoritative for current page bytes and commit
order. Replacing a page changes its image at the original descriptor and does
not move it. The fixed ordering fixture stages `9, 3, 9, 12, 3`; owner order
remains `9, 3, 12`, and the first two entries contain their latest valid
images. The hash table is never iterated for commit.

`Transaction::readPage` searches the index first and parses the owner image on
a hit. A miss follows the existing committed-page/base-page checks. Ordinary
database reads still use the committed cache only, preserving isolation.
Commit enumerates the overlay owner in descriptor order for WAL PAGE_IMAGE
records and committed cache publication. WAL framing, bytes, COMMIT position,
sync durability point, checkpoint ordering, and recovery code are unchanged.

`TransactionSavepoint` now deep-clones both the page owner and index before
statement execution. A partial index-clone failure destroys the cloned owner
and index without changing the live overlay. On statement rollback, overlay
move destroys the failed statement state and transfers the saved owner and
index without allocation. The test holds allocator failure armed during this
restore and confirms successful statement A remains, failed statement B is
removed, and successful statement C can follow. Full transaction rollback
clears all page images and every index slot; reuse of the index after clear
does not find a prior key.

## Fault injection, boundaries, and collisions

The focused P8 test covers:

- Initial fixed-index allocation failure after successful page-owner table
  allocation, cleanup, and transaction retry.
- Index clone allocation failure, owner clone followed by index allocation
  failure, image allocation before insertion, and page replacement failure.
- No allocation during index insertion, duplicate detection, clear, move, and
  savepoint restore; rollback works with the allocator failure still armed.
- Exact allocator outstanding/free balance and successful reinitialization
  after destruction.
- Empty lookup, page zero rejection, highest valid page ID, one entry, small
  tables, full 4,096-entry insertion, one-past-limit rejection, and large
  sparse keys.
- Deliberately discovered colliding keys, collision lookup and insert,
  duplicate-key rejection, wrong/missing keys, maximum load, clone, move,
  clear, and repeated destroy/init.
- Owner/index count agreement and key-to-owner validation on lookup and clone.
- Failed insertion removes the just-appended owner; failed replacement keeps
  the last valid image; duplicate page modification preserves order.

The 4,096-entry index test records 2,114 insertion collisions and a maximum
probe length of 27 on its deterministic fixture. A production transaction
that allocates 4,096 pages recorded 2,053 collisions before a simulated partial
WAL PAGE_IMAGE write. Commit failed before durable COMMIT, transaction state
was discarded, and reopen recovered the original page count and valid
integrity. This exercises the production index at maximum load before a WAL
failure; this fixed-size table has no post-init capacity growth or rebuild
allocation boundary.

## Real SQL9 production path

The new CTest target invokes the production `Database`, `Transaction`,
`SqlEngine`, and hosted file provider. Prepared INSERT, UPDATE, and DELETE
operate on a primary-key B+ tree, a secondary index, a foreign key, multiple
heap pages, variable-length row relocation, and repeated changes to the same
row. It performs 100 repeated indexed SELECTs inside a transaction and observes
the staged page index's hit/probe counters. It exercises transaction commit
and rollback, an FK constraint failure followed by successful statements,
and a unique-key statement failure between successful statements A and C.

The test validates every reported index and database integrity, verifies the
exact row IDs after insertion and deletion, confirms the relocated payload, and
closes/reopens the database to recheck committed rows and integrity. The
existing P7 isolation and SQL9 workloads also pass.

## WAL, crash recovery, and formats

The existing SQL3 transaction and prepared-DML crash matrices remain in the
full suite. The P8-specific maximum-load test interrupts a 4,096-page
transaction's WAL write after 192 bytes, before COMMIT, then reopens and
verifies the original page count and integrity. The P8 code adds no WAL record,
does not change PAGE_IMAGE bytes, and does not alter the durable COMMIT flush
boundary. No `.gxdb` or `.gxwal` format, page layout, catalog, index, heap, or
transaction identifier changed.

## Freestanding compilation and symbol audit

The existing `tools/compile_portable_freestanding.ps1` now includes
`gx_transaction_page_index.cpp` and `gx_transaction_page_overlay.cpp`. Both
compile using the Server AMD64 C++14 kernel flags, including
`-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti -nostdlib`, large code
model, no red zone, no SIMD, and no PIC. The exact boundary is the portable
lookup and overlay integration leaf together with the already qualified P1/P7
portable runtime/page-owner sources. It does **not** compile
`database_transaction.cpp`, `Database`, the WAL file adapter, the SQL executor,
catalog, relational containers, or hosted file provider.

The index object references only `gxAllocatorAllocate` and `gxAllocatorFree`.
The overlay object references portable owner and index functions. Neither new
object has `std::`, exception, RTTI, C runtime, hosted file-I/O, allocation, or
ABI guard references. The existing checksum object retains its documented
`__cxa_guard_acquire` and `__cxa_guard_release` references. No kernel runtime
symbol was duplicated in SQL.

## Diagnostics and regression results

On the configured hosted Windows build, the 4,096-entry fixture reported
approximately 38 microseconds for insertion, 40 microseconds for 4,096 hits,
and 67 microseconds for 4,096 misses. A same-process `std::map` reference
diagnostic reported approximately 170 microseconds for hits and 27
microseconds for misses. These are noisy diagnostic timings, not assertions or
a portable performance claim. The index used 131,072 bytes; its fixed capacity
means no growth or resize peak. The small overlay fixture reported 100
replacements around 133 microseconds, snapshot clone around 7 microseconds,
and restore move below 1 microsecond. The SQL9 prepared SELECT diagnostic
reported approximately 2.3 milliseconds for 100 indexed selects; overlay
lookup counters are reported in the focused test output.

P7 regression: **307 checks, 0 failures**.

P8 focused regression: **25,543 checks, 0 failures**.

Full CTest regression: **18/18 targets passed, 0 failures, 1,636.75 seconds**.
That is the accepted 119,464-check baseline plus 25,543 focused P8 checks,
for **145,007 checks** across the qualified suite.

## Remaining hosted dependencies and P9 direction

The live page-number map and page-order vector are removed from the
transaction-owned page overlay. The broader `Transaction`, statement catalog
snapshot, `DatabasePage` hosted decode view, `std::unique_ptr<Transaction>`,
`DbResult` strings, catalog/schema/query containers, relational executor, WAL
adapter, and hosted file provider remain hosted. The accepted bounded writer
limit remains 4,096 pages.

Recommended NDB1R4P9 scope: continue with the next isolated SQL9 ownership
boundary after reviewing the remaining hosted dependencies. Avoid combining a
transaction-manager rewrite, catalog-wide container conversion, parser/plan
conversion, or persistent-format work with that slice.

## Closeout

The SQL language marker remains SQL9 → SQL10. The portability marker advanced
to NDB1R4P8 → NDB1R4P9 after all acceptance gates and the full regression
passed. The Server reference remains read-only at the state in the gate table.
No branch, remote, upstream, or history topology operation and no push was
performed.
The final SQL HEAD and local commit are recorded in the task closeout.
