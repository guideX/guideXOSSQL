# guideXOS SQL — Phase SQL2: Relational Catalog and Heap Tables

Status: **implemented and hosted-proven**. This document is the authoritative
description of the SQL2 relational storage layer: the durable system catalog,
the bounded SQL2 type system, heap-organized row storage, the row encoding,
the buffer manager, and the SQL2 durability/crash contract.

SQL2 introduces durable table schemas, typed columns, heap-backed row storage,
bounded scanning and a small buffer/cache layer. It does **not** introduce a
SQL parser. The native relational API implemented here is the execution target
for the future SQL layer.

SQL1 guarantees (header validation, page CRC, root catalog validation,
corruption rejection, truncation handling, overflow-safe addressing, bounded
allocation, diagnostics, explicit flush) are unchanged and still pass.

---

## 1. Architecture

```
DatabaseEngine
    |
  Database
    |
    +-- DatabaseFile   page I/O, validation, durability (SQL1, unchanged)
    |
    +-- BufferManager  bounded LRU page cache over DatabaseFile
    |
    +-- Catalog        durable table/column metadata
    |     +-- root catalog page
    |     +-- catalog continuation pages
    |
    +-- HeapTable       row storage, insert, scan
          +-- heap pages (chained)
          +-- row encoding
          +-- table scan
```

The relational layer never performs raw page I/O directly; it goes through
`BufferManager`, which goes through `DatabaseFile`. No SQL parsing concepts
appear in the storage layer.

---

## 2. File layout

```
Database File (.gxdb)
│
├── Page 0  File Header                     (kHeaderPageId = 0)
│
├── Page 1  Root Catalog Page               (kBootstrapPageId = 1)
│           catalog header + table records
│
├── Page 2..  Catalog Continuation Pages    (as needed)
│           more table records
│
└── Page N..  Heap Table Pages               (PageType::Data)
            encoded rows
```

The file format version stays **1.0** (`kFormatMajor = 1`, `kFormatMinor = 0`).
SQL2 extends the *catalog record* (catalog version 2) without changing the
database header, so a valid SQL1 database with an empty catalog still opens.

---

## 3. Catalog

### 3.1 Catalog versions

| Version | Meaning |
|--------:|---------|
| 1 | SQL1: empty catalog, no tables. Accepted for backward compatibility. |
| 2 | SQL2: relational catalog with table/column records. |

Opening a version-1 catalog yields an empty catalog (`nextTableId = 1`). The
first `createTable` + `flush` rewrites the root page in place as version 2.
The catalog page is already allocated, so no new page is needed for the
upgrade.

### 3.2 Root catalog page payload

`kCatalogRootHeaderSize = 32` bytes, followed by table records.

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | catalogMagic | `'G','X','C','T'` little-endian |
| 4 | 2 | catalogVersion | `2` |
| 6 | 2 | reserved | 0 |
| 8 | 4 | tableCount | total tables across all catalog pages |
| 12 | 4 | nextTableId | next table id to assign (starts at 1) |
| 16 | 4 | continuationCount | number of catalog continuation pages |
| 20 | 4 | firstContinuationPageId | 0 when continuationCount == 0 |
| 24 | 4 | rootRecordCount | table records held in this root page |
| 28 | 4 | reserved | 0 |
| 32 | ... | table records | see 3.4 |

### 3.3 Catalog continuation page payload

`kCatalogContHeaderSize = 24` bytes, followed by table records.

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | catalogMagic | `'G','X','C','T'` |
| 4 | 2 | catalogVersion | `2` |
| 6 | 2 | reserved | 0 |
| 8 | 8 | nextContinuationPageId | 0 = last continuation |
| 16 | 4 | recordCount | table records held in this page |
| 20 | 4 | reserved | 0 |
| 24 | ... | table records | |

### 3.4 Table record

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | tableId | stable id, starts at 1, unique |
| 4 | 4 | schemaVersion | generation; 1 at creation |
| 8 | 4 | columnCount | 0..64 |
| 12 | 8 | firstHeapPageId | 0 when the table has no heap pages |
| 20 | 4 | heapPageCount | number of heap pages in the chain |
| 24 | 8 | rowCount | durable cached row count (see §9) |
| 32 | 4 | flags | reserved, 0 |
| 36 | 4 | nameLength | 1..64 bytes |
| 40 | nameLength | name | UTF-8 |
| ... | 16 + nameLength each | column records | see 3.5 |

### 3.5 Column record

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | ordinal | 0-based, exactly 0..columnCount-1 |
| 4 | 2 | type | DbType (see §4) |
| 6 | 2 | nullable | 0 = NOT NULL, 1 = nullable |
| 8 | 4 | flags | reserved, 0 |
| 12 | 4 | nameLength | 1..64 bytes |
| 16 | nameLength | name | UTF-8 |

All multi-byte fields are little-endian, explicitly serialized. No C++ struct
layout is ever written to disk.

### 3.6 Catalog growth

The root page holds as many table records as fit in
`pageSize - 32 - kCatalogRootHeaderSize` bytes. When full, records continue on
catalog continuation pages (chained via `firstContinuationPageId` /
`nextContinuationPageId`). Continuation pages are reused across saves and new
ones are allocated when the catalog grows. Leftover old continuation pages are
orphaned (allocated but unreachable) and ignored on reopen.

A single table record must fit in one catalog page. The maximum schema is
bounded by `kMaxColumnsPerTable = 64` and 64-byte names; very large schemas
require a larger page size and are rejected at creation time with
`InvalidArgument` ("schema too large for the database page size").

---

## 4. SQL2 data types

| Type | On-disk | Notes |
|------|---------|-------|
| Boolean | 1 byte | 0 = false, 1 = true |
| Int32 | 4 bytes | two's complement |
| Int64 | 8 bytes | two's complement |
| Float64 | 8 bytes | IEEE 754 binary64 |
| Text | u32 length + bytes | opaque byte string, no NUL, no conversion |
| Blob | u32 length + bytes | opaque byte string |
| NULL | null bitmap bit | for nullable columns |

Fixed-width types: Boolean, Int32, Int64, Float64. Variable-width types: Text,
Blob. The storage layer performs **no implicit coercion**: a value presented
for a column must already have exactly that column's type (or be NULL for a
nullable column).

Value limits: `kMaxTextBytes = 65536`, `kMaxBlobBytes = 65536`. The effective
maximum is additionally bounded by the heap page capacity minus row overhead.

---

## 5. Identifier policy

* Table and column names are non-empty UTF-8 byte strings.
* Maximum length: 64 bytes (`kMaxTableNameBytes` / `kMaxColumnNameBytes`).
* Malformed UTF-8 is rejected (`InvalidArgument`). The validator rejects
  overlong encodings, surrogates, code points above U+10FFFF, truncated
  sequences and stray continuation bytes. No normalization is performed.
* Comparison is **byte-wise and case-sensitive**: `Users`, `users` and `USERS`
  are three distinct identifiers. This rule is deterministic and does not
  inherit host-filesystem case semantics.

---

## 6. Row encoding

Rows are self-bounded and versioned. Layout:

| Region | Size | Notes |
|--------|------|-------|
| rowLength | u32 | total row bytes including this field |
| columnCount | u16 | must equal the schema column count |
| flags | u16 | reserved, must be 0 |
| null bitmap | ceil(columnCount/8) bytes | bit i set = column i is NULL |
| fixed data | sum of fixed sizes | non-null fixed columns, ordinal order |
| variable lengths | 4 bytes each | u32 byte length per non-null Text/Blob, ordinal order |
| variable payload | sum of lengths | concatenated variable bytes |

Decoding validates every offset and length before use: the internal `rowLength`
must equal the slot length, the column count must match the schema, the null
bitmap and fixed/variable tables must fit inside the row, and the variable
payload must not overflow the row. A malformed row length can never cause an
out-of-bounds access.

---

## 7. Heap pages

A table is a chain of heap pages (`PageType::Data`):

```
firstHeapPage -> heap page -> heap page -> ... -> (next = 0)
```

### 7.1 Heap page payload

`kHeapHeaderSize = 24` bytes.

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | heapMagic | `'G','X','H','P'` little-endian |
| 4 | 2 | heapVersion | `1` |
| 6 | 2 | flags | reserved, 0 |
| 8 | 8 | nextHeapPageId | 0 = last page |
| 16 | 4 | slotCount | number of row slots |
| 20 | 4 | rowAreaEnd | end of the row area (start of free space) |
| 24 | ... | row area | rows packed forward from offset 24 |
| ... | ... | free space | |
| capacity-8*slotCount | 8*slotCount | slot directory | grows backward from the end |

A slot is `{u32 rowOffset, u32 rowLength}`; slot *i* lives at payload offset
`capacity - 8*(i+1)`. Rows are packed forward from offset 24; the slot
directory grows backward from the end of the payload. A row is appended only
when `rowAreaEnd + rowLength <= capacity - 8*(slotCount+1)`, so the row area
and slot directory never overlap.

### 7.2 Chain validation

Following a heap chain validates: page type is `Data`, the page id is inside
the allocated range, the heap magic/version match, the slot count is possible
(`24 + 8*slotCount <= capacity`), `rowAreaEnd` is within
`[24 + 8*slotCount, capacity]`, and every slot's row lies inside the row area.
Cycles are detected by tracking visited page ids and by bounding the walk to
`pageCount` steps. A corrupt chain returns `CorruptPage`; it never loops
forever.

---

## 8. Buffer manager

`BufferManager` is a small bounded LRU page cache between the relational layer
and `DatabaseFile`.

* **Capacity**: a fixed slot count (default 32; configurable via
  `DatabaseCreateOptions::bufferCapacity` / `DatabaseOpenOptions::bufferCapacity`).
* **Dirty state**: slots carry an explicit dirty flag.
* **Eviction**: deterministic least-recently-used among unpinned slots. A
  dirty victim is written back before its slot is reused.
* **Pinning**: pinned slots are never evicted. If every slot is pinned and a
  new page is needed, the operation fails with `Internal` ("buffer pool
  exhausted").
* **Flush**: writes every dirty slot, then calls `DatabaseFile::flush()`
  (header-last ordering + OS durability barrier).
* **Validation**: pages are read through `DatabaseFile::readPage`, so CRC and
  structure validation still apply to every page that enters the cache.
* No threads, no background flushing.

---

## 9. Durability and flush ordering

SQL2 still has **no WAL and no atomic multi-page transactions**. The write
ordering is conservative:

1. `writePage` / `allocatePage` issue the underlying positional write
   immediately (through the buffer's write-back on eviction/flush).
2. `flush()` re-saves the catalog (if dirty), writes every dirty buffer slot,
   then writes the header page **last** and requests an OS durability barrier.
3. `close()` flushes best-effort, then closes.

Header-last ordering guarantees the header never references a page that was not
written first. A crash can leave uncommitted orphan pages past `pageCount`;
they are ignored on reopen and overwritten by the next append.

**SQL2 crash limitations (explicit):**

* A crash between `flush()` calls loses the un-flushed inserts (the standard
  no-WAL limitation). The heap pages and catalog re-save are only guaranteed
  durable after `flush()`.
* `rowCount` in the catalog is a durable cached count. It is exact after a
  clean `flush()` + `close()`, but may under-count rows if a crash interrupted
  an insert before the catalog page was re-saved. The rows themselves are
  still scanned from the heap.
* An interrupted multi-page operation (e.g. allocating a heap page and linking
  it) can leave an allocated-but-unlinked page. It is an orphan, ignored on
  reopen.

The next transaction/WAL phase closes this gap.

---

## 10. Corruption validation

All catalog and heap parsing treats disk contents as untrusted.

**Catalog:** invalid record size, invalid table id, duplicate table id/name,
impossible column count, malformed name length, unsupported type id, invalid
continuation page, continuation cycle, record count mismatch, trailing
structural inconsistency.

**Heap:** wrong page type, invalid next-page pointer, page-chain cycle,
impossible slot count, row extending outside the page, malformed null bitmap,
malformed variable-length offset, malformed text/blob length.

No unbounded loops, no untrusted-size allocation. Every fault yields a
`DbResult` error (`CorruptPage` / `UnsupportedVersion` / `Truncated` /
`OutOfBounds` as appropriate), never a crash or OOB access.

---

## 11. Backward compatibility with SQL1

* The database header is unchanged (format 1.0).
* A SQL1 database (catalog version 1, empty catalog) opens through the
  relational layer as an empty catalog.
* Creating a table upgrades the catalog to version 2 in place (the root page
  is already allocated).
* All SQL1 regression tests pass unchanged (172 checks).

---

## 12. Diagnostics

`DatabaseDiagnostics` exposes (read-only): open/read-only state, format
version, page size, page count, root page id, file size, database identity,
current state, last validation failure, and the SQL2 relational fields:
`tableCount`, `catalogPageCount`, `bufferCapacity`, `bufferResident`,
`bufferDirty`, and a per-table list (`tableId`, `name`, `columnCount`,
`heapPageCount`, `rowCount`). `gxdb_cli inspect <path>` prints these.

---

## 13. Verification status

* **Hosted proof: complete.** Both the SQL1 suite (172 checks) and the SQL2
  acceptance suite (10304 checks) pass on the hosted toolchain.
* **QEMU / bare-metal proof: not performed** in SQL2.
* **Native guideXOS VFS integration: deferred.** The guideXOS Server `FS`
  interface still lacks the positional read/write/flush primitives required by
  `IDatabaseFile`; see the SQL2 final report for the investigation outcome.
