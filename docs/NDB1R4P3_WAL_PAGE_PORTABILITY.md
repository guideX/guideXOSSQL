# NDB1R4P3: WAL Page Image and Encoding Portability

## Result

**Outcome A — WAL_PAGE_PORTABILITY_VERIFIED.** The production PAGE_IMAGE path
now serializes the existing database-page representation directly into one
P1-allocated WAL record buffer. CRC, page encoding, and WAL framing bytes match
the previous format. Explicit allocation failures leave the transaction
uncommitted, and a real SQL transaction/recovery test verifies discard, reopen,
retry, and committed index readability.

This accepts only the page-image staging and bounded page codec slice. The
larger SQL9 engine still uses hosted C++ containers and is not ready for native
kernel execution.

## Repository gates and baseline

| Checkout | Starting state | Result |
|---|---|---|
| SQL | `main`, `0186bb2c9b2eb6d0932c496f5cff2d975d59f52e`, P2 commit; clean | Gate passed; work stayed on `main` |
| SQL `.phase` | SQL9 → SQL10 | Unchanged |
| SQL `.native-sql.phase` | NDB1R4P2 → NDB1R4P3 | Initial value verified; advanced to NDB1R4P3 → NDB1R4P4 |
| Server | `main`, `028c493df824b4dc8c5f0df41171fb9b773c6543`, clean | Read only; HEAD, branch, worktree, and NDB1R3 → NDB1R4 marker preserved |

The P1 and P2 reports were reviewed before implementation. P1 retained 20,668
portable-runtime checks. P2 retained 305 WAL record checks and documented that
page-image staging and page serialization were still hosted. No P1/P2 work was
recreated.

## Conversion boundary and ownership

Before P3, `WriteAheadLog::appendPageImage` created three page-sized temporary
buffers:

1. `DatabasePage::serialize` allocated a vector for the serialized page.
2. `appendPageImage` allocated a PAGE_IMAGE payload vector and copied the page
   into it.
3. `appendRecord` allocated a P1 `GxArray` and copied the payload into the
   framed WAL record.

P3 adds a freestanding page codec and PAGE_IMAGE record encoder. The encoder
reserves one bounded `GxArray`, writes the 12-byte PAGE_IMAGE header, serializes
the database page in its final record location, writes the 20-byte WAL prefix,
and computes both checksums there. It copies the caller's page payload once
into the final page image; it adds no intermediate page-sized copy.

The transaction continues to own its `DatabasePage` and hosted payload vector.
The encoder borrows that payload only during the synchronous call. The WAL
record array owns temporary encoded bytes until `writeAt` returns, then is
destroyed. The file provider does not retain the pointer. Recovery scan buffers,
recovered `WalTransaction::pages`, transaction maps, page-cache pages, and
catalog/page ownership remain unchanged.

`DatabasePage::serialize` delegates wire-byte construction to the same page
codec, keeping the page layout and WAL page image on one implementation. Its
caller-owned `std::vector` interface remains hosted and unchanged.

## Encoding and bounds

The format remains WAL v1 and database format v1. Page-image record type,
flags/reserved bytes, little-endian length and transaction id, page type, page
id, serialized page header, payload, and CRCs are unchanged. The page still
zeros unused bytes and covers the full page with its established CRC rule. No
database version or persistent format changed.

The encoder validates supported 512–65,536-byte power-of-two page sizes,
payload capacity (`pageSize - 32`), non-null payload when length is nonzero,
WAL payload and record-buffer bounds, and page id. Page 0 and page ids at or
above `kMaxPageCount` are rejected; `kMaxPageCount - 1` is accepted. Size
addition is checked before allocation. Invalid or oversized input returns a
deterministic status without appending bytes.

The production array maximum is exactly the 20-byte record prefix, 12-byte
PAGE_IMAGE header, and open database page size. This is within the existing
65,600-byte WAL payload bound. The output is written only after reserve succeeds.
`appendEncodedRecord` checks the 64 MiB WAL limit and advances `_writeOffset`
and `_bytesSinceCheckpoint` only after a complete positional write succeeds.
BEGIN, DB_HEADER, and COMMIT framing remain unchanged.

## CRC compatibility

`database_checksum.h` now uses `<stddef.h>` and `<stdint.h>` rather than
`<cstddef>` / `<cstdint>`. The page codec also needs established page-size and
byte-order helpers, so `database_endian.h` and `database_format.h` use the C
headers. No namespace shim or compatibility container was added.

The CRC polynomial (`0xEDB88320`), initialization, reflected bit processing,
byte order, final XOR, table contents/generation, page and WAL CRC placement,
and zero-length behavior are unchanged. Fixed expected values in the P3 test
were independently recorded with Python's `zlib.crc32`; tests do not compute
expected CRCs through the production function.

| Vector | Expected CRC32 |
|---|---:|
| Empty input | `00000000` |
| One byte `A5` | `74BEB8EA` |
| 32 repetitions of `CC` | `CBD832D5` |
| ASCII `123456789` | `CBF43926` |
| Existing 20-byte BEGIN WAL record | `5CDF0D2E` |
| Full 512-byte serialized empty Data page | `8956487A` |
| Same page after one byte is corrupted | `49396B65` |
| 65,536-byte bounded input | `23E19BE8` |

The actual CRC translation unit compiles under the Server AMD64 flags. Its
function-local generated table references `__cxa_guard_acquire/release`, which
are implemented by the Server's `kernel/core/cxx_runtime.cpp`. The object has
no STL, exception, RTTI, hosted I/O, or C allocator references.

## Golden, failure, and recovery evidence

The P3 test uses an independent bitwise CRC/reference encoder to compare exact
PAGE_IMAGE bytes for an empty zero-filled page, a distinct nonzero page,
multiple ordered records, and the maximum valid page id. It checks page id 0,
the first unsupported id, unsupported page size, null/oversized page payload,
and an insufficient WAL payload limit. A captured production WAL scans all
three records; a flipped page byte is rejected as corrupt, and a truncated
COMMIT leaves an incomplete transaction with no committed output.

Allocation injection covers first reserve failure, reserve growth failure
while preserving previous encoded bytes, and a later PAGE_IMAGE allocation
failure after BEGIN plus one complete page image. The production transaction
test creates a real database, table, and index, then inserts through a real
`Transaction`. The injected second page-image allocation fails commit. Page
count remains unchanged, WAL scan reports `Incomplete` with no committed
transaction, and the allocator reports no outstanding blocks, invalid frees,
or allocation/free imbalance. Close/reopen discards the incomplete transaction;
a later insert commits, survives another reopen, and passes index validation.

The existing SQL3 transaction suite passed its crash matrix, incomplete WAL
discard, and **partial trailing-page recovery matrix**. No recovery code was
simplified or replaced. SQL3 reported 2,553 checks passed.

## Freestanding objects and remaining dependencies

Command:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/compile_portable_freestanding.ps1
```

The script uses the Server Makefile's AMD64 C++14 flags and configured
MinGW-W64 `g++`:

```text
-std=c++14 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti
-nostdlib -nostdinc++ -fno-builtin -DGXOS_BARE_METAL -m64 -mcmodel=large
-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-pic -fno-pie -fno-PIC
-fno-PIE -mno-stack-arg-probe -fno-stack-check -fno-stack-protector
-D__x86_64__
```

It compiled these production objects:

- P1: `gx_allocator`, `gx_array`, `gx_owner`, and `gx_string`.
- P2: `gx_wal_record`.
- P3: `database_checksum`, `gx_page_codec`, and `gx_wal_page_image`.

Only portable API/CRC cross-object references and the Server CRC-table guard
functions remain unresolved. No test stubs replace the production encoder or
checksum.

The broader engine remains hosted. `database_wal.cpp` still uses `std::vector`
for WAL scans and recovered transactions; `DatabasePage` payloads and
`Transaction` page overlays still use hosted vectors/maps. `DbResult` messages,
provider interfaces, paths, file I/O, catalogs, indexes, heap structures,
parser/executor state, and prepared-plan ownership remain outside P3. SQL9 as a
whole does not compile or run freestanding yet.

## Regression results

| Suite | Result |
|---|---:|
| Existing SQL CTest targets | 10/10; 92,655 checks, 0 failures |
| P1 portable runtime | 1/1; 20,668 checks, 0 failures |
| P2 WAL record regression | 1/1; 305 checks, 0 failures |
| P3 PAGE_IMAGE / CRC / recovery test | 1/1; 106 checks, 0 failures |
| Complete hosted CTest | **13/13; 113,734 checks, 0 failures** |
| Full CMake build | Exit 0 |
| Freestanding selected objects | All eight objects compiled |

The full CTest run took 2,688.24 seconds. The build reported only existing
unused-function warnings in `database_relational_test.cpp` and
`database_index_test.cpp`; P3 production files emitted no warnings.
After the final page-specific array maximum was tightened to the exact record
size, the full CMake build and all 106 P3 checks were rerun successfully; the
other 12 targets were not rerun after this allocation-bound-only refinement.

## Files and closeout

Production changes are limited to the page codec, PAGE_IMAGE record encoder,
WAL append integration, C-compatible checksum/header boundary, and compile
script. The new target is `database_wal_page_portable_test`. P1/P2 reports
remain unchanged. No SQL behavior, Server file, Server marker, database format,
WAL version, or ordinary SQL phase marker changed. No GitHub push was attempted.

## Recommended NDB1R4P4 scope

Convert the bounded WAL recovery scan byte buffer and record parsing scratch
path, with explicit allocation status and unchanged committed/incomplete/
corrupt classification. Keep `WalTransaction` page ownership and the general
transaction page map hosted until a typed-object lifetime strategy is tested;
the P1 byte array must not store those owning objects.
