# guideXOS SQL — Phase SQL6: B+ Tree Indexes, PRIMARY KEY, UNIQUE and Indexed Lookup

SQL6 adds the first persistent search structure to guideXOS SQL: **single-column
B+ tree indexes**, along with `CREATE INDEX`, `CREATE UNIQUE INDEX`,
`PRIMARY KEY` and `UNIQUE`, and a deterministic rule-based access-path selector
that uses an index for supported equality and range predicates.

The acceptance question is:

> Can guideXOS SQL persist and maintain transaction-safe B+ tree indexes across
> INSERT, UPDATE, DELETE, rollback, crash recovery and row relocation, while
> enforcing PRIMARY KEY / UNIQUE constraints and producing query results
> identical to full scans?

Answer: yes. This document describes the exact persistent layouts and
algorithms so another implementation can read SQL6 index pages without reading
the C++ sources.

---

## 1. Architecture and ownership

```
SQL (tokenizer / parser / AST / executor)
 |   CREATE INDEX / predicates
 v
Database (relational facade + access-path selector)
 |
 +-- Catalog              durable schema + index metadata (catalog v3)
 +-- Table (heap)         row storage, locator remapping, index maintenance
 +-- IndexTree (B+ tree)  key encoding, search, insert, delete, range, validate
 |
 PageAccess (Database committed view | Transaction private overlay)
 |
 WriteAheadLog (full page images)
 |
 .gxdb / .gxwal
```

SQL never touches index pages directly and the B+ tree knows nothing about SQL
syntax. Index maintenance happens **below** the language layer: `Table::insert`
and `Table::applyMutations` maintain every index, so native callers cannot
bypass constraints. The SQL executor is not the only UNIQUE enforcement path.

---

## 2. Catalog v3 and backward compatibility

The `.gxdb` **outer file format stays 1.0**. Only the catalog payload gains a
new version.

* Catalog v1 (SQL1): empty catalog (no tables). Still opens.
* Catalog v2 (SQL2–SQL5): table records only. Still opens.
* Catalog v3 (SQL6): type-tagged record stream holding table records **and**
  index records.

A database with **no indexes keeps the byte-identical v2 catalog layout**, so
all SQL2–SQL5 databases and their corruption tests behave exactly as before.
The catalog is upgraded to v3 the first time an index/constraint is created.

### v3 root header (payload, 40 bytes)

| offset | size | field |
|-------:|-----:|-------|
| 0  | 4 | magic `0x54435847` (`'G''X''C''T'`) |
| 4  | 2 | catalog version = 3 |
| 6  | 2 | reserved (0) |
| 8  | 4 | tableCount |
| 12 | 4 | nextTableId |
| 16 | 4 | continuationCount |
| 20 | 4 | firstContinuationPageId (u32) |
| 24 | 4 | rootRecordCount (table + index records on the root page) |
| 28 | 4 | indexCount |
| 32 | 4 | nextIndexId |
| 36 | 4 | reserved (0) |

### v3 continuation header (payload, 24 bytes)

| offset | size | field |
|-------:|-----:|-------|
| 0  | 4 | magic |
| 4  | 2 | catalog version = 3 |
| 6  | 2 | reserved |
| 8  | 8 | next continuation page id (0 = end) |
| 16 | 4 | recordCount |
| 20 | 4 | reserved |

### v3 record stream

Each record is `u8 recordType` followed by the record payload.

* `1` = table record — payload is **identical to the v2 table record**:
  `u32 tableId, u32 schemaVersion, u32 columnCount, u64 firstHeapPageId,
  u32 heapPageCount, u64 rowCount, u32 flags, u32 nameLength, name bytes,
  then columnCount × column record`.
* `2` = index record (36-byte fixed prefix + name):
  `u32 indexId, u32 tableId, u32 columnOrdinal, u64 rootPageId,
  u16 flags (bit0 unique, bit1 primary key), u16 formatVersion,
  u64 entryCount, u32 nameLength, name bytes`.

Column record (unchanged from v2):
`u32 ordinal, u16 type, u16 nullable, u32 flags, u32 nameLength, name bytes`.

`rootPageId == 0` means "empty index, no page allocated yet"; the root leaf is
allocated lazily on the first insert. All fields are little-endian and are
explicitly serialized (no C++ struct is dumped). Every parse treats disk bytes
as untrusted and validates bounds, counts, UTF-8 names, flags, types and page
ids.

**Index names** are UTF-8, ≤ 64 bytes, byte-wise case-sensitive, and unique
database-wide. Constraint-backed indexes (PRIMARY KEY / UNIQUE) use a
system-reserved `$`-prefixed name derived from their stable index id
(`$PK_<id>` / `$UQ_<id>`), a namespace the SQL tokenizer cannot produce, so
user names never collide. They are owned by stable index id, not by the string.

---

## 3. New page types

SQL1 page identity and CRC are unchanged. Two new `PageType` values:

| value | type |
|------:|------|
| 3 | `IndexLeaf` |
| 4 | `IndexInternal` |

Both use the ordinary fixed 32-byte page header and the page CRC over the whole
page. Every index page also stores the owning `indexId` in its index header, so
traversal rejects pages that belong to another index.

---

## 4. Key encoding

One canonical encoder per indexable type. The encoding is **order-preserving
under unsigned lexicographic byte comparison**, so the B+ tree can compare raw
encoded bytes and still agree exactly with SQL5 logical ordering.

| logical value | encoding |
|---|---|
| `NULL` | `0x00` |
| any non-NULL | `0x01` + payload |
| Boolean | `0x01, 0x00` (FALSE) / `0x01, 0x01` (TRUE) |
| Int32 | `0x01` + 4 bytes: `(u32)value XOR 0x80000000`, big-endian |
| Int64 | `0x01` + 8 bytes: `(u64)value XOR 0x8000000000000000`, big-endian |
| Float64 | `0x01` + 8 bytes: order-preserving transform of the IEEE-754 bits, big-endian |
| Text | `0x01` + raw UTF-8 bytes (unsigned byte-wise, prefix-shorter-first) |

Consequences:

* `NULL` sorts before every non-NULL value (matching SQL5 `ORDER BY`).
* `FALSE < TRUE`.
* Signed integers use a sign-flip so negative values sort before positive ones
  (little-endian raw bytes are **not** compared lexicographically).
* Text uses no collation/locale; it is the SQL5 unsigned UTF-8 byte order.

### Float64 policy

* Only finite values are indexable. `NaN`, `+Inf` and `-Inf` are rejected by
  index maintenance (`CREATE INDEX` over such a value fails atomically; an
  INSERT/UPDATE producing one fails).
* `-0.0` is normalized to `+0.0` before encoding, so the two zeroes compare
  equal for index/uniqueness purposes, matching SQL5 predicate equality.
* The transform is: if the sign bit is set, flip all 64 bits; otherwise set the
  sign bit. Combined with zero normalization this is strictly order-preserving
  over finite values.

### Maximum index-key size

`maxIndexKeyBytes(pageSize)` guarantees `kIndexMinEntriesPerLeaf = 4` entries
still fit in one leaf page, capped globally at 1024 bytes:

```
capacity  = pageSize - 32
reserved  = 24 (leaf header) + 4 * (8 slot + 16 entry overhead)
maxKey    = min(1024, (capacity - reserved) / 4)
```

For the default 4096-byte page this is 986 bytes. `CREATE INDEX` fails if any
existing non-NULL key exceeds it; INSERT/UPDATE fail atomically for an oversized
indexed key. Unindexed Text columns keep the SQL5 64 KiB limit. Keys are never
silently truncated.

---

## 5. Physical key and duplicate handling

An entry's **physical key** is `logicalKeyBytes || locatorBytes`, where
`locatorBytes` is the RowLocator `(u64 pageId, u32 slot)` big-endian. Therefore:

* All entries with the same logical key form one contiguous range ordered by
  locator. This is the stable physical tie-breaker required for duplicate
  logical keys.
* A non-unique index may hold many entries with the same logical key.
* A UNIQUE index may hold many `NULL` logical keys (NULLs never conflict), each
  distinguished physically by its locator.
* Uniqueness compares the **logical** value, never the physical byte sequence.

---

## 6. Leaf page layout (`PageType::IndexLeaf`)

Payload capacity = `pageSize - 32`. Header is 24 bytes; entries are packed
forward from offset 24; an 8-byte slot directory grows backward from the end of
the payload (exactly like a heap page).

Header (payload offsets):

| offset | size | field |
|-------:|-----:|-------|
| 0  | 4 | magic `0x4C495847` (`'G''X''I''L'`) |
| 4  | 2 | version = 1 |
| 6  | 2 | flags (0) |
| 8  | 8 | nextLeaf page id (0 = end of chain) |
| 16 | 4 | entryCount |
| 20 | 4 | ownerIndexId |

Slot `i` lives at `capacity - 8*(i+1)`: `u32 entryOffset, u32 entryLength`.

Entry at `entryOffset`:
`u32 keyLength, key bytes, u64 locator.pageId, u32 locator.slot`.
`entryLength == keyLength + 16`. Every offset, length, key length and locator is
validated before use; a malformed leaf can never cause an out-of-bounds read or
an untrusted-size allocation.

## 7. Internal page layout (`PageType::IndexInternal`)

Header (24 bytes):

| offset | size | field |
|-------:|-----:|-------|
| 0  | 4 | magic `0x4E495847` (`'G''X''I''N'`) |
| 4  | 2 | version = 1 |
| 6  | 2 | flags (0) |
| 8  | 8 | reserved (0) |
| 16 | 4 | childCount |
| 20 | 4 | ownerIndexId |

Slot directory as above. Child at `childOffset`:
`u64 childPageId, u32 keyLength, key bytes`; `childLength == keyLength + 12`.
The key is the child's **lower bound** (smallest physical key believed to be in
that subtree; empty means −∞).

**Search rule:** at an internal node, follow the *last* child whose lower bound
is `<=` the search key (the first child with an empty bound always qualifies).
Lower bounds are strictly increasing.

**Separator invariant.** A lower bound never decreases. Splits store the first
physical key of the right sibling. Deletion never merges or rebalances, so a
lower bound may become conservative (smaller than the subtree's true minimum),
but it can never become larger than every key it guards. Point lookups and
range seeks remain correct because the chosen child is always the correct one or
a subtree that cannot contain the key.

---

## 8. Root, height and traversal bounds

Each index has one root page, stored in the catalog. The root may be a leaf
(empty/small index) or an internal node after growth. Tree height is bounded by
the page count and by the defensive `kMaxIndexDepth = 32`. Catalog height is
never trusted; `validate()` recomputes it from the actual pages.

Traversal detects page-type mismatch, invalid child id, cycles (visited set and
page-count bound), impossible entry/child counts, malformed keys, unsorted
separators, non-uniform subtree heights, and excessive depth.

---

## 9. B+ tree insertion

1. Descend from the root to the target leaf, recording the internal path.
2. Read the leaf entries; reject an exact physical duplicate (idempotent) and,
   for a unique index, reject any other row with the same non-NULL logical key.
3. Rebuild the leaf in sorted physical order. If it fits, done.
4. Otherwise split at a point where both halves fit; allocate a new right leaf,
   splice the leaf chain (`left.next = new`, `new.next = oldNext`) and propagate
   the right leaf's first physical key as a separator.
5. Insert the new child into the parent after the split child. If the parent
   overflows, split it (each half keeps ≥ 2 children) and propagate upward.
6. If the root splits, allocate a new internal root with two children and update
   the catalog's `rootPageId`.

Every resulting page is transaction-private until COMMIT; the root change and
the catalog update are in the same transaction. The index is never rebuilt for
an INSERT.

## 10. B+ tree deletion (bounded underflow strategy)

SQL6 deliberately does **not** implement merge/rebalance-on-delete:

1. Find the leaf and remove the exactly matching `(logicalKey, locator)` entry.
2. Rebuild the leaf; leave an empty leaf in place and keep the leaf chain intact.
3. Never merge siblings, never shrink the root, never reclaim pages.

Search, equality and range traversal remain correct. Occupancy may drop, but
correct lookup is mandatory and perfect utilization is not. The first-active-key
value of a separator is not required; separators are conservative lower bounds
(section 7). No free-page allocator is introduced in SQL6.

## 11. Leaf chain and range scans

Leaves are linked in physical-key order through `nextLeaf`. A range scan seeks
to the first entry `>= lowerBound || minLocator` (or to the leftmost leaf when
there is no lower bound) and walks the chain:

* inclusive lower bound: start at the first entry `>=` the probe;
* exclusive lower bound: skip entries whose logical key equals the bound;
* upper bound: stop when the logical key exceeds (or reaches, when exclusive)
  the bound;
* `NULL` entries are ordinary entries and participate naturally.

The leaf chain is cycle-guarded and bounded by the page count.

---

## 12. Row-locator remapping (critical invariant)

Heap mutation (SQL5) repacks pages, relocates variable-width rows and compacts
on delete. **A row that was not itself updated can still change its
`pageId+slot` locator.** Index entries must never retain a stale locator.

`Table::applyMutations` therefore reports the complete physical change set to
the index layer, not only the explicitly updated rows:

* `deleted` — every removed row's old locator and old row bytes;
* `changes` — every surviving row whose locator **or** indexed values changed,
  with old locator/old bytes and new locator/new bytes. This includes rows moved
  only by page compaction or by the `UPDATE` relocation of another row, and rows
  appended to the tail because they no longer fit their page.

For each index the maintenance pass:

1. computes the old and new encoded key for every deleted/changed row;
2. applies **all removals first** (so key swaps and same-statement moves are
   legal);
3. applies all insertions, enforcing uniqueness against the post-removal state;
4. updates the diagnostic entry count and marks the catalog dirty.

Consequently page compaction, slot-directory rewrite, UPDATE relocation and
DELETE compaction can never stale an index locator.

## 13. Fetch validation

When an index returns a locator, the executor validates the page id and slot,
reads the heap page, and decodes the row. A stale or impossible locator is
reported as `CorruptPage` (index corruption), never silently dropped as "row
not found". This surfaces maintenance defects immediately.

---

## 14. PRIMARY KEY and UNIQUE semantics

* A table may have at most one single-column PRIMARY KEY. Table-level and
  composite PRIMARY KEY syntax is rejected.
* PRIMARY KEY = UNIQUE + NOT NULL + a persistent unique B+ tree index. The
  column is forced NOT NULL; `NULL PRIMARY KEY` is rejected at parse time.
* UNIQUE is a single-column unique index. **NULL values do not conflict with
  each other** in a UNIQUE index, so multiple rows may hold `NULL`; a second
  non-NULL duplicate fails.
* Constraint violations are atomic: the failing statement leaves the heap and
  every index unchanged, and an enclosing explicit transaction remains usable
  (SQL5 statement-savepoint behaviour).
* Composite `CREATE INDEX ix ON t (a, b)` is rejected as unsupported.
* `Blob` columns cannot be indexed (SQL5 defines BLOB equality but no ordering).

---

## 15. CREATE INDEX

`CREATE [UNIQUE] INDEX name ON table (column)` and constraint-backed indexes:

1. validate table, column, indexable type, index name and name uniqueness;
2. add the index record to the catalog (transaction-private);
3. scan the existing table and insert every row's entry through the B+ tree,
   enforcing uniqueness for a UNIQUE index (multiple NULLs allowed);
4. fail atomically on an oversized key, a non-finite Float64 or a duplicate —
   leaving no catalog-visible index, no durable index pages and no constraint
   side effects.

Inside an explicit transaction the new index is immediately usable
(read-your-writes applies to catalog/index metadata); ROLLBACK removes it and
COMMIT makes it durable. `kMaxTransactionPages = 4096` bounds the build; a
larger table fails with a bounded error and no visible index.

## 16. Access-path selection

A deterministic rule-based selector (no statistics, histograms, cost model,
index intersection or adaptive plans) walks the WHERE conjunction and picks:

```
indexed equality        (best)
then indexed bounded/range comparison
then full table scan
```

It only considers a `column op literal` conjunct whose column has an index,
whose type is indexable, and whose literal coerces to the column type and is
encodable. OR branches and NOT are never indexed. The index produces candidate
row locators; the **complete SQL predicate is still evaluated** against every
fetched row, so the index is an access path and never changes SQL semantics.
Compound predicates may use one indexed conjunct and post-filter the rest.
`ORDER BY` and covering indexes are not index-assisted in SQL6.

Read-only diagnostics expose `accessPath` (`FullScan` / `IndexLookup` /
`IndexRange`), the chosen index name, and the candidate count; these are not
persistent database state and no SQL `EXPLAIN` statement is added.

---

## 17. Transactions, WAL and crash recovery

Index pages use the ordinary transaction-private page overlay and the SQL3 WAL
of full page images. **No WAL-format change is required.** A commit that touches
heap, catalog and index pages is logged as:

```
BEGIN
PAGE_IMAGE  heap/catalog/index leaf/index internal ...
DB_HEADER   (only when the page count changed)
COMMIT
```

The durability point remains the WAL flush. Recovery replays the complete page
images, so after any interruption the database exposes either the complete
pre-state or the complete committed state — never a mixed heap/index state. A
CREATE INDEX crash yields "no index" or "complete valid index"; INSERT, UPDATE
and DELETE crash matrices yield "row absent everywhere" or "row present
everywhere" with every index consistent.

## 18. Validation and corruption

`IndexTree::validate` checks the root, page types and magic/version, owner
index id, no cycles, bounded depth, strictly increasing physical keys within a
leaf, strictly increasing separators, child page ids in range, uniform subtree
heights, acyclic leaf chain, and row locators within the allocated range. The
test harness additionally compares each index against its heap: structural
validity, `entryCount == rowCount`, and every row's locator returned by a lookup
of its own key (proving no missing, stale, duplicate or orphan entries).

Malformed index bytes fail safely with a `CorruptPage`/`CorruptIndex`-class
error. A query that selects a corrupt index returns a corruption failure; it
does **not** silently fall back to a full scan. No traversal is unbounded and no
untrusted size drives an allocation.

---

## 19. Limits and non-goals

* Single-column indexes only; no composite/covering indexes.
* No `DROP INDEX`, no index-page reclamation, no free-page allocator.
* No `FOREIGN KEY` / `REFERENCES`.
* No concurrency expansion: SQL3's single-writer transaction model is retained
  (no MVCC, row/page locks or deadlock detection).
* No statistics/cost-based optimizer; `ORDER BY` is not index-assisted.
* Maximum index-key size and `kMaxTransactionPages = 4096` are enforced.
