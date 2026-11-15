"""R3BG one-shot input actions and screenshot-bound UI acceptance analysis.

This module composes the existing fail-closed QMP ownership helper. It does not
launch QEMU or request QAC admission. Callers make UI decisions from an already
captured frame, then each command below obtains a new authoritative snapshot.
"""

from __future__ import annotations

import datetime as _datetime
import hashlib
import json
import os
import re
from pathlib import Path
from typing import Any, Callable

from . import r3ay_qmp_preflight as qmp


class InputQualificationError(RuntimeError):
    """A one-shot action or offline UI acceptance record is incomplete."""


class DuplicateInputAction(InputQualificationError):
    """An action ID has already been attempted and cannot be repeated."""


def _utc_now() -> str:
    return _datetime.datetime.now(_datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def _parse_utc(value: Any, name: str) -> _datetime.datetime:
    if not isinstance(value, str):
        raise InputQualificationError(f"{name} timestamp is missing")
    try:
        parsed = _datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise InputQualificationError(f"{name} timestamp is malformed") from exc
    if parsed.tzinfo is None:
        raise InputQualificationError(f"{name} timestamp must include a UTC offset")
    return parsed.astimezone(_datetime.timezone.utc)


class InputActionLedger:
    """Append-only write-ahead ledger that refuses duplicate input actions."""

    def __init__(self, path: str | os.PathLike[str]) -> None:
        self.path = Path(path)
        self._states: dict[str, str] = {}
        self._rows: list[dict[str, Any]] = []
        if self.path.exists():
            try:
                for raw in self.path.read_text(encoding="utf-8").splitlines():
                    if not raw:
                        continue
                    row = json.loads(raw)
                    action_id, state = row.get("actionId"), row.get("state")
                    if (not isinstance(action_id, str)
                            or state not in ("intent", "qmp_acknowledged", "ambiguous",
                                             "rejected_before_send")):
                        raise ValueError("malformed action ledger row")
                    self._rows.append(row)
                    self._states[action_id] = "in_flight" if state == "intent" else state
            except (OSError, ValueError, json.JSONDecodeError) as exc:
                raise InputQualificationError(f"input action ledger is unreadable: {exc}") from exc

    def _append(self, record: dict[str, Any]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        wire = (json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        with self.path.open("ab") as stream:
            stream.write(wire)
            stream.flush()
            os.fsync(stream.fileno())
        self._rows.append(record)

    def state(self, action_id: str) -> str | None:
        return self._states.get(action_id)

    def action_records(self) -> list[dict[str, Any]]:
        """Return normalized one-shot action records for the offline UI analyzer."""
        records: dict[str, dict[str, Any]] = {}
        for row in self._rows:
            action_id = row["actionId"]
            if row["state"] == "intent":
                context = row.get("context")
                records[action_id] = {
                    "actionId": action_id,
                    "state": "in_flight",
                    "startedAtUtc": row.get("atUtc"),
                    "context": context if isinstance(context, dict) else {},
                }
                continue
            record = records.setdefault(action_id, {"actionId": action_id})
            record["state"] = row["state"]
            record["completedAtUtc"] = row.get("atUtc")
            if "qmpResponse" in row:
                record["qmpResponse"] = row["qmpResponse"]
            if "error" in row:
                record["error"] = row["error"]
            if "context" in row:
                record["context"] = row["context"]
            context = record.get("context")
            if isinstance(context, dict):
                for field in ("appId", "actionKind", "entryFrameSha256"):
                    if field in context:
                        record[field] = context[field]
        return list(records.values())

    def run_once(
        self,
        action_id: str,
        operation: Callable[[], Any],
        *,
        context: dict[str, Any] | None = None,
    ) -> Any:
        if (not isinstance(action_id, str)
                or re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,127}", action_id) is None):
            raise InputQualificationError("input action ID is malformed")
        if action_id in self._states:
            raise DuplicateInputAction(
                f"input action {action_id!r} already has state {self._states[action_id]!r}"
            )
        self._states[action_id] = "in_flight"
        self._append({"actionId": action_id, "state": "intent", "atUtc": _utc_now(),
                      "context": context or {}})
        try:
            result = operation()
        except Exception as exc:
            attempted = getattr(exc, "qmp_send_attempted", False) is True
            state = "ambiguous" if attempted else "rejected_before_send"
            self._states[action_id] = state
            self._append({"actionId": action_id, "state": state, "atUtc": _utc_now(),
                          "qmpSendAttempted": attempted,
                          "errorType": type(exc).__name__, "error": str(exc)})
            raise
        self._states[action_id] = "qmp_acknowledged"
        response = result if isinstance(result, dict) else {"resultType": type(result).__name__}
        self._append({"actionId": action_id, "state": "qmp_acknowledged", "atUtc": _utc_now(),
                      "qmpResponse": response,
                      "guestReceiptInferred": False})
        return result


class FreshQmpOperations:
    """Refresh QAC/OS ownership immediately before each operation on a QMP client."""

    def __init__(
        self,
        client: qmp.QmpClient,
        admission_provider: Callable[[], Any],
        expected_executable: str,
        ledger: InputActionLedger,
        *,
        timeout_seconds: float = 5.0,
        interval_seconds: float = 0.2,
        observe: Callable[[int, int], dict[str, Any]] | None = None,
        ownership_poller: Callable[..., qmp.OwnershipEvidence] = qmp.poll_ownership,
    ) -> None:
        self.client = client
        self.admission_provider = admission_provider
        self.expected_executable = expected_executable
        self.ledger = ledger
        self.timeout_seconds = timeout_seconds
        self.interval_seconds = interval_seconds
        self.observe = observe
        self.ownership_poller = ownership_poller

    def fresh_ownership(self) -> qmp.OwnershipEvidence:
        kwargs: dict[str, Any] = {
            "expected_executable": self.expected_executable,
            "timeout_seconds": self.timeout_seconds,
            "interval_seconds": self.interval_seconds,
        }
        if self.observe is not None:
            kwargs["observe"] = self.observe
        evidence = self.ownership_poller(
            self.admission_provider, self.client.endpoint, **kwargs
        )
        qmp.require_owned(evidence, self.client.endpoint)
        return evidence

    def query_qmp_schema(self) -> dict[str, Any]:
        evidence = self.fresh_ownership()
        return self.client.query_qmp_schema(evidence)

    def query_mice(self) -> dict[str, Any]:
        evidence = self.fresh_ownership()
        return self.client.query_mice(evidence)

    def screendump(self, path: str | os.PathLike[str]) -> dict[str, Any]:
        evidence = self.fresh_ownership()
        return self.client.screendump(evidence, path)

    def send_input(
        self,
        action_id: str,
        events: list[dict[str, Any]],
        *,
        context: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        action_context = dict(context or {})
        if {"endpoint", "events"} & action_context.keys():
            raise InputQualificationError("input context cannot replace the endpoint or event record")
        if action_context.get("actionKind") == "select-application":
            expected_selection = [{"type": "btn", "data": {"button": "left", "down": True}}]
            if (action_id != "select-sql-read-demo"
                    or action_context.get("appId") != "com.guidexos.sqlread"
                    or events != expected_selection):
                raise InputQualificationError("SQL Read Demo selection must use its fixed one-shot action ID")
        record = self.ledger.run_once(
            action_id,
            lambda: self._send_input_after_intent(events),
            context={
                **action_context,
                "endpoint": self.client.endpoint.qemu_value,
                "events": events,
            },
        )
        return record["qmpResponse"]

    def move_pointer(self, action_id: str, delta_x: int, delta_y: int,
                     *, context: dict[str, Any] | None = None) -> dict[str, Any]:
        events = []
        if delta_x:
            events.append({"type": "rel", "data": {"axis": "x", "value": delta_x}})
        if delta_y:
            events.append({"type": "rel", "data": {"axis": "y", "value": delta_y}})
        if not events:
            raise InputQualificationError("pointer action must change at least one axis")
        return self.send_input(action_id, events, context=context)

    def left_button(self, action_id: str, down: bool,
                    *, context: dict[str, Any] | None = None) -> dict[str, Any]:
        if not isinstance(down, bool):
            raise InputQualificationError("left-button action state must be boolean")
        return self.send_input(action_id, [
            {"type": "btn", "data": {"button": "left", "down": down}}
        ], context=context)

    def key_transition(self, action_id: str, qcode: str, down: bool,
                       *, context: dict[str, Any] | None = None) -> dict[str, Any]:
        return self.send_input(action_id, [{
            "type": "key",
            "data": {"key": {"type": "qcode", "data": qcode}, "down": down},
        }], context=context)

    def select_sql_read_demo(self, entry_frame_sha256: str) -> dict[str, Any]:
        if (not isinstance(entry_frame_sha256, str)
                or re.fullmatch(r"[0-9a-f]{64}", entry_frame_sha256) is None):
            raise InputQualificationError("SQL Read Demo selection requires the visible entry frame SHA-256")
        return self.left_button(
            "select-sql-read-demo", True,
            context={"actionKind": "select-application",
                     "appId": "com.guidexos.sqlread",
                     "entryFrameSha256": entry_frame_sha256},
        )

    def _send_input_after_intent(self, events: list[dict[str, Any]]) -> dict[str, Any]:
        # The ledger fsync can take time, so acquire final evidence only after it.
        evidence = self.fresh_ownership()
        reply = self.client.send_input_events(evidence, events)
        return {
            "qmpResponse": reply,
            "ownership": {
                "reservationId": evidence.reservation_id,
                "qemuPid": evidence.qemu_pid,
                "ownerPid": evidence.owner_pid,
                "endpoint": self.client.endpoint.qemu_value,
                "qacObservedAtUtc": evidence.qac_observed_at_utc,
            },
        }

    def quit(self, action_id: str = "qmp-shutdown") -> dict[str, Any]:
        record = self.ledger.run_once(
            action_id,
            self._quit_after_intent,
            context={"endpoint": self.client.endpoint.qemu_value},
        )
        return record["qmpResponse"]

    def _quit_after_intent(self) -> dict[str, Any]:
        evidence = self.fresh_ownership()
        reply = self.client.quit(evidence)
        return {
            "qmpResponse": reply,
            "ownership": {
                "reservationId": evidence.reservation_id,
                "qemuPid": evidence.qemu_pid,
                "ownerPid": evidence.owner_pid,
                "endpoint": self.client.endpoint.qemu_value,
                "qacObservedAtUtc": evidence.qac_observed_at_utc,
            },
        }


def _file_sha256(path: Path) -> tuple[int, str, int, int]:
    try:
        with path.open("rb") as stream:
            header = stream.read(24)
        if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
            raise InputQualificationError(f"screenshot is missing or is not a PNG: {path}")
        size = path.stat().st_size
        if size < 33 or int.from_bytes(header[16:20], "big") <= 0 \
                or int.from_bytes(header[20:24], "big") <= 0:
            raise InputQualificationError(f"screenshot PNG dimensions or file size are invalid: {path}")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return size, digest.hexdigest(), int.from_bytes(header[16:20], "big"), int.from_bytes(header[20:24], "big")
    except OSError as exc:
        raise InputQualificationError(f"screenshot is unavailable: {path}: {exc}") from exc


def analyze_ui_evidence(
    screenshots: list[dict[str, Any]],
    actions: list[dict[str, Any]],
    app_events: list[dict[str, Any]],
    *,
    expected_guest: dict[str, Any],
) -> dict[str, Any]:
    """Validate screenshot transitions and production app-selection/loader ordering.

    Screenshot facts are observations made during review, not inferred from QMP
    acknowledgements. This analyzer validates their file hashes, times, and guest
    identity. Synthetic input must stay marked synthetic in its result.
    """
    if not isinstance(expected_guest, dict) or not {
        "reservationId", "qemuPid", "endpoint"
    }.issubset(expected_guest):
        raise InputQualificationError("expected guest identity is incomplete")
    action_map: dict[str, dict[str, Any]] = {}
    for action in actions:
        if not isinstance(action, dict) or not isinstance(action.get("actionId"), str):
            raise InputQualificationError("input action record is malformed")
        if action["actionId"] in action_map:
            raise InputQualificationError(f"duplicate input action record {action['actionId']!r}")
        action_map[action["actionId"]] = action

    facts: dict[str, _datetime.datetime] = {}
    fact_frames: dict[str, dict[str, Any]] = {}
    synthetic = False
    if any(not isinstance(frame, dict) for frame in screenshots):
        raise InputQualificationError("screenshot record is malformed")
    for frame in sorted(screenshots, key=lambda item: item.get("capturedAtUtc", "")):
        if not isinstance(frame, dict) or frame.get("source") != "qmp-screendump":
            raise InputQualificationError("UI frame must come from the owned guest screendump")
        identity = frame.get("guestIdentity")
        if identity != expected_guest:
            raise InputQualificationError("screenshot guest identity differs from the active QAC child")
        path = Path(frame.get("path", ""))
        size, digest, width, height = _file_sha256(path)
        if (size != frame.get("bytes") or digest != frame.get("sha256")
                or width != frame.get("width") or height != frame.get("height")):
            raise InputQualificationError("screenshot metadata does not match its file")
        captured = _parse_utc(frame.get("capturedAtUtc"), "screenshot capture")
        synthetic = synthetic or frame.get("synthetic") is True
        frame_facts = frame.get("facts")
        if not isinstance(frame_facts, list) or any(not isinstance(fact, str) for fact in frame_facts):
            raise InputQualificationError("screenshot facts are malformed")
        after_action = frame.get("afterActionId")
        if after_action is not None:
            action = action_map.get(after_action)
            if action is None:
                raise InputQualificationError(f"screenshot refers to unknown input action {after_action!r}")
            action_time = _parse_utc(action.get("completedAtUtc"), "input action completion")
            if captured <= action_time:
                raise InputQualificationError("screenshot is stale relative to the input event")
        for fact in frame_facts:
            facts[fact] = captured
            fact_frames[fact] = frame

    for required in ("desktop_visible", "expected_desktop_focus", "start_menu_open", "all_programs_visible",
                     "sql_read_demo_visible"):
        if required not in facts:
            raise InputQualificationError(f"required UI state was not observed: {required}")
    if any("wrong_menu_focus" in frame.get("facts", []) for frame in screenshots):
        raise InputQualificationError("Start-menu focus was on an unexpected window or control")
    first_frame = min(screenshots, key=lambda item: item.get("capturedAtUtc", ""))
    if ("desktop_visible" not in first_frame.get("facts", [])
            or "expected_desktop_focus" not in first_frame.get("facts", [])
            or first_frame.get("afterActionId") is not None):
        raise InputQualificationError("initial screenshot does not establish the focused guideXOS desktop")
    if "start_menu_focus_verified" not in fact_frames["start_menu_open"].get("facts", []):
        raise InputQualificationError("opened Start menu does not have verified guest UI focus")
    if not (facts["desktop_visible"] <= facts["start_menu_open"]
            <= facts["all_programs_visible"] <= facts["sql_read_demo_visible"]):
        raise InputQualificationError("observed UI states are out of order")

    start_action = action_map.get("open-start-menu")
    if start_action is None or start_action.get("state") != "qmp_acknowledged":
        raise InputQualificationError("Start-menu input action is missing or unacknowledged")
    start_time = _parse_utc(start_action.get("completedAtUtc"), "Start-menu action completion")
    if facts["start_menu_open"] <= start_time:
        raise InputQualificationError("Start-menu screenshot predates its input event")
    if fact_frames["start_menu_open"].get("afterActionId") != "open-start-menu":
        raise InputQualificationError("Start-menu screenshot is not tied to its successful input action")

    all_programs_frame = fact_frames["all_programs_visible"]
    all_programs_action_id = all_programs_frame.get("afterActionId")
    if all_programs_action_id == "open-start-menu":
        if facts["all_programs_visible"] <= start_time:
            raise InputQualificationError("All Programs screenshot predates the Start-menu input")
    elif all_programs_action_id == "open-all-programs":
        all_programs_action = action_map.get("open-all-programs")
        if all_programs_action is None or all_programs_action.get("state") != "qmp_acknowledged":
            raise InputQualificationError("All Programs input action is missing or unacknowledged")
        all_programs_time = _parse_utc(
            all_programs_action.get("completedAtUtc"), "All Programs action completion")
        if facts["all_programs_visible"] <= all_programs_time:
            raise InputQualificationError("All Programs screenshot predates its input event")
    else:
        raise InputQualificationError("All Programs screenshot is not tied to a navigation action")
    if fact_frames["sql_read_demo_visible"].get("afterActionId") not in (
        "open-start-menu", "open-all-programs"
    ):
        raise InputQualificationError("SQL Read Demo visibility lacks a current All Programs screenshot")

    selection = action_map.get("select-sql-read-demo")
    if selection is None or selection.get("state") != "qmp_acknowledged":
        raise InputQualificationError("SQL Read Demo selection action lacks a QMP acknowledgement")
    if selection.get("actionKind") != "select-application":
        raise InputQualificationError("SQL Read Demo selection action lacks its fixed one-shot identity")
    selected_at = _parse_utc(selection.get("completedAtUtc"), "SQL Read Demo selection completion")
    if selection.get("appId") != "com.guidexos.sqlread":
        raise InputQualificationError("selection action targets the wrong application")
    if selection.get("entryFrameSha256") != fact_frames["sql_read_demo_visible"].get("sha256"):
        raise InputQualificationError("selection action is not bound to the visible SQL Read Demo frame")
    if facts["sql_read_demo_visible"] >= selected_at:
        raise InputQualificationError("SQL Read Demo selection predates its visible menu entry")

    launch = next((event for event in app_events
                   if event.get("event") == "app_launch_dispatch_selected"), None)
    loader = next((event for event in app_events
                   if event.get("event") == "native_elf_loader_invocation"), None)
    entry = next((event for event in app_events
                  if event.get("event") == "gx_main_entry"), None)
    if (not launch or launch.get("appId") != "com.guidexos.sqlread"
            or launch.get("target") != "SQL Read Demo"
            or launch.get("resolvedType") != "NativeElf"
            or launch.get("selectedDispatch") != "SQL Read Demo"):
        raise InputQualificationError("production App Model trace does not confirm SQL Read Demo selection")
    if not loader:
        raise InputQualificationError("application selection was reported without Native ELF loader invocation")
    if not entry:
        raise InputQualificationError("Native ELF loader invocation did not establish gx_main entry")
    launch_time = _parse_utc(launch.get("atUtc"), "App Model launch event")
    loader_time = _parse_utc(loader.get("atUtc"), "Native ELF loader event")
    entry_time = _parse_utc(entry.get("atUtc"), "gx_main entry event")
    if (loader.get("appName") != "SQL Read Demo"
            or not selected_at <= launch_time <= loader_time <= entry_time):
        raise InputQualificationError("App Model selection and Native ELF events are out of order")
    runtime_ids = {loader.get("runtimeId"), entry.get("runtimeId")}
    if len(runtime_ids) != 1 or None in runtime_ids:
        raise InputQualificationError("App Model and loader events do not share one runtime identity")

    return {
        "result": "PASS",
        "applicationId": "com.guidexos.sqlread",
        "runtimeId": next(iter(runtime_ids)),
        "observedFacts": sorted(facts),
        "screenshotCount": len(screenshots),
        "guestIdentity": expected_guest,
        "synthetic": synthetic,
        "liveGuestInteractionQualified": not synthetic,
        "qmpAcknowledgementUsedAsGuestReceipt": False,
    }
