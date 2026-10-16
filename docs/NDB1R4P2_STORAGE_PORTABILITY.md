# NDB1R4P2: First Production Storage Conversion

## Scope recorded before implementation

The selected slice is WAL record construction in `WriteAheadLog::appendRecord`.
Its local `std::vector<uint8_t>` contains one bounded record: a 20-byte prefix
plus a payload capped at `kMaxWalRecordPayload` (65,600 bytes). The function
computes a CRC over these exact bytes, writes the complete record in one
positional operation, and only then advances `_writeOffset` and
`_bytesSinceCheckpoint`. The replacement will use P1's explicit-allocator
byte array and preserve the record bytes and write ordering.

This slice is chosen ahead of page payloads and transaction overlays because
it is a plain byte buffer with no object lifetime, aliasing, or container
ordering requirements. The failure boundary is also explicit: an encoding
allocation failure must leave the WAL file and in-memory offsets unchanged.
Page payloads, page-image staging, WAL scan collections, and the transaction
page map remain outside this conversion.

## Repository and P1 verification

SQL started on clean `main` at
`b2c1cbca8514c62d08805d0bbc30c1875908a95e`, subject
`NDB1R4P1: add portable runtime foundation`, descending from
`47862b78d3d643483798d8874b5bc321d782df51`. The ordinary `.phase` is
`project=guideXOSSql`, `last_completed=SQL9`, `next_expected=SQL10`.

The read-only Server checkout is clean on `main` at
`028c493df824b4dc8c5f0df41171fb9b773c6543`; its `.native-db.phase` remains
NDB1R3 → NDB1R4. No dedicated SQL portability marker existed before this
phase; `.native-sql.phase` is independent of both existing markers.

P1 source review confirms an explicit `GxAllocator` callback interface,
checked array/string size arithmetic, failure-preserving reserve/append paths,
length-aware strings, and explicit `GxOwner` cleanup/move behavior. `GxArray`
stores bytes only and does not claim typed C++ object lifetime support. The
hosted runtime test uses a failure-injecting allocator and independent
reference containers. The P1 freestanding script applies the Server AMD64
flags, including `-ffreestanding`, `-nostdinc++`, `-fno-exceptions`, and
`-fno-rtti`.

Independent verification before P2 implementation:

| Check | Result |
|---|---|
| `ctest --test-dir build -R database_portable_runtime_test --output-on-failure` | 1/1 passed |
| `build/database_portable_runtime_test.exe` | 20,668 passed, 0 failed |
| `tools/compile_portable_freestanding.ps1` | Four objects compiled; only portable allocator cross-object references remain unresolved |

The P1 report has a stale intermediate sentence saying 20,663 checks; its
final recorded count is 20,668, which matches the executed binary and its
reported aggregate (92,655 + 20,668 = 113,323). Sanitizer linking was
documented as unavailable with the configured MinGW runtime libraries and is
not claimed here.

## Storage candidate audit

| Candidate | Hosted storage | Bounds and ownership | Failure behavior / portability direction |
|---|---|---|---|
| Page encode/decode (`database_page`) | Page payload and encoded page are `std::vector<uint8_t>`; `DatabasePage` is copied by value | Page size 512–65,536 bytes; payload ≤ page size minus 32-byte header | Encoding has a vector resize with implicit allocation failure. Payload conversion touches many value copies; encoded output is a possible later byte-buffer slice. |
| File page staging (`database_file`) | Page reads/writes and header writes use vector buffers | One page, at most 65,536 bytes | I/O status is explicit, but vector allocation is not. This path has header/flush ordering and would need broader allocator plumbing. |
| Page checksum (`database_checksum`) | No hosted container in the CRC input path; a function-local table is built by a C++ constructor | CRC covers the fixed header/page or bounded WAL record bytes | The algorithm itself is byte-oriented. Its header currently includes `<cstddef>`, which blocks compiling that source with `-nostdinc++`; the converted WAL object calls the established CRC interface. |
| Storage metadata (`database_header`, `database_file`) | Header serialization uses caller-provided fixed bytes; `writeHeaderPage` stages one page in a vector | Header fields occupy 128 bytes; containing page is at most 65,536 bytes | Header field encoding is fixed and status is returned by surrounding I/O. The page-sized staging allocation remains hosted and unchanged. |
| WAL append record (`database_wal`) | `appendRecord` constructs a `std::vector<uint8_t>` containing prefix and payload | Prefix 20 bytes; payload ≤ 65,600 bytes; log ≤ 64 MiB | The record is fully built before `writeAt`; offsets advance only after a successful write. A byte array is a direct replacement with a narrow failure edge. |
| WAL page-image staging (`database_wal`) | Page serialization and page-image payload use separate vectors | One page image ≤ page size + 12-byte page-image header | More than one temporary allocation and page serializer ownership; deferred with the transaction protocol surface. |
| Transaction overlay (`database_transaction`) | `std::map<uint64_t, DatabasePage>` and order vector | ≤ 4,096 pages; each page may be 65,536 bytes | Owns nontrivial page values and savepoint snapshots. P1 byte array cannot store these objects; defer. |
| Buffer cache (`database_buffer`) | `new BufferSlot[_capacity]`; each slot owns a `DatabasePage` payload vector | Capacity defaults to 32 and is caller-configurable | Stable typed slots and dirty eviction semantics require a typed lifetime plan; defer. |
| Catalog and metadata (`database_catalog`) | Record, payload, and page-payload vectors | Schema and page limits; multiple nested buffers | Serialization plus catalog update ordering and continuation allocation; defer. |
| File provider (`database_io`, `database_native_io`) | Provider methods take caller buffers; positional read/write return `bool`, while lifecycle/metadata calls return `DbResult`; hosted provider owns a `FILE*` and path string | Request length is caller-supplied; page I/O is bounded by page size | The provider does not own the selected record scratch buffer. Provider mapping and NDB1R3 status contracts remain unchanged. |

The selected converted path will use no `std::vector` for the WAL record
under construction. The WAL scan result, page-image payload staging, page
serializer, transaction overlay, catalog buffers, and public SQL interfaces
remain hosted and unchanged in this phase.

## Implementation

`database/portable/gx_wal_record.{h,cpp}` now encodes the existing WAL record
prefix and CRC into an initialized P1 `GxArray`. `WriteAheadLog::appendRecord`
uses this byte array instead of its record-sized `std::vector`. It completes
allocation and encoding before calling `writeAt`; `_writeOffset` and
`_bytesSinceCheckpoint` change only after a successful write. The local array
is destroyed on encode failure, write failure, and success.

The default hosted WAL allocator uses the P1 hosted adapter with a hard request
bound of 20 + 65,600 bytes. `WriteAheadLog` also accepts an explicit allocator.
`DatabaseOpenOptions::walRecordAllocator` copies that allocator into the
database for both WAL recovery-open and lazy WAL-open paths; the allocator
context must remain alive while the database is open. Existing callers leave
the option null and keep working through the hosted adapter. The portable
encoder itself has no STL, C++ exceptions, RTTI, global allocation, or hosted
allocator dependency.

The record format is unchanged: zero flags/reserved bytes, little-endian
payload length and transaction id, payload bytes, and the established CRC32
with the CRC field treated as zero. Null payload with a nonzero length retains
the old zero-filled behavior. Oversize payloads fail before allocation. The
encoder returns `GxStatus`; the WAL maps allocation/resource failure to
`DbStatus::NoSpace`, overflow to `OutOfBounds`, and invalid input to
`InvalidArgument`. The exact database page, catalog, heap, B+ tree, and WAL
formats are unchanged.

`tests/database_wal_portable_test.cpp` adds fixed golden vectors and an
independent reference encoder using a separate bitwise CRC calculation. It
checks zero and partial payloads, full 65,600-byte payloads, maximum rejection,
repeated output reuse, allocation failure on first reserve and on growth,
preservation of old bytes/size/capacity, cleanup, and retry. The integration
test opens a real database, starts a production `Transaction`, injects failure
for the first WAL record allocation and then for the page-image record after a
BEGIN was written, and verifies `CommitFailed`, an unchanged database page
count, and correct WAL lengths. Closing/reopening clears the incomplete WAL;
a retry commits successfully, survives close/reopen, and reads back the new
page.

## Verification results

| Check | Result |
|---|---|
| Focused WAL portability CTest | 1/1 passed; 305 checks, 0 failures |
| Complete hosted build | Exit 0 |
| Complete hosted CTest | 12/12 passed; 113,628 checks, 0 failures; 755.95 seconds |
| Preserved SQL CTest targets | 10/10 passed; 92,655 checks, 0 failures |
| P1 portable runtime target | 1/1 passed; 20,668 checks, 0 failures |
| New P2 WAL portability target | 1/1 passed; 305 checks, 0 failures |
| Freestanding P1 + WAL encoder objects | Five objects compile with the Server AMD64 C++14 flags; no STL, exception, RTTI, hosted I/O, or C allocator symbols in the encoder object |

The four injected allocation-failure scenarios were: first portable reserve,
growth of an already-valid output buffer, first WAL record during a real
transaction, and page-image record allocation after BEGIN. All left portable
ownership balanced; failed commits did not increase the database page count;
retry succeeded. The second transaction failure leaves an incomplete BEGIN
record in the WAL by design; the existing close/reopen recovery path safely
clears it before retry.

The full build reported only unused-function warnings in existing test files
`database_relational_test.cpp` and `database_index_test.cpp`. The new production
and test files emitted no warnings. No performance benchmark was run; each
WAL record uses one exact-size bounded allocation, and there is no record copy
or geometric over-allocation because the local array maximum is the exact
record size.

The freestanding compile script now includes `gx_wal_record.cpp`. Its object
references only the portable array API and the production `crc32Init`,
`crc32Update`, and `crc32Final` functions. A direct freestanding compile probe
of `database_checksum.cpp` still stops at `<cstddef>` under `-nostdinc++`;
therefore the checksum translation unit is a remaining native compile blocker
for linking this slice. The `database_wal.cpp` integration unit and broader SQL
engine also remain hosted because they include `std::vector`, `std::string`,
`std::map`, `std::unique_ptr`, and other hosted database dependencies. No stub
functions were used.

Remaining `std::` use in the broader WAL/database call path includes page
serialization and the page-image payload vectors in `appendPageImage`, the
WAL scan byte buffer and `std::vector<WalTransaction>`, the transaction page
map/order vector, and hosted `DbResult` messages. These remain outside the
selected record-construction boundary. Database header/page/catalog metadata
buffers and file-provider interfaces were not changed. The ordinary SQL
`.phase` remains SQL9 → SQL10; the Server checkout and `.native-db.phase` were
not modified.

## Closeout and next boundary

After all acceptance checks passed, `.native-sql.phase` advances to
`project=guideXOSSqlNativePortability`, `last_completed=NDB1R4P2`,
`next_expected=NDB1R4P3`. SQL remains on `main`; P2 is committed locally. No
push was attempted. The Server checkout remains at
`028c493df824b4dc8c5f0df41171fb9b773c6543` on `main`, and its marker remains
NDB1R3 → NDB1R4.

Recommended P3 boundary: convert WAL page-image staging (`appendPageImage`)
and page encode output to the portable byte-buffer path, with the checksum
implementation and status/error surface made freestanding-compatible. Keep
transaction map ownership for a later, separately bounded phase.
