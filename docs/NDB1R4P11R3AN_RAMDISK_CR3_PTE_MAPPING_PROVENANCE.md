# NDB1R4P11R3AN RAM Disk CR3/PTE Mapping Provenance

**Phase:** NATIVE_RAMDISK_CR3_PTE_MAPPING_PROVENANCE
**Outcome:** C — NATIVE_RAMDISK_MAPPING_GUEST_PENDING
**Run status:** Diagnostic candidate prepared; no R3AN guest was started.
**Date:** 2026-11-05

## Repository identity and scope

| Repository | Starting identity | Result |
|---|---|---|
| SQL: D:\dev\guideXOS_SqlDatabase | main, 33f989e3525f130afc3740b8e2368dc836132064 | This report only; SQL production code unchanged |
| Server integration: D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH | codex/ndb1r4p11rah-sql-read-integration, 83e5438cd883cdd1158761f99b0aee1a3943452e | Diagnostic work committed locally as 5af7597497ddead12e63ccad87160ffaf5077ee5 |
| Original Server checkout | baseline f29850c93bbac0a0763a31e806d2816310156575 | Unchanged |

The isolated Server worktree was clean at its stated starting commit. Existing untracked output and evidence directories were preserved and not staged. The only staged Server files were the loader/kernel diagnostics, hosted PTE test and runner, and candidate packaging script. No reset, stash, rebase, clean, push, or merge was performed.

## Outcome and evidence boundary

R3AN adds bounded, gated page-table and byte-provenance diagnostics. Its ISO, ESP, kernel, loader, RAM disk, SQL fixture, and trusted fixture manifest were built and hash-checked. The R3AN guest did not run because QAC already had an owned acceptance guest for another Server workstream and another queued acceptance reservation. No R3AN reservation was requested, no guest was launched, and no other reservation was touched.

Therefore this phase has no R3AN runtime CR3 values, PTEs, translated physical addresses, allocation addresses for this candidate, or runtime byte samples. The R3AM guest observations below are historical context only; they are not attributed to R3AN. The R3AN candidate manifest records guest qualification as pending.

## Production memory contract

The relevant source is in the isolated Server worktree:

- guideXOSBootLoader/main.cpp allocates BootInfo, the RAM disk, the handoff stack, the trampoline, and a preallocated memory-map buffer. The RAM disk is allocated with AllocatePages(EfiLoaderData), filled by EFI_FILE_PROTOCOL::Read, and its allocation address and exact file length are stored as RamdiskBase and RamdiskSize.
- guideXOSBootLoader/elf.cpp::LoadElf reads kernel ELF program headers, allocates contiguous EfiLoaderData pages for the destination, zeros the image span, then streams each PT_LOAD file payload into its physical destination. It does not allocate a full contiguous copy of kernel.elf; its temporary program-header pool is freed after loading.
- guideXOSBootLoader/paging.cpp::BuildIdentityPageTables maps requested physical ranges at the same numerical virtual addresses using 4 KiB pages. The requested ranges include the RAM disk. main.cpp then calls MapRange(kernelMinVaddr, kernelPhysBase, kernelSpanBytes), which replaces mappings over the kernel virtual span. The builder's MapRange uses 4 KiB leaves; the hosted walker also supports 2 MiB and 1 GiB leaves.
- guideXOSBootLoader/handoff_trampoline.asm loads the supplied PML4 value into CR3 before changing stacks and jumping to the kernel entry. The kernel's first R3AN mapping observation runs from kernel_main before device initialization or later kernel CR3 changes.
- kernel/core/main.cpp reads RamdiskBase as a pointer. The loader records the UEFI allocation's physical address, so this implementation requires that numeric address to be mapped in the new kernel page tables. The BootInfo field is a plain uint64_t with no physical-versus-virtual contract comment. The code implies “physical address, identity-mapped at kernel entry,” but the ABI does not explicitly document that requirement.
- BootInfo itself is one EfiLoaderData page and is included in the identity-mapped ranges. The R3AN-only page-table sidecar is placed in the unused tail of that page; it does not change BootInfo size, checksum coverage, or field values.
- EfiLoaderData allocations survive ExitBootServices for the kernel to own. A source search found no kernel consumer of BootInfo.MemoryMap and no explicit physical-page reservation keyed by RamdiskBase. No later reclamation or overwrite was proven. This remains an ownership-contract gap to resolve if kernel memory management later begins reclaiming UEFI memory.

The address contract is thus implicit rather than absent in implementation: the kernel dereferences the physical number as a virtual pointer, and the loader intentionally identity-maps payload ranges before switching CR3. The interface should document this explicitly in a later ABI change.

## Allocation ranges and source-derived ELF intervals

Exact R3AN runtime allocation bases were not captured because the candidate was not booted. The following are exact allocation rules and, where possible, exact lengths; “base unknown” means the UEFI runtime result is unavailable.

| Region | Owner/type and lifetime | Source-derived physical range |
|---|---|---|
| RAM disk | UEFI AllocatePages, EfiLoaderData; passed to the kernel and not copied | R3AN length is 0x03B00000 (0x3B00 pages); base is runtime-selected and unknown for R3AN. If the R3AM-observed base 0x155C1000 recurs, its range is [0x155C1000, 0x190C1000). |
| Kernel ELF source | ESP file read through EFI_FILE_PROTOCOL; no whole-file RAM buffer allocated by LoadElf | No single loader-owned physical source range is exposed. Firmware file/cache buffers and the short-lived program-header pool are not address-logged. |
| Kernel destination allocation | UEFI AllocatePages, EfiLoaderData; retained for kernel lifetime | R3AN ELF span is 0x22AB5E80 bytes; AllocatePages rounds to 0x22AB6000 bytes (0x22AB6 pages). If physical base is K, allocation is [K, K+0x22AB6000). K is unknown for R3AN. |
| Kernel zero initialization | LoadElf SetMem through the physical allocation pointer before PT_LOAD writes | Exact interval [K, K+0x22AB5E80); the final 0x180 bytes of the page-rounded allocation are not part of this SetMem call. |
| PT_LOAD 0 | File-backed segment written by LoadElf | ELF VA [0x00100000, 0x0032703C); physical destination [K, K+0x0022703C); filesz and memsz are both 0x0022703C. |
| PT_LOAD 1 | File bytes followed by zero-initialized BSS | ELF VA [0x00328000, 0x22BB5E80); physical destination [K+0x00228000, K+0x22AB5E80); filesz 0x00012048, memsz 0x2288DE80. File bytes write [K+0x00228000, K+0x0023A048); the remainder was zeroed first. |
| Page tables | Each tracked page allocated as one EfiLoaderData page; retained through handoff | Each page is 0x1000 bytes; pages are not guaranteed contiguous. R3AN's loader tracks at most 512 pages. Runtime frame list/count is unknown without a guest log. The relevant table path is copied to the sidecar for safe MAP-E reads. |
| BootInfo | One EfiLoaderData page; identity-mapped and handed to kernel | 0x1000 bytes at runtime base B; B is unknown for R3AN. Historical R3AM base was 0x3DD3F000. |
| Handoff stack | 16 EfiLoaderData pages (0x10000 bytes); fixed preference 0x200000, then 0x100000, then an any-page fallback | Runtime choice is unknown for R3AN. The entire allocated stack range is identity-mapped. |
| Trampoline | GetTrampolineCodeSize returns 256 bytes; one EfiLoaderData page is allocated and identity-mapped | 0x1000 bytes at runtime base T; T is unknown for R3AN. |
| Preallocated memory-map buffer | 256 KiB / 64 EfiLoaderData pages; used across ExitBootServices and identity-mapped | 0x40000 bytes at runtime base M; M is unknown for R3AN. Temporary GetMemoryMap pools are freed before successful ExitBootServices. |

The RAM disk length is page-aligned, so its UEFI allocation length equals its reported payload length. Physical overlap must be checked between physical ranges; the fact that a physical base's number lies inside a virtual span is a separate virtual-address collision.

## Page-table and LoadElf audit

For the R3AN diagnostic ELF, readelf reports PT_LOAD virtual starts 0x00100000 and 0x00328000. The last segment ends at 0x22BB5E80, so LoadElf's exact span is [0x00100000, 0x22BB5E80), length 0x22AB5E80. MapRange rounds its final page boundary and maps virtual pages through end-exclusive 0x22BB6000. Its source-level translation is:

    PA = K + (VA - 0x00100000)

If, solely as a comparison scenario, the R3AM-observed RAM disk base 0x155C1000 and kernel physical base 0x190C1000 recur, the same numeric VA is in the kernel virtual span and the formula yields PA 0x2E582000. That frame is inside the kernel destination and well past the second segment's file-backed bytes, in its zero-initialized BSS. Under those same historical values, the physical RAM disk range ends exactly at the kernel physical allocation start, so the two physical allocations are adjacent and do not overlap. Neither the recurrence nor the actual R3AN PTE is established.

This recalculation corrects the R3AM report's source-derived arithmetic, which stated PA 0x2E671000. The inputs in that report produce 0x2E582000. The R3AM runtime byte observations remain unchanged; this correction affects only the inferred MapRange target.

MAP-A is sampled after the loader builds identity tables and before MapRange. MAP-B is sampled immediately after MapRange. MAP-C is explicitly unavailable: LoadElf completes before the RAM disk is allocated, so there are no RAM disk bytes to sample at that point; the firmware page tables are also outside the loader's tracked safe-reader set. MAP-D walks the loader's own page tables before the trampoline switch and captures the original physical allocation's first 16 bytes through the still-active UEFI pointer. MAP-E reads active CR3 at the earliest kernel entry and walks only page-table frames listed by the loader sidecar. If the sidecar is invalid or its root differs from active CR3, MAP-E reports the reader unavailable rather than dereferencing arbitrary physical memory.

The walker reports present/non-present/invalid/unavailable status, leaf PTE, effective writable/user/NX permissions, leaf level/page size, translated physical frame, intended backing comparison, and overlap with the kernel physical destination. Sampled virtual addresses are RAM disk base, base + 4 KiB, and the last page. Each stage is limited to three addresses, with five stage slots. The kernel cannot safely read the original physical bytes when its active VA no longer aliases that allocation; MAP-E reports those bytes unavailable instead of using an unrestricted physical-memory read.

## Byte provenance and confirmed divergence

The pinned R3AI RAM disk is 61,865,984 bytes with SHA-256 22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78. Its reference first 16 bytes are EB 58 90 47 55 49 44 45 58 4F 53 00 02 10 20 00; bytes 0x0B–0x0C are 00 02, or 512 bytes per sector.

| Observation | R3AN result | Historical R3AM result |
|---|---|---|
| Offline RAM disk image | Hash and first 16 bytes verified | Same hash and reference bytes |
| UEFI file read | Not observed; no guest | Base 0x155C1000, full 0x03B00000 read, reference bytes |
| MAP-A / pre-MapRange | Not observed; candidate log is prepared | Not sampled |
| MAP-B / post-MapRange | Not observed; candidate log is prepared | Not sampled |
| MAP-C / post-LoadElf | Unavailable by source ordering; RAM disk allocation occurs later | Not sampled |
| MAP-D / final loader handoff | Not observed; candidate log is prepared | Correct bytes at base 0x155C1000 |
| MAP-E / active kernel CR3 | Not observed; candidate log is prepared | First kernel dereference read zeros at unchanged BootInfo base |
| Original physical backing after CR3 switch | Not observed | Not independently sampled |

R3AM therefore established a byte mismatch boundary between the correct UEFI pre-handoff sample and the first kernel dereference, but did not prove its active PTE or the post-switch state of the original physical backing. R3AN adds the missing observation mechanism but has no new runtime divergence. No R3AN virtual alias, physical overwrite, BootInfo interpretation failure, or lifetime violation is classified as proven.

## Hosted qualification

The user-requested hosted checks completed:

| Suite | Result |
|---|---:|
| New x86-64 PTE provenance suite | 34/34 |
| R3AM RAM disk provenance regression | 34/34 |
| Current R3AK mount regression source | 73/73 |
| R3AI production VFS diagnostic | 22/22 |
| Retained R3AL hosted baseline | 73/73 |
| Retained earlier R3AK baseline | 60/60 |
| Earlier unchanged SQL Debug CTest baseline | 39/39; not rerun because SQL production sources did not change |

The new controlled tests cover 4 KiB, 2 MiB, and 1 GiB translation leaves; non-present/invalid/unavailable observations; wrong CR3; BootInfo physical-pointer mismatch; overflow and overlap; mapping collision versus changed physical bytes; and bounded output. The collision model keeps region A's reference bytes intact while the shared virtual address resolves to zero-filled region B, and distinguishes that from a case where A itself changes.

## Builds and candidate identity

Ordinary and gated AMD64 kernels and UEFI loaders built successfully in isolated output directories. The ordinary kernel SHA-256 is the same as the R3AM ordinary kernel: c5788717feabaede109c697105815a5b7af92873328c85c988c0576d6b1b7f14. The ordinary loader hash is 5b7897af2c4ba4fe0606ac2127ad52c2a5c70c5a890012a7b7b3778290c2d8b2. Marker inspection found no R3AN mapping-trace strings in ordinary artifacts; gated diagnostic kernel and loader contain their R3AN markers.

| Artifact | Size | SHA-256 |
|---|---:|---|
| Diagnostic kernel ELF | 2,757,608 | 7c8ce60f88e3a5eb32297ef61d5d3a03f96ad3cd594d7b07a4c182d79d876c17 |
| Diagnostic kernel PE | — | cf1193728c04fd45b5302568094cba9a5a233eafbb5f2eb36d74694617a55573 |
| Diagnostic UEFI loader | 73,216 | 42cdba007939dad5c3dc8f0d328c3bcf5b817361c22a34605ae9d8e7b64fec7f |
| Embedded RAM disk | 61,865,984 | 22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78 |
| SQL fixture / trusted manifest | 250,880 / 12,937 | cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57 / 43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3 |
| R3AN ESP image | 100,663,296 | 290b5c4bdaf5adb95c299ebfdce1289bb6a783c6073def42aed24b8c6647fca6 |
| R3AN ISO | 100,730,880 | 95c74d7d36deaad08fd712961cad42c283d5746e78b9bd37d552b439c5a0183a |

The ISO verifier reports a bootable UEFI El Torito image (platform 0xEF), boot image LBA 32. The ESP verifier confirms the diagnostic loader, kernel ELF, and RAM disk hashes above. The candidate is at out/ndb1r4p11r3an/boot-candidate/guideXOS-NDB1R4P11R3AN-amd64.iso in the Server integration worktree. No guest evidence or SQL acceptance is implied by successful packaging.

## QAC admission, SQL frontier, and cleanup

The read-only QAC census showed QAC v2.0.0 online with one managed QEMU acceptance process, PID 5924, owned by active reservation 2755eece88ce4fd2a1f7cb85463f831e for project guideXOSServer; the verified wrapper PID was 10048. A separate acceptance request, reservation be8b4c6fb57d49c2a901ebb8dee27b89 for guideXOSServer-AARCH64-ABI3-Studio, was queued. R3AN did not request admission, poll for availability, launch a QEMU process, or disturb either workstream.

Because the candidate never booted, no SQL guest marker was emitted. The fixture was not opened, IX_Rows_K64 point lookup (root 234, owner 1, 73 ordered locators) and IX_Rows_K32 range scan (root 148, owner 2, 32 ordered locators) did not run, and native SQL acceptance was not reached. No R3AN QMP or serial log exists. There was no R3AN-owned guest to shut down or reservation to release; the other QAC guest remains owned by its workstream. The prepared ISO hash is unchanged from packaging verification.

## Remaining uncertainty and next correction

Outcome C is required because guest authorization/admission was unavailable. The single smallest missing evidence set is one admitted boot of the exact candidate with MAP-A, MAP-B, MAP-D, and MAP-E PTE output plus first-16-byte samples at UEFI load, MAP-A, MAP-B, MAP-D, and kernel entry. It will establish whether the kernel's active translation changes and whether original physical bytes remain correct across CR3 activation.

No repair is justified from R3AN because no R3AN mapping defect has been observed. If an admitted run confirms that the RAM disk's numerical VA is consumed by the kernel image mapping and resolves into zero-initialized kernel backing, the next phase should choose a noncolliding kernel virtual address for the RAM disk and make that address contract explicit in BootInfo (or relocate the conflicting kernel virtual span). Simply restoring the identity PTE at the current address may collide with the kernel's linked BSS range, so the mapping correction must preserve both kernel BSS and RAM disk access. If MAP-E instead shows the intended translation, compare backing-byte and ownership evidence before proposing any overwrite or lifetime repair.

R3AN made no mapping, zeroing, FAT32, RAM-disk ownership, SQL, or writable-storage change.
