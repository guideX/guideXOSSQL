# NDB1R4P11R3AH — Native SQL Read Integration Release Candidate

## Outcome

**Outcome B — `NATIVE_SQL_READ_INTEGRATION_PARTIAL`.** The SQL production
reader/provider sources now enter the Server's normal AMD64 Makefile source,
object, PE-link, and ELF-conversion path under an explicit qualification
configuration. A baseline and candidate image were built from isolated
Server worktrees. The candidate adds no unresolved symbols or PE imports.
Two closed production-written fixtures and a machine-readable manifest are
committed and pass hosted native-provider qualification.

The Server does not yet run the reader or a SQL diagnostic at boot, execute
the production Server VFS read path in a hosted harness, or provide an
operation-lifetime immutable-file guarantee. The qualification option is
disabled by default. No guest test was run. Those missing runtime and
stability requirements prevent Outcome A.

## Repository state and isolation

- SQL repository: `D:\dev\guideXOS_SqlDatabase`, branch `main`.
- Starting SQL HEAD: `5663b8a3cdc80b6a2ae3033df5b4891e2cbd104e`.
- The requested starting HEAD was clean and was two commits ahead of its
  existing `origin/main` tracking ref. No fetch, push, or history rewrite was
  performed.
- `.phase` remains SQL9 → SQL10; SHA-256
  `f84ef02a6b82d4c53cdb2bdd0b80c083a49c6fa2e5df46914d9aadd7555295f0`.
- `.native-sql.phase` remains NDB1R4P10 → NDB1R4P11; SHA-256
  `fe722cf1a92a04c9f16e91e556933c682a7e3475bf3aabb06ce994410a28a52a`.
- Original Server checkout: `D:\dev\guideXOSServer_NATIVE_DB_STORAGE`,
  branch `NATIVE_DB_STORAGE`, HEAD
  `f29850c93bbac0a0763a31e806d2816310156575`. It remained clean.
- Dedicated Server integration worktree:
  `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH`, created from that exact
  Server HEAD on `codex/ndb1r4p11rah-sql-read-integration`.
- Dedicated comparison worktree:
  `D:\dev\guideXOSServer_SQL_READ_BASELINE_NDB1R4P11RAH`, detached at the
  same Server HEAD. It contains no tracked changes.
- Each worktree used a separate `BUILD_DIR`. Their ignored Mbed TLS dependency
  path is a junction to the already present dependency in the original Server
  checkout; the dependency and tracked files were not modified.
- The only tracked Server integration change is `kernel/Makefile` in the
  dedicated branch. No active Server development checkout was edited and no
  existing Server branch was merged.

The two ignored files identified by R3AG in the original Server checkout
remain present and unchanged:

| File | SHA-256 at start and end |
|---|---|
| `kernel/build/amd64/obj/core/main.o` | `A3886D8953D91C5CA20F19031FE445A7AA54E069D9E22FC08EEE671ECECE84F8` |
| `kernel/build/amd64/obj/core/main.d` | `EB8ACF16BB92BEADB4C5FD12DA3AD1F017A2D1403A0F0D72B32CF734EEDD7BD0` |

Neither file was deleted, cleaned, overwritten, or used as a build input. The
side-effecting `mingw32-make ... -q` probe was not repeated.

## Normal Server build integration

The normal kernel Makefile selects production C++ sources from
`kernel/core/*.cpp` and `kernel/arch/<arch>/*.cpp`, compiles them into
`build/<arch>/obj`, and links those objects with the architecture linker
script. AMD64 links a PE image, then converts it to ELF with the Makefile's
normal `objcopy` command. The root Makefile delegates AMD64 builds to this
kernel Makefile. Existing `all`, `clean`, and architecture targets remain
intact.

R3AG proved candidate-image linkage by compiling SQL objects separately and
manually linking them with a preexisting Server object set. That did not test
the Server Makefile's source list, normal per-source object rules, dependency
files, or standard `all` PE/ELF targets. R3AH adds one disabled-by-default
option to those existing rules:

```text
GX_NATIVE_SQL_READ_QUALIFICATION=1
```

When set for `ARCH=amd64`, the Makefile requires the configured SQL source
tree, compiles the fixed production source list into a distinct
`$(OBJ_DIR)/sql` subtree using the Server's existing freestanding C++ flags,
adds those objects to `ALL_OBJS`, and uses the normal PE-link and ELF-convert
targets. The ordinary Server object rules do not receive SQL-only macros or
include directories. With the default value `0`, the release source/object
set and startup code are unchanged.

The option is a build qualification gate, not an executing diagnostic. No
SQL initialization or reader call was added to `kernel_main` or other Server
startup paths.

### Build commands and evidence paths

The build toolchain was MinGW-W64 `g++ 15.2.0`, `ld`, and `objcopy`. The
profile verification passed against Mbed TLS 4.1.0 and TF-PSA-Crypto 1.1.0.
The ordinary build configured Debug source units with the Server's existing
flags:

```text
-std=c++14 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti
-nostdlib -nostdinc++ -fno-builtin -DGXOS_BARE_METAL
-m64 -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2
-fno-pic -fno-pie -fno-PIC -fno-PIE -mno-stack-arg-probe
-fno-stack-check -fno-stack-protector -D__x86_64__
```

SQL translation units additionally receive only
`-DGX_NATIVE_SQL_READ_QUALIFICATION=1` and the SQL `database`, `database/native`,
and `database/portable` include roots.

From the baseline worktree's `kernel` directory:

```powershell
mingw32-make ARCH=amd64 BUILD_DIR=build/amd64-r3ah-baseline
```

From the integration worktree's `kernel` directory:

```powershell
mingw32-make ARCH=amd64 BUILD_DIR=build/amd64-r3ah-native-sql-final `
  GX_NATIVE_SQL_READ_QUALIFICATION=1 `
  GX_SQL_DATABASE_ROOT=D:/dev/guideXOS_SqlDatabase
```

Both commands used the normal `all` target (no query-only or clean target).
Full build logs and objects are under each worktree's named isolated build
directory. The baseline build compiled the entire Server source set and
linked successfully. The candidate compiled the same Server units plus the
following 16 SQL production translation units through the Makefile rule:

1. `database/native/gx_native_sql_index_page_provider.cpp`
2. `database/database_checksum.cpp`
3. `database/database_format.cpp`
4. `database/portable/gx_page_codec.cpp`
5. `database/portable/gx_allocator.cpp`
6. `database/portable/gx_array.cpp`
7. `database/portable/gx_page_visit_set.cpp`
8. `database/portable/gx_index_boolean_key.cpp`
9. `database/portable/gx_index_integer_key.cpp`
10. `database/portable/gx_index_text_key.cpp`
11. `database/portable/gx_index_physical_key.cpp`
12. `database/portable/gx_index_leaf_entry.cpp`
13. `database/portable/gx_index_leaf_traversal.cpp`
14. `database/portable/gx_index_internal_node.cpp`
15. `database/portable/gx_index_tree_descent.cpp`
16. `database/portable/gx_index_lookup.cpp`

The existing Server wildcard source selection also compiled the production
`native_database_file.cpp` bridge and `vfs.cpp`; the full kernel object set
was passed to the regular linker. No dummy SQL/provider symbols or VFS link
stubs were used in either kernel image.

### Baseline versus candidate image

| Artifact | Baseline size | Candidate size | Candidate SHA-256 |
|---|---:|---:|---|
| PE `kernel.pe` | 2,773,060 bytes | 2,822,063 bytes | `D0B3BE63CA09536CC8459BB726322C58CC7A5EA14F1E86646CD464E823CEC947` |
| ELF `kernel.elf` | 2,670,224 bytes | 2,712,936 bytes | `935AC933051095D88E292964E96759C5A8A06CA60D98463C8ADE7967927C9CB1` |

Exact candidate paths:

- `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\kernel\build\amd64-r3ah-native-sql-final\bin\kernel.pe`
- `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\kernel\build\amd64-r3ah-native-sql-final\bin\kernel.elf`

Exact baseline paths:

- `D:\dev\guideXOSServer_SQL_READ_BASELINE_NDB1R4P11RAH\kernel\build\amd64-r3ah-baseline\bin\kernel.pe`
- `D:\dev\guideXOSServer_SQL_READ_BASELINE_NDB1R4P11RAH\kernel\build\amd64-r3ah-baseline\bin\kernel.elf`

Baseline hashes are `758E851710CF99801F98D487CF4A0179FF8E376080BD5FDD062E36596AE41406`
(PE) and `1F841F6E7C749068924344ED304A13D90162FA02BB7523403BAEA4CE0AB577CB`
(ELF). The linker produced both candidate images and `nm` found the native
provider, `gxIndexLookupAll`, `gxIndexRangeScan`, and bridge/VFS symbols in
the candidate.

`nm -u -C` found 21 unique undefined symbols in each PE. The sets are
identical; SQL introduced none. These are existing unresolved symbols, not
evidence of an import-free kernel. Separately, PE inspection shows empty PE
import and export directories in both images. Candidate `nm` global defined
symbol inventory
increased from 3,724 to 3,808 entries, including the linked SQL/provider
implementation. The 16 SQL objects have no forbidden STL, exception/RTTI,
hosted I/O, or allocator dependency. The candidate build log has 393 warning
occurrences from existing Server source units, zero errors, and no warning in
the SQL units. No baseline/candidate startup source was changed, and no SQL
entry point is called at boot.

Images were linked, not booted. This does not establish production VFS
execution or guest success. No boot ISO or guest disk was assembled or
modified.

## Production-written fixture and provenance manifest

The committed package is:

- `tests/fixtures/NDB1R4P11R3AH/leaf-only.gxdb`
- `tests/fixtures/NDB1R4P11R3AH/multilevel.gxdb`
- `tests/fixtures/NDB1R4P11R3AH/fixture.manifest`

The packaging command builds the Windows R3AG/R3AH hosted target and invokes
its production-writer packaging mode through
`tools/package_ndb1r4p11rah_fixture.ps1`. It refuses to overwrite an existing
package. The generator uses the existing SQL `Database::create`,
`createTable`, `createIndex`, `Table::insert`, `DatabaseReadView`, and
`Database::close` paths. It closes the writer, confirms there is no `.gxwal`
sidecar, then packages the image. Validation reopens read-only and recovers
no WAL. The package contains no user database data.

Both images use 512-byte pages, format 1.0, one table, and six indexes. The
leaf image has two rows and 10 pages. Its catalog root is page 1 and all six
index roots are leaf pages (pages 4 through 9). The multilevel image has 80
rows and 490 pages. Its catalog root is page 1; the six index root pages are
234, 148, 124, 375, 218, and 259. Each is an encoded internal index page.
The indexes include nullable Int64, Int32, Boolean, and text keys, plus
non-null Int64/Int32 boundary indexes. Text values include the three bytes
`61 00 62`. The multilevel Int64 key 7 occurs in 73 rows across leaves; the
remaining seven nullable entries are NULL. Both signed integer extrema are
present.

| Image | Length | SHA-256 | Catalog | Index roots (owner IDs match index IDs) |
|---|---:|---|---|---|
| Leaf-only | 5,120 | `75e25cbad04087696a06b4ef22276b42d094a9cf72600406210edb46a4a2129c` | v3, page 1, table 1 | 4, 5, 6, 7, 8, 9 |
| Multilevel | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` | v3, page 1, table 1 | 234, 148, 124, 375, 218, 259 |

Manifest SHA-256: `43687CBD0087E3FF8747605A1F3510DD4EE3EFF780FE8A7E1EF5B45E7F90A8D3`.

The manifest format is UTF-8-compatible, bounded, strict `key=value` text
with header `GUIDEXOS_SQL_FIXTURE_MANIFEST_V1`. It has 17 query entries with
expected counts and full ordered `page:slot` locator lists. Point cases cover
leaf and multilevel keys, duplicates, missing keys, NULL, Boolean, embedded
zero text, and Int32/Int64 extrema. Range cases cover inclusive and exclusive
bounds, each one-sided bound, and unbounded scans. The two fixtures, manifest,
Server baseline ID, SQL writer engine source ID, catalog/index fields, file
lengths, database IDs, creation times, page metadata, and file hashes are
bound together in this package.

The manifest validator rejects malformed, duplicate, missing, or unexpected
fields and checks image size, SHA-256, header CRC/format/identity, page size,
page count, page-aligned length, catalog version, and catalog-root bounds. It
opens each image through the production SQL read-only database/catalog path,
compares each index record and owner with the same database read view's
`indexRoot`, inspects the encoded root page, and asks the native provider to
read and decode that exact page. It never searches pages to guess a root. It
then independently scans rows and compares every expected manifest query
against the production native point/range reader.

The SQL file format has no signature field that authenticates an external
manifest. A matching SHA-256 only binds the bytes to a manifest presented by
the same source. The manifest and files must come from a trusted release or
qualification package whose origin is verified out of band. The manifest is
not a cryptographic signature and does not create a filesystem immutability
lease.

## Qualification results

The new `database_ndb1r4p11rah_fixture_test` CTest target reads the committed
package. Its verifier passed **340 checks, 0 failures**, including two repeats
of each of the 17 expected queries, page-buffer borrow/release checks, exact
root and owner matching, query staging on partial failure, and unchanged
source bytes/SHA-256 before and after.

The new validator rejection cases include malformed manifest syntax, changed
hash and length, unsupported format major, wrong page size, invalid page
count, out-of-range/stale root, wrong root owner, missing index identity, and
incorrect expected query locators. Existing R3AG coverage also rejects
unsupported live consistency mode, truncated pages, invalid successor
references, provider/storage read failure, invalid header/page CRC and type,
wrong owner association, and early/failed visitors; it checks that failed
staged operations publish no partial result and release every page borrow.
Corruption cases use disposable copies. The committed fixture itself is
rechecked byte-for-byte and by SHA-256 after reads.

Focused CTest after the integration/fixture changes: **5/5 passed**:
R3AE read-view, R3AF provider, R3AG native provider, R3AH fixture, and R3AG
candidate-link qualification.

The direct assertion totals from the rebuilt hosted binaries were:

| Qualification | Checks passed | Failed |
|---|---:|---:|
| R3AE production read view | 98 | 0 |
| R3AF immutable image provider | 280 | 0 |
| R3AG native image provider | 340 | 0 |
| R3AH committed fixture and manifest | 340 | 0 |

Freestanding census: **26/44 passed**, preserving all 26 prior passes. The
other 18 units remain blocked by hosted standard-library headers such as
`<cstdio>`, `<memory>`, `<string>`, `<vector>`, and `<map>`. The separate
production point/range and root-to-leaf probes compile, link without undefined
symbols, and execute against their fixtures. The native provider is also
compiled as part of the actual Server build. No broad STL-removal change was
made.

The complete final Debug CTest aggregate passed **39/39 targets**, with zero
failures, in **2,037.77 seconds**. This includes the focused R3AH fixture
target and all existing SQL regression targets.

```text
100% tests passed, 0 tests failed out of 39
```

No expectation was suppressed or changed. Sanitizers were not run: the
R3AG-reported Clang and GCC ASan/UBSan runtime prerequisites remain
unavailable. No QEMU or QAC was launched.

## Supported access and limits

The native provider accepts only `GX_NATIVE_SQL_CONSISTENCY_IMMUTABLE_IMAGE`
and rejects `LIVE_SINGLE_INSTANCE`. The SQL source set is linked only when
the explicit Makefile qualification option is set. General `.gxdb` selection,
native SQL writes, persistent database mutation, WAL recovery through the
provider, and writer/live-handle access remain unsupported. The harness
compiles the actual Server bridge implementation but injects hosted file
callbacks; it does not execute production `kernel::vfs` open/read/seek.

The bridge's read-only handle prevents writes through that handle only. The
VFS marks a mount read-only when its block device has no write callback; the
normal boot storage path prefers writable devices. There is no per-file
immutable lease, cross-handle/process writer exclusion, or atomic positional
read operation. A hash check cannot prevent mutation after verification. No
general caller may treat the provider's immutable mode flag as enforcement of
these external conditions.

## Future guest acceptance package

The exact kernel candidate and fixture hashes, root identities, and query
expectations are recorded in this report and the committed manifest. Proposed
serial transcript format (every locator payload must match the manifest
exactly; the placeholders below are replaced by those complete lists):

```text
[GXDB-R3AH] START
[GXDB-R3AH] IMAGE file=LEAF.GDB sha256=<image.0.sha256> page_size=512 page_count=10 catalog_root=1
[GXDB-R3AH] IMAGE file=MULTI.GDB sha256=<image.1.sha256> page_size=512 page_count=490 catalog_root=1
[GXDB-R3AH] INDEX file=MULTI.GDB name=IX_Rows_K64 id=1 root=234 owner=1 type=internal
[GXDB-R3AH] QUERY name=duplicate_k64_7 status=PASS count=73 locators=<query.1.expected_ordered_locators>
[GXDB-R3AH] QUERY name=range32_inclusive_1_2 status=PASS count=32 locators=<query.10.expected_ordered_locators>
[GXDB-R3AH] QUERY name=range32_unbounded status=PASS count=80 locators=<query.14.expected_ordered_locators>
[GXDB-R3AH] QUERY name=range64_full_bounds status=PASS count=80 locators=<query.15.expected_ordered_locators>
[GXDB-R3AH] PASS queries=17 borrows=0 writes=0
```

Every query must emit a line in manifest order. Errors should emit
`[GXDB-R3AH] FAIL code=<stable-error-code>` and never emit the final `PASS`.
Boot admission requires AMD64, successful VFS initialization, a designated
fixture volume mounted read-only at `/R3AH`, the fixed files at
`/R3AH/LEAF.gxdb` and `/R3AH/MULTI.gxdb` (mapped by the bridge to `LEAF.GDB`
and `MULTI.GDB`), and a trusted fixture/manifest identity. Missing VFS,
mount, file, root/catalog identity, or immutability prerequisites fail closed.

The current `kernel_main` persistent-storage loop skips block devices without
a write callback, even though `vfs::mount_type` marks a mount read-only when
the device lacks one. Therefore the read-only fixture volume needs an
explicit controlled mount hook or equivalent boot configuration; the normal
writable-root mount is no stability guarantee. VFS has FAT and ext mount
paths, but this Server does not package the fixture into a guest volume or
bind its manifest identity to a diagnostic. The smallest next integration is
that fixed-volume mount plus a compile-time-gated post-mount diagnostic,
with rollback by omitting the flag and fixture volume from the guest artifact.

No guest test was executed and no QAC slot or shared artifact was touched.
Image files existing on disk are evidence of linkage only.

## Roadmap checkpoint

1. **Portable read-side maturity:** qualified for point lookup and range scans
   over production B+ tree pages, including duplicate keys and page-provider
   failure handling.
2. **Next milestone:** native guest acceptance of this fixed fixture through
   the production VFS, using an explicitly read-only image and a small gated
   serial diagnostic. Another long chain of isolated reader helpers would
   have diminishing returns.
3. **Native read/write blockers:** native catalog parsing/root handoff,
   recovery and WAL policy, writer exclusion, atomic/stable file reads,
   operation coordination across handles/processes, and durability behavior.
4. **Hosted-only contracts:** transactions, rollback, WAL commit/checkpoint,
   recovery, storage durability, and cross-operation coordination have not
   been qualified in the native guest.
5. **Multi-handle coordination:** must precede native writes and should be
   designed with the Server's storage ownership contract before enabling
   concurrent native reads of mutable files.
6. **Freestanding SQL:** broader execution remains blocked by the concrete
   hosted STL dependencies in the 18 failed census units; keep that work
   separate from this bounded index reader.
7. **Float64 portability:** belongs in a separate cross-architecture key
   encoding/ordering milestone before indexing Float64 values natively.
8. **Lightweight SQL management application:** first needs a stable native
   open/catalog/read contract, query result transport, and a controlled
   storage lifecycle; GUI work is premature.
9. **Stored-procedure-like features:** remain future parser, execution,
   isolation, and persistence work.
10. **Phase choice:** proceed to one meaningful production-VFS guest fixture
    acceptance milestone, then reassess read/write coordination and broader
    freestanding work.

## Final evidence and commits

- SQL release-code ending HEAD before this documentation-only closeout:
  `23ef98d1f2f11bab0538414d2d85dabeb7636ed3`.
- This report is a documentation-only commit after the release-code ending
  HEAD above; its full commit ID is the final SQL HEAD reported at closeout.
- Server integration branch and commit:
  `codex/ndb1r4p11rah-sql-read-integration` at
  `552dd4ee17b26cf0b367f03dd1d8a46a443ff29c`.
- Server integration starting baseline:
  `f29850c93bbac0a0763a31e806d2816310156575`.
- Server integration modifications: `kernel/Makefile` only.
- No push or merge occurred.
