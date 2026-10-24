# NDB1R4P11R3O — Portable Split Planning

## Result

**PORTABLE_SPLIT_PLANNING_VERIFIED**

SQL9 now plans the shared internal-parent split through transaction-owned,
bounded GxArray storage. The R3N fitting-grandparent route consumes that plan
directly to choose the split point and prepare the parent, right-parent, and
grandparent images. A planning allocation failure is returned before the
structural split stages any page image. The accepted R3L, R3M, and R3N
behaviors remain covered.

This qualifies the selected parent-split planning boundary. It does not qualify
arbitrary-depth recursive splits or remove the SQL index translation unit's
other hosted dependencies.

## Starting gates and accepted R3N

The writable checkout was D:\dev\guideXOS_SqlDatabase, branch main, at
starting HEAD
3afad8b447d6e02bb9d44c7de7021ad3cc8b2dde. It was clean before editing.
The ordinary phase marker remained:

    project=guideXOSSql
    last_completed=SQL9
    next_expected=SQL10

The native portability marker remained:

    project=guideXOSSqlNativePortability
    last_completed=NDB1R4P10
    next_expected=NDB1R4P11

The accepted R3N report and implementation were reviewed before changes.
R3N's baseline was 8,423 focused checks and 21/21 hosted CTest targets,
158,714 checks, zero failures. Its freestanding census was 34 production
translation units, 16 passing and 18 blocked.

The R3N topology remained intact: a full nonroot parent splits while the
grandparent fits; the root ID and height remain unchanged; both 512-byte and
4,096-byte page configurations pass; all old leaves and the new leaf remain
reachable exactly once; child lower bounds and the forward leaf chain remain
valid. The 102-byte 512-byte-page separator and 998-byte 4,096-byte-page
separator remain valid. The 102-byte fixture includes its encoded logical key
and 12-byte big-endian row locator.

R3N's existing planning and partial-staging savepoint faults, earlier UPDATE
preservation, retry, explicit rollback, close/reopen checks, and 512-byte
post-durable-COMMIT cache-failure recovery remained in the focused suite.
R3F-R1's multi-COMMIT WAL recovery and invalid-history rejection remained in
the full regression suite.

The read-only Server reference remained at branch NATIVE_DB_STORAGE and HEAD
f29850c93bbac0a0763a31e806d2816310156575. Its AMD64 Makefiles and kernel
allocator were inspected; the allocator uses an 8-byte-aligned free list and
the configured x64 heap is 512 MiB. No Server files, artifacts, configuration,
Git state, or phase markers were changed.

## Production call graph and staging boundary

The selected production call path is:

    Table::insert
      -> Table::insertInTransaction
        -> IndexTree::insert
          -> findLeaf
          -> stageNonCascadingLeafSplit
            -> stageInternalParentSplit
              -> readInternalChildPlan
                -> decodeInternalChildRecords
              -> internalChildPlanFits
              -> prepareLeafPayload (left and right)
              -> prepareInternalPlanPayload (left parent, right parent,
                 fitting grandparent or new root)
                -> prepareInternalPayloadBytes
              -> Transaction::allocatePagePayload
              -> Transaction::writePagePayload
        -> rollbackStatement on a failed INSERT

The shared function handles the R3M internal-root case (one path level) and
the R3N fitting-grandparent case (two path levels). It declines deeper or
non-fitting-grandparent cases before changing the transaction so the existing
fallback can handle them.

The planner derives candidate page IDs from the transaction page count; it
does not reserve or stage them while planning. It validates the source parent,
allocates a complete parent child-reference array, inserts the new leaf
separator, finds a legal split point, then allocates and validates the
grandparent plan. For R3N it checks that the enlarged grandparent fits. It then
prepares both leaf images, both parent images, and the grandparent image before
the first structural transaction stage. The first structural stages allocate
the new leaf and right internal page; checked writes replace the left leaf,
left parent, and fitting grandparent. The root variant allocates a new root
instead of writing a grandparent.

If a later stage fails, the active INSERT statement savepoint restores the
transaction overlay, catalog snapshot, page count, and cached heap-tail epoch.
The outer explicit transaction and its work from before the INSERT remain
active.

## Hosted allocation inventory at the selected boundary

| Operation and source | Before R3O | Bound and lifetime | R3O storage and failure |
|---|---|---|---|
| Internal child decode in the selected split, formerly through readInternalChildren at database/database_index.cpp:945 | std::vector<Child>, reserve(childCount), then one std::vector<uint8_t> copy per child lower bound | At most floor((payloadCapacity - 24) / 20) existing children; references and bounds are consumed while the validated DatabasePage payload is alive | readInternalChildPlan writes fixed records to a fully reserved GxArray using the active Transaction page allocator. GxStatus is checked and mapped to DbStatus. The shared decodeInternalChildRecords parser preserves the old range, length, page ID, and output validation |
| Insert the new leaf child in stageInternalParentSplit, database/database_index.cpp:1338–1412 | std::vector insert could move and allocate child objects and bound vectors | Existing child count plus one, proven from the page capacity | gxArrayInsert operates inside capacity reserved up front. Existing bound pointers borrow parentPage.payload; the new bound borrows the owned separator buffer |
| Physical separator construction, database/database_index.cpp:1354–1400 | makePhysicalKey copied a std::vector logical key and appended the 12-byte locator | maxIndexKeyBytes(pageSize) plus 12; at most 1,036 bytes with the 1,024-byte SQL key cap | One byte GxArray allocated through Transaction::pageAllocator. Its bytes are the right leaf's logical key followed by the same big-endian page ID and slot bytes |
| Parent redistribution and grandparent insertion, database/database_index.cpp:1420–1529 | Hosted child vectors and copied bounds fed fit checks, split search, and grandparent insertion | Parent and grandparent each have at most the page-derived child bound; one extra record is allowed for the inserted child | The same GxArray plan is authoritative. internalChildPlanFits reads it directly; a pre-reserved grandparent plan receives the new right-parent reference |
| Internal candidate encoding, database/database_index.cpp:284–421 and 1141–1165 | The vector wrapper and an internal payload encoder previously had separate representation access | One payload per candidate page, each no larger than pageSize minus the 32-byte page header | Both the existing vector writer and the new plan writer call prepareInternalPayloadBytes. The plan is serialized directly; there is no second hosted child plan |
| Page number selection and stage order | No separate owning list was found in this path; IDs are calculated from Transaction::pageCount | Two new pages for R3N, three for the root case; bounded by kMaxPageCount | Scalar IDs and checked transaction calls; no planning allocation |

No std::map, raw hosted new, or owned std::string is used to represent child
ordering in this planner. DbResult's existing diagnostic message is a
std::string facility shared by the database; it remains a hosted error-reporting
dependency outside the GxArray storage contract. The leaf entries and path
passed into this function also remain std::vector-based, as recorded below.

## Bounded storage and ownership

The existing format defines page payload capacity as pageSize minus the
32-byte database page header. Internal pages use a 24-byte internal header,
an 8-byte slot, and a 12-byte minimum child record before any lower-bound
bytes. Thus the maximum existing child count is:

    maxInternalChildCount = floor((pageCapacity - 24) / (12 + 8))

The supported page-size limits yield these bounds:

| Page size | Payload capacity | Maximum existing child refs | Plan refs including one insertion | Maximum plan-record bytes on AMD64 |
|---:|---:|---:|---:|---:|
| 512 | 480 | 22 | 23 | 552 |
| 4,096 | 4,064 | 202 | 203 | 4,872 |
| 65,536 format maximum | 65,504 | 3,274 | 3,275 | 78,600 |

PlannedChildRef is a fixed 24-byte record on the qualified AMD64 ABI: a u64
page ID, an 8-byte pointer to the lower-bound bytes, and a u32 length with
padding. GxArray stores raw bytes and does not start C++ object lifetimes, so
the implementation explicitly copies records to and from the array with
memcpy. Each array reserves its full proven element count before decoding or
insertion; the subsequent insert cannot grow it.

The maximum physical separator is 1,024 encoded logical-key bytes plus the
12-byte locator, or 1,036 bytes. A plan's lower-bound pointers remain valid
because source bounds borrow the still-live validated parent or grandparent
DatabasePage payload, and the inserted separator is owned by a scoped GxArray.
ScopedIndexPlan and ScopedIndexPayload release through the allocator recorded
in each GxArray. Their lifetime is limited to the active split call and
therefore to the active transaction allocator's lifetime.

At the SQL format maximum, the two plan arrays require at most 157,200 record
bytes together. Five prepared candidate payload buffers require at most
327,520 bytes. The separator requires at most 1,036 bytes. Planner and
candidate-payload scratch therefore totals at most 485,756 bytes, excluding
allocator bookkeeping and alignment. When the five candidates are staged,
the transaction overlay can additionally own up to five complete 65,536-byte
page images (327,680 bytes). The combined bound for these plan buffers,
candidate payload buffers, and those staged images is 813,436 bytes. Source
page payloads and the pre-existing leaf-entry vectors are separate. These
buffers are heap allocations via the transaction allocator, never large
service-stack arrays.

Allocation status mapping is explicit: OutOfMemory becomes NoSpace; resource
limit and overflow become TransactionTooLarge; invalid/corrupt data becomes
CorruptPage; invalid allocator state becomes Internal. INSERT maps NoSpace to
SQL ResourceLimit. RAII destroys any earlier plan buffers if a later plan or
candidate preparation fails. Transaction page count and existing page images
are not changed during this preparation.

## Exact conversion and remaining split allocations

The selected R3N path no longer allocates a hosted std::vector<Child> for the
parent or grandparent plan, copies each existing lower-bound key into an owned
vector, grows that child vector to insert the new child, or constructs the
separator with makePhysicalKey's owned vector. The direct plan serializer uses
the same common byte writer as the existing child-vector wrapper.

The conversion does not remove the following hosted work:

| Remaining operation | Actual source and current ownership | Boundedness, failure path, and portable follow-up |
|---|---|---|
| Leaf entry collection, sorting, and split partitions | IndexTree::readLeaf, IndexTree::insert and stageNonCascadingLeafSplit in database/database_index.cpp:700–900, 1801–1895, 1170–1273; vectors own decoded keys and copied left/right entries | Entry count is limited by page payload and key bounds, but vector growth/copies use the hosted allocator. Existing bad_alloc catches at Table::insert are the outer compatibility path. A leaf-plan continuation can store key slices plus locators in transaction GxArray buffers and encode the two candidates from the plan |
| Search path and legacy recursive internal splits | findLeaf and IndexTree::insert in database/database_index.cpp:1658–1700 and 1938–2040; path and child/partition data use vectors | Traversal depth is capped at kMaxIndexDepth 32 and each page is bounded, but child vectors and recursive partition copies remain hosted. Arbitrary-depth cascade remains unqualified. Continue one bounded leaf or parent propagation case at a time |
| Existing parent readers and writers outside the converted route | readInternalChildren, writeInternal, internalChildrenFit and prepareInternalPayload's vector wrapper in database/database_index.cpp:945–1165 | Page child counts are bounded; the compatibility writer catches payload bad_alloc but child list allocations are not transaction-allocator-backed. The common decoder and serializer reduce duplication; remaining callers can be moved individually to checked plan APIs |
| Index leaf cycle sets | lookupAll at :1707–1770; rangeScan at :2083–2204; validate and validateNode at :2206–2364. The header includes <set> at database/database_index.h:27 | std::set is used as membership/cycle detection: only insert's duplicate result and visited count are observed; its ordering is not part of index order. It is not used by split planning. Its possible membership count is bounded by pageCount, itself up to 2^32, but that is not a practical fixed scratch limit. Keep it as a separate bounded-membership migration |
| Page materialization and legacy DatabasePage writers | DatabasePage::parse copies into std::vector<uint8_t>; serialize resizes an output vector, database/database_page.cpp:30–83. IndexTree::writeLeaf and writeInternal populate page.payload vectors | Payload is at most pageSize minus 32, but ownership uses the hosted allocator. A future caller-owned page view or GxOwnedPage path must preserve CRC/type/identity validation and explicit allocation status |
| Transaction page reads and statement snapshots | Transaction::readPage calls DatabasePage::parse and catches bad_alloc, database/database_transaction.cpp:81–114. beginStatement clones the portable page overlay then copies the catalog; rollbackStatement restores by move and swap, :350–395 | Page images use the transaction allocator, while parsed payload and catalog compatibility copies use hosted containers. Existing failure returns NoSpace and the rollback path is allocation-free after the snapshot. Migrate the page view before broad payload portability |
| SQL row and parser materialization | SQL token, parser, relational, and heap code use std::string/vector; database/database_sql.cpp also has a map dependency | Capacities are controlled by SQL row, text, schema, and page limits at individual boundaries, but hosted ownership remains across interfaces. A bounded row-view conversion should follow after page and index consumers accept caller-owned bytes |
| UPDATE/DELETE index mutations | Table update/delete code builds transaction savepoints and invokes IndexTree::remove; IndexTree::remove uses vector paths/entries and existing writeLeaf/writeInternal compatibility writers | Savepoint restoration is covered, but delete merge/root shrink is not part of SQL6 behavior (the root does not shrink). Reuse a checked leaf/page writer before changing those operations |

The converted parent and grandparent plan has no unordered representation:
it preserves child sequence, inserts the new child immediately after the old
parent's leaf reference, and tests the same first legal split point. The
promoted separator remains the first lower bound of the right parent. Existing
logical-key order and locator tie-breaking remain lexicographic and unchanged.

## <set> freestanding blocker

The final Windows PowerShell 5.1 freestanding run still stops compiling
database/database_index.cpp at database/database_index.h:27, the <set> include.
The header's set type is needed by validateNode, and the implementation uses
sets in lookupAll, rangeScan, and validate's tree and leaf-chain walks. These
sets detect repeated page IDs; no caller reads their sorted iteration order.
Their maximum membership is bounded by database page count, but the format's
2^32 page ceiling is not a small static allocation bound.

The selected split planner does not use a set. Converting its child vectors
does not remove the shared header include or change the first compiler
diagnostic. Other standard headers in the index header and implementation
remain, so removing <set> alone would not qualify the translation unit.

## Fault injection and rollback evidence

The focused split test injects a failure at each of the three R3N planning
allocations for both page sizes. The matching allocator request sizes are
derived from the exact plan records and physical separator in the fixture:

| Site | 512-byte fixture | 4,096-byte fixture | Observed outcome |
|---|---:|---:|---|
| Parent child plan, 4 records | 96 bytes | 96 bytes | Exact planner error, SQL ResourceLimit, no structural image staged |
| Physical separator | 102 bytes | 998 bytes | Exact planner error, SQL ResourceLimit, no structural image staged |
| Grandparent child plan, 3 records | 72 bytes | 72 bytes | Exact planner error, SQL ResourceLimit, no structural image staged |

Each injected case runs after a successful UPDATE in the same explicit
transaction. Assertions verify one matching allocation attempt, one failure,
zero success at the target, unchanged modified-page count, page count, and
outstanding transaction-owned bytes, plus no structural page-image allocation
beyond the already completed heap append. Source roots, parent/leaf images,
index entries, and the failed row remain unchanged; the successful UPDATE
remains visible. The exact error is checked. The same INSERT is retried
successfully, then an explicit transaction ROLLBACK removes the retry and
restores the original pre-UPDATE state.

Plan capacity is reserved completely by initSplitPlanArray before decode or
insert. There is no incremental plan growth after initialization; the exact
buffer-allocation faults above target each complete bounded allocation.
Candidate preparation allocation faults and the accepted partial-stage faults
remain enabled for both page sizes. Failures after one structural stage and
after four stages restore the INSERT savepoint, including page count, staged
images, index/catalog state, and allocator ownership. The prior UPDATE
survives those failures as well.

The 4,096-byte R3N case now runs the same expanded fault matrix as the
512-byte case. Success tests retain both primary and secondary index shape,
key ordering, uniqueness, range behavior around the promoted separator,
root identity, tree height, leaf reachability, page counts, reopen validation,
and relational integrity.

## Persistence, WAL, and compatibility

No .gxdb or .gxwal format, page header, CRC, index page version, internal
entry layout, child slot, catalog record, separator encoding, row locator, WAL
page-image framing, or COMMIT semantics changed. The portable plan is serialized
through prepareInternalPayloadBytes, which is also used by the existing
vector-facing internal payload preparation API. This shares the byte encoder
instead of maintaining a competing serializer.

Tests inspect the resulting page structure and separator bytes, verify the
same key and child relationships, commit and reopen the tree, and exercise the
accepted post-durable-COMMIT cache failure and WAL replay. Existing
multi-COMMIT valid-history recovery (including 4 → 8 → 23 → 48), rejection of
the invalid 4 → 3 history, and repeated reopen coverage remain in the full
suite. No artificial checkpoint was introduced.

No separate full-page golden-file differential run was added. Byte
compatibility evidence comes from the unchanged persistent constants and page
codec, use of the shared internal byte writer, separator and child-layout
assertions, and commit/reopen/WAL tests.

## Qualification

The final complete build passed. The final serial verbose CTest run passed
21/21 targets, 160,567 checks, zero failures. It completed in 1,764.90 seconds.
The focused split executable reported 10,276 checks and zero failures; the
R3O-specific additions account for 1,853 checks over the accepted R3N
focused count of 8,423.

The final PowerShell 5.1 freestanding census remained 34 production
translation units: 16 pass, 18 blocked. The previously passing object census
was retained; no new production translation unit was added and no previously
blocked object became passing. The changed index translation unit still fails
at <set>, before producing an object or symbol listing.

The first diagnostics for the 18 blocked production units are:

| Production translation unit | First hosted-header diagnostic |
|---|---|
| database_io.cpp | cstdio |
| database_native_io.cpp | memory |
| database_header.cpp | string |
| database_page.cpp | vector |
| database_file.cpp | memory |
| database_engine.cpp | memory |
| database_buffer.cpp | memory |
| database_catalog.cpp | string |
| database_index.cpp | set |
| database_heap.cpp | memory |
| database_wal.cpp | memory |
| database_transaction.cpp | memory |
| database_relational.cpp | memory |
| database_sql_token.cpp | string |
| database_sql_tokenizer.cpp | string |
| database_sql_parser.cpp | vector |
| database_sql_expr.cpp | vector |
| database_sql.cpp | map |

The portable allocator, GxArray, owned-page, heap-page builder, page codec,
cache, recovery, transaction-page, and WAL objects that passed before still
pass. The hosted
allocator adapter retains undefined malloc/free references. The checksum
object retains __cxa_guard_acquire and __cxa_guard_release. There is no new
object or ABI symbol attributable to the modified index translation unit
because it is rejected at the pre-existing <set> include. No SQL native link
or QEMU qualification is claimed.

## Payload conversion readiness and next continuation

The selected planner is now ready as a bounded transaction-owned planning
boundary. Global DatabasePage::payload conversion is not ready as a single
change: parse/read callers, legacy leaf/internal writers, transaction reads,
catalog and parser values, and compatibility-facing APIs still exchange
hosted vectors and strings. The freestanding census confirms that many of
those translation units still stop at standard-library headers.

The next bounded P11 continuation should convert one leaf-split planning
boundary: decode the split leaf into transaction-allocator-owned fixed records
whose key slices borrow the validated source payload and whose inserted
physical key is transaction-owned; choose and validate the same split point;
then prepare the two leaf images through the existing checked page builder.
Keep path traversal, recursive parent splits, <set> cycle detection, and
global DatabasePage payload changes outside that continuation. Add exact
allocation faults before its first stage and preserve the current savepoint,
retry, and WAL evidence. Do not begin that continuation as part of R3O.

## Final acceptance state

The source and tests were committed locally on the existing SQL main branch.
The ending checkout was clean; its exact local HEAD is reported with the
acceptance summary for this report. Both phase markers remained at SQL9 and
NDB1R4P10 respectively. No push, fetch, branch change, topology operation,
Server edit, phase advance, or P12 work occurred.

**Acceptance answer: Yes.** SQL9 replaced the selected production B+ tree
split-planning allocation dependency with bounded, transaction-allocator
storage and checked failure propagation while retaining separator ordering,
structural planning, savepoint restoration, and accepted split and WAL
recovery behavior.
