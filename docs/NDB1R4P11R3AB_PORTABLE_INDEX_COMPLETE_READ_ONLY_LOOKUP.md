# NDB1R4P11R3AB — Portable Index Complete Read-Only Lookup

## Result

**Outcome A — `PORTABLE_INDEX_COMPLETE_READ_ONLY_LOOKUP_VERIFIED`**

The complete production lookup is `gxIndexLookupAll` in
`database/portable/gx_index_lookup.h` and `.cpp`. `IndexTree::lookupAll` now
uses it with one page provider, then publishes its owned result vector only
after the portable operation completes successfully.

Starting branch and HEAD: `main`,
`acb741ee511fbd3c15abea30d21dc7ba8234afd7`. The starting worktree was clean.
The local `origin/main` tracking ref pointed at the same commit, so the local
ahead/behind count was `0/0`, rather than the six commits ahead described in
the handoff. A read-only `git ls-remote origin refs/heads/main` check could not
authenticate (`Permission denied (publickey)`); no remote ref was changed or
verified. Both phase markers were unchanged: `.phase` SHA-256
`F84EF02A6B82D4C53CDB2BDD0B80C083A49C6FA2E5DF46914D9AADD7555295F0` and
`.native-sql.phase` SHA-256
`FE722CF1A92A04C9F16E91E556933C682A7E3475BF3AABB06CE994410A28A52`.

The production implementation and qualification changes are committed as
`c0fbcbc31411d3f1cf07babc8ee9bb87b544e9f6`. This is the ending production
revision qualified below. The report is a documentation-only follow-up.

## Existing lookup orchestration audit

Before R3AB, `IndexTree::lookupAll` in `database/database_index.cpp` performed
the hosted coordination itself:

1. Its callers supplied already encoded logical-key bytes. Typed `DbValue`
   conversion is performed by hosted `encodeIndexKey`; relational lookup gets
   the key bytes and root page id from the existing query/catalog path.
2. It copied the logical key and appended a big-endian `RowLocator(0, 0)`.
   This is the minimum physical locator, so descent begins before every stored
   row locator for that logical key.
3. It read `PageAccess::pageCount()` and selected the root page id supplied by
   the catalog record/caller. `Database::indexLookup` constructs `IndexTree`
   with `pages()`, the active transaction, and the database scratch allocator.
4. It built a descent provider over `PageAccess::readPage`. The provider kept
   one `DatabasePage` alive as a borrow, checked page identity/type and payload
   size, and released the view before the next read.
5. It passed the minimum physical probe to R3AA
   `gxIndexTreeDescendToLeaf`, then passed the resulting leaf id and the same
   probe to R3Z `gxIndexLeafLookupChain` through a second provider context.
6. The R3Z visitor copied each matching key and row locator into the hosted
   output vector while the page borrow was active. Logical equality compared
   key bytes; physical order included the 12-byte locator suffix.
7. Leaf traversal continued across sibling pages while keys equaled the
   logical key and stopped with `KeyBoundary` at the first greater key, or at
   the end of the chain. The hosted visit set diagnosed repeated leaf pages;
   R3Z also had a finite page-count bound.
8. Hosted code translated provider, page-format, ownership, cycle, and bound
   errors to the existing `DbResult` categories. `PageAccess` owned the page
   storage; the provider context owned only its temporary page copy and borrow
   state. The public method cleared `out` at entry and appended visitor results
   directly to it.

The main remaining orchestration boundary was therefore the composition of
already production-qualified descent and leaf-chain operations. The prior
implementation did not have a single portable entry point accepting an
encoded logical key and delivering locator values.

## Complete portable lookup API

`gxIndexLookupAll` accepts:

- The root page id, page count, and expected owner index id.
- Encoded logical-key bytes and explicit length.
- The established `GxIndexPageProvider` acquire/release contract.
- A finite maximum descent depth and caller-owned path-id scratch for exact
  descent cycle detection.
- A finite maximum leaf-page visit count.
- Caller-owned scratch for the physical lower bound and previous leaf key.
- A result visitor and opaque context, plus an explicit status and result.

The operation checks arguments and checked key-length addition, copies the
logical key into the caller's lower-bound scratch, and appends twelve zero
bytes. It calls the production R3AA descent, verifies all borrows are closed,
then calls the production R3Z chain lookup through the same provider contract.
It does not parse internal or leaf records itself, duplicate separator routing,
or introduce a second comparison algorithm.

The provider lends a read-only view valid until its matching `release`. A
provider may reuse the same backing address after release. The operation
releases every successful acquisition on success and failure, checks borrowed
views against caller scratch/output, checks leaf-chain page identity/type, and
retains no page pointer after release. A configured leaf-page limit that still
has a successor returns `TraversalLimit`; it cannot become a successful
not-found result. The hosted provider retains its existing `GxPageVisitSet`
cycle diagnosis for the leaf chain. Portable callers that do not provide an
additional provider-side cycle set still have the finite R3Z traversal bound.

The visitor receives `locatorPageId` and `locatorSlot` by value, so it cannot
retain a page-owned entry view through the public callback. Returning
`Continue` visits the next match. `Stop` returns `VisitorStopped` and makes no
further callback. `Error` returns `VisitorFailure` and makes no further
callback. The callback must not re-enter the lookup or manipulate the active
provider borrow.

`Complete` means the matching range ended normally; it includes zero matches.
`VisitorStopped` is an explicit early termination, not a complete result set.
Provider, invalid-id, missing-page, corruption, cycle, traversal-limit, and
visitor failures have distinct statuses. If a failure follows earlier visitor
calls, that failure status remains authoritative and `matchesVisited` reports
the callback count, including a callback that returned `Error`. Callers must
discard partial results on any non-completion status. Invalid arguments leave
the output result unchanged; operational outcomes publish counters and
diagnostics.

## Duplicate and ordering correctness

The complete API builds the existing `logicalKeyBytes || 12 zero bytes` lower
bound. Internal routing compares the full physical probe against stored
separator bounds using the existing comparator and separator equality routes
right as before. The leaf operation compares only the logical-key prefix when
deciding whether an entry matches, while validated physical ordering still
includes the complete 12-byte `(page id, slot)` suffix. Thus distinct row
locators remain physically distinct, locator order is preserved, and duplicate
logical keys split across multiple linked leaves are all visited.

The operation stops at the first greater logical key or at chain end. It does
not treat a traversal limit, invalid successor, provider error, or corrupt
page as absence of a key.

## Hosted production integration

`IndexTree::lookupAll` now passes one provider to `gxIndexLookupAll`. Its
provider context still reads through the current `PageAccess` view and keeps
the same transaction context. It resets its hosted leaf-cycle visit set at the
descent/chain boundary, then tracks chain pages using the existing early-cycle
diagnostic. The context releases its `DatabasePage` borrow on every provider
release.

The hosted visitor copies the requested logical key and locator values into a
temporary `std::vector<IndexEntry>`. On complete lookup, `lookupAll` swaps that
staged vector into `out`. On any lookup/provider error it returns the existing
translated `DbResult` and leaves the public output empty, even if the visitor
already received rows before a later error. It does not change transaction,
recovery, locking, persistence, or write behavior.

`Database::indexLookup`, foreign-key checks, and other existing callers that
use `IndexTree::lookupAll` now reach the complete portable operation. Root
selection, transaction-aware page access, typed conversion, storage error
translation, and public collection ownership remain hosted.

## Focused and full qualification

The dedicated `database_ndb1r4p11r3ab_test` target exercises the production
complete lookup against both the checked-in R3AA production-writer fixture and
new production-writer trees. It reports **1,354 passed, 0 failed** direct
checks. Coverage includes:

- Empty leaf root, a single-level internal root, and a multi-level tree.
- First, middle, and last keys; misses before, between, and after stored keys;
  repeated lookup; no-match and multiple-match visitors.
- NULL, Boolean false/true, Int32 and Int64 boundaries, empty Text, embedded
  zero Text, and leaf-local duplicates.
- One hundred Text duplicates across multiple leaves, exact locator sequences,
  and hosted-vs-portable result equivalence.
- Visitor continuation, first/several-result stop, visitor failure, provider
  failure after earlier callbacks, and hosted output staging after a later
  missing page.
- Invalid root/child, internal cycle, descent depth bound, missing and invalid
  successor, leaf cycle, leaf-page budget, truncated internal/leaf pages,
  invalid physical order, short provider view, identity/type mismatch, reused
  provider storage, and a provider view aliasing caller scratch.
- Release counts on success and all tested failures, copied locator lifetime,
  bounded termination, and unchanged source pages during lookup.

The existing R3AA fixture header is `512 767 37 615 100 107`: 512-byte pages,
page count 767, owner 37, root 615, a 100-row duplicate run, and 107 serialized
path/duplicate-chain pages. The R3AB test also builds a 420-entry tree with 160
keys below the duplicate run and 160 above it; production validation confirms
at least three levels, and the duplicate run crosses leaves. A 17-key Int32
fixture verifies a single internal root.

The final Debug CTest aggregate passed **32/32 targets** in 4,209.28 seconds,
including the storage, relational, transaction, SQL, index, portable WAL,
native I/O, and cache-publication suites. The previous R3AA report records
31/31 targets and a calculated 171,113 full-suite-equivalent checks. Adding
the R3AB target's 1,354 direct checks gives a calculated **172,467
full-suite-equivalent checks**. CTest reports target results and does not
report a grand assertion count; the R3AB 1,354 is the direct test output,
while 172,467 is calculated.

## Freestanding proof and census

The unchanged AMD64 freestanding flags compile the new
`database/portable/gx_index_lookup.cpp` without STL, `DbValue`, exceptions,
RTTI, or hosted OS services. The qualification script compiles the complete
probe with those flags, links the actual production lookup, descent, internal
reader, physical comparator, leaf reader, and leaf-chain objects using
`ld -r --no-undefined`, and verifies the required production symbols. `nm -u`
reports no unresolved symbols in the linked probe. The hosted runner loads the
production-writer R3AA fixture and executes the freestanding-compiled code; it
checks all 100 duplicate locators in order across leaves, early stop, provider
failure after callbacks, invalid root, corrupt page type, and release counts.
The R3AA proof and all earlier portable-symbol gates remain in the script.

The unchanged compiler/linker census now has **26/44 passing translation
units**, up from the R3AA baseline of 25/43; the new lookup unit passes and all
25 prior passes remain passing. No new compiler blocker was introduced. The
first blocker for `database/database_index.cpp` remains
`database/database_index.h:27:10: fatal error: string: No such file or directory`
under `-nostdinc++`. This boundary proves complete portable index lookup; it
does not make the hosted SQL engine or `database_index.cpp` freestanding.

`SANITIZER_NOT_RUN — TOOLCHAIN_UNAVAILABLE`. The active MinGW compiler reports
no ASan or UBSan libraries, and neither `clang++` nor MSVC `cl` is installed.
No toolchain was installed.

## Native integration readiness

1. **Complete portable point lookup:** Yes. One bounded production operation
   now combines root descent with all matching entries across linked leaves.
2. **Production callers:** Hosted `IndexTree::lookupAll`, reached by
   `Database::indexLookup` and existing index/foreign-key lookup callers.
3. **Key types:** The portable typed codecs cover NULL, Boolean, Int32, Int64,
   and Text. The complete lookup itself compares encoded bytes and does not
   validate a type-specific encoding. Existing hosted Float64 conversion and
   byte ordering continue to work through it; no portable Float64 codec or
   Float64 migration was added. Blob remains non-indexable.
4. **Hosted conversions:** `DbValue` to encoded bytes, including Float64
   normalization/non-finite rejection, remains in `encodeIndexKey`.
5. **Server adapters:** A Server caller must provide the catalog's root,
   owner, and page-count metadata; encoded key bytes; an acquire/release
   provider over real index pages; bounded descent/leaf scratch; and an owned
   visitor result sink. It must map storage errors and page types into the
   existing provider contract.
6. **Consistency:** Root/catalog metadata and every page must come from the
   same transaction snapshot. Each page's bytes and identity/type must remain
   valid through `release`; mutation or a snapshot change during traversal
   would invalidate the result.
7. **Range scans:** A portable bounded read-only range scan is the next useful
   read-side boundary. It can reuse the provider, byte ordering, and visitor
   conventions while keeping writes and persistent formats out of scope.
8. **Float64 prerequisite:** A portable Float64 codec is not required for an
   adapter that supplies the existing hosted encoded bytes. It is required
   before claiming portable typed conversion/validation for Float64.
9. **Hosted writes:** Catalog mutation; index create/insert/remove; leaf and
   internal splits, balancing decisions, and write publication; transaction
   and WAL handling; and range scans remain hosted.
10. **Guest evidence:** Before guest integration, add the real Server page
    provider against its transaction snapshot, verify catalog root/owner/page
    count consistency, and run end-to-end guest cases for empty/missing keys,
    duplicate locators across leaves, provider failures, and page corruption.

Recommended next phase: a narrowly scoped portable bounded read-only range
scan integrated into hosted `IndexTree::rangeScan`, using encoded inclusive or
exclusive bounds and a caller visitor. Keep Float64 conversion hosted and keep
index writes, file formats, and transaction/recovery behavior unchanged.
