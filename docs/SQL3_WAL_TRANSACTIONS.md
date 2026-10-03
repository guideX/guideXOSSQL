# guideXOS SQL — Phase SQL3: Write-Ahead Log and Atomic Transactions

This document defines the SQL3 crash-atomic transaction model, the sidecar
write-ahead log (WAL) byte format, the commit ordering, the durability point,
and the recovery algorithm. It is written so the format is understandable
without reading any C++ object layout.

## 1. Goal and invariant

SQL3 makes a bounded multi-page relational transaction crash-atomic:

> After any interruption point, recovery exposes either the complete state
> before a transaction or the complete state after a durably committed
> transaction — never a partially committed relational state.

The fundamental invariant is:

> **No uncommitted transaction may modify durable `.gxdb` state.**

Uncommitted pages live only in a transaction-private overlay. They are written
to the `.gxdb` only after the transaction's WAL records (including COMMIT) have
been made durable.

## 2. Files

For a database at `path/database.gxdb` the sidecar log is
`path/database.gxwal` (the final extension is replaced with `.gxwal`; when
there is no extension the suffix is appended). The WAL is created lazily on the
first write transaction and removed/replaced whenever a database is created.

## 3. Byte order and primitives

* All multi-byte integers are **little-endian**.
* `u8`, `u16`, `u32`, `u64` are unsigned integers of the given width.
* Checksums are **CRC32 (IEEE 802.3, reflected polynomial `0xEDB88320`)** — the
  same CRC used by the `.gxdb` header and pages. It detects accidental
  corruption, not deliberate tampering.
* No C++ struct is ever serialized directly. Fields are written one at a time
  at fixed offsets.

## 4. WAL header (64 bytes, at offset 0)

| Offset | Size | Field              | Notes |
|-------:|-----:|--------------------|-------|
| 0      | 8    | `magic`            | `47 58 57 41 4C 0D 0A 1A` = `"GXWAL\r\n\x1a"` |
| 8      | 2    | `formatVersion`    | currently `1` |
| 10     | 2    | `headerSize`       | must be `64` |
| 12     | 4    | `pageSize`         | must equal the database page size |
| 16     | 16   | `databaseId`       | SQL1 database UUID (raw bytes) |
| 32     | 8    | `generation`       | increments at every checkpoint |
| 40     | 8    | `lastTransactionId`| highest committed transaction id recorded |
| 48     | 4    | `flags`            | reserved, `0` |
| 52     | 4    | `reserved0`        | `0` |
| 56     | 4    | `headerCrc32`      | CRC32 over `[0,64)` with this field zeroed |
| 60     | 4    | `reserved1`        | `0` |

The WAL is structurally tied to exactly one database: `databaseId` and
`pageSize` must match the opened `.gxdb` or the log is rejected
(`WalDatabaseMismatch`). Filename similarity is never used as proof of
identity.

## 5. Record prefix (20 bytes)

Every record begins with this prefix.

| Offset | Size | Field            | Notes |
|-------:|-----:|------------------|-------|
| 0      | 1    | `recordType`     | see below |
| 1      | 1    | `flags`          | reserved, `0` |
| 2      | 2    | `reserved`       | `0` |
| 4      | 4    | `payloadLength`  | length of the payload that follows |
| 8      | 8    | `transactionId`  | monotonic `u64` |
| 16     | 4    | `recordCrc32`    | CRC32 over the prefix with this field zeroed, then the payload |

Record types:

| Id | Name         | Payload |
|---:|--------------|---------|
| 1  | `BEGIN`      | empty |
| 2  | `PAGE_IMAGE` | see below |
| 3  | `DB_HEADER`  | 128-byte serialized `.gxdb` header image |
| 4  | `COMMIT`     | empty |

A committed transaction is the ordered sequence:

```
BEGIN  PAGE_IMAGE*  DB_HEADER?  COMMIT
```

`DB_HEADER` is present exactly when the transaction changed the allocated page
count. It is required because the `.gxdb` header's `pageCount` cannot be
reconstructed from page images alone.

### PAGE_IMAGE payload

| Offset | Size        | Field       |
|-------:|------------:|-------------|
| 0      | 2           | `pageType`  |
| 2      | 2           | `reserved`  |
| 4      | 8           | `pageId`    |
| 12     | `pageSize`  | full post-transaction page bytes (including the page CRC) |

Full-page redo images are used. They keep recovery independent of page layout
and make redo idempotent.

### Bounds

* `kMaxWalRecordPayload` = `65536 + 64` (a full-page image plus its record
  header). A record claiming more is rejected as corruption, not treated as a
  torn tail.
* `kMaxWalFileBytes` = 64 MiB. Appending beyond this fails with
  `TransactionTooLarge`.
* `kAutoCheckpointWalBytes` = 16 MiB. A new transaction first checkpoints when
  the log has grown past this, bounding the log during long sessions.
* `kMaxTransactionPages` = 4096. The transaction overlay refuses to hold more
  modified pages than this (`TransactionTooLarge`).

## 6. Transaction model

One database, one active writer transaction, no MVCC and no concurrent
writers. The API (conceptual):

```cpp
std::unique_ptr<Transaction> tx;
db->beginTransaction(tx);      // TransactionAlreadyActive if one is open
tx->createTable(def, id);
std::unique_ptr<Table> table;
tx->openTable("Users", table);
table->insert({...});
tx->commit();                  // or tx->rollback();
```

Convenience mutators wrap a single operation in an implicit transaction:

```cpp
db->createTable(def, id);      // BEGIN create COMMIT
table->insert(row);            // BEGIN insert COMMIT
```

If an explicit transaction is active, these calls participate in it instead of
nesting. Nested independent writer transactions are rejected
(`TransactionAlreadyActive`); nested transaction semantics are not faked.

### Transaction-private overlay

A transaction keeps a complete page image for every page it modifies or
allocates. Reads observe the overlay first and the committed database second.
Newly allocated pages get ids above the committed page count and exist only in
the overlay until commit. The committed buffer cache is never dirtied by an
uncommitted transaction.

## 7. Commit ordering and the durability point

```
1. Serialize the catalog (if changed) into the transaction overlay.
2. Append BEGIN.
3. Append every PAGE_IMAGE.
4. Append DB_HEADER (only when the page count changed).
5. Append COMMIT.
6. Flush the WAL (fsync).            <-- DURABILITY POINT
7. Publish page images into the committed buffer and raise the page count.
```

Once `commit()` returns success, either the `.gxdb` already contains the
complete transaction or the WAL durably contains enough information to redo it
completely. Database pages are written to the `.gxdb` lazily by `flush()`,
`close()`, or buffer eviction (a no-force policy); until then the WAL protects
the transaction.

## 8. Checkpoint and cleanup

`Database::flush()` (and `close()`) writes every dirty committed page and the
header, fsyncs the `.gxdb`, then **checkpoints** the WAL: it writes a fresh
header with `generation + 1` and truncates the file to 64 bytes. A clean WAL is
therefore a header with no records. Cleanup is not part of the durability
requirement: if the process crashes after the `.gxdb` flush but before the WAL
checkpoint, the next open simply redoes the still-committed log (idempotently)
and checkpoints again.

## 9. Recovery

On open, before any relational state is exposed:

1. If the WAL is absent or clean, open normally.
2. If the WAL header/records fail integrity checks, fail with a distinct
   status: `WalCorrupt`, `WalUnsupportedVersion`, or `WalDatabaseMismatch`.
   Malformed WAL data never causes arbitrary database writes.
3. Scan records in order. A record that is fully present but fails its CRC is
   corruption. A record that is truncated (the file ends inside the prefix or
   the payload) is a torn tail and ends the valid log.
4. Every transaction terminated by a fully validated `COMMIT` is committed.
   A `BEGIN` without a `COMMIT` is incomplete and is discarded. Because
   uncommitted pages never reached the `.gxdb`, no undo is needed.
5. Redo each committed transaction in order: apply its `DB_HEADER` (raising the
   page count) if present, then write every `PAGE_IMAGE` idempotently.
6. Flush the `.gxdb`, then checkpoint the WAL.

Redo is idempotent: page images are absolute, so replaying a committed
transaction any number of times yields the same bytes. If recovery itself is
interrupted, the next open redoes the same committed log.

### Read-only opens

A read-only open never mutates the database. If the WAL contains committed
transactions it returns `RecoveryRequired`; the caller must perform a writable
open to recover. A clean or absent WAL opens read-only normally.

## 10. Diagnostics

`DatabaseDiagnostics` exposes (read-only) `transactionActive`, `transactionId`,
`transactionModifiedPages`, `walPresent`, `walState`, `walBytes`,
`recoveryRequired`, `lastRecoveryResult` and `pagesRedone`. `WalState` is one
of `Absent`, `Clean`, `Committed`, `Incomplete`, `Corrupt`, `Foreign`,
`UnsupportedVersion`.

## 11. Status model

SQL3 adds `TransactionAlreadyActive`, `NoActiveTransaction`,
`TransactionTooLarge`, `WalCorrupt`, `WalUnsupportedVersion`,
`WalDatabaseMismatch`, `RecoveryRequired`, `RecoveryFailed`, `CommitFailed`
and `RollbackFailed` so callers can distinguish failure classes.

## 12. Backend boundary

All database and WAL I/O goes through `IDatabaseFile`, created by an
`IDatabaseFileSystem` provider. The hosted build uses a stdio provider; a
future guideXOS phase can supply a native VFS provider without touching the
format, transaction or recovery logic. Crash injection in the hosted tests is
implemented as an alternative provider that preserves exactly the bytes written
before a chosen boundary.
