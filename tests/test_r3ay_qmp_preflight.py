from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import threading
import time
import unittest
import uuid
from datetime import datetime, timezone

from tools import r3ay_qmp_preflight as qmp


EXE = r"C:\Program Files\QEMU\qemu-system-x86_64.exe"
START = "2026-11-12T03:06:53.646Z"


def endpoint(port=51288, host="127.0.0.1"):
    target = f"[{host}]" if ":" in host else host
    return qmp.QmpEndpoint.parse(f"tcp:{target}:{port},server=on,wait=off")


def admission(pid=28664, owner_pid=25408, start=START, state="Running", exe=EXE):
    rid = uuid.uuid4().hex
    owner = f"synthetic (PID {owner_pid})"
    relation = {
        "reservationId": rid, "state": state, "project": "guideXOSServer",
        "mode": "acceptance", "owner": owner, "ownerPidMatches": True,
        "qemuPid": pid, "processIdentity": exe,
        "listedProcess": {"pid": pid, "exe": exe, "start": start, "ownership": "managed"},
        "childIdentityMatches": True, "running": state == "Running", "pending": False,
        "reservation": {
            "id": rid, "project": "guideXOSServer", "owner": owner,
            "mode": "acceptance", "state": state, "processId": pid,
            "processStartTimeUtc": start, "processIdentity": exe,
            "ownerLiveness": "Verified" if state == "Running" else "Stopped",
        },
    }
    return {"reservationId": rid, "qemuPid": pid, "relationship": relation,
            "observedAtUtc": datetime.now(timezone.utc).isoformat()}


def observation(pid=28664, owner_pid=25408, start=START, port=51288,
                address="127.0.0.1", listener_pid=None, command_port=51288, exe=EXE):
    return {
        "process": {
            "ProcessId": pid, "ParentProcessId": owner_pid, "ExecutablePath": exe,
            "CommandLine": f'"{exe}" -machine pc -qmp tcp:127.0.0.1:{command_port},server=on,wait=off',
            "CreationTimeUtc": start,
        },
        "listeners": ([{
            "LocalAddress": address, "LocalPort": port, "State": "Listen",
            "OwningProcess": listener_pid if listener_pid is not None else pid,
        }] if port else []),
    }


def trusted_evidence(state=qmp.OwnershipState.OWNED_LISTENER_VERIFIED):
    return qmp.OwnershipEvidence(
        state, endpoint(), "a" * 32, 28664, 25408, "test",
        qac_observed_at_utc=datetime.now(timezone.utc).isoformat())


class EndpointTests(unittest.TestCase):
    def test_ipv4_ipv6_and_ipv4_mapped_representation(self):
        ep = qmp.QmpEndpoint.from_qemu_args(
            ["-machine", "pc", "-qmp", "tcp:127.0.0.1:51288,server=on,wait=off"],
            requested_port=51288)
        self.assertEqual(ep, endpoint())
        self.assertEqual(qmp.parse_qmp_command_line(
            'qemu.exe -qmp "tcp:[::1]:52000,server=on,wait=off"'), endpoint(52000, "::1"))
        result = qmp.evaluate_ownership(admission(), endpoint(),
            observation(address="::ffff:127.0.0.1"), expected_executable=EXE)
        self.assertEqual(result.state, qmp.OwnershipState.OWNED_LISTENER_VERIFIED)
        windows_bound = observation()
        windows_bound["listeners"][0].update(
            {"State": None, "RemoteAddress": "0.0.0.0", "RemotePort": 0})
        result = qmp.evaluate_ownership(admission(), endpoint(), windows_bound,
                                        expected_executable=EXE)
        self.assertEqual(result.state, qmp.OwnershipState.OWNED_LISTENER_VERIFIED)

    def test_rejects_wrong_port_transport_wildcard_duplicates_and_missing_server(self):
        with self.assertRaisesRegex(qmp.PreflightError, "differs"):
            qmp.QmpEndpoint.from_qemu_args(
                ["-qmp", "tcp:127.0.0.1:51289,server=on,wait=off"], requested_port=51288)
        for value in ("tcp:0.0.0.0:51288,server=on", "unix:C:/qmp.sock,server=on",
                      "tcp:127.0.0.1:0,server=on", "tcp:127.0.0.1:51288",
                      "tcp:127.0.0.1:51288,server=on,server=on",
                      "tcp:127.0.0.1:51288,server=on,wait=on"):
            with self.subTest(value=value), self.assertRaises(qmp.PreflightError):
                qmp.QmpEndpoint.parse(value)
        with self.assertRaisesRegex(qmp.PreflightError, "exactly one"):
            qmp.QmpEndpoint.from_qemu_args(
                ["-qmp", "tcp:127.0.0.1:1,server=on", "-qmp", "bad"])


class OwnershipTests(unittest.TestCase):
    def verify(self, obs=None, qac=None, ep=None, exe=EXE):
        return qmp.evaluate_ownership(qac or admission(), ep or endpoint(),
                                      obs or observation(), expected_executable=exe)

    def test_owned_foreign_unexpected_missing_and_wrong_port_are_distinct(self):
        self.assertEqual(self.verify().state, qmp.OwnershipState.OWNED_LISTENER_VERIFIED)
        self.assertEqual(self.verify(observation(listener_pid=99999)).state,
                         qmp.OwnershipState.FOREIGN_LISTENER)
        self.assertEqual(self.verify(observation(address="127.0.0.2")).state,
                         qmp.OwnershipState.UNEXPECTED_LISTENER)
        missing = observation(); missing["listeners"] = []
        self.assertEqual(self.verify(missing).state, qmp.OwnershipState.LISTENER_MISSING)
        self.assertEqual(self.verify(observation(port=51289)).state,
                         qmp.OwnershipState.WRONG_PORT)

    def test_stale_reservation_pid_reuse_and_live_identity_mismatch_fail_closed(self):
        self.assertEqual(self.verify(qac=admission(state="Released")).state,
                         qmp.OwnershipState.OWNERSHIP_UNKNOWN)
        stale_snapshot = admission()
        stale_snapshot["observedAtUtc"] = "2026-11-11T00:00:00Z"
        self.assertEqual(self.verify(qac=stale_snapshot).state,
                         qmp.OwnershipState.OWNERSHIP_UNKNOWN)
        stale_qac = admission()
        stale_qac["relationship"]["listedProcess"]["start"] = "2026-11-12T03:06:40Z"
        self.assertEqual(self.verify(qac=stale_qac).state,
                         qmp.OwnershipState.OWNERSHIP_UNKNOWN)
        reused = observation(start="2026-11-12T03:06:50.000Z")
        result = self.verify(reused)
        self.assertEqual(result.state, qmp.OwnershipState.OWNERSHIP_UNKNOWN)
        self.assertIn("start differs", result.reason)
        wrong_exe = observation(exe=r"C:\Temp\qemu-system-x86_64.exe")
        self.assertEqual(self.verify(wrong_exe).state, qmp.OwnershipState.OWNERSHIP_UNKNOWN)
        wrong_command = observation(command_port=51289)
        self.assertEqual(self.verify(wrong_command).state, qmp.OwnershipState.OWNERSHIP_UNKNOWN)

    def test_wrong_qac_pid_is_foreign_and_os_pid_reuse_is_unknown(self):
        wrong_qac_pid = admission(pid=28665)
        self.assertEqual(self.verify(qac=wrong_qac_pid).state,
                         qmp.OwnershipState.FOREIGN_LISTENER)
        reused = observation(pid=99999, listener_pid=28664)
        self.assertEqual(self.verify(reused).state, qmp.OwnershipState.OWNERSHIP_UNKNOWN)

    def test_malformed_observation_and_os_inspection_errors_are_unknown(self):
        malformed = observation()
        malformed["listeners"] = [{"LocalAddress": "not-an-ip", "LocalPort": 51288,
                                   "State": "Listen", "OwningProcess": 28664}]
        self.assertEqual(self.verify(malformed).state, qmp.OwnershipState.OWNERSHIP_UNKNOWN)

        def fail(_pid, _port):
            raise qmp.PreflightError("Get-NetTCPConnection denied; stderr=access denied")

        result = qmp.poll_ownership(admission(), endpoint(), expected_executable=EXE,
                                    timeout_seconds=0, observe=fail)
        self.assertEqual(result.state, qmp.OwnershipState.OWNERSHIP_UNKNOWN)
        self.assertIn("access denied", result.reason)

    def test_accepted_connection_tuple_must_belong_to_same_child(self):
        obs = observation()
        peer = {"LocalAddress": "127.0.0.1", "LocalPort": 51288,
                "RemoteAddress": "127.0.0.1", "RemotePort": 60123,
                "State": "Established", "OwningProcess": 28664}
        obs["listeners"].append(peer)
        verified = self.verify(obs)
        qmp.require_connected_peer(verified, endpoint(), ("127.0.0.1", 60123))
        peer["OwningProcess"] = 99999
        mismatched = self.verify(obs)
        with self.assertRaisesRegex(qmp.PreflightError, "TCP tuple"):
            qmp.require_connected_peer(mismatched, endpoint(), ("127.0.0.1", 60123))

    def test_delayed_readiness_is_transient_and_missing_is_final(self):
        snapshots = [observation(), observation()]
        snapshots[0]["listeners"] = []
        result = qmp.poll_ownership(admission(), endpoint(), expected_executable=EXE,
                                    timeout_seconds=0.05, interval_seconds=0,
                                    observe=lambda _pid, _port: snapshots.pop(0))
        self.assertEqual(result.state, qmp.OwnershipState.OWNED_LISTENER_VERIFIED)
        self.assertEqual(result.observation["attempts"][0]["state"],
                         qmp.OwnershipState.LISTENER_NOT_YET_READY.value)
        missing = observation(); missing["listeners"] = []
        result = qmp.poll_ownership(admission(), endpoint(), expected_executable=EXE,
                                    timeout_seconds=0, observe=lambda _pid, _port: missing)
        self.assertEqual(result.state, qmp.OwnershipState.LISTENER_MISSING)


class ProtocolTests(unittest.TestCase):
    def test_register_widths_full_precision_wp_la57_and_cr3_active(self):
        response = {"id": "regs",
                    "return": "CR0=80010033 CR3=000000003DD3C000 CR4=00000668"}
        cr0 = qmp.parse_qmp_register(response, "regs", "cr0")
        cr3 = qmp.parse_qmp_register(response, "regs", "CR3")
        cr4 = qmp.parse_qmp_register(response, "regs", "CR4")
        self.assertEqual((cr0, cr3, cr4), (0x80010033, 0x3DD3C000, 0x668))
        self.assertEqual(qmp.parse_qmp_register(
            {"id": "r", "return": "CR4=0000000100001000"}, "r", "CR4"),
            0x0000000100001000)
        self.assertEqual(qmp.inspect_register_policy(cr0, cr4),
                         {"cr0_wp": True, "cr4_la57": False, "four_level_supported": True})
        qmp.require_four_level_paging(cr4)
        self.assertTrue(qmp.inspect_register_policy(cr0, 1 << 12)["cr4_la57"])
        self.assertFalse(qmp.inspect_register_policy(cr0, 1 << 12)["four_level_supported"])
        with self.assertRaisesRegex(qmp.PreflightError, "unsupported five-level"):
            qmp.require_four_level_paging(1 << 12)
        self.assertFalse(qmp.inspect_register_policy(0x33, cr4)["cr0_wp"])
        self.assertEqual(qmp.parse_cr3_active(
            "SQL_R3AU_MAP stage=MAP-E cr3_active=000000003DD3C000 cr0_wp=1"), 0x3DD3C000)
        for value in ({"id": "x", "return": "CR0=80010033"},
                      {"id": "regs", "return": "CR0=123456789"},
                      {"id": "regs", "return": "CR0=80010033 CR0=80010033"}):
            with self.subTest(value=value), self.assertRaises(qmp.PreflightError):
                qmp.parse_qmp_register(value, "regs", "CR0")
        for record in ("SQL_R3AU_MAP stage=MAP-E cr0=80010033",
                       "cr3_active=3DD3C000",
                       "cr3_active=000000003DD3C000 cr3_active=000000003DD3C000"):
            with self.subTest(record=record), self.assertRaises(qmp.PreflightError):
                qmp.parse_cr3_active(record)

    def test_partial_reads_utf8_crlf_and_no_bom(self):
        left, right = socket.socketpair()
        client = qmp.QmpClient(left, endpoint(), trusted_evidence())
        try:
            frame = '{"QMP":{"version":{"package":"tést"}}}\r\n'.encode("utf-8")
            for chunk in (frame[:8], frame[8:20], frame[20:]):
                right.sendall(chunk)
            self.assertEqual(client._receive()["QMP"]["version"]["package"], "tést")
            wire = client._send({"execute": "qmp_capabilities", "id": "r3ay-1"})
            self.assertTrue(wire.endswith(b"\r\n"))
            self.assertNotIn(b"\xef\xbb\xbf", wire)
            self.assertEqual(json.loads(right.recv(4096).decode("utf-8"))["execute"],
                             "qmp_capabilities")
        finally:
            client.close(); right.close()

    def test_invalid_truncated_and_uncorrelated_qmp_frames_fail(self):
        for payload, error in ((b"not-json\r\n", "valid UTF-8 JSON"),
                               (b'{"event":"RESET"}\n', "CRLF"),
                               (b'{"QMP":', "truncated frame")):
            left, right = socket.socketpair()
            client = qmp.QmpClient(left, endpoint(), trusted_evidence())
            try:
                right.sendall(payload)
                if payload == b'{"QMP":':
                    right.close()
                with self.assertRaisesRegex(qmp.QmpProtocolError, error):
                    client._receive()
            finally:
                client.close()
                try: right.close()
                except OSError: pass

        left, right = socket.socketpair()
        client = qmp.QmpClient(left, endpoint(), trusted_evidence())
        def wrong_id():
            right.recv(4096)
            right.sendall(b'{"return":{},"id":"wrong-id"}\r\n')
        thread = threading.Thread(target=wrong_id)
        thread.start()
        try:
            with self.assertRaisesRegex(qmp.QmpProtocolError, "does not match"):
                client.command("qmp_capabilities")
        finally:
            client.close(); right.close(); thread.join(timeout=2)

    def test_unverified_or_changed_ownership_blocks_connection_and_quit(self):
        left, right = socket.socketpair()
        try:
            with self.assertRaisesRegex(qmp.PreflightError, "not verified"):
                qmp.QmpClient(left, endpoint(),
                    trusted_evidence(qmp.OwnershipState.OWNERSHIP_UNKNOWN))
            client = qmp.QmpClient(left, endpoint(), trusted_evidence())
            client.capabilities_ready = True
            for operation, arguments in (("quit", None), ("cont", None),
                                         ("system_reset", None),
                                         ("human-monitor-command", {"command-line": "quit"})):
                with self.subTest(operation=operation), self.assertRaises(qmp.PreflightError):
                    client.command(operation, arguments)
            changed = qmp.OwnershipEvidence(
                qmp.OwnershipState.OWNED_LISTENER_VERIFIED, endpoint(), "b" * 32,
                28664, 25408, "changed reservation",
                qac_observed_at_utc=datetime.now(timezone.utc).isoformat())
            with self.assertRaisesRegex(qmp.PreflightError, "identity changed"):
                client.quit(changed)
            stale = qmp.OwnershipEvidence(
                qmp.OwnershipState.OWNED_LISTENER_VERIFIED, endpoint(), "a" * 32,
                28664, 25408, "stale snapshot",
                verified_at_monotonic=time.monotonic() - 3)
            with self.assertRaisesRegex(qmp.PreflightError, "stale"):
                client.quit(stale)
            right.setblocking(False)
            with self.assertRaises(BlockingIOError):
                right.recv(4096)
        finally:
            left.close(); right.close()


@unittest.skipUnless(os.name == "nt", "real listener attribution requires Windows")
class WindowsOwnershipIntegrationTest(unittest.TestCase):
    def test_real_os_attribution_synthetic_qmp_handshake_and_owned_quit(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        ep = endpoint(port)
        rid = uuid.uuid4().hex
        server_code = r'''
import json, socket, sys, time
value = sys.argv[sys.argv.index("-qmp") + 1]
hostport = value.split(",", 1)[0].removeprefix("tcp:")
host, port = hostport.rsplit(":", 1)
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.bind((host, int(port)))
server.listen(1)
conn, _ = server.accept()
conn.sendall(b'{"QMP":')
time.sleep(0.01)
conn.sendall(b'{"version":{"qemu":{"major":11,"minor":0,"micro":0}},"capabilities":[]}}\r\n')
running = False
while True:
    raw = b""
    while not raw.endswith(b"\n"):
        part = conn.recv(4096)
        if not part: raise SystemExit(0)
        raw += part
    request = json.loads(raw)
    name = request["execute"]
    if name == "query-status":
        response = {"return":{"status":"running" if running else "prelaunch","running":running}}
    elif name == "cont":
        running = True
        response = {"return":{}}
    elif name == "human-monitor-command":
        response = {"return":"CR0=80010033 CR3=000000003DD3C000 CR4=00000668"}
    else: response = {"return":{}}
    response["id"] = request["id"]
    wire = json.dumps(response, separators=(",", ":")).encode() + b"\r\n"
    middle = max(1, len(wire) // 2)
    conn.sendall(wire[:middle]); time.sleep(0.005); conn.sendall(wire[middle:])
    if name == "quit": break
conn.close(); server.close()
'''
        child = subprocess.Popen([sys.executable, "-u", "-c", server_code, "-qmp", ep.qemu_value],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE, text=True, encoding="utf-8")
        client = None
        try:
            deadline = time.monotonic() + 10
            raw = None
            while time.monotonic() < deadline:
                if child.poll() is not None:
                    self.fail("synthetic endpoint child exited before listener qualification")
                raw = qmp.observe_windows_process_and_listeners(child.pid, port)
                if isinstance(raw.get("process"), dict):
                    break
                time.sleep(0.1)
            self.assertIsInstance(raw.get("process"), dict)
            process = raw["process"]
            owner_pid = os.getpid()
            start = process["CreationTimeUtc"]
            exe = process["ExecutablePath"]
            owner = f"offline synthetic reservation (PID {owner_pid})"
            relation = {
                "reservationId": rid, "state": "Running", "project": "guideXOSServer",
                "mode": "acceptance", "owner": owner, "ownerPidMatches": True,
                "qemuPid": child.pid, "processIdentity": exe,
                "listedProcess": {"pid": child.pid, "exe": exe, "start": start,
                                  "ownership": "managed"},
                "childIdentityMatches": True, "running": True, "pending": False,
                "reservation": {
                    "id": rid, "project": "guideXOSServer", "owner": owner,
                    "mode": "acceptance", "state": "Running", "processId": child.pid,
                    "processStartTimeUtc": start, "processIdentity": exe,
                    "ownerLiveness": "Verified",
                },
            }
            test_admission = {"reservationId": rid, "qemuPid": child.pid,
                              "relationship": relation,
                              "observedAtUtc": datetime.now(timezone.utc).isoformat()}
            def refresh_ownership():
                test_admission["observedAtUtc"] = datetime.now(timezone.utc).isoformat()
                return qmp.poll_ownership(test_admission, ep, expected_executable=exe,
                                          timeout_seconds=1, interval_seconds=0.1)

            owned = qmp.poll_ownership(test_admission, ep, expected_executable=exe,
                                       timeout_seconds=5, interval_seconds=0.1)
            self.assertEqual(owned.state, qmp.OwnershipState.OWNED_LISTENER_VERIFIED,
                             f"{owned.reason}; snapshot={owned.observation!r}")
            client = qmp.QmpClient.connect_verified(ep, owned, timeout_seconds=5,
                                                    expected_running=False,
                                                    reverify=refresh_ownership)
            self.assertFalse(client.initial_status["running"])
            before_cont = refresh_ownership()
            self.assertEqual(before_cont.state, qmp.OwnershipState.OWNED_LISTENER_VERIFIED,
                             before_cont.reason)
            resumed = client.continue_guest(before_cont)
            self.assertTrue(resumed["return"]["running"])
            reply = client.command("human-monitor-command",
                                   {"command-line": "info registers"})
            self.assertEqual(qmp.parse_qmp_register(reply, reply["id"], "CR0"), 0x80010033)
            fresh = refresh_ownership()
            self.assertEqual(fresh.state, qmp.OwnershipState.OWNED_LISTENER_VERIFIED,
                             f"{fresh.reason}; snapshot={fresh.observation!r}")
            self.assertNotIn("error", client.quit(fresh))
            child.wait(timeout=5)
            self.assertEqual(child.returncode, 0)
        finally:
            if client is not None:
                client.close()
            if child.poll() is None:
                child.terminate()
                try: child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait(timeout=3)
            if child.stderr:
                child.stderr.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
