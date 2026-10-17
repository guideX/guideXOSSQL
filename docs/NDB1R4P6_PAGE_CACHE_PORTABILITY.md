# NDB1R4P6: Portable Page-Cache Ownership and Safe Eviction

## Qualification result

**Outcome A — PAGE_CACHE_PORTABILITY_VERIFIED.** The production SQL9
`BufferManager` now stores each resident page as an allocator-owned serialized
image in the P1 portable runtime. Descriptors have fixed, bounded storage;
page views exist only while pinned. Cache misses, replacement, dirty writes,
allocation failures, and post-commit WAL recovery are covered by focused and
full-suite tests. The database and WAL formats are unchanged.

This qualifies the selected cache entry and byte-ownership slice. It does not
claim that the full SQL9 engine or the hosted `BufferManager` adapter is
freestanding.

## Starting gates and closeout state

| Gate | Observed value | Result |
|---|---|---|
| SQL checkout | `D:\dev\guideXOS_SqlDatabase` | Passed |
| SQL branch / starting HEAD | `main`, `1378949988a0269473a7f5bd16be2bc0ab97cd2e` | Passed |
| SQL worktree before edits | Clean; branch already two local commits ahead of `origin/main` | Passed |
| `.phase` | `guideXOSSql`, SQL9 → SQL10 | Preserved |
| `.native-sql.phase` before P6 | `guideXOSSqlNativePortability`, NDB1R4P5 → NDB1R4P6 | Passed |
| Server reference | `NATIVE_DB_STORAGE`, `f29850c93bbac0a0763a31e806d2816310156575`, clean | Read only |

The Server native marker remained at NDB1R3 → NDB1R4. Its checkout, branch,
HEAD, and working tree were unchanged. There was no P6 implementation or P6
acceptance document at the start. The ordinary SQL marker remains SQL9 →
SQL10; only the native portability marker advances to P6 → P7.

## Existing cache audit

Before P6, the production cache was a `new BufferSlot[capacity]` array. Each
slot held a page ID, dirty and pin counts, an LRU timestamp, and a
`DatabasePage`. The page payload was a hosted `std::vector<uint8_t>`. The
array's capacity was fixed at construction; zero was normalized to one, and
normal database opens defaulted to 32 slots. `findSlot` scanned all slots by
page ID, so lookup was O(capacity).

Each `BufferManager` references exactly one `DatabaseFile`; therefore a page
ID is scoped to that database instance. The cache is not shared between
files. The replacement policy selects the least-recently-used unpinned slot.
Pins prevent eviction. Dirty pages are written before an eviction and are
cleared only after a successful write. `flush()` writes all dirty pages and
then calls `DatabaseFile::flush()` for the header and durability barrier.
There is no background flush or concurrent cache access.

The former miss path evicted a chosen slot before loading the requested page.
That could discard a clean resident page on a later read failure, although a
dirty page was written first. P6 prepares and validates the requested page
before publishing replacement, preserving the old entry on candidate
allocation, read, parse, or dirty-victim write failure.

### Hosted dependency inventory

| Dependency and role | Bound, ownership, and behavior | P6 treatment |
|---|---|---|
| Former `BufferSlot[]` in `database/database_buffer.cpp` | `new[]` allocated a fixed slot table; each slot embedded a nontrivial `DatabasePage`. References to slots were stable until manager destruction. | Replaced with a fixed P1-allocator-owned `BufferSlot` view array, placement-constructed and explicitly destroyed, plus a portable descriptor table. |
| `DatabasePage::payload` (`std::vector<uint8_t>`) | Hosted decoded view of one page; copies and page parsing can allocate. The old cache retained it until eviction. | No longer owns the resident cache bytes. A decoded view is materialized only while an entry is pinned and cleared on its final unpin. The hosted vector remains in the SQL adapter. |
| Former miss-read temporary in `DatabaseFile::readPage` | A hosted vector was allocated, read, validated, parsed, then copied into the cache page view. | Cache misses use `readPageImage` to read and validate directly into the candidate image owned by the portable allocator. |
| SQL3 transaction overlay: `std::map<uint64_t, DatabasePage>` and page-order `std::vector` | Private to one transaction; the writer caps changed pages at 4,096. It carries uncommitted changes and is not the committed cache. | Remains hosted and unchanged. It is deliberately outside P6. |
| `std::string` in `DbResult` and file-provider/`unique_ptr` handles | Error text and hosted file lifetime, not cache entry bytes. | Remains outside the conversion. |

There is no cache `unordered_map`, `list`, `shared_ptr`, or tree. Adding an
index would change the established simple lookup behavior and was not needed
for the selected ownership conversion.

## Portable entry storage and ownership

`GxPageCacheEntry` is a plain-data descriptor containing the page ID, LRU
clock, pin count, dirty bit, and pointer to its owned serialized image. The
descriptor table is allocated once and never relocated. Each resident image
is a separate fixed-size allocation from the copied P1 `GxAllocator`; it is
never borrowed from file-read scratch or the P5 recovered-page collection.
Destroying the cache frees every resident image and then the descriptor
table.

The hosted `BufferSlot` views are also allocated once through the same
allocator. Their nontrivial C++ lifetimes are started with placement new and
ended explicitly; they are not relocated. A `BufferSlot*` therefore keeps a
stable address for the manager's lifetime, though eviction may later make
that slot represent a different page. A caller may rely on the page view
only while it holds the corresponding pin and must unpin exactly once. The
final unpin encodes dirty view changes into the portable image and drops the
decoded vector. Failed operations return a `DbResult` and do not hand out a
pinned slot.

The portable image is canonical cache ownership. A successful miss allocates
a candidate, reads the complete serialized page directly into it, checks the
page ID and CRC, and parses the view before it can be published. The cache
publishes only a complete image. `DatabasePage` move assignment is statically
checked as non-throwing, so no allocating step remains after victim
replacement begins.

At steady state, portable cache storage is bounded by the descriptor table,
the fixed view table, and at most `capacity * pageSize` image bytes. A miss
may temporarily own one candidate image while all resident images remain
intact, so the portable image peak is at most `(capacity + 1) * pageSize`.
Initialization and replacement arithmetic is checked; cache page sizes must
be powers of two from 512 through 65,536 bytes, and unsupported sizes or
allocation overflows are rejected. Zero capacity continues to normalize to
one; the default remains 32 slots. Memory diagnostics include the portable
descriptor, view-table, and serialized-image allocations, not allocator
metadata or hosted `DatabasePage` payload vectors. The decoded views remain
bounded by the fixed entry count and validated page size, but still use the
hosted allocator.

## Hits, misses, replacement, and dirty pages

A hit linearly scans the descriptors by page ID, advances the LRU timestamp,
materializes a view if necessary, and pins the entry. A miss chooses an empty
descriptor or the oldest unpinned resident. If all entries are pinned, it
returns an error without evicting anything. The replacement candidate is
allocated and fully validated first. If the victim is dirty, its serialized
image is written before replacement; a failed write keeps the old image,
dirty state, and resident identity intact. Only then is the candidate
published and the old image released.

Newly allocated pages and committed pages enter the cache dirty. `markDirty`
and dirty `unpin` serialize the pinned view into its owned image. Flush writes
each dirty image through `DatabaseFile::writePageImage`, clears the dirty bit
only after a successful page write, and finally propagates the file flush
barrier. A page-write or file-flush failure is returned and can be retried.
As in the existing design, dirty eviction performs a page write but not a
database-wide durability barrier; the barrier occurs in `flush()`.

The allocator maps out-of-memory to `DbStatus::NoSpace`, and resource or
arithmetic limits to `DbStatus::TransactionTooLarge`. Partial initialization
destroys the descriptor table if view-table allocation fails. Candidate
allocation, read, CRC/parse validation, or victim-write failures free the
candidate and preserve existing entries. Saturating diagnostic counters
cannot wrap; LRU clock exhaustion returns an explicit error.

## Transaction isolation and WAL durability

The SQL3 overlay continues to hold uncommitted pages in its private hosted
map. Reads inside the transaction consult that overlay first; ordinary
`Database::readPage` reads only the committed cache. `installPage` publishes
only after the WAL COMMIT is synchronized, and recovery publishes validated
committed WAL images. No uncommitted transaction page is installed in the
cache.

P6 closes a durability gap for cache publication failure after the WAL COMMIT
is durable. If page-count update or committed-page installation fails at that
point, the database is marked `recoveryRequired`. Committed reads and flushes
are blocked, close does not checkpoint the WAL, and reopen replays the
retained committed WAL. This prevents reads from observing a stale committed
cache after a durable transaction whose cache publication was incomplete.

## Fault injection and SQL workload

The P6 focused suite reports **612 checks, 0 failures**. It covers:

- Descriptor and view-table allocation failure, cleanup after partial init,
  zero/unsupported capacity, page-size bounds, and checked size overflow.
- Repeated first-miss allocation failure without publishing an entry;
  replacement allocation failure while preserving resident page bytes;
  stable slot addresses on hits; pin-protected eviction; exact bytes after
  eviction and reload; and allocation release on cache destruction.
- Failed page reads and failed dirty-victim writes, including retry with the
  dirty entry still resident; page write failure; file flush-barrier failure
  and retry; and CRC-validated direct page-image I/O.
- Two live databases with the same page ID and different content, proving
  the per-file cache scope prevents identity aliasing.
- Production open failures injected at descriptor/image allocation, with
  database and WAL files compared byte-for-byte.
- A SQL9 workload with 36 variable-size rows, a primary key and secondary
  index, multiple heap pages, committed and rolled-back transactions,
  repeated indexed selects, reopen checks, and index validation. A committed
  cache read is compared with a transaction-overlay read to confirm isolation.
- A durable transaction whose cache-image allocation fails after WAL sync.
  The suite verifies blocked reads/flush, retained WAL on close, successful
  reopen/replay, and all 32 committed updated rows.

The focused cache workload recorded 214 hits, 478 misses, 476 evictions, 478
image allocations, and 1,776 bytes peak portable cache ownership. A separate
10,000-hit diagnostic took 12,024 microseconds and recorded 20,000 lookup
comparisons at test capacity two. This confirms linear lookup and provides a
diagnostic only; it is not a strict timing threshold or a direct before/after
benchmark. No equivalent pre-P6 hit instrumentation was available. Memory
figures exclude hosted decoded page payload vectors and allocator metadata.

## Hosted regression results

The rebuilt full suite passed **16/16 CTest targets, 119,157 checks, 0
failures** in 1,291.41 seconds with
`ctest --test-dir build --output-on-failure`.

| CTest target | Checks | Result |
|---|---:|---|
| `database_storage_tests` | 172 | Passed |
| `database_relational_test` | 10,411 | Passed |
| `database_transaction_test` | 2,553 | Passed |
| `database_sql_test` | 1,779 | Passed |
| `database_index_test` | 2,752 | Passed |
| `database_sql7_test` | 1,307 | Passed |
| `database_sql8_test` | 4,503 | Passed |
| `database_sql9_test` | 15,274 | Passed |
| `database_sql9r_test` | 53,898 | Passed |
| `database_native_io_test` | 19 | Passed |
| `database_portable_runtime_test` (P1) | 20,668 | Passed |
| `database_wal_portable_test` (P2) | 305 | Passed |
| `database_wal_page_portable_test` (P3) | 106 | Passed |
| `database_wal_recovery_portable_test` (P4) | 204 | Passed |
| `database_ndb1r4p5_test` | 4,594 | Passed |
| `database_ndb1r4p6_test` | 612 | Passed |

The pre-P6 accepted baseline was 15/15 targets and 118,532 checks. The final
count includes the P6 test and the 13 additional relational checks for
explicit buffer-manager initialization and status handling.

## Freestanding compilation and remaining boundary

`tools/compile_portable_freestanding.ps1` compiled the accepted portable
objects plus the production `gx_page_cache.cpp` with the Server AMD64 flags:
`-std=c++14 -ffreestanding -fno-exceptions -fno-rtti -nostdlib
-nostdinc++`, the large code model, no red zone, no SIMD, and no PIC. The
compiled objects were `gx_allocator`, `gx_array`, `gx_recovered_pages`,
`gx_page_cache`, `gx_owner`, `gx_string`, `database_checksum`,
`gx_page_codec`, `gx_wal_record`, `gx_wal_scan`, and `gx_wal_page_image`.

The symbol audit found no hosted STL, allocation, exception, RTTI, or C runtime
references in `gx_page_cache.o`; it references only the P1 `gxAllocatorAllocate`
and `gxAllocatorFree` functions. The existing checksum object has the
previously accepted `__cxa_guard_acquire` and `__cxa_guard_release`
references. Compilation does not include or qualify the full hosted
`BufferManager`, `DatabaseFile`, transaction overlay, catalog, parser, or
SQL9 engine.

Remaining hosted ownership includes the decoded `DatabasePage` vector views
and page copies, `std::map` plus `std::vector` in the live transaction
overlay, `std::string` result messages, hosted file providers, and broader
catalog/query containers. A later phase can address live transaction page
ownership after reviewing that dependency inventory.

No `.gxdb` or `.gxwal` version, page layout, page CRC, catalog layout, heap
layout, B+ tree format, WAL record, or transaction-ID encoding changed. The
Server repository and Git topology were not changed; no push was performed.

## Recommended P7 scope

Review the remaining ownership inventory, then focus narrowly on **Portable
Live Transaction Page Ownership** if no smaller prerequisite emerges. Keep
parser AST, query-result, catalog, and JOIN/GROUP BY conversions out of that
phase.
