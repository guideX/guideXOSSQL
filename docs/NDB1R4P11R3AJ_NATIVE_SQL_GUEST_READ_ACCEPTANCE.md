# NDB1R4P11R3AJ — Native SQL Guest Read Acceptance

## Outcome

**Outcome C — `NATIVE_SQL_GUEST_PENDING_OR_BLOCKED` (`QAC_GUEST_PENDING`).** The exact R3AI candidate and fixture passed integrity preflight, and the SQL diagnostic's two expected locator arrays match the trusted candidate manifest. Guest execution did not occur because another workstream held QAC's exclusive acceptance reservation when the final pre-launch check ran. No R3AJ reservation was requested, no QEMU process was launched for this phase, and no guest result is claimed.

The user explicitly requested the R3AJ guest acceptance in this task. QAC admission was still mandatory. The coordinator showed an empty lane in the first snapshot, then a verified active acceptance reservation in the second snapshot. The R3AJ runner failed closed on that second snapshot before invoking `qac run`.

## Candidate and source identity

| Item | Verified identity |
|---|---|
| SQL checkout | `main`, HEAD `aa5e0f1958f7847fd2bc121c59f13f801ee4ecda` |
| Server integration checkout | `codex/ndb1r4p11rah-sql-read-integration`, HEAD `3e102c3bce7e1a3ecef8634a6fa640b05e72ec10` |
| Candidate manifest build provenance | Server `3e102c3bce7e1a3ecef8634a6fa640b05e72ec10`; SQL build source `80a594e4a30b652a2d207ac60d8100b214885c4a` |
| ISO | `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3ai\boot-candidate-final-v4\guideXOS-NDB1R4P11R3AI-amd64.iso` |
| ISO size / SHA-256 | `100730880` bytes / `f22e3f3627b2ce6541bf9499934a4d726135574b9b654aa12a20351ed860d497` — exact match |
| Candidate manifest SHA-256 | `f8dea45ea84bf6b117fb9d0a761499b1e3e2ff0e27de4cbadef896d18a7ff835` |
| ISO verification report | Present; UEFI El Torito platform `0xEF`, bootable, no-emulation; ESP hash matches the candidate manifest |

The current SQL HEAD includes the R3AI report receipt, while the candidate manifest correctly records the earlier SQL production-source commit used to build the candidate. SQL and Server tracked source diffs were clean before this report was added. No build, repackage, or candidate substitution was performed.

## Fixture provenance

The candidate's trusted manifest and staged fixture agree with the required controlled fixture:

- Fixture: `MULTI.GDB`, 250,880 bytes, SHA-256 `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57`.
- Trusted `FIXTURE.MAN`: 12,937 bytes, SHA-256 `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3`.
- Format 1.0; page size 512; page count 490; catalog root 1; UUID `32b46622-aa73-493d-a26d-e826de2bb790`.
- `IX_Rows_K64`: root 234, owner 1, 73 expected point locators.
- `IX_Rows_K32`: root 148, owner 2, 32 expected range locators.
- Packaged read-only RAM-disk image: 61,865,984 bytes, SHA-256 `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78`.

The two ordered locator arrays in `native_sql_read_diagnostic.cpp` were compared element-by-element with the manifest arrays: all 73 point locators and all 32 range locators match in order. This is source/manifest contract evidence only; no guest traversal ran and no runtime locator equality is claimed.

## QAC admission

The applicable QAC 2.0 policy requires acceptance reservations to be exclusive, blocks admission for a detectable foreign QEMU or incomplete census, and requires QEMU to be a direct `qac run` child. The active coordinator was online and its read-only `status`, `list`, and `queue` commands were used for both snapshots.

| Snapshot | QAC state |
|---|---|
| Initial read-only snapshot, 2026-11-01 22:30:56 UTC | 0 QEMU processes; 0 managed sessions; 0 queued; 0 blocked |
| Final pre-launch snapshot, 2026-11-01 22:34:54 UTC and follow-up at 22:35:30 UTC | 1 managed QEMU; 1 running acceptance reservation; 0 queued; 0 blocked; 0 foreign QEMU |

The active reservation was `21bf0822b1b34a43831659693e04c65b`, project `guideXOSServer`, mode `acceptance`, state `Running`, owner verified, QEMU PID `30576`. It was granted at `2026-11-01T22:32:43.0357676Z`. This reservation belongs to another workstream. R3AJ did not queue behind it, reuse its QMP port, inspect or alter its guest, release its reservation, or signal its QEMU process.

The runner's second status check detected the active session and exited before the `qac run` command. Therefore R3AJ has no reservation ID and no QAC child PID. The QEMU binary was available at `C:\Program Files\qemu\qemu-system-x86_64.exe` (QEMU 11.0.0, SHA-256 `a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02`). Firmware `OVMF.fd` was present (SHA-256 `2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792`). Neither was used to start a guest.

**Admission result: unavailable for this phase.** No reservation was created by R3AJ, so there is no R3AJ reservation to release. The QAC post-check still showed only the other workstream's reservation; this report does not claim QAC is globally idle or clean.

## Guest and SQL evidence

No guest was launched. Consequently, the following gates remain unobserved:

| Gate | R3AJ result |
|---|---|
| UEFI loader, kernel entry, and successful kernel boot | Not attempted |
| RAM-disk attachment and production FAT32 `/system` mount | Not observed |
| Production VFS fixture open and database header validation | Not observed |
| Native SQL provider page reads | Not observed |
| Point lookup on `IX_Rows_K64` | Not executed |
| Range scan on `IX_Rows_K32` | Not executed |
| Runtime ordered locator equality | Not qualified |
| SQL serial marker order or failure scan | No guest serial trace |
| QMP observations and guest exit status | No guest process |
| Guest shutdown and R3AJ QAC release | Not applicable; no R3AJ reservation or guest existed |

The exact successful SQL marker contract remains the five strings in the candidate manifest and R3AI report:

```text
SQL_NATIVE_R3AI BEGIN fixture=NDB1R4P11R3AH_MULTILEVEL
SQL_NATIVE_R3AI FIXTURE_OK length=250880 page_size=512 page_count=490 catalog_root=1
SQL_NATIVE_R3AI POINT_PASS index=IX_Rows_K64 root=234 owner=1 count=73
SQL_NATIVE_R3AI RANGE_PASS index=IX_Rows_K32 root=148 owner=2 lower=1 upper=2 count=32
SQL_NATIVE_R3AI END PASS
```

R3AI's source contract ties `FIXTURE_OK` to provider open and header/metadata checks; `POINT_PASS` and `RANGE_PASS` are emitted only after production lookup/scan completion, exact full-array comparison, and provider close; `END PASS` follows both successful cleanups. The boot-RAM-disk `/system` mount log is emitted separately. These source-level conditions describe what a future trace must prove; they are not R3AJ runtime evidence.

The prior R3AI report remains the qualification for the hosted production-VFS path (22/22 checks) and SQL Debug CTest baseline (39/39). It did not run a guest. Neither result is substituted for this pending guest acceptance.

## Evidence and integrity

Phase evidence is isolated under:

`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3aj-guest-acceptance-20261101`

It contains the candidate/fixture/QEMU/firmware hash inventory, the initial and blocking QAC snapshots, the active reservation inspection, the fail-closed runner error, and the run disposition. No serial or QMP log exists because no guest was started.

The final preflight recheck found:

- SQL HEAD and Server integration HEAD still match the phase identities above; tracked production sources have no diff.
- SQL `.native-sql.phase` and `.phase`, and Server `.native-db.phase`, retain their prior contents.
- The original Server checkout remains at `f29850c93bbac0a0763a31e806d2816310156575` with a clean status. Its protected `main.o` and `main.d` are absent and were not touched.
- The ISO, candidate manifest, ISO report, fixture, trusted manifest, and ram-disk hashes match the recorded values. The candidate and fixture output trees were not written by this phase.
- No SQL algorithm, Server/VFS/FAT32 source, database format, fixture, or phase marker was changed.

The R3AJ runner opened the ISO read-only for its final integrity check, observed the QAC blocker, and exited before requesting admission. It created no guest process or QAC reservation. No shutdown or cleanup action was taken against the other workstream.

## Limitations and next milestone

This outcome establishes candidate integrity and a QAC admission blocker only. It does not establish kernel boot, native VFS access, provider page reads, SQL point/range execution, or any guest error-free condition. The fixed diagnostic remains limited to the caller-attested read-only RAM-disk fixture; it does not provide general native catalog discovery, writes, recovery, concurrent-writer safety, or an in-kernel whole-file cryptographic hash.

The next milestone is to repeat this same R3AJ acceptance against the exact verified R3AI ISO after reservation `21bf0822b1b34a43831659693e04c65b` is released. Recheck QAC ownership and candidate hashes immediately before requesting the exclusive acceptance reservation. Do not substitute or rebuild the candidate.
