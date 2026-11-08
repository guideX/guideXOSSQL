# NDB1R4P11R3AO — Native RAM Disk Mapping and Boot Handoff Repair

## Outcome

**Outcome A — `NATIVE_RAMDISK_MAPPING_REPAIR_HOSTED_VERIFIED`.** The demonstrated
virtual-address collision is repaired with an explicit BootInfo v3 contract
and a bounded loader-created alias window. The production mapping regression,
retained hosted regressions, ordinary and diagnostic builds, and a newly
identified UEFI ISO all passed their source-backed checks.

This is not native guest acceptance. R3AO did not start QEMU or QAC. The R3AO
candidate remains pending the separately authorized R3AP guest run.

## Repository state and preservation

| Checkout | R3AO starting state | R3AO result |
|---|---|---|
| SQL | `main`, `cb08e0ebdf3fb25bb9aae77aaecbc6490d807e1b`; the modified R3AN-R1 evidence report was preserved | R3AN-R1 evidence committed locally as `6d2d52ab812ae8c89a14da8dc6908499bdec9a77`; R3AO report and R3AP procedure committed locally with this phase |
| Isolated Server integration | `codex/ndb1r4p11rah-sql-read-integration`, `5af7597497ddead12e63ccad87160ffaf5077ee5` | source/test commits `7958d0c3c5b1df76cd0d169e7b16408bebc3579b` and `04bc1900398debffa44611e89af4024edd68cf54` |
| Original Server checkout | Read-only inspection found `main` at `9486b696b7cfd1f02e522e0b7d83385b8c5a9701`, eight commits ahead of `origin/main`, with pre-existing `?? assets/Backgrounds/troncity.png` | Not modified |

The pasted request named `f29850c93bbac0a0763a31e806d2816310156575` as the
original Server baseline; the actual original checkout was already at
`9486b696b7cfd1f02e522e0b7d83385b8c5a9701` when inspected. No operation was
performed in that checkout. No push, merge, reset, rebase, stash, or clean was
performed. Existing R3AN and R3AL artifacts and the trusted R3AI fixture were
left unchanged. Generated R3AO build and candidate files remain in the
isolated Server checkout under its R3AO output paths.

The R3AN-R1 evidence report was committed before this report was started. Its
raw guest files remain under `E:\CodexArtifacts\NDB1R4P11R3ANR1`.

## Confirmed cause

R3AN-R1 observed a RAM disk allocation at
`[0x155BF000, 0x190BF000)`, length `0x03B00000` (59 MiB). Under MAP-A, the
UEFI page tables identity-mapped the allocation. The linked kernel virtual
span was `[0x00100000, 0x22BB5E80)`. The kernel `MapRange` then mapped that
virtual span onto the kernel's physical image, so the old RAM disk virtual
address `0x155BF000` resolved to kernel physical page `0x2E57E000` under
MAP-B, MAP-D, and the active kernel CR3 at MAP-E.

The original physical page still contained
`EB 58 90 47 55 49 44 45 58 4F 53 00 02 10 20 00`, but the old virtual
dereference returned zero-filled kernel destination bytes. The block callback
therefore supplied a zero bytes-per-sector value to FAT32, which rejected the
volume. SQL did not run in that guest. The handoff stack at physical
`0x200000` also fell inside the linked kernel virtual span and had the same
aliasing risk.

The boundary fault was the BootInfo v2 contract: `RamdiskBase` carried a
physical address, while the kernel consumed it as a virtual pointer after
switching to page tables that replaced low identity mappings with kernel
image mappings. The issue was a mismatch between physical identity and
virtual pointer semantics, not evidence of bad FAT32 geometry.

## Selected design and ownership

The loader now maps each loader-owned payload into a dedicated supervisor
virtual window, `0xFFFF900000000000` through `0xFFFF900100000000` (4 GiB).
The audited Server sources had no mapping in that PML4 slot; its MMIO window
is separate. The mapper rejects already-mapped destinations, noncanonical or
overflowing ranges, mismatched page offsets, and physical frames outside the
52-bit x86-64 PTE address field. It rounds only the backing page envelope and
maps the full payload length, including unaligned first and last pages.

This uses the existing loader-owned page-table construction boundary and does
not allocate a second 59 MiB kernel copy. UEFI allocates the RAM disk as
`EfiLoaderData` pages. Those pages remain reserved after
`ExitBootServices`; the current kernel does not consume the BootInfo memory
map or provide a reclaimer for loader payload pages. The kernel therefore
retains these backing pages for its lifetime. There is no early free or
double-owner transfer; a future reclaimer would need an explicit ownership
handoff before releasing them.

The RAM disk, BootInfo, memory map, and ACPI RSDP aliases have supervisor
read-only leaf PTEs. The handoff stack has a writable alias. The trampoline
sets CR0.WP after loading the kernel CR3 so supervisor writes also honor the
read-only payload PTEs. The RAM disk remains attached through the existing
read-only block device and FAT32/VFS paths.

BootInfo v3 now defines:

- `RamdiskBase`, `MemoryMap`, and `AcpiRsdp` as kernel virtual pointers.
- `RamdiskPhysicalBase`, `MemoryMapPhysicalBase`, and
  `AcpiRsdpPhysicalBase` as their original physical backing addresses.
- `BootInfoPhysicalBase` as the BootInfo page backing.
- `BootStackPhysicalBase`, `BootStackVirtualBase`, and `BootStackSize` as the
  handoff stack relation.
- `RamdiskMappingFlags` as the read-only mapping contract.

The producer and early kernel validator agree on BootInfo version, required
fields, range arithmetic, canonical virtual addresses, and the RAM disk
read-only flag. The gated first-kernel diagnostic walks the active CR3 and
checks that BootInfo and first/interior/last RAM disk samples map to their
recorded physical backing with supervisor read-only permissions, then checks
the expected first 16 backing bytes. The memory-map and ACPI pointers are also
mapped before handoff. Framebuffer, MMIO, and trampoline mappings retain their
identity convention; the loader rejects collisions between those ranges and
the linked kernel virtual span. No separate module/initrd payload exists in
this handoff, and `CommandLine` remains unused.

## Production changes

Server source is on
`codex/ndb1r4p11rah-sql-read-integration` at
`04bc1900398debffa44611e89af4024edd68cf54`.

- `guideXOSBootLoader/boot_payload_mapping.h`: generic checked planning for
  page-aligned aliases, offsets, the 4 GiB virtual window, and supported
  physical frames.
- `guideXOSBootLoader/paging.cpp` / `.h`: protected kernel virtual span,
  collision-rejecting read-only or writable payload mappings, leaf rollback
  after incomplete mappings, physical-width validation, and table-page
  allocation that avoids identity collisions with the linked kernel span.
  The tracked table-page capacity is now 1024 to cover kernel mappings and
  payload aliases.
- `guideXOSBootLoader/main.cpp`: maps the RAM disk, stack, BootInfo, memory
  map, and ACPI RSDP; publishes BootInfo v3 virtual/physical fields; rejects
  unrelated identity-map collisions; and retains bounded R3AO mapping
  diagnostics behind the qualification gate.
- `guideXOSBootLoader/guidexOSBootInfo.h`: BootInfo v3 address and ownership
  fields plus its mapping-contract validator.
- `guideXOSBootLoader/trampoline_msvc.cpp` and `trampoline.asm`: enable
  CR0.WP after the CR3 transition. The loader project includes the mapping
  helper header.
- `kernel/core/main.cpp`: validates the v3 handoff, reports the active-CR3
  translation when mapping diagnostics are enabled, and passes the virtual
  alias to the existing read-only RAM disk attachment.
- `tests/native_boot_payload_mapping_test.cpp` and
  `scripts/run-native-boot-payload-mapping-test.ps1`: hosted regression using
  the actual production `paging.cpp` mapper and BootInfo v3 contract.
- `scripts/prepare-native-sql-r3ao-boot-candidate.ps1`: reproducible packaging
  of a uniquely identified candidate with fixture, manifest, and hashes.

## Hosted verification

All listed results below were freshly executed in R3AO. The SQL production
tree did not change, so the full SQL Debug CTest aggregate was not rerun; its
previously reported 39/39 result remains historical and is not claimed as a
fresh R3AO result.

| Regression | Fresh result | Evidence |
|---|---:|---|
| R3AO production boot-payload mapper and BootInfo v3 | **36/36** | Reproduces the R3AN virtual collision and stack collision; checks original physical bytes, repaired read-only alias, first/interior/last backing, overflow, 52-bit boundary, unsupported physical frame rejection, conflicts, unaligned bounds, allocation failure, rollback, kernel mapping preservation, and inherited CR3 |
| R3AI production VFS/SQL diagnostic | **22/22** | LBA 0 image attachment, read-only mount, byte-identical fixture, repeatable point/range diagnostic, mutation rejection, cleanup, and controlled missing-fixture behavior |
| R3AK production FAT32/VFS mount regression | **73/73** | Exact package geometry, block read, mount rejection cases, writable-control behavior, read-only registration, and complete SQL diagnostic through production paths |
| R3AL candidate mount regression | **73/73** | The current R3AK production runner freshly exercised the preserved R3AL candidate RAM disk |
| R3AM production RAM disk byte provenance | **34/34** | LBA 0 source/destination bytes, production FAT32 consumption, bounds, and fault attribution |
| R3AN production PTE provenance | **34/34** | Retained CR3/PTE walker, range checks, byte comparison, and diagnostic bounds |

The production SQL diagnostic compares every returned locator, in order, with
its trusted expected array. The candidate manifest contains 73 expected
locators for `IX_Rows_K64` (root 234, owner 1) and 32 for `IX_Rows_K32`
(root 148, owner 2). Hosted tests reached fixture metadata/root/owner
validation, point lookup, range scan, and diagnostic PASS. The SQL page
provider closes the opened image, VFS handles are closed, and the read-only
block device exposes no write callback. Hosted execution does not validate
the native CR3 transition.

## Build qualification

Both kernel profiles were built with the repository's AMD64 GNU toolchain in
fresh directories. The kernel sources did not change after the successful
build; the second Server commit only tightened loader physical-range checks
and its tests/package defaults.

- Ordinary: `mingw32-make ARCH=amd64 BUILD_DIR=build/amd64-r3ao-ordinary -j2`
- Qualification: the same production Makefile with
  `GX_NATIVE_SQL_READ_QUALIFICATION=1`,
  `GX_NATIVE_SQL_READ_DIAGNOSTIC=1`,
  `GX_NATIVE_RAMDISK_PROVENANCE_TRACE=1`, and
  `GX_NATIVE_RAMDISK_MAPPING_TRACE=1`, using
  `BUILD_DIR=build/amd64-r3ao-qualification`.

Both produced kernel PE and ELF files. The ELF starts at `0x100000`. The
ordinary kernel contains no R3AO mapping or SQL diagnostic markers; the
qualification kernel includes them. The qualification ELF's 22 unresolved
symbol names exactly match the retained R3AN diagnostic ELF, with no new
unresolved symbols. The kernel PE import directory is empty.

The MSVC Release x64 UEFI loader was built with mapping diagnostics disabled
and enabled, using unique R3AO-R3 output and intermediate directories. Both
builds succeeded. The diagnostic loader uses the same mapping/provenance
defines recorded in the candidate manifest. The PE subsystem is EFI
Application (`0x0A`) and its import directory is empty. The MSVC build reports
the project's existing `/sdl` to `/GS-` and `/Oi` to `/Oi-` override warnings;
there were no build errors.

## Candidate identity and artifact hashes

The final candidate is
`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ao\candidate-r3\guideXOS-NDB1R4P11R3AO-amd64.iso`.
The ISO volume ID is `GUIDEXOSR3AO`; the ESP volume ID value passed to the
packager is `5233414F`. The ISO verifier reports a bootable no-emulation UEFI
image with platform ID `0xEF`. The ESP is 100,663,296 bytes and the ISO is
100,730,880 bytes. This is a new identity; R3AN and R3AL candidates were not
reused or modified.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| Ordinary kernel PE | 2,774,807 | `0222370f055e8ab38e51cbd08b1566bf2f823902d47d2af525c6638712421bd4` |
| Ordinary kernel ELF | 2,670,368 | `43db7160b3620107a9d5af8de68c2f154f20e20dcec685e3919242f809214561` |
| Qualification kernel PE | 2,868,832 | `1b583fc394a0bdbdd623211005797180a956c6eca2e28e45e87ef8ff194f484f` |
| Qualification kernel ELF, packaged | 2,758,776 | `3d79f4fa99be3ff282ea157602659851bdb529e16cf0b2ce6a018a7005f51b80` |
| Ordinary UEFI loader | 65,024 | `1defd407fdb128a32e97cec467cf3f5d245906a6e024e3683c43f674abeed262` |
| Qualification UEFI loader, packaged | 81,920 | `db8789dfbf08b21f3959d32fab01354af3f96cd8c1eb715466f21edf40214351` |
| Embedded ESP image | 100,663,296 | `df241b901f3b8d8a0722a974538bfd97f0792e56c3debb4dbc4633b32345cff3` |
| R3AO ISO | 100,730,880 | `305b56a6001597afa049340501c4a4b30fa47ec6fe63ead8a8accc9185c750f0` |
| ESP file manifest | 489 | `14751864ab22826596becf50ffc69933079a5568d2cd93b9d573aca12c35ca2f` |
| ESP packaging report | 576 | `d78384326a55361456caec9bfc6c8ff0df99012d90b57a40448303aec5711e2d` |
| ISO file manifest | 354 | `e659c68fd50ec6a62428725db886537e152b02285a0fbfd56c35d68cd5428106` |
| ISO packaging report | 677 | `847c41f750b61695ce2a149c5d2559a0ccd7179ca6a43b5dfa582f6c8ade7f1f` |
| ISO README | 519 | `d9b844ac261d4c175d49e3994410012f741bef1880a5f2dbe7c6098fe6a1f3d9` |
| Read-only RAM disk | 61,865,984 | `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| SQL fixture `MULTI.GDB` | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted `FIXTURE.MAN` | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| Candidate manifest | 6,511 | `e89385508bb82863c4cb3e1fc66d9947156947fa59b073b427f9121115cd730d` |

The candidate manifest records Server source commit
`04bc1900398debffa44611e89af4024edd68cf54`, SQL source commit
`6d2d52ab812ae8c89a14da8dc6908499bdec9a77`, BootInfo v3 semantics, fixture
metadata, both complete ordered locator arrays, and expected R3AO markers.

## Remaining runtime uncertainty and R3AP

R3AO has not exercised the UEFI loader, its CR3 switch, CR0.WP, the alias under
real guest page tables, or native FAT32/SQL startup. The mapping regression
uses the production mapper and walker in a hosted firmware fixture; it cannot
prove native hardware translation or full Server startup. No guest success is
claimed.

The R3AP run contract is in
`NDB1R4P11R3AP_NATIVE_RAMDISK_MAPPING_GUEST_ACCEPTANCE.md`. It requires an
authorized exclusive QAC reservation and checks UEFI/kernel entry, active-CR3
and physical-byte identity, read-only FAT32/VFS mounting, complete ordered SQL
results, final PASS markers, clean shutdown, and QAC release.

## Commits

- SQL preservation commit for R3AN-R1 evidence: `6d2d52ab812ae8c89a14da8dc6908499bdec9a77`.
- Server repair commits: `7958d0c3c5b1df76cd0d169e7b16408bebc3579b` and
  `04bc1900398debffa44611e89af4024edd68cf54`.
- The final local SQL commit contains this report and the R3AP procedure; its
  object ID is returned with the R3AO completion. Nothing was pushed.
