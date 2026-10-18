# NDB1R4P11R2: Portable Heap-Page Construction

## Outcome

**Outcome A — `HEAP_PAGE_CONSTRUCTION_SEAM_VERIFIED`.** SQL heap mutation
repacking now prepares the existing heap payload format in bounded
`GxArray` storage, checks the preparation status at its production caller, and
stages the hosted `DatabasePage::payload` replacement before publishing it.
The result qualifies this packing and error-propagation seam only. It does not
convert the `DatabasePage` owner or other heap/index page writers.

## Starting state and gates

- SQL checkout: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting HEAD: `d08585f6d2188e5d3078113765aa7689297808d0`, the committed R1
  report and allocator-contract changes.
- Pre-edit worktree: clean. R2 report did not exist.
- `.phase` remained `project=guideXOSSql`, `last_completed=SQL9`,
  `next_expected=SQL10`.
- `.native-sql.phase` remained `project=guideXOSSqlNativePortability`,
  `last_completed=NDB1R4P10`, `next_expected=NDB1R4P11`.
- The preserved P11 ownership audit, R1 allocator report, and P10
  qualification report were read. R1 Outcome A remains the accepted
  `PAGE_ALLOCATOR_CONTRACT_VERIFIED` baseline: its 18/18 CTest suite had
  145,014 checks, and its focused P6 allocator checks numbered 619.
- The read-only Server checkout remained on `NATIVE_DB_STORAGE` at
  `f29850c93bbac0a0763a31e806d2816310156575`; it was inspected only.

R1's allocator rules remain intact. A null page-cache allocator selects the
existing hosted adapter. An explicitly supplied incomplete allocator is
rejected with `InvalidArgument`. Valid callback/context tuples are copied into
the cache manager; their context remains borrowed and must outlive the
database. R2 uses that same database-scoped allocator only for temporary heap
preparation storage and releases the scratch allocation before returning.
The page payload does not retain or transfer this allocator provenance.

## `packHeapPage` audit and call graph

The production implementation is the internal `packHeapPage` in
`database/database_heap.cpp`. Before R2 it accepted a `DatabasePage`, ordered
encoded row byte vectors, the next heap-page id, and payload capacity. It
assigned a zero-filled payload vector, copied rows forward from the 24-byte
heap header, wrote `{rowOffset,rowLength}` slots backward from payload end,
then wrote the existing heap header and set `PageType::Data` and
`payloadSize`. It retained the page id, flags, and generation. The function
used `std::vector::assign` without a returned allocation status and could
return `NoSpace` after directly replacing the caller's local payload.

There was one production caller: `Table::applyMutationsInTransaction`. It
reads each affected page through the active `PageAccess`, builds rows in
physical slot order, computes the prefix that fits, and calls
`packHeapPage`. The actual path is:

```text
SqlEngine::executeUpdate / executeDelete
  → SqlEngine::applyMutationPlan
  → Table::applyMutations
  → Table::applyMutationsInTransaction
  → packHeapPage
  → gxHeapPagePrepare
  → checked GxStatus
  → staged hosted-vector transfer and swap
  → PageAccess::writePage (transaction overlay)
  → append relocated rows, then maintainIndexes and updateStats
```

When an explicit transaction is active, `applyMutationPlan` takes a statement
savepoint and restores it on error. Without an explicit transaction,
`Table::applyMutations` owns an implicit transaction and rolls it back if the
mutation fails. The `changes` and relocation vectors used for locator mapping
are local planning data; `maintainIndexes` runs only after all affected pages
have packed and staged successfully. A failed pack therefore cannot make a
new locator authoritative. R2 does not change WAL ordering or the index
publication order.

`Table::appendEncodedRowToTail` remains a separate hosted path for incremental
tail-page insertion and one-row new-page construction. It did not call
`packHeapPage` before R2 and is not part of this conversion.

## Selected preparation seam and allocator propagation

The old pack loop is replaced by `gxHeapPagePrepare` in the new portable
`database/portable/gx_heap_page_builder.cpp`. Its row source is a bounded
callback view over the already-planned row slots, so building descriptors does
not add an allocation. The portable helper uses the existing `GxArray`
facility and the new status-returning `gxArrayResize` and owner-preserving
`gxArraySwap` operations.

The helper validates the configured page size against the existing 512–65,536
byte policy, derives payload capacity as `pageSize - kPageHeaderSize`, checks
slot and row-byte arithmetic, validates row lengths, verifies capacity, and
only then allocates its replacement array. It writes the existing 24-byte
heap header, contiguous row area, backward-growing slot directory, and zeroed
free space. It preserves the little-endian fields and row order. It does not
write the 32-byte outer database page header or CRC; the existing page codec
still does that unchanged.

The helper prepares into a temporary `GxArray`. It swaps that owner into its
output only after all row callbacks and encoding succeed. Allocation failure,
invalid input, capacity failure, overflow, or a later row-source error frees
the temporary and preserves the previous output. The result distinguishes
`InvalidArgument`, `ResourceLimit`, `Overflow`, `OutOfMemory`,
`UnsupportedAlignment`, and `CorruptData` through `GxStatus`.

`packHeapPage` obtains the effective allocator from
`_db.buffer().scratchAllocator()`. `BufferManager::init` has already applied
the R1 default-or-validate rule, so this is the supplied valid allocator or
the existing hosted default. The callback context is valid for the open
database, and the scratch allocation is destroyed before `packHeapPage`
returns. It is not borrowed from a transaction or retained in a public page.

## Publication and failure guarantees

The hosted page vector remains in place. After portable preparation succeeds,
`packHeapPage` copies the complete bounded result into a separate temporary
`std::vector<uint8_t>` inside a `bad_alloc` handler. If that hosted allocation
fails, it returns `DbStatus::NoSpace`, frees the portable scratch, and leaves
the existing page payload and metadata unchanged. Only after the vector is
complete does it swap the payload into the local `DatabasePage` and set the
page type and payload size. The normal `writePage` call follows.

Thus the selected path checks both its portable scratch allocation and its
hosted handoff before page staging. This does not make every `std::vector`
allocation in row planning, other heap operations, index builders, readers, or
the page owner failure-aware. The test fault allocator injects at the portable
boundary; it does not exhaustively inject the standard-library vector
allocator.

For `GxStatus`, the heap caller maps out-of-memory to `NoSpace`, resource or
arithmetic limits to `TransactionTooLarge`, corrupt row bytes to
`CorruptPage`, invalid inputs to `InvalidArgument`, and unsupported alignment
to `Internal`. The production caller checks the `DbResult` and returns it
before `writePage`. A failure after a prior page was staged is removed by the
existing statement savepoint or implicit-transaction rollback.

## Focused encoding and failure tests

Added CTest target `database_heap_page_construction_test`, with **159 checks
and zero failures**. Its independent test oracle compares complete deterministic
payload bytes, including free-space zeroes and slot bytes. Coverage includes:

- Empty, one-row, multi-row, variable-width, compacted, and repeatedly packed
  pages, with embedded zero bytes and next-page metadata.
- A 512-byte page and the normal 4,096-byte page size.
- Exact maximum utilization and a one-byte-over-capacity rejection.
- The 512-byte slot-directory boundary (28 minimum rows fit; 29 do not).
- Invalid short and inconsistent row lengths, invalid page sizes, and checked
  row-count arithmetic overflow.
- First scratch allocation failure, allocator-capacity/reserve failure,
  failure after prior successful use, output preservation, cleanup after a
  callback failure partway through preparation, and retry after failure.
- A row provider that changes its returned length between validation and
  encoding is rejected before it can overlap the slot directory; the previous
  output remains intact.
- A valid explicit allocator, an incomplete explicit allocator, and the
  hosted allocator adapter. The R1 P6 test retains the production checks for
  omitted allocator defaulting and explicit incomplete allocator rejection.
- Balanced temporary allocation cleanup.

The production SQL test uses a 512-byte database with primary and secondary
indexes and variable-width rows. It injects failure on the second heap-page
scratch allocation during a multi-page DELETE, after the first rewritten page
has been staged in the transaction overlay. The SQL statement fails, the
explicit transaction remains active, and the statement savepoint restores all
rows and index entries while preserving an earlier successful UPDATE. The
same DELETE then succeeds on retry; an explicit rollback restores the original
rows. A later variable-width UPDATE relocates rows and changes a secondary
index key, commits, closes, and reopens. The relocated row is found by the
secondary index and all indexes validate after reopen.

The full retained SQL9 and P8 workload also passed, including INSERT, UPDATE,
DELETE, primary and secondary indexes, foreign-key enforcement, statement
failure and reuse, explicit rollback, commit/reopen, WAL recovery, and
savepoint coverage.

## Freestanding qualification and hosted regression

The qualification script now includes the new production translation unit.
It ran under Windows PowerShell 5.1 (`5.1.26100.9711`) with MinGW-W64 `g++`
15.2.0 and GNU `nm` 2.46.0.20260210. The script used the Server AMD64
configuration: C++14, `-ffreestanding -nostdlib -nostdinc++ -fno-exceptions
-fno-rtti -fno-builtin -DGXOS_BARE_METAL`, the Server kernel include roots,
and AMD64 `-m64 -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2`,
non-PIC/PIE, no stack probe/check, and no stack protector flags.

The expanded inventory is 33 translation units: **14 of the original 32
passes remain, and the newly added `gx_heap_page_builder.cpp` is the one new
portable pass. Eighteen prior hosted blockers remain.** This is not a newly
qualified `database_heap.cpp` object; it still stops at the hosted
`<memory>` dependency in `database_heap.h`.

New object undefined symbols are limited to project array APIs:

```text
gxArrayInit
gxArrayResize
gxArraySwap
gxArrayDestroy
```

No standard-library, exception/unwind, RTTI, global allocation, hosted I/O,
or additional runtime symbols appeared in the new portable object. The other
15 passing units are the retained portable leaves: `database_format.cpp`,
`database_checksum.cpp`, `gx_allocator.cpp`, `gx_allocator_hosted.cpp`,
`gx_array.cpp`, `gx_recovered_pages.cpp`, `gx_page_cache.cpp`,
`gx_page_codec.cpp`, `gx_wal_record.cpp`, `gx_wal_scan.cpp`,
`gx_wal_page_image.cpp`, `gx_transaction_pages.cpp`,
`gx_transaction_page_index.cpp`, and `gx_transaction_page_overlay.cpp`.

The 18 first diagnostics remain:

| Production unit | Current first blocker |
|---|---|
| `database_io.cpp` | `database_io.h`: missing `<cstdio>` |
| `database_native_io.cpp` | `database_native_io.h`: missing `<memory>` |
| `database_header.cpp` | `database_header.h`: missing `<string>` |
| `database_page.cpp` | `database_page.h`: missing `<vector>` |
| `database_file.cpp` | `database_file.h`: missing `<memory>` |
| `database_engine.cpp` | `database_engine.h`: missing `<memory>` |
| `database_buffer.cpp` | `database_file.h`: missing `<memory>` |
| `database_catalog.cpp` | `database_catalog.h`: missing `<string>` |
| `database_index.cpp` | `database_index.h`: missing `<set>` |
| `database_heap.cpp` | `database_heap.h`: missing `<memory>` |
| `database_wal.cpp` | `database_wal.h`: missing `<memory>` |
| `database_transaction.cpp` | `database_transaction.h`: missing `<memory>` |
| `database_relational.cpp` | `database_relational.h`: missing `<memory>` |
| `database_sql_token.cpp` | `database_sql_token.h`: missing `<string>` |
| `database_sql_tokenizer.cpp` | `database_sql_token.h`: missing `<string>` |
| `database_sql_parser.cpp` | `database_sql_parser.h`: missing `<vector>` |
| `database_sql_expr.cpp` | `database_sql_expr.h`: missing `<vector>` |
| `database_sql.cpp` | `database_sql.h`: missing `<map>` |

The full hosted build completed. It emitted only the same unused test-helper
warnings in `database_relational_test.cpp` and `database_index_test.cpp`; no
production warning was introduced. The complete CTest suite passed **19/19
targets, 145,173 checks, zero failures** in 1,203.99 seconds. This total is
the R1 baseline of 145,014 plus 159 R2 checks. WAL recovery and all prior
P1–P8 and R1 targets passed.

Qualification artifacts were written outside the SQL checkout under
`%TEMP%\guideXOSSql-NDB1R4P11R2-focused` and
`%TEMP%\guideXOSSql-NDB1R4P11R2-freestanding-final`.

## Remaining hosted page operations

R2 leaves `DatabasePage::payload` as `std::vector<uint8_t>`. The current
classification is:

| Operation | Status after R2 |
|---|---|
| `DatabasePage::payload` ownership | Hosted/unconverted; public vector, implicit allocation-dependent copies remain |
| `DatabaseFile::readPage` | Hosted/unconverted; parsed page is copied to output |
| `Database::readPage` | Hosted/unconverted; copy still allocates, with existing `bad_alloc` conversion and pin cleanup |
| `BufferManager::installPage` | Hosted/unconverted; pinned view copy remains hosted and caught |
| Transaction page staging | Partially converted; serialized overlay/WAL images use explicit portable owners, while page parse/materialization still uses the hosted vector |
| Heap/index page construction | Partially converted; mutation repacking uses this seam; tail append/new-page, incremental heap writes, index `writeLeaf`/`writeInternal`, and catalog payload replacement remain hosted |
| Implicit `DatabasePage` copies | Hosted/unconverted; the R1 file-read, database-read, and pinned-cache install copy sites remain, along with allocator-dependent vector parse/materialization |

No claim is made that all `DatabasePage` operations, hosted vector
allocations, or heap/index writers are portable or failure-qualified.

## R3 migration boundary

R3 should address the next coherent ownership group: explicit allocator-aware
page materialization, clone, and publication. The boundary should include:

1. Add a distinct page-payload allocator to database create/open options and
   retain the callback tuple in a database-owned page-allocation context.
   Keep it separate from cache scratch, transaction-image, and WAL allocators.
   A returned `DatabasePage` may outlive `Database`, so document and enforce
   that the borrowed callback context outlives every page allocated with it.
   Preserve hosted null-default behavior and reject incomplete explicit
   allocators.
2. Convert `DatabasePage::parse`, `DatabaseFile::readPage`,
   `BufferManager::materialize`/`pinPage`/`installPage`, `Database::readPage`,
   and `Transaction::readPage` plus commit-time parse/install as one
   prepare-then-publish group. Preserve source pages and balance cache pins on
   every clone/parse failure.
3. Add explicit status-returning `clonePage` and nonallocating move/clear
   operations that carry allocator provenance. After the listed production
   sites and the five explicit P7/P8 test copies are migrated, delete
   `DatabasePage` copy construction/assignment; keep move semantics
   nonallocating. Do not make the transaction manager or unrelated containers
   noncopyable as part of that change.
4. Map allocation failure to existing `DbStatus` values before cache or
   transaction publication. Preserve WAL's durable-commit recovery-required
   behavior if post-WAL cache installation fails.
5. Test failed parse/clone preservation, pinned cache install/read cleanup,
   transaction read and commit, savepoint rollback, WAL recovery, caller-owned
   page lifetime, allocator provenance, and byte-identical outer page codec
   fixtures. Run SQL9/index/foreign-key/reopen regressions and compile the new
   portable owner component under the unchanged AMD64 flags. `database_page.cpp`
   may remain hosted until the separate `DbResult` string dependency is
   addressed.

The heap tail builder `Table::appendEncodedRowToTail` and index writers
`IndexTree::writeLeaf` and `IndexTree::writeInternal` remain later producer
work. Before converting insert-time page construction, verify or add the
explicit-transaction statement rollback boundary in `SqlEngine::executeInsert`;
the current insert path does not take the UPDATE/DELETE statement savepoint.
No R3 work is started by this report.

## Closeout

Both phase markers remain at SQL9 → SQL10 and NDB1R4P10 → NDB1R4P11. The
Server checkout remained read-only at its recorded branch and HEAD. The R2
changes, focused tests, qualification-script inventory update, and this report
are committed locally on the existing SQL `main`; no push or Git topology
change was performed.
