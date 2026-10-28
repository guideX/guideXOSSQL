# NDB1R4P11R3Z — Portable Index Cross-Page Traversal

## Result

**Outcome A — PORTABLE_INDEX_CROSS_PAGE_TRAVERSAL_VERIFIED**

Starting HEAD: `0d2efa3fcf1d7b51e76f482b88c8b0994a3b6520` on `main`.
Qualification ending HEAD (R3Z implementation commit):
`8f4d355fc559ac8557ce600952201d661b758fe7`. At qualification start, local
`main` was two commits ahead of `origin/main` and zero behind it. The
SQL9/SQL10 and NDB1R4P10/NDB1R4P11 phase markers were unchanged. A report-only
follow-up commit records this ending hash; no implementation source changed
after the qualification run.

The production `IndexTree::lookupAll` path now uses the portable leaf-chain
lookup operation after its existing hosted root-to-leaf descent. The existing
page format and writer remain unchanged.

## Existing navigation and storage contract

The leaf payload decoder exposes `nextLeafPageId` as a 64-bit page identifier.
Inspection of the existing writer shows that the link is encoded in the leaf
payload at offset 8. Page 0 is reserved and is also the end-of-chain sentinel;
ordinary page identifiers are in `[1, pageCount)`. A nonzero successor outside
that range is corrupt. The production leaf decoder validates the existing
signature, version, entry count, owner index id, slots, entry regions, physical
keys, and locator bytes before exposing entries.

`IndexTree::lookupAll` first uses hosted `findLeaf` to select a starting leaf.
Previously it then loaded each successor itself. Other range-scan and cursor
paths remain hosted in this phase. `lookupAll` builds its probe from the
logical key plus the minimum 12-byte row locator, so duplicates start at their
first physical position and stay ordered by locator.

`PageAccess::readPage` returns an owned `DatabasePage`; the adapter keeps that
object and its payload vector alive for the duration of one borrow. It checks
the returned page id, leaf page type, and payload length. There is no cross-call
pin guarantee used by this adapter: the next acquire may reuse the same vector
storage. The portable operation releases a page before acquiring its
successor. It copies the previous physical boundary to caller scratch while
the current page is borrowed and returns no borrowed entry views.

The call remains synchronous inside the existing `IndexTree` and
`PageAccess`/transaction-overlay context. It adds no lock, transaction, or page
ownership policy. `lookupAll` still materializes its public `std::vector` result
in the hosted visitor.

## Portable provider and traversal

`database/portable/gx_index_leaf_traversal.h` defines a small callback
provider with opaque context, `acquire`, and `release` functions. A successful
acquire returns a read-only byte span, actual length, and opaque token, and must
be paired with exactly one release. Failed acquires create no view. The
provider must keep bytes stable through release, but may reuse an address on
the next acquire. The visitor may consume or copy an entry only before it
returns. No pointer escapes the traversal call.

`gxIndexLeafLookupChain` uses `gxIndexLeafLookup` for the page-local lower
bound, the R3X cursor to inspect and visit entries, and the R3W physical-key
comparator to validate ordering across nonempty leaf boundaries. It visits all
matching logical keys, including duplicates on successor pages, and stops at
the existing zero-link end marker or at the first greater logical key. It
preserves the 12-byte locator and the existing NULL, Boolean, Int32, Int64,
and Text encodings. It allocates no memory, writes no page bytes, uses no STL,
`DbValue`, exceptions, RTTI, or hosted OS service.

The operation accepts a finite hop bound and caps it at `pageCount - 1`.
Without caller-owned visited-page storage, exhausting this bound reports
`TraversalLimit`; it does not claim to have identified a particular cycle. It
checks start and successor ids, page span validity, leaf format, owner id,
entry structure, physical ordering, counter overflow, and cross-page
ordering. Provider missing-page, invalid-id, and general-error results remain
distinct from end-of-chain. Successful views are released on normal completion,
provider/format errors, visitor stop, and visitor failure.

The hosted adapter also retains the existing `GxPageVisitSet`. This preserves
precise cycle diagnosis and existing allocator-failure behavior: a repeated
page maps to `CorruptPage`, and visit-tracker allocation failure still returns
`NoSpace`. The adapter retains the original `DbResult` from `readPage` and
returns it rather than converting storage failures to successful termination.
The portable page-count bound remains a final termination guarantee.

## Hosted integration and regression evidence

`IndexTree::lookupAll` delegates successor traversal to
`gxIndexLeafLookupChain`; the hosted adapter checks page identity/type and
owner, and maps portable corruption/provider statuses back to existing
`DbResult` categories. Its output preserves ordered duplicates and missing-key
behavior. `lookupFirst` continues to call `lookupAll`. No insert, split, merge,
balancing, page-writing, recovery, or WAL path was changed.

The dedicated R3Z target builds real production leaf pages through `IndexTree`
and compares portable results with both hosted lookup and an ordered physical
scan. It covers empty/single/multiple-entry leaves, chains of two and three or
more pages, first/intermediate/final matches, before/between/after misses,
duplicates across page boundaries, distinct locators, NULL/Boolean/integer
boundaries, embedded-zero Text, repeat reads, and termination. Corruption and
ownership cases cover missing pages, invalid ids and links, self and multi-page
cycles, short/malformed payloads, provider errors, order violations, bound
exhaustion, release on early exit, same-buffer reuse, non-mutation, and
borrowed-view lifetime.

The writer-generated freestanding fixture
`tests/fixtures/NDB1R4P11R3Z_production_leaf_chain.hex` contains 59 serialized
leaf payloads produced by the real hosted writer. Its header records
`pageCount=94`, `ownerIndexId=37`, `firstLeafPageId=1`, and `leafCount=59`.
The freestanding probe loads those bytes through its in-memory provider,
traverses the real production implementation, checks duplicate locators across
pages and terminal link zero, and verifies provider-failure and malformed-link
cleanup.

Focused and full regression results:

| Target | Result |
| --- | ---: |
| R3Z portable cross-page traversal | 4,186 passed, 0 failed |
| `database_index_test` (including R3Q) | 2,940 passed, 0 failed |
| R3Y | 160 passed, 0 failed |
| R3X | 196 passed, 0 failed |
| R3W / R3V / R3U / R3T / R3S / R3R | 39 / 232 / 619 / 52 / 121 / 51 passed |
| R3Q / R3P | 188 / 10,716 passed |
| Portable runtime regressions | 20,912 passed, 0 failed |
| Full Debug CTest | 30/30 targets passed, 0 failed, 0 skipped |

The R3Y report's final full-suite-equivalent count was 162,909 (including its
three post-aggregate checks). Adding the final R3Z target's 4,186 checks gives
**167,095 full-suite-equivalent checks** for this source state. CTest itself
reports target pass/fail and elapsed time, not a grand assertion total. The
final full CTest run took 1,613.75 seconds. The affected index executable was
also rerun directly on this source state and reported 2,940 checks passed.

## Freestanding proof and census

The SQL9 freestanding census now compiles 41 production translation units,
using the same effective C++14 AMD64 flags as the prior 40-unit census:
`-ffreestanding -nostdlib -nostdinc++ -fno-exceptions -fno-rtti` and the
existing architecture flags. It reported **23/41 passing**, preserving all 22
previously passing units. The new traversal translation unit compiles under
those flags. The first remaining index blocker is still
`database/database_index.cpp`, whose inclusion of `database/database_index.h`
fails on hosted `<string>` at line 27.

The freestanding probe compiles the actual traversal source and links it with
the real key, physical comparator, leaf-entry, and probe objects using
`ld -r --no-undefined`. `nm` found zero unresolved symbols, and the probe
executed successfully against the production-writer fixture. The required
portable codec, comparison, cursor, validator, local-lookup, and new
`gxIndexLeafLookupChain` symbols were present. No dummy traversal or hosted
database implementation was used.

## Sanitizers

`SANITIZER_NOT_RUN — TOOLCHAIN_UNAVAILABLE`. The available MinGW toolchain
cannot link `-lasan` or `-lubsan`; neither `clang++` nor MSVC `cl` is available
on this host. No sanitizer coverage is claimed, and no toolchain was installed.

## Remaining boundaries and next milestone

Portable index capabilities now include Boolean, Int32, Int64, and Text key
codecs; NULL handling; logical and physical key comparisons; leaf cursor
traversal and validation; leaf-local lower-bound lookup; and bounded
cross-page leaf-chain lookup. Float64 remains deferred.

Internal-node decoding and root-to-leaf descent still depend on hosted
`IndexTree::findLeaf`, `DatabasePage`, `PageAccess`, `std::vector`, and hosted
error/value types. The remaining index surface also depends on STL containers,
`std::string`, and `DbValue`. A portable descent boundary needs an internal
node decoder and child/lower-bound selector, bounded depth and page validation,
and the same borrowed page-view and transaction-overlay guarantees. The
existing read path's caller-owned transaction and locking scope must continue
to enclose that synchronous traversal.

The next bounded production milestone should extract a **portable
read-only internal-node child/lower-bound reader and bounded root-to-leaf
descent through the existing provider contract**. Keep the hosted adapter
responsible for page ownership and transaction-overlay reads. Do not add
internal descent, Float64, or write-side operations to this R3Z phase.
