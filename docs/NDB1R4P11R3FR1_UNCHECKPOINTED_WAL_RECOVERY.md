# NDB1R4P11R3F-R1: Uncheckpointed WAL Recovery

## Outcome

**Outcome A — `UNCHECKPOINTED_WAL_RECOVERY_VERIFIED`.** Recovery now validates
committed header images in WAL order and treats the database header as a
possibly later durable state. The uncheckpointed SQL9 workload recovers all
committed rows and index state after the deterministic post-COMMIT hosted-view
allocation failure. A committed WAL header-count regression remains rejected.

`DatabasePage::payload` remains hosted `std::vector<uint8_t>` storage. This
repair does not qualify it for freestanding use and does not close parent
NDB1R4P11.

## 1. Starting checkout and preserved evidence

- SQL checkout: `D:\dev\guideXOS_SqlDatabase`.
- Starting and ending branch: `main`.
- Starting SQL HEAD: `a231629129329ebcb7f86cb1f6efe4a319535893`.
- Starting status contained only the expected R3F changes:
  `tests/database_cache_publication_test.cpp` and
  `docs/NDB1R4P11R3F_PINNED_VIEW_FAILURE.md`.
- The original R3F report remains unchanged and preserves Outcome B and its
  observed `WalCorrupt` failure. Its hosted allocation fault coverage remains
  in the test; this repair adds uncheckpointed and checkpointed control
  histories, multi-page SQL rows, and header-history validation.
- No concurrent agent was active. R3FR1 source and report did not exist at
  the start.
- `.phase` remains `SQL9` → `SQL10`.
- `.native-sql.phase` remains `NDB1R4P10` → `NDB1R4P11`.
- Read-only Server reference remains branch `NATIVE_DB_STORAGE`, HEAD
  `f29850c93bbac0a0763a31e806d2816310156575`, clean and unmodified.
- No branch, remote, worktree, or Git topology operation was performed.

## 2. Original failure reproduction

The original SQL fault fixture was run without its seed checkpoint against the
original recovery implementation. Five earlier committed transactions stayed
in the WAL before the allocation fault:

| WAL transaction | Page images | Header image | Header page count |
|---:|---:|---|---:|
| 1 | 1 | no | — |
| 2 | 1 | no | — |
| 3 | 1 | no | — |
| 4 | 3 | yes | 4 |
| 5 | 5 | yes | 8 |
| 6, faulted COMMIT | 3 | no | — |

Before the faulted COMMIT, the live database page count was 8 while the
on-disk header still said 2 and the physical file was 8,192 bytes (two
4,096-byte pages). The injected allocation was reached once in the real
post-WAL-sync page installation path. `COMMIT` returned the pinned-view
allocation error, marked recovery required, and left the durable WAL intact.

Close returned `RecoveryRequired`. The low-level file close persisted the
already-durable logical page count 8 into the database header, while the file
still contained only two physical pages. Reopen therefore began with a valid
database header page count of 8 and retained WAL history whose first page-count
header image was 4. The original check at
`database/database_relational.cpp` compared that first historical image with
the later on-disk count and returned:

```text
WalCorrupt: recovery header images are incompatible or regress page count
```

The page size and database identity matched. The count sequence inside the
committed WAL was monotonic (4, then 8); it did not describe a shrink.

## 3. Expanded uncheckpointed and checkpointed comparison

The regression uses the same SQL workload in both cases. It creates parent and
child tables, a child primary key, a secondary index, a foreign key, two parent
rows, and 48 variable-width child rows in three explicit insert transactions.
It then updates row 3 and deletes row 4 in another transaction. The faulted
transaction updates row 7. The expected post-recovery state is 47 rows, with
row 3 equal to `pre_fault_update`, row 7 equal to `newtag`, and no row 4.

### Uncheckpointed history

- Page size: 4,096 bytes.
- Before the fault: database header page count 2; physical database length
  8,192 bytes; in-memory page count 48.
- Nine earlier commits were in WAL. Header-image page counts, in commit order,
  were 4, 8, 23, and 48, from transactions 4, 6, 7, and 8.
- The other earlier committed transaction page-image counts were
  1, 1, 1, 3, 3, 5, 20, 31, and 14 for transactions 1–9.
- WAL header: format version 1, page size 4,096, generation 1, header
  last-transaction ID 0. Scanning the record stream found all nine committed
  transactions; the header's last-transaction field is only advanced at
  checkpoint.
- The injected durable COMMIT was transaction 10, with four page images and
  no header image. WAL scanning found ten committed transactions.
- WAL length after the fault and close: 343,680 bytes.
- After close, database header page count was 48 while physical database
  length remained 8,192 bytes. The WAL supplies the missing pages.

### Checkpointed control

After the same nine seed commits, `flush()` checkpointed the database before
the fault. The database header page count was 48 and the physical file length
was 196,608 bytes. The WAL header was version 1, page size 4,096, generation 2,
last-transaction ID 9. The subsequent faulted COMMIT was transaction 10; it was
the only committed transaction retained in WAL (16,616 bytes). Reopen
recovered successfully. This control confirms the SQL rows and fault path are
otherwise equivalent and isolates retained earlier header images as the
uncheckpointed condition.

The same uncheckpointed workload also passed at the supported 512-byte page
size.

## 4. Recovery audit and correction

`Database::openImpl` defers base-page validation when a WAL sidecar exists.
`openAndRecoverWal` scans the record stream, and the scanner retains each
transaction only when its COMMIT record is present. `applyRecovery` first
checks recovery coverage, then replays committed transactions in scanner
order, applying page images, flushes the database, validates opened pages, and
checkpoints WAL only after successful recovery. Incomplete uncommitted tails
remain excluded under the existing scanner contract.

The old `validateRecoveryCoverage` initialized its running page count from the
physical database header, then required every historical WAL header image to
be at least that value. This mixed two different points in time: the disk
header can already contain a later committed count while older committed WAL
images remain necessary to reconstruct pages.

The corrected invariant is:

1. Each header image must match the database page size and identity.
2. Committed WAL header-image page counts must not regress relative to the
   preceding committed WAL header image. This preserves the engine's
   no-shrink rule (`DatabaseFile::setPageCount` rejects decreases).
3. The recovery extent is the maximum of the physical database header count
   and all retained committed WAL header counts.
4. Replay does not apply an older header image over a later current logical
   page count. Header images at or above the current count are still applied
   in commit order before their transaction's page images.
5. Existing extent arithmetic, missing-page coverage bitmap, page identity,
   page type, and page CRC validation remain active.

This accepts an already-advanced but incomplete database header only when the
committed WAL still provides valid redo and complete coverage for the missing
physical tail. It does not choose an arbitrary maximum without validating the
WAL history.

## 5. Corruption and allocation-failure evidence

A new negative test writes two individually valid, durably committed WAL
header images with page counts 4 then 3. Recovery returns `WalCorrupt` and
retains the WAL. The compatibility correction therefore preserves rejection
of an impermissible committed page-count regression.

Existing SQL9 WAL tests remain enabled for invalid record/header checksums,
foreign database identity, malformed framing, incomplete trailing
transactions, invalid page images, unsupported versions, and missing recovery
coverage. Existing recovery I/O failures continue to return their I/O status.

The R3F allocation hook still targets the `DatabasePage::parse` payload
allocation in the pinned page installation path. It fires once, returns the
SQL transaction error `pinned page-view allocation failed` after durable WAL
COMMIT, preserves the prior cached bytes and slot pointer, and leaves pin
counts balanced. Reads and flush are blocked with recovery required before
reopen. Direct cache tests retain repeated failure, disabled-fault retry,
cache-image preservation, eviction, close/reopen, and allocator-accounting
checks.

## 6. SQL9 recovery and durable COMMIT classification

The SQL regression has 48 child rows with variable-width text across multiple
heap pages, primary and secondary indexes, and a foreign-key relationship.
It covers insert, update, delete, multi-transaction commits, and page-count
growth. After recovery, an independent expected-row model checks all 47
remaining rows, parent IDs, exact text values, absence of deleted row 4,
primary-index row locators, and that rows still occupy multiple heap pages.
Every index is structurally validated; catalog tables and the foreign key are
present; `validateIntegrity` succeeds; secondary-index lookups find row 7's
`newtag` value and row 3's committed pre-fault update.

The caller-visible fault result is a SQL `TransactionError` with message
`pinned page-view allocation failed`; the SQL transaction is no longer active.
The database reports `recoveryRequired`, so it cannot serve stale committed
reads or checkpoint. WAL scanning proves transaction 10 is committed after
the injected post-sync fault. This is a durable commit with incomplete cache
installation, not a rollback. Close reports `RecoveryRequired`; reopen replays
the commit, exposes the exact expected data and indexes, and checkpoints the
WAL. A second close/reopen reports no replay and preserves identical rows and
index locators.

## 7. Changed files and persistent compatibility

- `database/database_relational.cpp`: separate retained-WAL header
  monotonicity from the physical database header baseline; preserve the
  maximum recovered page extent and avoid header-count regression during
  replay.
- `tests/database_cache_publication_test.cpp`: retain original R3F fault
  evidence and add uncheckpointed SQL9, checkpointed control, 512-byte page,
  exact data/index/FK checks, and a committed-header-regression negative test.
- `docs/NDB1R4P11R3FR1_UNCHECKPOINTED_WAL_RECOVERY.md`: this report.

No `.gxdb` or `.gxwal` format, header layout, page encoding, checksum,
page type, heap encoding, B+ tree encoding, PAGE_IMAGE framing, or COMMIT
semantics changed. `DatabasePage::payload` remains `std::vector<uint8_t>`.
No Server source, build setting, marker, or Git state was changed.

## 8. Qualification

Focused regression: **4,884 checks passed, zero failed** across the preserved
cache-publication/fault fixture, uncheckpointed and checkpointed SQL recovery,
512-byte pages, and corruption history test.

Full hosted regression: **21/21 CTest targets passed, 150,342 checks passed,
zero failures**, in **1,005.10 seconds**. This includes SQL1–SQL9, SQL9
prepared statements, native I/O, P1–P8 portability, allocator/page ownership,
R3A–R3F, WAL recovery, statement savepoints, indexes, and foreign keys. No
target or existing assertion was removed or weakened. The full build completed
without a new warning.

The required freestanding command was run with Windows PowerShell
**5.1.26100.9711**, MinGW-w64 `g++` **15.2.0**, and GNU `nm`
**2.46.0.20260210**, using a fresh output directory:

```text
%TEMP%\guideXOSSql-NDB1R4P11R3FR1-20261021
```

The census is **34 production translation units: 16 pass, 18 blocked** by the
established hosted C++ header dependencies. The changed
`database/database_relational.cpp` unit remains blocked through `<memory>`;
the same 16 units pass and the same 18 remain blocked as at R3F. `nm -u -C`
reported 32 distinct undefined symbols in the passing objects: the established
`__cxa_guard_acquire`/`__cxa_guard_release`, `malloc`/`free`, allocator, array,
page codec, CRC32, and transaction-page/index APIs. There are no
`__cxa_throw`, exception-personality, RTTI, `operator new`, or `operator delete`
imports. No new passing unit or portable production dependency was added. This
is object compilation and symbol inspection, not a native SQL link or
execution claim.

The existing hosted page-copy boundaries remain: `DatabaseFile::readPageBytes`
and `readPage`, cache `materialize` and miss-page parsing,
`Database::readPage`, `Transaction::readPage`, the materialized-page cache
compatibility writer, and heap/index/catalog payload construction. The SQL9
`executeInsert` statement/savepoint boundary also remains. No page ownership
conversion was included in this repair.

## 9. Closeout and next continuation

- Outcome: `UNCHECKPOINTED_WAL_RECOVERY_VERIFIED`.
- Both phase markers remain unchanged; parent NDB1R4P11 remains open.
- Original R3F Outcome B report remains preserved.
- No push or Git topology change was performed.
- The local acceptance commit stays on `main`; its SHA is in the final task
  closeout.
- Recommended next P11 continuation: proceed to the separately scoped R3G
  checked reader/`GxOwnedPage` boundary described in the preserved R3F report.
  Do not treat this repair as making `DatabasePage::payload` freestanding.

## Acceptance answer

**Yes.** SQL9 recovers valid multiple uncheckpointed durable WAL commits,
including page-count growth and the later post-COMMIT hosted-view allocation
failure, without requiring an artificial checkpoint, losing committed rows,
or weakening committed header-history regression detection.
