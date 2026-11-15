from __future__ import annotations

import hashlib
import json
import struct
import tempfile
import time
import unittest
import zlib
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

from tools import r3ay_qmp_preflight as qmp
from tools import r3bg_input_qualification as r3bg


GUEST = {"reservationId": "r3bg-test-reservation", "qemuPid": 4242,
         "endpoint": "tcp:127.0.0.1:51288,server=on,wait=off"}


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)


PNG = (b"\x89PNG\r\n\x1a\n"
       + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
       + _png_chunk(b"IDAT", zlib.compress(b"\x00\x00\x00\x00\xff"))
       + _png_chunk(b"IEND", b""))


def _action(action_id: str, completed: str, **fields):
    return {"actionId": action_id, "state": "qmp_acknowledged",
            "completedAtUtc": completed, **fields}


class InputContractTests(unittest.TestCase):
    def test_qmp_key_pointer_and_press_release_contract(self):
        states = qmp._validate_input_events([
            {"type": "key", "data": {"key": {"type": "qcode", "data": "tab"}, "down": True}},
            {"type": "key", "data": {"key": {"type": "qcode", "data": "tab"}, "down": False}},
            {"type": "btn", "data": {"button": "left", "down": True}},
            {"type": "btn", "data": {"button": "left", "down": False}},
            {"type": "rel", "data": {"axis": "x", "value": 12}},
            {"type": "rel", "data": {"axis": "y", "value": -4}},
        ], {})
        self.assertEqual(states, {("key", "tab"): "up", ("btn", "left"): "up"})
        for event in (
            {"type": "key", "data": {"key": {"type": "qcode", "data": "not-a-key"}, "down": True}},
            {"type": "rel", "data": {"axis": "x", "value": 4097}},
            {"type": "btn", "data": {"button": "right", "down": True}},
        ):
            with self.subTest(event=event), self.assertRaises(qmp.PreflightError):
                qmp._validate_input_events([event], {})
        with self.assertRaisesRegex(qmp.PreflightError, "already down"):
            qmp._validate_input_events([
                {"type": "key", "data": {"key": {"type": "qcode", "data": "tab"}, "down": True}},
                {"type": "key", "data": {"key": {"type": "qcode", "data": "tab"}, "down": True}},
            ], {})
        with self.assertRaisesRegex(qmp.PreflightError, "no matching press"):
            qmp._validate_input_events([
                {"type": "btn", "data": {"button": "left", "down": False}},
            ], {})

    def test_ledger_persists_acknowledgement_and_prevents_duplicate_action(self):
        with tempfile.TemporaryDirectory(prefix="r3bg-ledger-") as temp_dir:
            ledger = r3bg.InputActionLedger(Path(temp_dir) / "actions.jsonl")
            self.assertEqual(ledger.run_once("select-sql-read-demo", lambda: {"return": {}}),
                             {"return": {}})
            self.assertEqual(ledger.state("select-sql-read-demo"), "qmp_acknowledged")
            with self.assertRaises(r3bg.DuplicateInputAction):
                ledger.run_once("select-sql-read-demo", lambda: self.fail("must not retry"))
            loaded = r3bg.InputActionLedger(Path(temp_dir) / "actions.jsonl")
            self.assertEqual(loaded.state("select-sql-read-demo"), "qmp_acknowledged")

    def test_missing_qmp_ack_is_ambiguous_and_never_retried(self):
        with tempfile.TemporaryDirectory(prefix="r3bg-ambiguous-") as temp_dir:
            ledger = r3bg.InputActionLedger(Path(temp_dir) / "actions.jsonl")
            left, right = __import__("socket").socketpair()
            client = qmp.QmpClient(left, qmp.QmpEndpoint.parse(GUEST["endpoint"]),
                                   qmp.OwnershipEvidence(
                                       qmp.OwnershipState.OWNED_LISTENER_VERIFIED,
                                       qmp.QmpEndpoint.parse(GUEST["endpoint"]),
                                       GUEST["reservationId"], GUEST["qemuPid"], 1, "test",
                                       observation={},
                                       qac_observed_at_utc=datetime.now(timezone.utc).isoformat()))
            client.capabilities_ready = True
            client.initial_status = {"running": True}
            client.input_send_event_available = True
            event = [{"type": "key", "data": {"key": {"type": "qcode", "data": "tab"}, "down": True}}]
            try:
                with patch.object(client, "_require_fresh_peer"), \
                        patch.object(client, "_exchange", side_effect=qmp.QmpProtocolError("response lost")):
                    with self.assertRaises(qmp.QmpProtocolError):
                        ledger.run_once("select-sql-read-demo",
                                        lambda: client.send_input_events(client.evidence, event))
                self.assertEqual(ledger.state("select-sql-read-demo"), "ambiguous")
                self.assertEqual(client.input_states[("key", "tab")], "unknown")
                action = ledger.action_records()[0]
                self.assertEqual(action["state"], "ambiguous")
                self.assertIn("response lost", action["error"])
                with self.assertRaises(r3bg.DuplicateInputAction):
                    ledger.run_once("select-sql-read-demo", lambda: {"return": {}})
            finally:
                client.close()
                right.close()

    def test_qmp_pointer_requires_live_schema_and_a_current_mouse(self):
        left, right = __import__("socket").socketpair()
        client = qmp.QmpClient(left, qmp.QmpEndpoint.parse(GUEST["endpoint"]),
                               qmp.OwnershipEvidence(
                                   qmp.OwnershipState.OWNED_LISTENER_VERIFIED,
                                   qmp.QmpEndpoint.parse(GUEST["endpoint"]),
                                   GUEST["reservationId"], GUEST["qemuPid"], 1, "test",
                                   observation={},
                                   qac_observed_at_utc=datetime.now(timezone.utc).isoformat()))
        client.capabilities_ready = True
        client.initial_status = {"running": True}
        event = [{"type": "rel", "data": {"axis": "x", "value": 1}}]
        try:
            with patch.object(client, "_require_fresh_peer") as guard, \
                    patch.object(client, "_exchange") as exchange:
                with self.assertRaisesRegex(qmp.PreflightError, "live schema"):
                    client.send_input_events(client.evidence, event)
                client.input_send_event_available = True
                client.current_mouse_available = False
                with self.assertRaisesRegex(qmp.PreflightError, "current mouse"):
                    client.send_input_events(client.evidence, event)
                guard.assert_not_called()
                exchange.assert_not_called()
                client.current_mouse_available = True
                exchange.return_value = {"id": "r3ay-test", "return": {}}
                client.send_input_events(client.evidence, event)
                guard.assert_called_once()
                exchange.assert_called_once_with("input-send-event", {"events": event})
        finally:
            client.close()
            right.close()

    def test_fresh_qac_snapshot_is_taken_after_durable_intent(self):
        endpoint = qmp.QmpEndpoint.parse(GUEST["endpoint"])
        evidence = qmp.OwnershipEvidence(
            qmp.OwnershipState.OWNED_LISTENER_VERIFIED, endpoint, GUEST["reservationId"],
            GUEST["qemuPid"], 1, "synthetic", verified_at_monotonic=time.monotonic(),
            observation={"process": {"ProcessId": GUEST["qemuPid"]}, "listeners": []},
            qac_observed_at_utc=datetime.now(timezone.utc).isoformat())

        class FakeClient:
            def __init__(self):
                self.endpoint = endpoint
                self.sent = []

            def send_input_events(self, fresh, events):
                self.sent.append((fresh, events))
                return {"return": {}}

        with tempfile.TemporaryDirectory(prefix="r3bg-fresh-order-") as temp_dir:
            ledger_path = Path(temp_dir) / "actions.jsonl"
            ledger = r3bg.InputActionLedger(ledger_path)
            poll_count = 0

            def poller(*_args, **_kwargs):
                nonlocal poll_count
                poll_count += 1
                rows = [json.loads(line) for line in ledger_path.read_text(encoding="utf-8").splitlines()]
                self.assertEqual(rows[-1]["state"], "intent")
                return evidence

            client = FakeClient()
            operations = r3bg.FreshQmpOperations(
                client, lambda: object(), "qemu.exe", ledger,
                ownership_poller=poller)
            operations.send_input("mouse-1", [{"type": "rel", "data": {"axis": "x", "value": 1}}])
            self.assertEqual(len(client.sent), 1)
            self.assertEqual(poll_count, 1)
            with self.assertRaises(r3bg.DuplicateInputAction):
                operations.send_input("mouse-1", [{"type": "rel", "data": {"axis": "x", "value": 1}}])
            self.assertEqual(poll_count, 1)
            operations.select_sql_read_demo("a" * 64)
            self.assertEqual(poll_count, 2)
            selection = next(row for row in ledger.action_records()
                             if row["actionId"] == "select-sql-read-demo")
            self.assertEqual(selection["appId"], "com.guidexos.sqlread")
            self.assertEqual(selection["actionKind"], "select-application")
            self.assertEqual(selection["entryFrameSha256"], "a" * 64)
            self.assertEqual(selection["state"], "qmp_acknowledged")
            with self.assertRaises(r3bg.DuplicateInputAction):
                operations.select_sql_read_demo("a" * 64)
            self.assertEqual(poll_count, 2)


class UiEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="r3bg-ui-")
        self.png_path = Path(self.temp_dir.name) / "synthetic-screen.png"
        self.png_path.write_bytes(PNG)
        size = self.png_path.stat().st_size
        digest = hashlib.sha256(self.png_path.read_bytes()).hexdigest()
        self.png_sha256 = digest
        shared = {"source": "qmp-screendump", "path": str(self.png_path),
                  "bytes": size, "sha256": digest, "width": 1, "height": 1,
                  "guestIdentity": dict(GUEST), "synthetic": True}
        self.screenshots = [
            {**shared, "capturedAtUtc": "2026-11-14T20:00:00Z",
             "facts": ["desktop_visible", "expected_desktop_focus"]},
            {**shared, "capturedAtUtc": "2026-11-14T20:00:02Z",
             "afterActionId": "open-start-menu",
             "facts": ["start_menu_open", "start_menu_focus_verified"]},
            {**shared, "capturedAtUtc": "2026-11-14T20:00:04Z",
             "afterActionId": "open-start-menu",
             "facts": ["all_programs_visible", "sql_read_demo_visible"]},
        ]
        self.actions = [
            _action("open-start-menu", "2026-11-14T20:00:01Z"),
            _action("select-sql-read-demo", "2026-11-14T20:00:05Z",
                    appId="com.guidexos.sqlread", actionKind="select-application",
                    entryFrameSha256=self.png_sha256),
        ]
        self.events = [
            {"event": "app_launch_dispatch_selected", "appId": "com.guidexos.sqlread",
             "target": "SQL Read Demo", "resolvedType": "NativeElf",
             "selectedDispatch": "SQL Read Demo", "atUtc": "2026-11-14T20:00:06Z"},
            {"event": "native_elf_loader_invocation", "appName": "SQL Read Demo",
             "runtimeId": "runtime-1",
             "atUtc": "2026-11-14T20:00:07Z"},
            {"event": "gx_main_entry", "runtimeId": "runtime-1",
             "atUtc": "2026-11-14T20:00:08Z"},
        ]

    def tearDown(self):
        self.temp_dir.cleanup()

    def analyze(self):
        return r3bg.analyze_ui_evidence(self.screenshots, self.actions, self.events,
                                        expected_guest=GUEST)

    def test_ordered_synthetic_ui_and_loader_trace_passes_without_live_claim(self):
        result = self.analyze()
        self.assertEqual(result["result"], "PASS")
        self.assertTrue(result["synthetic"])
        self.assertFalse(result["liveGuestInteractionQualified"])
        self.assertFalse(result["qmpAcknowledgementUsedAsGuestReceipt"])

    def test_menu_failure_wrong_focus_and_wrong_application_selection_fail(self):
        self.screenshots[1]["facts"] = ["start_menu_focus_verified"]
        with self.assertRaisesRegex(r3bg.InputQualificationError, "required UI state"):
            self.analyze()
        self.screenshots[1]["facts"] = ["start_menu_open", "wrong_menu_focus"]
        with self.assertRaisesRegex(r3bg.InputQualificationError, "unexpected window"):
            self.analyze()
        self.screenshots[1]["facts"] = ["start_menu_open", "start_menu_focus_verified"]
        self.actions[1]["appId"] = "com.example.wrong"
        with self.assertRaisesRegex(r3bg.InputQualificationError, "wrong application"):
            self.analyze()

    def test_missing_or_stale_screenshot_fails(self):
        self.screenshots[2]["path"] = str(Path(self.temp_dir.name) / "missing.png")
        with self.assertRaisesRegex(r3bg.InputQualificationError, "screenshot is unavailable"):
            self.analyze()
        self.screenshots[2]["path"] = str(self.png_path)
        self.screenshots[1]["capturedAtUtc"] = "2026-11-14T20:00:00.500Z"
        with self.assertRaisesRegex(r3bg.InputQualificationError, "stale relative to the input event"):
            self.analyze()

    def test_selection_without_loader_and_loader_without_entry_fail(self):
        self.events = [event for event in self.events
                       if event["event"] != "native_elf_loader_invocation"]
        with self.assertRaisesRegex(r3bg.InputQualificationError, "without Native ELF loader"):
            self.analyze()
        self.events.append({"event": "native_elf_loader_invocation", "appName": "SQL Read Demo",
                            "runtimeId": "runtime-1",
                            "atUtc": "2026-11-14T20:00:07Z"})
        self.events = [event for event in self.events if event["event"] != "gx_main_entry"]
        with self.assertRaisesRegex(r3bg.InputQualificationError, "did not establish gx_main"):
            self.analyze()

    def test_loader_trace_for_another_guest_identity_fails(self):
        self.screenshots[0]["guestIdentity"] = {**GUEST, "qemuPid": 9999}
        with self.assertRaisesRegex(r3bg.InputQualificationError, "identity differs"):
            self.analyze()


if __name__ == "__main__":
    unittest.main(verbosity=2)
