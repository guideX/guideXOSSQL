# NDB1R4P11R3BG — Native ELF QAC Input Qualification

**Outcome:** `NATIVE_SQL_APP_INPUT_HOSTED_VERIFIED` (Outcome A)  
**Phase boundary:** offline input tooling, QMP framing, evidence analysis, and R3BH procedure only  
**Guest launches / QAC admissions in R3BG:** 0 / 0

## Result

The SQL-owned helper now obtains and validates fresh QAC ownership evidence before every QMP input command, screenshot, and shutdown. The two-second freshness limit remains unchanged. Input is gated on the running QMP schema and, for all mouse events, a current mouse reported by `query-mice`. A durable one-shot ledger prevents repeating an input action after an acknowledgement or an ambiguous result. Screenshots carry their SHA-256, capture time, dimensions, and QAC guest identity; the offline analyzer does not treat QMP acknowledgement as proof of guest receipt.

The route and ownership lifecycle passed the focused offline suite, including a local synthetic QMP listener tested with Windows process and TCP-table attribution. Synthetic UI observations pass the acceptance analyzer while remaining explicitly marked synthetic. **R3BG did not prove live guest input or launch SQL Read Demo.** R3BH remains the first authorized phase for that work.

## Starting repositories and preservation

| Checkout | Branch and starting HEAD | R3BG disposition |
|---|---|---|
| SQL: `D:\dev\guideXOS_SqlDatabase` | `main` / `541a021bff0512e33f12ddc64d50a0c44aa12a1e` | Only the QMP qualification helper, its tests, and this report were changed. Existing `tests/__pycache__/` and `tools/__pycache__/` were preserved and not staged. |
| Server: `D:\dev\guideXOSServer_SQL_READ_RC_NDB1R4P11RAH` | `codex/ndb1r4p11rah-sql-read-integration` / `af0e1706d0ebac475a2b205938b14fa6471dc8af` | Read-only source review. Existing untracked build/output/cache paths, including the pre-existing `?? $PWD/` entry, were left untouched. |
| QAC: `D:\dev\guideXOS_QAC` | `main` / `1ea193258690f9e7b39390b101a117166ef8bbdd` | Read-only source/state review; no QAC source or shared service was changed. |

Repository identities, preserved untracked paths, and the read-only QAC census are recorded in `E:\CodexArtifacts\NDB1R4P11R3BG\attempt-20261114-offline-01\preflight.json`. QAC 2.0.0 was online with zero QEMU processes, managed sessions, queued reservations, or blocked reservations during that read-only census. That census is historical and is not suitable ownership evidence for a later guest operation.

## Frozen R3BE candidate integrity

The ISO and candidate were read and hashed only. No candidate build, packaging, or mutation occurred.

- ISO: `E:\CodexArtifacts\NDB1R4P11R3BE\candidate-final\guideXOS-NDB1R4P11R3BE-amd64.iso`
- Size: **100,730,880 bytes**
- SHA-256: **`10d4a361c612c496ac2f39cb2cc9f272cd61d7166920c7458ad2bb6e8982e197`**
- `candidate-manifest.json`: parsed; SHA-256 `81569e7628790237e870985436df7df94690cdb50785629d3d3cae17fab9dd88`
- `native-sql-package-manifest.json`: parsed; SHA-256 `7e416e7c5e1c64adea065298a96a44eacbcb6a2ef2bb84f697c10b233f9e30c6`
- The manifests contain 38 and 14 file references respectively. Their union is **37 unique files**; every pinned size and SHA-256 matched, with no conflicting pins.

The complete per-file verification is saved as `E:\CodexArtifacts\NDB1R4P11R3BG\attempt-20261114-offline-01\candidate-integrity.json`.

## R3BF first failed frontier

R3BF remains historically `NATIVE_SQL_APPLICATION_LAUNCH_BLOCKED`. Its preserved `input-delivery-blocker.json` reports one guest launch, zero application selections, and zero application runs.

### Observed facts

- The guest reached the 1280×800 guideXOS desktop and discovered `SQLReadDemo`, app ID `com.guidexos.sqlread`, display name `SQL Read Demo`, `NativeElf`, `amd64`, entry `gx_main`, and ABI `guidexos-c-abi-v1`.
- The guest screenshot shows Start closed. The host CUA route clicked at the visible Start control, but the menu did not visibly open. The host UIA tree contained the QEMU host window only.
- The records do not establish the QEMU client-area-to-guest coordinate mapping, host-window focus at the time of the click, or receipt of a host click by the guest. Coordinate mismatch and focus are plausible explanations, but neither is proven.
- R3BF serial records reported a PS/2 keyboard and PS/2 mouse, and no persisted Start-menu recent programs. These establish the guest-side device/initial-state observations from that run; they do not show that a host CUA click was received.
- The QMP helper intended relative mouse movement and button input. The first ownership check stopped at **2.040 seconds** before QMP connection. The next stopped at **3.618 seconds** before `query-status`, after the QMP greeting and `qmp_capabilities` response. Both exceed the unchanged 2.000-second maximum.
- `input-send-event` does not appear in the R3BF raw QMP log. No guest mouse or keyboard event was sent; no application was selected or invoked.
- QMP `screendump`, QMP shutdown, QEMU exit, and QAC reservation release were demonstrated in R3BF. The QMP greeting identified QEMU `v11.0.0-12122-ga4bb4b10c9`.

The 3.618-second stale observation blocked the QMP path before `query-status`; it does not explain the earlier host UI non-response. R3BF's host-side QMP helper used a separate capture path that performed QAC census and Windows process/listener observations before applying its freshness check. It did not use the SQL helper's corrected `poll_ownership(provider, endpoint)` lifecycle for each later command. The snapshot aged while those operations ran. R3BA had already corrected readiness ordering; R3BF did not acquire a new authoritative snapshot immediately before each sensitive input operation. Since R3BF stopped before `query-status`, it supplies no evidence that a guest input command was actually attempted with that stale snapshot.

The complete blocker and selected serial evidence are summarized from the preserved R3BF files in `E:\CodexArtifacts\NDB1R4P11R3BG\attempt-20261114-offline-01\r3bf-input-analysis.json`. The original R3BF directory and outcome were not changed.

### Acknowledgement and guest effect

A QMP response acknowledges the QMP command. It does not prove that the guideXOS input handler ran, that the Start menu changed, or that the intended app was selected. The later screenshot must be captured after the input action and tied to the same reservation, QEMU PID, endpoint, time, and frame hash. SQL Read Demo dispatch must also be established by the production serial trace described below.

## Production Start-menu route

The reviewed source is `kernel/core/desktop.cpp` and `kernel/core/native_elf_baremetal.cpp` in the unchanged Server worktree.

1. The Start button calls `toggle_start_menu()`. The keyboard handler also maps `shell::KEY_SUPER` to that toggle. No keyboard navigation path from the open menu through its program rows is implemented. F12 is a separate Nexgen PacMan shortcut and is not part of SQL acceptance.
2. Opening the menu refreshes validated package entries. With zero recent programs, the clean menu starts directly in All Programs. The footer then reads **Recent Programs**; when the Recent view is active, it reads **All Programs** and one click toggles to the full list.
3. `SQL Read Demo` is added only when the validated Native ELF package is marked `startMenuVisible`. With the R3BE package, it is sorted into the full list after Paint and before TaskManager.
4. A left-button press on the row closes the menu and calls `show_start_menu_notification()`, then `try_launch_kernel_app()`. The shared launch-target resolver identifies `com.guidexos.sqlread` as `NativeElf`; that route calls `native_elf::launch()` through the normal desktop path.

The full keyboard path does not exist. R3BH will use QMP `meta_l` press/release to open Start, then use the verified guest mouse for the menu and SQL Read Demo row. It will not use the host CUA route or the PacMan shortcut.

### Guest-coordinate reference

R3BF used a 1280×800 guest display with the taskbar at the bottom. For that layout, the source geometry yields a Start-menu origin near `(4,342)`, a 420×418 menu, content starting at `y=373`, 22-pixel rows, and a 260-pixel left column. With 16 All Programs entries, SQL Read Demo is row 13 (zero-based), approximately `y=659..681`; a sample point inside the row is `(100,670)`. The footer toggle is approximately `(80,742)`, and the Start button center is approximately `(54,780)`.

These are guest-frame reference values, not host-window coordinates. R3BH must recalculate from its current screenshot, resolution, taskbar placement, and visible row count. QMP pointer events are relative; the helper does not convert absolute framebuffer coordinates to QMP absolute input. Move in bounded increments and use a new screenshot to confirm the cursor and target before clicking. If the cursor or target cannot be identified in the captured guest frame, stop.

## Input and capture mechanism audit

- QAC provides reservation/child ownership and process lifecycle controls. Its reviewed CLI and source expose no guest keyboard/mouse input API.
- R3BF ran QEMU with `-display gtk`, `-monitor none`, `-net none`, and no VNC server configuration. QEMU host-window UIA did not expose guest controls. No approved VNC input or separate desktop-acceptance helper was found.
- The smallest supported route is direct QMP on the QAC-owned QEMU child, after exact endpoint, listener, and accepted-peer attribution. QMP `screendump` already succeeded in R3BF. QEMU documents `input-send-event`, `query-mice`, `query-qmp-schema`, and `screendump` in the [QMP Reference Manual](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html).
- R3BF did not query the running instance's QMP schema or mouse inventory. R3BG's offline QMP server advertised `input-send-event` and a current `QEMU PS/2 Mouse`; this validates the helper's command encoding and gates, not the R3BE guest's live device response. R3BH must check the actual running instance before input.
- The R3BE guest boot log reported PS/2 keyboard and mouse availability. At R3BH, require that serial evidence again; require `query-mice` to report a current mouse; and require live `query-qmp-schema` to contain `input-send-event` as a command. If any check fails, stop before sending input.

## SQL-owned freshness and input safeguards

`tools/r3ay_qmp_preflight.py` keeps `MAX_QAC_SNAPSHOT_AGE_SECONDS = 2.0`. `poll_ownership(provider, endpoint)` first pins reservation and child identity, performs bounded Windows process/listener readiness observations, then obtains a new authoritative QAC census and correlates it with the latest OS observation. It validates reservation, owner PID, QEMU child PID and start identity, executable, command-line endpoint, listener, accepted TCP peer, and both monotonic and QAC timestamp age. The initial snapshot cannot authorize the later command.

The new `FreshQmpOperations` wrapper obtains that evidence immediately before each schema query, mouse query, screenshot, input command, and QMP shutdown. Input and shutdown write and flush a ledger intent before acquiring their final snapshot, so ledger I/O cannot age the snapshot. `cont` requires fresh evidence before resume and a new verification before its post-resume status query. A previous successful `cont` is never reused as authorization for later commands. Generic post-capabilities QMP commands require fresh evidence.

The input contract is intentionally narrow:

- At most eight ordered events in a QMP command.
- Keyboard events use a small qcode allowlist, including `meta_l`, arrows, Enter, Escape, and Tab, with boolean press/release transitions.
- Pointer events use nonzero relative x/y deltas bounded to ±4096. Relative pointer and left-button events are refused until the live schema confirms `input-send-event` and `query-mice` reports a current mouse.
- Only the left button is accepted. Button/key state is tracked across commands. If a send fails after exchange begins, the state becomes unknown; a release may clear unknown state only after a new ownership check. No new press is allowed while state is down or uncertain.
- Every distinct press, release, pointer move, and shutdown action gets its own ownership refresh. If ownership is stale, mismatched, released, or uncertain, no input event is sent.
- The append-only action ledger writes `intent` before a sensitive action and records `qmp_acknowledged`, `rejected_before_send`, or `ambiguous`. Reusing an action ID is refused in every case. SQL selection uses the fixed ID `select-sql-read-demo` and binds that action to the SHA-256 of the screenshot that showed the entry. An ambiguous selection is never repeated.

## Screenshot and App Model evidence contract

`QmpClient.screendump()` refuses to overwrite a frame and records the QMP acknowledgement, capture start/completion times, PNG dimensions/size/SHA-256, and QAC guest identity. Every screenshot is preceded by fresh ownership/peer validation. `analyze_ui_evidence()` re-hashes each frame and requires correctly ordered observations for the initial desktop, opened Start menu, All Programs, and visible SQL Read Demo entry. It checks the screenshot-after-action relationship and guest identity. Synthetic frames remain marked synthetic and never set `liveGuestInteractionQualified`.

R3BH should capture these frames in order:

1. Initial desktop, menu closed, no unexpected dialog/window, and expected guest display visible.
2. Start open after the `open-start-menu` action.
3. All Programs visible after Start or after the optional `open-all-programs` action.
4. SQL Read Demo visible. The selection ledger entry must carry this frame's hash.
5. A fresh screenshot after the one selection and release action.
6. Application-visible state if it remains on screen; otherwise explicitly record that the visual application-window gate was not observed and use the production trace below for dispatch/entry.

If graphical capture becomes unavailable, the production serial trace can establish selection and dispatch when the lines are correlated and ordered:

- `[LaunchDispatch] ... target=SQL Read Demo resolvedType=NativeElf appId=com.guidexos.sqlread ... selectedDispatch=SQL Read Demo`
- `[NATIVE-ELF] runtime=<id> app=SQL Read Demo launch begin path=package-relative runtime=native-elf abi=guidexos-c-abi-v1`
- `PACBM 04 GX_MAIN_ENTER runtime=<same id> ...`

The first line establishes resolver selection; the next establishes Native ELF launch; the `PACBM` runtime ID correlates actual `gx_main` entry. The generic `AppLogger::logLaunch()` updates an in-memory log and is not itself serial evidence. Missing visual gates must remain marked unverified even if the serial trace passes.

## Offline qualification

Command:

```powershell
python -B -m unittest tests.test_r3ay_qmp_preflight tests.test_r3bg_input_qualification -v
```

**Result: 30 tests passed.** Full output is in `E:\CodexArtifacts\NDB1R4P11R3BG\attempt-20261114-offline-01\offline-tests.log`.

The suite retains the two-second boundary and existing ownership cases for fresh evidence after slow OS readiness, stale callers, reservation changes/releases, foreign/unexpected listeners, PID reuse, wrong child/owner, endpoint mismatch, and accepted-peer mismatch. It reproduces the R3BF **3.618-second** stale age and verifies the input guard refuses before wire send. The Windows synthetic-QMP integration launches a local Python listener (not QEMU), uses Windows process/TCP-table APIs to attribute its listener and connected peer, negotiates capabilities, checks schema/mouse/screendump, sends supported pointer/button/key event sequences, checks a fresh census for each command, and performs one-shot QMP shutdown.

Input tests cover qcode and pointer encodings, bounds, press/release ordering, absent schema/device refusal, missing-ack ambiguity, and duplicate-action rejection. Synthetic UI cases cover ordered Start transitions, failed opening, wrong focus, wrong app, screenshot missing/stale/identity mismatch, selection without loader invocation, and loader without `gx_main`. Synthetic passes are not live desktop qualification.

## R3BH one-launch guest procedure

R3BG does not authorize the following run. Obtain the separate R3BH authorization and a new exclusive QAC admission before any guest launch.

1. Recheck repositories, candidate manifest, OVMF identity, and the unchanged R3BE ISO hash/size above. Take a fresh read-only QAC status/list/queue/inspect census; require no conflicting guest or reservation. Preserve those records in a new R3BH attempt folder.
2. Request exactly one QAC-managed acceptance launch of the frozen R3BE ISO. Do not start QEMU directly. Keep the ISO and pflash read-only, networking disabled, and capture serial/debug output to the attempt folder. Start paused with the approved `-S` lifecycle.
3. Use the provider-backed QAC ownership poller to verify the reservation, owner, managed QEMU child, process start identity, executable, endpoint, listener, and connected peer. Negotiate QMP and verify paused status. Before `cont`, poll fresh ownership; after `cont`, poll again and require a fresh `query-status` result of `running`.
4. Before any input, query the live QMP schema and require `input-send-event`; query `query-mice` and require a current mouse; inspect serial for keyboard and mouse initialization. If any prerequisite is absent, preserve the first failed frontier and stop.
5. Capture the initial guest screenshot. Confirm 1280×800 (or record the actual dimensions), menu closed, and no unexpected overlay. Save its hash, timestamp, and guest identity.
6. Send `meta_l` down with action ID `open-start-menu`, then send its release with a distinct ID. Obtain fresh ownership immediately before each QMP command. Capture a new frame and require the Start menu to be visibly open. Do not infer this from the QMP responses.
7. Confirm All Programs. On the clean R3BE boot, no recent programs were persisted, so Start should open directly to All Programs and its footer should read Recent Programs. If the screenshot instead shows the Recent list and the footer reads All Programs, move the guest pointer to that button and click it once; use separately guarded down/up commands and confirm All Programs with a new screenshot.
8. Locate SQL Read Demo in the current screenshot. Move the guest-relative pointer in bounded increments, taking fresh screenshots to confirm cursor and row alignment. Check the current row and menu geometry; use the reference coordinates above only if the same 1280×800 layout, bottom taskbar, and 16-entry list are visible. Capture/hash the entry-visible frame.
9. Call the fixed one-shot selection action bound to that frame hash, then send a separately guarded left-button release. Do not repeat the selection action. Capture a post-action frame. If the command acknowledgement is missing, ownership is uncertain, the menu does not respond, or the application cannot be selected, preserve the first failed frontier and do not retry or rebuild.
10. Correlate the selected `com.guidexos.sqlread` launch through `[LaunchDispatch]`, Native ELF `launch begin`, and `PACBM 04 GX_MAIN_ENTER` for one runtime. Require the existing R3BE application lifecycle contract: read-only database open; 73 ordered K64 point locators and expected digest; inclusive Int32 `[1,2]` range with 32 ordered locators and expected digest; no-match; invalid request without output mutation; successful recovery; explicit close; stale-handle rejection; a second outstanding database session; normal `gx_main` return; production detach; and reclamation of the outstanding session. Require all expected `SQL_APP_API`/`SQL_APP_LIFECYCLE` markers and no unexpected fault. Boot-time `SQL_NATIVE_R3AQ` diagnostics do not count as application callbacks.
11. Obtain fresh ownership immediately before QMP `quit`. Verify QEMU child exit and release of the same QAC reservation. Rehash the frozen ISO. If safe ownership for shutdown cannot be re-established, stop and use only the existing QAC-owned cleanup policy; never connect to a foreign endpoint or bypass the guard.

At most one guest launch is allowed in R3BH. Do not rebuild or retry the candidate after an input or selection failure.

## Remaining live uncertainties and separate issue

- The R3BE guest's live QMP schema and `query-mice` response were not queried in R3BG. R3BH must confirm both before input.
- The host CUA route remains unqualified; its earlier failure has not been assigned to focus or coordinate mapping. R3BH uses direct, ownership-checked QMP instead.
- No screenshot-confirmed real guest input, menu navigation, SQL Read Demo dispatch, or SQL application callback is claimed by this report.
- R3BB's independent QMP page-table-walker Gate 9 remains **UNVERIFIED**. R3BG did not run that witness.

## Local commits and evidence

The accepted helper and focused tests were committed locally as `97c9e44be4c7b995b2478f306c76d8527eac248e` (`Add QAC-owned guest input qualification guards`), followed by `23f414a2f4d584b1ea7829506c9f5a5e6aeae269` (`Require verified mouse for QMP clicks`). This report is a separate local follow-up commit. The final SQL HEAD and all phase evidence hashes are recorded in `E:\CodexArtifacts\NDB1R4P11R3BG\attempt-20261114-offline-01\final-state.json` after that commit. No push or merge was performed.
