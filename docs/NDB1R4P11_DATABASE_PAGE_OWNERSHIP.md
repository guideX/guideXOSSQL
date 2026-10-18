# NDB1R4P11: DatabasePage Payload Ownership Audit

## Outcome

**Outcome B — `DATABASE_PAGE_OWNERSHIP_PARTIAL`.** The required baseline,
compiler census, page ownership, copy/move, and borrowed-buffer audits are
complete. The writable checkout remains at the accepted P10 revision. No
source conversion was made because the current page API has no allocator
provenance, and adding safe page allocation requires threading an allocator
through file, cache, page-access, transaction, and heap/index construction
paths. This report records that bounded continuation rather than introducing
a partially failure-aware owner.

No P11 payload owner or ownership tests have been added. No page bytes,
transaction behavior, or database files were changed.

## 1. Starting state and phase markers

The SQL checkout is `D:\dev\guideXOS_SqlDatabase`, branch `main`, at
`37a948000d4503adf528a517843b125e975fd14d`, subject
`NDB1R4P10: remove shared freestanding header blocker`. The worktree was clean
at preflight. `.phase` remains:

```text
project=guideXOSSql
last_completed=SQL9
next_expected=SQL10
```

`.native-sql.phase` remains:

```text
project=guideXOSSqlNativePortability
last_completed=NDB1R4P10
next_expected=NDB1R4P11
```

No P11 report or implementation existed at preflight. No P11 work was already
underway or completed.

## 2. P10 acceptance evidence

`docs/NDB1R4P10_SHARED_DEPENDENCY_CONVERSION.md` records the accepted P10
conversion of 20 production headers from `<cstddef>` / `<cstdint>` to
`<stddef.h>` / `<stdint.h>`. Fourteen previously blocked translation units
advanced to their next compiler error; the 14 passing units remained passing,
and no translation unit newly completed qualification. P10 reports unchanged
runtime behavior and object symbol sets, and all hosted regressions passing:
18/18 CTest targets, 145,007 checks, zero failures.

The P9 census was read. It identifies `DatabasePage::payload` as an owning
`std::vector<uint8_t>` with implicit deep-copy allocation, and explicitly
warns that moving it to the portable allocator requires failure-aware
copy/move semantics and caller migration.

## 3. Reproduced freestanding census

Ran `tools/qualify_sql9_freestanding.ps1` with Windows PowerShell 5.1
(`5.1.26100.9711`), the Server AMD64 kernel include roots, MinGW-W64 `g++`
15.2.0, and GNU `nm` 2.46.0.20260210. The script was not modified. Its
compiler and symbol logs are in
`%TEMP%\guideXOSSql-NDB1R4P11-before-20261018`.

The result reproduces the accepted 32-unit census: 14 pass and 18 fail. All
14 portable leaves still pass. `database_page.cpp` remains blocked first by
the hosted `<vector>` include in `database/database_page.h`; this run therefore
does not test a converted page owner. The full 32-unit matrix is unchanged
from P10. The selected page-owner object was not converted or independently
compiled.

The 18 first diagnostics are unchanged from P10: `database_io.cpp` at
`<cstdio>`; `database_native_io.cpp`, `database_file.cpp`, `database_engine.cpp`,
`database_buffer.cpp`, `database_heap.cpp`, `database_wal.cpp`,
`database_transaction.cpp`, and `database_relational.cpp` at `<memory>`;
`database_header.cpp`, `database_catalog.cpp`, `database_sql_token.cpp`, and
`database_sql_tokenizer.cpp` at `<string>`; `database_page.cpp` and
`database_sql_parser.cpp` at `<vector>`; `database_index.cpp` at `<set>`;
`database_sql_expr.cpp` at `<vector>`; and `database_sql.cpp` at `<map>`.

The 14 passing objects retain the P10 symbol profile: portable allocator,
array, cache, page codec, WAL, recovered-page, and transaction-owner references
only, plus the known checksum `__cxa_guard_acquire` / `__cxa_guard_release`
references. The hosted allocator adapter refers to `malloc/free` and remains
outside the native kernel path. Because no production object changed, this run
introduced no new `std::`, exception/unwind, RTTI, global new/delete, hosted
I/O, or static-initialization symbols. There is no P11 page-owner object to
inspect; object compilation still does not establish a native link or runtime.

The script's effective AMD64 options include C++14,
`-ffreestanding -nostdlib -nostdinc++ -fno-exceptions -fno-rtti`,
`-fno-builtin`, `-DGXOS_BARE_METAL`, the AMD64 large-code-model/no-red-zone/no
SIMD/no PIC flags, and the Server kernel include roots. P10's existing
Windows PowerShell stderr-capture fix remained intact.

## 4. Original `DatabasePage` representation

The authoritative declaration is in `database/database_page.h`; its
implementation is in `database/database_page.cpp`.

| Field | Actual representation |
|---|---|
| Page identifier | `uint64_t pageId` |
| Type | `PageType`, whose serialized field is a 16-bit value |
| Flags | `uint16_t flags` |
| Logical payload length | `uint32_t payloadSize` |
| Generation | `uint32_t generation` |
| Payload | Owning `std::vector<uint8_t> payload` |

The default constructor initializes metadata to zero/`Unknown`; the vector
starts empty. There is no custom page destructor, copy constructor, or copy
assignment. The vector supplies destruction and allocation-dependent deep
copy; the compiler supplies move operations. `database_buffer.cpp` has a
static assertion that page move assignment is nonthrowing. There is no
explicit page-owner initialization or allocation status. `payloadSize` is
separate from `payload.size()`, so callers must keep the two synchronized.

Supported page sizes are powers of two from 512 through 65,536 bytes. The
serialized header is 32 bytes, so payload capacity is `pageSize - 32`: 480
bytes for a 512-byte page, 4,064 for a 4,096-byte page, and 65,504 for a
64-KiB page. `DatabasePage` does not retain the configured page size and its
vector is not capped when independently resized. Serialization checks the
current vector length against the requested page size.

## 5. Production page-copy and move inventory

The explicit production copy assignments found are:

| Site | Operation and context | Current allocation/failure behavior |
|---|---|---|
| `database/database_file.cpp` | `DatabaseFile::readPage`: assigns a parsed local page into caller output | Vector deep copy may allocate; this assignment has no local `bad_alloc` conversion. |
| `database/database_relational.cpp` | `Database::readPage`: copies a pinned cache page to caller output | Catches `std::bad_alloc`, unpins, returns `NoSpace`; otherwise unpins after copy. |
| `database/database_buffer.cpp` | `BufferManager::installPage`: copies the incoming page only when an existing cache entry is pinned | Catches `std::bad_alloc` before cache publication; then moves the prepared copy into the stable slot. |

`DatabasePage::parse` builds a temporary and move-assigns it into output after
validating the serialized image and copying its payload. This gives the
current vector implementation a prepare-then-publish shape, but allocation
failure is represented by exceptions. `BufferManager` also resets a view by
move-assigning an empty page and moves parsed pages into stable slots. Slots
are placement-constructed in a fixed array and explicitly destroyed; they are
not raw-relocated.

P7 and P8 tests contain five explicit page-to-page copies: three in
`database_ndb1r4p7_test.cpp` and two in `database_ndb1r4p8_test.cpp`. Test
helpers returning newly built pages use return-by-value and rely on move/NRVO.
No production `std::vector<DatabasePage>` or map of page objects was found.
P5 recovery, P6 cache, and P7 transaction storage use their portable
serialized-image owners rather than collections of `DatabasePage` values.

The current implicit copies are unsafe to retain unchanged in a
freestanding/no-exception contract: ordinary C++ copy construction and
assignment cannot return `GxStatus`. A bounded conversion must delete them,
provide an explicit strong-failure deep-copy operation, migrate the three
production sites, and update the five test copies. Move needs to remain
nonallocating, leave the source safely destructible/reusable, and release the
destination's prior allocation exactly once.

## 6. Borrowed payload lifetime audit

Production accesses to `payload.data()`, payload iterators, and indexed bytes
are concentrated in eight translation units: catalog, heap, index, file,
buffer, page, transaction, and WAL code. The observed borrows are used
synchronously to inspect or encode a page, populate page bytes, stage a page
image into the transaction owner, or append a WAL image. The transaction and
WAL staging paths copy the bytes into their own portable image buffers before
returning. The heap iterator use creates a separate row byte vector. No
production pointer or iterator into a page payload was found stored beyond the
operation that acquired it.

Cache slot page views live while materialized and pinned. A view is cleared
after its last unpin or before slot replacement/eviction; callers must not
retain a payload pointer beyond that page-view lifetime. Direct references
into a `DatabasePage` can be invalidated by vector growth, assignment, move,
replacement, or destruction. Because `payload` is a public member, the type
cannot enforce this rule for external callers.

The audit found no span API or persistent page-codec reference. The codec
receives caller-owned byte pointers and does not retain them. This review
establishes current call-site use, not a general lifetime guarantee for the
public vector interface.

## 7. Portable allocator and scope boundary

P1's `GxAllocator` and `GxArray` are the established candidates. `GxArray`
provides explicit size/capacity, bounded reserve, checked byte multiplication,
and status returns. Its reserve operation preserves the old buffer on
allocation failure. It stores its allocator by value and owns its `data`, but
the public struct itself is byte-copyable; copying it as an owner would be a
shallow copy. `DatabasePage` therefore needs a noncopyable RAII payload wrapper
with explicit move and deep-copy operations, rather than embedding or
relocating an owning `GxArray` as a raw struct.

The missing propagation point is concrete: `DatabasePage::parse` accepts no
allocator; `PageAccess` exposes no allocator; `DatabaseFile` has no page
payload allocator contract; `BufferManager` accepts an allocator for its
portable cache, while `Transaction` accepts one for its portable transaction
image overlay. Heap and index page builders allocate payloads directly. A
failure-aware owner needs one stable allocator source for construction,
materialization, copying, reuse, and destruction. It must not accidentally
retain a transaction-scoped allocator context in a page that outlives that
transaction.

The minimum safe continuation is to settle and document allocator lifetime
and provenance first; add a bounded move-only page payload with page-size
capacity and explicit status-returning assignment/resize; add strong
`copyPage` and move/clear semantics; then thread the existing allocator
contract through file/page access, cache materialization, transaction reads,
and heap/index builders. Convert all page copy sites and check every
allocation status before publishing metadata or page images. This crosses
multiple SQL storage and relational construction paths, so this phase stops
before replacing the vector with a partially initialized or silently failing
wrapper.

## 8. Initialization, failure, format, and test status

Current default pages are empty and safe to destroy. Nonempty payloads are
usually created by four production `assign(capacity, 0)` sites: heap page
packing, heap page split/replacement, index leaf building, and index internal
page building. Deserialization also assigns a bounded payload after the page
codec verifies page size, page id, payload length, and CRC. `std::vector`
allocation failures can throw; there is no payload-specific injected
`GxAllocator` failure status or retry matrix. The individual page serializer's
output vector resize can also allocate.

| Allocation point | Current handling | P11 failure qualification |
|---|---|---|
| Four heap/index payload assignments | `std::vector::assign`; no returned `DbResult` from the allocation itself | Not injected; builder propagation not converted |
| Page parse payload assignment | Exception-based vector allocation before moving the temporary into output | Not injected; catches differ by caller |
| File and relational page copies | Implicit vector copy; file path has no local status conversion, relational path catches `bad_alloc` and unpins | Not injected; explicit `copyPage` absent |
| Pinned cache view replacement copy | Catches `bad_alloc` before image publication | Not injected; explicit status copy absent |
| Page serialization output growth | `std::vector::resize` before portable codec invocation | Not injected; serialized-output allocation remains hosted |
| Portable transaction/WAL staging | Existing Gx owners return explicit `GxStatus` and own copied serialized bytes | Existing P5–P8 tests remain covered; this is not the `DatabasePage` payload owner |

No page-payload leak counter, copy independence check, repeated failure/retry
cycle, or transaction allocation-failure test was added in P11. The proposed
owner's allocation-failure behavior therefore remains unqualified.

No P11 allocation count, peak-page-memory, or creation/copy/move/codec timing
was measured. The current hosted vector does not expose the portable allocator
accounting used by the P5–P8 image owners. The existing P8 portable index and
overlay measurements remain in their accepted report and are not measurements
of `DatabasePage::payload`.

The authoritative codec remains `gxDatabasePageEncode` /
`gxDatabasePageInspect`. It writes the existing little-endian header, zeroes
the whole page before encoding (including padding), and computes the page CRC
with the CRC field treated as zero across the full page. Inspection checks
supported page size, expected page id, payload bounds, and CRC before
publishing metadata. No codec, encoding, format, or fixture was changed in
P11. No P11 golden-byte, round-trip, copy-independence, move-lifetime, or
allocation-failure test was run because no owner conversion was made.

Existing fixed-byte coverage remains in the portable WAL/page test suite and
P3's accepted page encoder report. The P11 run did not requalify those fixtures
against a different in-memory owner; page-size boundaries, zero padding, CRC,
and serialization equivalence under a new owner remain open acceptance gates.

The accepted P10 real SQL9 and WAL/savepoint/recovery test coverage remains
applicable to the unchanged source. P10 recorded 18/18 CTest targets,
145,007 checks, zero failures. The P11 baseline rerun used the same isolated
P10 build because the source is unchanged; all 18 targets passed again with
145,007 checks and zero failures in 3,596.02 seconds. This confirms the
unchanged hosted baseline but does not qualify new owner behavior. No P11
changed-owner SQL workload or transactional allocation-fault test exists.

## 9. Server reference and ABI notes

The read-only Server checkout remains branch `NATIVE_DB_STORAGE` at
`f29850c93bbac0a0763a31e806d2816310156575`, clean and unmodified. Its AMD64
Makefile confirms C++14, freestanding/no standard C++ headers or libraries,
no exceptions/RTTI, no SIMD, no red zone, and the MinGW-specific large code
model flags used by the SQL qualification script.

The Server's current native database file bridge is a C ABI over VFS file
operations. It does not supply the SQL `GxAllocator` contract. The kernel
runtime defines global `operator new` in terms of its heap allocator, but that
interface may return null and is not an acceptable replacement for this
page owner's explicit status-returning allocator. The existing C++ ABI guard
helpers support the previously accepted checksum object; they do not supply a
hosted STL or a page allocator. No Server file, branch, build, or marker was
changed.

## 10. Qualification and closeout

| Check | P11 audit result |
|---|---|
| Required SQL branch, HEAD, subject, clean preflight | Pass: exact P10 baseline on `main` |
| Ordinary SQL marker | Preserved at SQL9 → SQL10 |
| Native marker | Preserved at NDB1R4P10 → NDB1R4P11 |
| P10 acceptance report and P9 census | Read and verified against requested claims |
| Windows PowerShell 5.1 AMD64 qualification | Reproduced: 14/32 pass, 18/32 blocked |
| First page translation-unit diagnostic | Still hosted `<vector>` in `database_page.h` |
| Page ownership / copy / move / borrow audit | Complete; findings above |
| Full hosted CTest rerun | Pass: 18/18 targets, 145,007 checks, zero failures; 3,596.02 seconds |
| P11 owner and focused tests | Not implemented |
| Golden bytes / SQL transaction / WAL under new owner | Not run; no changed owner |
| Full P11 acceptance | Not met; P11 remains open |
| Server checkout | Same branch/HEAD, clean, unmodified |
| SQL commit / push | No commit and no push under Outcome B |

P11 recommendation: continue with the allocator lifetime/provenance design
and bounded page-owner API as the smallest coherent storage migration. Do not
advance the native portability marker until explicit copy failure, allocator
failure preservation, page-byte equivalence, real SQL transaction/recovery,
full regressions, and the selected freestanding compilation path are all
qualified. P12 should not start while P11 remains open. After P11 acceptance,
the next bounded conversion candidate is the `DbResult` message-string owner
that will remain in `database_page.cpp`'s header closure after `<vector>` is
removed; keep parser/AST and general catalog containers outside that phase.
The ordinary SQL marker remains unchanged. Server NDB1R4 remains open.
