# guideXOS SQL — Phase SQL1: Durable Database Storage Foundation

Status: **implemented and hosted-proven**. This document is the authoritative
description of the SQL1 on-disk format, storage engine, integrity model,
durability contract and limitations.

SQL1 deliberately stops at durable, validated, page-oriented storage. It does
**not** implement SQL, tables, indexes, transactions or networking. Those are
later phases built on this foundation.

---

## 1. Ownership and relationship to guideXOS Server

This repository (`guideXOS_SqlDatabase`) is the home of the native guideXOS
relational database subsystem. It is currently a standalone, portable C++
library plus hosted tests so it can be developed and verified independently of
the full `guideXOSServer` tree (which lives at `D:\dev\guideXOSServer` and was
used as the conventions reference).

Ownership decisions:

- The storage engine owns the `.gxdb` file format, the page layout, allocation
  and the integrity/durability contract.
- The storage engine does **not** own a filesystem. All byte I/O goes through
  the `IDatabaseFile` interface. The hosted implementation uses `stdio`.
- A future guideXOS integration supplies an `IDatabaseFile` adapter over the
  native VFS / block device. The existing server `FS` abstraction exposes only
  whole-file `readAll`/`writeAll` and no seek/flush primitive, so it cannot back
  page I/O as-is; that adapter is a deliberate later-phase task. No parallel
  filesystem abstraction is introduced here.
- Future SQL parser/executor code will live in a separate module and consume
  `DatabaseFile`; it will not reach into page internals.

Naming follows guideXOS conventions: namespace `gxos::db`, `#pragma once`
headers, `std::uint*_t` types, bounded-resource philosophy, and the existing
CRC32 polynomial (`0xEDB88320`) already used by the web content decoder.

---

## 2. File layout

```
Database File (.gxdb)
│
├── Page 0  File Header        (kHeaderPageId = 0)
│           serialized header in bytes [0,128); rest of page zero
│
├── Page 1  Root / Bootstrap   (kBootstrapPageId = 1)
│           page header + versioned catalog record
│
└── Page 2..N  Data Pages      (append-only)
             page header + opaque payload
```

All pages are fixed size. `pageCount` in the header counts every allocated page,
including page 0.

### Byte order

Every multi-byte integer is stored **little-endian**, explicitly serialized via
`database_endian.h`. No C++ struct is ever written to disk directly; layout,
size and byte order are controlled and stable across compilers and
architectures.

---

## 3. File header (page 0)

`kHeaderSize = 128` bytes. The remainder of page 0 is reserved and zeroed.

| Offset | Size | Field                  | Notes |
|-------:|-----:|------------------------|-------|
| 0      | 8    | Magic                  | `47 58 44 42 0D 0A 1A 0A` = `"GXDB\r\n\x1a\n"` |
| 8      | 2    | formatMajor            | currently `1` |
| 10     | 2    | formatMinor            | currently `0`; minor may advance compatibly |
| 12     | 4    | pageSize               | power of two, 512..65536 |
| 16     | 16   | databaseId             | RFC 4122 version-4 identity |
| 32     | 8    | creationTimeUnixNanos  | `0` = unknown |
| 40     | 8    | pageCount              | total allocated pages incl. page 0 |
| 48     | 8    | rootPageId             | bootstrap/catalog page (normally 1) |
| 56     | 4    | flags                  | reserved |
| 60     | 4    | headerSize             | must equal `128` |
| 64     | 4    | headerCrc32            | CRC32 over `[0,128)` with this field zeroed |
| 68     | 60   | reserved               | zero |

### Database identity

A 16-byte RFC 4122 version-4 UUID generated from `std::random_device` seeded
with the clock. It is rendered canonically (`8-4-4-4-12`) for diagnostics. A
caller may supply a deterministic identity via `DatabaseCreateOptions`
(`generateDatabaseId = false`), which the tests use.

---

## 4. Page header (pages 1..N)

`kPageHeaderSize = 32` bytes, followed by the payload.

| Offset | Size | Field        | Notes |
|-------:|-----:|--------------|-------|
| 0      | 8    | pageId       | must equal the page index |
| 8      | 2    | pageType     | `0 Unknown`, `1 Catalog`, `2 Data` |
| 10     | 2    | flags        | reserved |
| 12     | 4    | payloadSize  | used bytes in the payload area |
| 16     | 4    | generation   | reserved for later versioning |
| 20     | 4    | reserved0    | zero |
| 24     | 4    | pageCrc32    | CRC32 over the whole page with this field zeroed |
| 28     | 4    | reserved1    | zero |
| 32     | pageSize-32 | payload | opaque |

Payload capacity is `pageSize - 32` (e.g. 4064 bytes at 4 KiB).

### Bootstrap / catalog page

Page 1 carries a versioned catalog record in its payload:

| Offset | Size | Field          | Value |
|-------:|-----:|----------------|-------|
| 0      | 4    | catalogMagic   | `'G','X','C','T'` little-endian |
| 4      | 2    | catalogVersion | `1` |
| 6      | 2    | reserved       | 0 |
| 8      | 4    | entryCount     | `0` in SQL1 (no tables yet) |
| 12     | 4    | reserved       | 0 |

The chain `header -> rootPageId -> catalog page` is followed and validated on
every open. Later phases attach schemas, tables and indexes here.

---

## 5. Page size decision

Default **4096 bytes**, with 512 / 1024 / 2048 / 4096 / 8192 / 16384 / 32768 /
65536 accepted. Rationale:

- 4096 is already the guideXOS server allocator page granularity
  (`gxos::PageSize = 4096`) and a common device/cluster size.
- Power-of-two sizes keep offset arithmetic cheap and alignment checks simple.
- 65536 upper bound keeps every page buffer small and bounds any per-page
  allocation to at most 64 KiB, even for hostile input.

Page size is stored in the header; readers honor it and reject unsupported
values.

---

## 6. Integrity / checksum strategy

CRC32 (IEEE 802.3, reflected polynomial `0xEDB88320`) protects:

- the **header** — over `[0,128)` with the CRC field zeroed;
- every **page** — over the whole page with the CRC field zeroed.

What it detects: accidental corruption (bit flips, stray writes, most
truncations that change covered bytes, torn writes that alter content).

What it does **not** provide: cryptographic authentication. An adversary who
rewrites a page can recompute the CRC. This is corruption detection, not
tamper-proofing.

---

## 7. Allocation strategy

- **Append-only.** `allocatePage` appends at `pageCount * pageSize`, writes a
  zeroed page, then increments the in-memory `pageCount`.
- No free-page reuse in SQL1. Freed pages are deferred to a later phase.
- Bounded by `kMaxPageCount = 2^32` pages; exceeding it returns `NoSpace`.
- Every offset is computed with overflow-checked 64-bit arithmetic
  (`checkedMulU64` / `checkedAddU64`). `pageId > kMaxPageCount` and any
  arithmetic overflow are rejected before I/O.
- Writes outside `[0, pageCount)` are rejected with `OutOfBounds`.

---

## 8. Durability contract

SQL1 does **not** implement a write-ahead log, MVCC or atomic multi-page
transactions. The contract is intentionally modest but explicit:

1. `writePage` / `allocatePage` issue the underlying positional write
   immediately.
2. `flush()` writes the header page **last** (only if it changed) and then
   requests an OS durability barrier (`_commit` on Windows, `fsync` elsewhere).
3. `close()` flushes best-effort, then closes.
4. Reopen after a successful `flush()` + `close()` observes the committed bytes.

Header-last ordering guarantees the header never references a page that was not
written first. A crash can therefore leave **uncommitted orphan pages** past
`pageCount`; these are ignored on reopen and overwritten by the next append.

Explicitly deferred to a later phase: power-loss-atomic multi-page
transactions, WAL, torn-page protection and crash recovery.

Hosted note: `HostDatabaseFile::flush()` performs both `fflush` and an
OS-level barrier, which is stronger than a plain C++ stream flush. The guideXOS
native backend must provide an equivalent barrier; if a future VFS only offers
buffered writes, that limitation must be reported rather than hidden.

---

## 9. Creation and open behavior

### Create (`DatabaseEngine::createDatabase`)

1. Reject empty path and unsupported page size (`InvalidArgument`).
2. Refuse to overwrite an existing non-empty file unless
   `overwriteExisting == true` (`AlreadyExists`).
3. Build header with `pageCount = 2`, `rootPageId = 1`.
4. Write the bootstrap page, then write the header page via `flush()`.
5. Flush; a successful return means the minimum structure is durable.
6. On failure, a newly created partial file is removed.

### Open (`DatabaseEngine::openDatabase`)

Validation order (fail-fast, bounded, no silent repair):

1. File exists; otherwise `NotDatabase`.
2. Size ≥ 128; otherwise `Truncated`.
3. Parse header: magic (`NotDatabase`), major version
   (`UnsupportedVersion`), page size (`CorruptHeader`), header size
   (`CorruptHeader`), header CRC (`CorruptHeader`), `pageCount ≥ 2`,
   `pageCount ≤ kMaxPageCount`, `rootPageId ∈ [1, pageCount)` (all
   `CorruptHeader`).
4. `pageCount * pageSize` must not overflow; file must be at least that large
   (`Truncated`); file size must be page-aligned (`CorruptHeader`). Extra full
   pages (orphans) are tolerated and ignored.
5. Read and validate the root page (CRC, page id, `Catalog` type, catalog
   magic/version). A bad catalog version yields `UnsupportedVersion`.
6. If `validateAllPages`, CRC-check every page.

An unknown future **major** version is never reinterpreted; it fails with
`UnsupportedVersion`.

### Failure classes

`Ok`, `InvalidArgument`, `NotDatabase`, `UnsupportedVersion`, `Truncated`,
`CorruptHeader`, `CorruptPage`, `IoError`, `OutOfBounds`, `NoSpace`, `NotOpen`,
`AlreadyExists`, `ReadOnly`, `Internal`. These are returned in a `DbResult`
(status + bounded message), not an exception hierarchy.

---

## 10. Corruption and truncation behavior

| Fault | Result |
|-------|--------|
| Wrong magic | `NotDatabase` |
| Unknown major version | `UnsupportedVersion` |
| Header field tampered / header CRC mismatch | `CorruptHeader` |
| Invalid page size / header size | `CorruptHeader` |
| `pageCount` below 2 or above policy max | `CorruptHeader` |
| `rootPageId` outside allocation | `CorruptHeader` |
| File smaller than header | `Truncated` |
| File smaller than allocated pages | `Truncated` |
| Non-page-aligned size | `CorruptHeader` |
| Root page CRC / id / type / catalog bad | `CorruptPage` / `UnsupportedVersion` |
| Data page CRC bad (on read or full validation) | `CorruptPage` |

A malformed `.gxdb` always produces an error; it never causes an
out-of-bounds read/write, unbounded allocation or a crash.

---

## 11. Diagnostics

`DatabaseDiagnostics` (read-only) exposes: open/read-only state, format
version, page size, page count, root page id, file size, canonical database
identity, current state and the last structural validation failure. The
`gxdb_cli inspect <path>` helper prints these for later Database Manager /
Developer Studio tooling.

---

## 12. Bounded-resource rules honored

- No unbounded recursion.
- No allocation proportional to untrusted on-disk values; page buffers are
  bounded by the validated page size (≤ 64 KiB) and payloads by page capacity.
- Every file offset/size uses overflow-checked 64-bit arithmetic.
- No silent narrowing: sizes are range-checked before any `uint32_t` cast.
- No gigantic static buffers (header scratch is 128 bytes).
- No background threads.
- Database files are treated as untrusted input.

---

## 13. What SQL1 intentionally does NOT implement

SQL tokenizer/parser; SELECT/INSERT/UPDATE/DELETE; tables/columns beyond the
bootstrap record; B+ tree indexes; joins; query optimizer; WAL; MVCC;
multi-user locking; users/passwords; networking; HTTP database APIs; REXX
commands; Developer Studio UI; Database Manager; `system.*` virtual tables;
application permission/capability integration; change notifications.

---

## 14. Verification status

- **Hosted proof: complete.** The engine is built and its full test suite runs
  on the host toolchain (MinGW-w64 g++ 15.2 / CMake / Ninja). Tests exercise
  real files on disk through `HostDatabaseFile`.
- **QEMU proof: not performed** in SQL1.
- **Bare-metal proof: not performed** in SQL1.

The native guideXOS VFS integration is deferred because the current server `FS`
abstraction offers no positional seek/flush primitive. This is a proof gap, not
a format gap: the format and engine are backend-agnostic behind
`IDatabaseFile`.

See `docs/SQL1_TEST_REPORT.md` for the test inventory and totals.
