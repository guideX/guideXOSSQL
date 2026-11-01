# NDB1R4P11R3AJ-R1 — Native SQL Guest Read Acceptance Retry

## Outcome

**Outcome B — `NATIVE_SQL_GUEST_PARTIAL_OR_FAILED`.** The exact authorized ISO booted under an exclusive QAC acceptance reservation and entered the kernel. The boot RAM disk attached, but its production FAT32 mount failed. The SQL diagnostic stopped with `SQL_NATIVE_R3AI FAIL code=VFS_UNAVAILABLE_OR_WRITABLE` before opening the fixture, so neither SQL operation ran and no native SQL acceptance is claimed.

The owned guest was shut down through QMP. QAC verified the child exit and released this run's reservation. The post-run census showed no QEMU process, reservation, or queued run.

## Source and candidate identity

| Item | Verified identity |
|---|---|
| SQL checkout before this report | `main`, `3b9a186238aa6de73ee2af93d9d6840f3e3ffded` |
| SQL commit used to build the candidate | `80a594e4a30b652a2d207ac60d8100b214885c4a` |
| Server integration checkout | `codex/ndb1r4p11rah-sql-read-integration`, `3e102c3bce7e1a3ecef8634a6fa640b05e72ec10` |
| ISO | `100730880` bytes, SHA-256 `f22e3f3627b2ce6541bf9499934a4d726135574b9b654aa12a20351ed860d497` |
| Trusted R3AI candidate manifest | SHA-256 `f8dea45ea84bf6b117fb9d0a761499b1e3e2ff0e27de4cbadef896d18a7ff835` |
| ISO verification report | SHA-256 `8c68e6e1543db925a70a1692330e7c4b7f2d398f51f8be20a61611db7e24a40a`; UEFI El Torito platform `0xEF`, bootable, no-emulation |
| ESP image | SHA-256 `3670d859e3b386039bcc72be06a2fa2e64c59cf77ace3e326f3add8041289e0f` |
| Kernel ELF | SHA-256 `bfe3360b0d6437858e06381b0713be6c400a28c9f4f2b1bf6249b1510c6ac8fb` |
| Embedded RAM disk | `61,865,984` bytes, SHA-256 `22d1aa794339a85cca30adc5f63edbca20d9dd91840a87cc12696daf5a6d6f78` |
| UEFI bootloader | SHA-256 `002ce8def31974997f10179e6139b08e6985816eef31b641411bded60ce59b1e` |
| Fixture `MULTI.GDB` | `250,880` bytes, SHA-256 `cf100504a5cbe5eaeebaa483d3b9265e3042860cb8363e383819f5d1e88d3c57` |
| Trusted `FIXTURE.MAN` | `12,937` bytes, SHA-256 `43687cbd0087e3ff8747605a1f3510dd4ee3eff780fe8a7e1ef5b45e7f90a8d3` |
| QEMU / OVMF | QEMU 11.0.0, SHA-256 `a930e028f93d0fa47e4d58bdad2432f7466dc2b6af0ae376f77ef7a298ffdd02`; OVMF SHA-256 `2df617a6fd1bae41ec7c0e3bceca96c928a5a88d8dc198468d34733393ab2792` |

The candidate manifest, ESP report, ISO report, embedded kernel and RAM-disk payloads, fixture, and trusted fixture manifest all matched their pinned identities before launch. The same artifact hashes matched after shutdown. The manifest records the fixture UUID `32b46622-aa73-493d-a26d-e826de2bb790`, format 1.0, page size 512, page count 490, and catalog root 1.

SQL's only changes after the candidate build commit are the R3AI and R3AJ documentation receipts. SQL and Server tracked working trees were clean before and after the guest run. No production source, phase marker, fixture, ISO, or candidate output was modified.

## QAC admission and guest configuration

The current QAC 2.0 rules were read from `D:\dev\guideXOS_QAC\README.md` and the reservation implementation. Acceptance reservations are exclusive; a foreign QEMU or incomplete census blocks admission; and QEMU must be a direct child of `qac run`. The user's pasted task explicitly authorized this acceptance.

The pre-run QAC census was online and empty: zero QEMU processes, zero reservations, zero queued or blocked runs. QAC granted this run reservation `4eacb03de9c84b659aea27091aabe185` in `acceptance` mode at `2026-11-01T23:46:32.7912268Z`. QAC recorded direct QEMU child PID `19836`, owned by the `qac run` process PID `29628`; the process command line names this exact ISO and this R1 evidence directory's serial and QMP paths.

The guest used `pc,usb=off`, 1024 MiB RAM, read-only OVMF firmware, the exact ISO as a read-only CD-ROM, serial output to `serial.log`, QMP on `127.0.0.1:62829`, no display, and `-no-reboot`. The complete arguments are preserved in `launch-config.json`.

## Runtime evidence and execution frontier

| Stage | Result | Evidence |
|---|---|---|
| UEFI boot and kernel entry | Observed | UEFI DVD boot entry, guideXOS loader, kernel load, and `[BOOT] Kernel entry` appear in the serial trace. |
| RAM disk attachment | Observed | Bootloader reported the expected 61,865,984-byte image; kernel attached it read-only as `wallimg`. |
| Production FAT32 mount | **Failed** | Serial reports `[VFS] ERROR: Filesystem mount failed` and `[desktop] failed to mount boot runtime image at /`. No `/system` FAT32 mount marker follows. |
| Production VFS fixture access | Not reached | `immutableFixtureMountReady()` rejected the missing active, read-only FAT32 mount before provider open. |
| Fixture open and metadata validation | Not reached | `SQL_NATIVE_R3AI FIXTURE_OK` is absent. |
| Root and owner validation | Not reached | Diagnostic stopped before `selectedRootMatches()` for either index. |
| Point lookup, `IX_Rows_K64` | Not executed | No `POINT_PASS`; no runtime locator sequence was produced. |
| Range scan, `IX_Rows_K32` | Not executed | No `RANGE_PASS`; no runtime locator sequence was produced. |
| SQL provider cleanup | Not reached | No database handle was opened. |
| Final diagnostic PASS | **Failed** | The trace contains `SQL_NATIVE_R3AI BEGIN`, `SQL_NATIVE_R3AI FAIL code=VFS_UNAVAILABLE_OR_WRITABLE`, and `SQL_NATIVE_R3AI END FAIL`; it contains none of the four success markers. |

The kernel later reached its main loop, but this does not advance the SQL execution frontier. The captured serial file is 40,202 bytes with SHA-256 `71098cfbe8c1d21751ff1ac4c380410ce6cf40c2aba7c24b8c6ea501debaf8a5`.

The expected point sequence has 73 locators for root 234 / owner 1; the expected range sequence has 32 locators for root 148 / owner 2. Both complete ordered sequences in the trusted manifest match their C++ diagnostic arrays element by element. The reviewed `matches()` implementation rejects a count mismatch and compares each actual page/slot pair to the expected pair at the same position. The `POINT_PASS` and `RANGE_PASS` markers follow traversal completion, exact comparison, and successful provider close. These are source/manifest checks only: runtime actual locator arrays are **not available** because the FAT32 mount failed first. Full expected sequences are recorded in the isolated evidence directory's `candidate-preflight.json` and `launch-config.json`.

## Shutdown, QAC release, and evidence

The runner failed to surface QAC's launch notification while the child was running and then emitted a misleading “no QEMU was launched” runner error. The QAC pipe log files remained empty. The live QAC reservation, verified owner/child relationship, exact child command line, and recorded QMP port identified this as the R1 guest. I issued QMP `query-status` and `quit` only to that child; QMP returned the `host-qmp-quit` shutdown event. QAC then recorded the child as exited and the reservation as `Released` with reason `Managed child exited.`

At `2026-11-01T23:48:23.5615786Z`, the QEMU PID and QAC owner PID were absent. QAC status showed zero QEMU processes, reservations, managed sessions, queued runs, and blocked runs. No other workstream resources were terminated or modified. The runner notification issue is recorded alongside the authoritative QAC census and QMP evidence; it does not change the guest's VFS failure or qualify SQL results.

The isolated R3AJ-R1 evidence directory is:

`D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH\out\ndb1r4p11r3aj-r1-guest-acceptance-20261101`

It contains the full `serial.log`, QMP shutdown transcript, launch configuration, copied trusted candidate manifest, pre- and post-run artifact identities, QAC preflight and ownership/release snapshots, exact source-expected locator sequences, runner error, and `postrun-reconciliation.json`. No QEMU process or reservation owned by this run remains.

## Disposition

This retry verifies the candidate and exclusive QAC admission, and it establishes a real guest failure at the production FAT32 mount boundary. It does not establish SQL fixture reads, point lookup, range scan, or native SQL acceptance.

Recommend a separate bounded Server VFS/kernel integration repair phase to determine why the exact packaged read-only FAT32 RAM disk is rejected by `vfs::mount_type`, then qualify that mount path before planning any new candidate build or guest acceptance. Preserve this trace and the current ISO as evidence; do not treat the source-level locator expectations or hosted tests as guest SQL results.
