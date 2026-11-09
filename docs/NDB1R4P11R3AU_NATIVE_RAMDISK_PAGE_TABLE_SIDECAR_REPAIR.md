# NDB1R4P11R3AU — Native RAM-Disk Page-Table Sidecar Coverage Repair

**Outcome A — `NATIVE_RAMDISK_SIDECAR_REPAIR_HOSTED_VERIFIED`**

The loader now publishes the page-table frames needed to inspect every 2 MiB region in the RAM-disk alias. Hosted tests cover the full 15,104-page extent and the first 2 MiB transition, memory protections remain mandatory, and the candidate ISO is independently verified. R3AU did not launch a guest or request QAC admission. Runtime acceptance remains for R3AV.

## Repositories and preservation

| Repository | Starting identity | R3AU result |
| --- | --- | --- |
| `D:\dev\guideXOS_SqlDatabase` | `main`, `6b58cc154251bfdf290c74d2e6239ed5e1d62212`, tracked tree clean | `main`, this report and the R3AV procedure committed locally; final containing commit is reported in the closeout |
| `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH` | `codex/ndb1r4p11rah-sql-read-integration`, `12df41bdc29d6608055d0dbbe16d997c661a219d` | `e1c8c4a71c1a01fec7d7a6ddce095fd5c4986082` |

Server commit `0dff95ea05e956cff0527e18b1dd1500cd487f7c` contains the loader/kernel repair, sidecar tests, QMP evidence parser base, and candidate packager. The candidate manifest records this commit as its image source commit. Commit `cea4b50649ca79a03e9b3ef34173ae63719821fc` adds the host-only QMP page-table walker and its hosted tests. Commit `c3ca0c6941e285c5a9df28b0bca7772352b9b2bd` requires an observed CR4 and rejects LA57 before using the four-level QMP walker. Final Server commit `e1c8c4a71c1a01fec7d7a6ddce095fd5c4986082` also checks MAP-E CR0.WP/read-only contract agreement and rejects reserved paging-entry address bits. These later commits affect only host-side evidence tools and do not change the already verified candidate binaries.

The unrelated `D:\dev\guideXOSServer` checkout was not modified. Existing generated output directories and historical candidates were left intact. R3AU build and hosted-test outputs use new phase-specific paths. No reset, rebase, stash, clean, push, or merge was used.

## R3AT evidence and failure diagnosis

The preserved R3AT ISO remains `100730880` bytes with SHA-256 `1388b65415bd7501e4bf3a3dd00d90d2799ea5ef822d46d18b600a82789050f9`.

The serial record at `E:\CodexArtifacts\NDB1R4P11R3AT\serial.log` has SHA-256 `8fc046983ece36a9a032b8bc94ee6f101625d2eb7e4ecdee879abffb82f9fa11`. It reports:

- MAP-E active CR3 `0x3DD3C000`, alias `0xFFFF900000000000`, backing `0x155BE000`, length `0x03B00000`, and `CR0.WP=1`.
- The selected base, base-plus-4-KiB, and final-page PTEs were present, supervisor-only, read-only, and translated to the expected backing frames.
- The loader sidecar reported `count=5`, `capacity=0x1B2`.
- The complete-extent checker reached `pages_checked=0x201` (513 of 15,104), then failed at `0xFFFF900000200000` with `PAGE_TABLE_READER_UNAVAILABLE`. Package validation and dependent SQL operations were skipped, and the overall R3AS marker was FAIL.

Source inspection confirms that the old sidecar gathered paths only for three virtual addresses: alias base, base plus 4 KiB, and final page. With the 4 KiB leaf tables built by `MapBootPayloadRange`, those samples deduplicated to the PML4, PDPT, PD, and two PT frames: the first 2 MiB's PT and the final region's PT. The walk at the first 2 MiB boundary needs a different PT frame for PD index 1. It was not in the five-frame sidecar allowlist. This explains the reader-unavailable result; it does not prove the runtime PTE was absent or the RAM-disk mapping invalid.

R3AT did not record the physical identity of the unavailable frame. The source and failing address identify the missing registration as the PT path for the next 2 MiB region, but the exact runtime frame address cannot be recovered from R3AT. The new loader emits the failing VA, table physical frame, index, and collection status if frame registration itself fails in a future run.

The preserved QMP JSONL transcript is 2,518 bytes, SHA-256 `b1bd1609af7439a8f7a6cf5af3d7a96283cf3441cd0710e682f86531858048ae`. It contains a QMP greeting, successful capability negotiation, two correlated running-status replies, and a controlled `quit`/SHUTDOWN event. It contains no `human-monitor-command` or `xp` request. The separate `runtime-page-table-walk.json` records `KeyError('cr3_active')`, before the prior helper issued `info registers`; no live page-table observations were lost because none had yet been requested.

## Sidecar repair and safety invariants

The loader allocates PML4, PDPT, PD, and PT pages through its tracked `EfiLoaderData` page-table allocator. It identity maps the tracked table pages before activating the final CR3. The new range collector walks the actual loader-owned tables at one address in every intersecting 2 MiB PDE region, records only frames read through the existing tracked-frame reader, and deduplicates shared upper-level frames.

For the R3AR range, `0x03B00000 / 0x1000 = 15,104` pages span 30 2 MiB regions. With the pinned layout, hosted fixtures confirm 30 distinct leaf PTs plus the shared PML4, PDPT, and PD: 33 unique frames. The collector is capped at 4,096 range samples, checks canonical addresses and end arithmetic, and returns a failure instead of dropping records when capacity is exhausted.

The sidecar is now version 2. Its frame list has an FNV-1a 64 checksum. The added checksum field reduces the BootInfo-tail array capacity from 434 to 433 frames; the pinned workload requires 33. Before walking, the kernel validates the sidecar header/version, storage bounds, count/capacity, active CR3 root, frame alignment and physical address bits, duplicate absence, root registration, and checksum. The sidecar remains in the unused tail of the existing BootInfo page; the BootInfo v3 layout and checksum range are unchanged. Page-table frames retain their loader allocation lifetime, and the kernel dereferences only identity-mapped, allowlisted frames.

The kernel still validates every 4 KiB page against the current BootInfo physical backing, checks present status, frame progression, effective supervisor-only/read-only permissions, mapping level/size, range end, and kernel physical overlap. `CR0.WP` remains required. Failure remains closed: mapping failure is reported precisely, package and SQL work is skipped, no alias dereference follows a failed gate, and the overall R3AU marker remains FAIL. The repair does not add writable RAM-disk aliases or alter the SQL service, FAT driver, database format, or fixture.

## QMP evidence parser repair

The preserved R3AT MAP-E value uses fixed-width hexadecimal fields such as `cr3_active=000000003DD3C000`. The old `kv()` regular expression matched letters and underscores in field names but omitted digits, so `cr3_active` was not captured and the subsequent lookup raised `KeyError` before QMP `info registers` or page-table reads.

The R3AU-owned `scripts/r3au_qmp_evidence.py` accepts field names containing digits, parses fixed-width 64-bit values without losing low control bits, rejects reserved/out-of-range/zero-root CR3 values, correlates QMP response IDs, requires a running-status response, and checks the same verified QAC reservation/owner/managed-QEMU relationship before allowing capture. MAP-E parsing checks that `cr0_wp` agrees with CR0.WP and that the alias is declared read-only. Its `walk_qmp_page_tables` adapter requires the observed CR4 value, rejects `CR4.LA57`, issues correlated `xp /1gx` reads through a caller-supplied QMP exchange function, and records the replies. It walks four-level tables, supports 4 KiB, 2 MiB, and 1 GiB leaves, reports effective permissions, and fails on unavailable entries, noncanonical addresses, or invalid/reserved paging bits. The preserved R3AT helper and QAC infrastructure were not modified.

R3AT's `qac-admission.json` passes the new ownership predicate. This is a parser regression only: no active guest was contacted, no historical CR3 was substituted, and no page-table walk was fabricated from R3AT.

## Fresh hosted regressions

| Suite | Result | Notes |
| --- | ---: | --- |
| R3AU page-table sidecar | 28/28 | 15,104-page production-shaped table fixture; every 2 MiB table transition; final page/end-exclusive boundary; capacity, ownership, duplicate, alignment, checksum, stale-root, missing-frame, and malformed-range negatives |
| R3AU QMP evidence/parser | 25/25 | Preserved R3AT serial/QMP/QAC inputs plus synthetic correlated QMP page-table reads across `+0x1FE000`, `+0x1FF000`, `+0x200000`, `+0x201000`, and `+0x202000`; no guest connection |
| R3AS full mapping/package diagnostic | 31/31 | Production mapping validator, full R3AR extent, read-only/supervisor/CR0.WP, endpoint and fail-closed gates |
| R3AO boot-payload mapping | 36/36 | Production `paging.cpp` and BootInfo v3 alias/stack mapping tests |
| R3AN PTE provenance | 34/34 | Four-level walk, large pages, reader-unavailable and range checks |
| R3AM byte provenance | 34/34 | Production block, RAM-disk, and FAT32 byte-flow checks |
| R3AR production FAT32 VFS | 17/17 | R3AR RAM disk, trusted fixture/manifest, and read-only SQL path |
| R3AQ SQL service | 50/50 | Production read-only VFS service, point/range, error recovery and output invariants |
| R3AI VFS | 22/22 | Production VFS with fixture and missing-fixture controls |
| R3AK FAT/VFS mount regression | 73/73 | Original R3AI geometry candidate and matching missing-fixture image |

Development iterations are recorded here rather than hidden. The first sidecar test run exposed a 32-bit `~0xFFFu` mask that truncated high canonical addresses; it was corrected to `~0xFFFULL`, then the version-2 final suite passed 28/28. The first expanded QMP parser run had four failing cases because `parse_map_e_cr3` was routed through a full-record parser; the functions were separated and the final suite passed 25/25. The first R3AK invocation mistakenly used the newer R3AR image against a regression expecting R3AI's 7,518-cluster geometry (71/73); rerunning with the historical R3AI candidate and its matching missing-fixture image passed 73/73. The first parallel qualification-kernel `make -j4` invocation returned nonzero; the same fresh phase build was resumed serially without cleaning and compiled/linked successfully. No test expectation was weakened.

The historical SQL Debug CTest 39/39 result was not rerun: no SQL production source changed.

## Build and link qualification

Both AMD64 kernel profiles and both Release UEFI loader profiles were built in new R3AU-specific output directories. The kernel used the freestanding C++14/`-O2` settings, `-Wall -Wextra`, no exceptions/RTTI, no standard C++ headers, no builtins, and the existing linker script and PE-to-ELF conversion.

| Profile | Kernel outputs | UEFI loader | Result |
| --- | --- | --- | --- |
| Ordinary | `kernel/build/amd64-r3au-normal/bin/kernel.pe` 2,774,807 bytes, SHA-256 `972f29290c5f8a10b0689690e71037c96fdc245da35a66b8c1d7f48cb8821121`; `kernel.elf` 2,670,368 bytes, SHA-256 `43db7160b3620107a9d5af8de68c2f154f20e20dcec685e3919242f809214561` | `guideXOSBootLoader/out/r3au-normal/guideXOSBootLoader.exe` 65,536 bytes, SHA-256 `49d2f7256de218efcc345037df62a582df4a74e2cf4687a5f0a4c47b92437499` | Pass |
| R3AU qualification | `kernel/build/amd64-r3au-qualification/bin/kernel.pe` 2,878,679 bytes, SHA-256 `a359fbde37f902760e8ea343fde28e9d2c463df70b5e0ce98dcd2e9ea1c3f1e2`; `kernel.elf` 2,764,480 bytes, SHA-256 `f58a277b6021b1ac89054607beed72f09d083e12e5ef43949e7274c032605ae3` | `guideXOSBootLoader/out/r3au-qualification/guideXOSBootLoader.exe` 83,968 bytes, SHA-256 `f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77` | Pass |

Ordinary build flags leave SQL qualification/diagnostic tracing disabled; the ordinary kernel contains no R3AQ/R3AU SQL qualification marker strings. All four PE outputs have a zero-sized import directory. `nm -u` identity sets match the R3AS baselines exactly: 21 unique undefined identities for ordinary and 22 for qualification, with no new unresolved identity.

## Frozen candidate and preserved inputs

The immutable candidate is in `E:\CodexArtifacts\NDB1R4P11R3AU\candidate`.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `guideXOS-NDB1R4P11R3AU-amd64.iso` | 100,730,880 | `749f909c38d20c9c6e61426ceec85bf397fea7112f8c2ec36aca22b45273194e` |
| `EFI-BOOT.IMG` | 100,663,296 | `b408175820b5190be63c1c7584d72c188c81953af52c5d94beb82400f9a0ce57` |
| `BOOTX64.EFI` | 83,968 | `f68c570adb3c71ae21b99756baa868ae88be0bb00478c33c58466c1ec0af4f77` |
| Qualification `kernel.pe` | 2,878,679 | `a359fbde37f902760e8ea343fde28e9d2c463df70b5e0ce98dcd2e9ea1c3f1e2` |
| Qualification `kernel.elf` | 2,764,480 | `f58a277b6021b1ac89054607beed72f09d083e12e5ef43949e7274c032605ae3` |
| R3AR FAT32 `ramdisk.img` | 61,865,984 | `50ce34332c34054633814a7fdeaef6fad5ee28eae3cf7c3a96cc2911abe4440c` |
| Trusted SQL fixture | 250,880 | `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted SQL manifest | 12,937 | `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| R3AU diagnostic expectation report | 1,762 | `09c2c50fbb12036404c566529dccb9ac3369c71ec14d8ac9ac5ff8b91aa5585d` |
| Candidate manifest | 12,818 | `4c326831441e37fd5da86f903be2ac6625197099ecb0a39f31d5059e5bcd976e` |

The candidate manifest also records the expectation header (3,895 bytes), R3AR package manifest (4,713 bytes), packaging provenance, independent FAT32 validation report, ESP/ISO reports, and source commit. The ISO verifier independently reports a bootable UEFI El Torito entry (platform `0xEF`, no-emulation) containing the verified ESP hash. The ESP verifier reports FAT32 and confirms the embedded loader, kernel, and RAM-disk hashes. The package generator verified the complete 512-byte boot-sector expectation without changing its header.

The accepted R3AR RAM disk, SQL fixture, trusted manifest, and provenance reports match their reviewed hashes. The old R3AS ISO and R3AT serial/QMP evidence remain unchanged. The candidate is prepared for R3AV; its successful guest boot, live active CR3, runtime frame identities, QMP PTE observations, full guest mapping PASS, FAT mount, SQL results, and QAC release remain unverified until that phase.

The separate 20-gate R3AV procedure is [NDB1R4P11R3AV_NATIVE_RAMDISK_GUEST_ACCEPTANCE.md](NDB1R4P11R3AV_NATIVE_RAMDISK_GUEST_ACCEPTANCE.md).

## Final commits

- Server integration worktree: `e1c8c4a71c1a01fec7d7a6ddce095fd5c4986082`.
- SQL repository: the report and R3AV procedure were first committed together on `main` as `0a935ab040993eed0c928e29d184a3466a613f42`. A follow-up documentation correction and the final SQL `HEAD` are reported in the closeout.

