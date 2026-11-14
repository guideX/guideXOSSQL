"""Fail-closed QMP endpoint ownership and protocol helpers for R3AZ.

This module is intentionally independent of QAC admission. Callers must pass a
fresh QAC relationship and the exact QEMU executable/arguments expected for the
reserved child. Windows process and listener observations are collected from
the OS; an open port or QMP greeting is never treated as ownership evidence.
"""

from __future__ import annotations

import datetime as _datetime
import enum
import ipaddress
import json
import os
import re
import socket
import subprocess
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable


class PreflightError(RuntimeError):
    """A malformed or incomplete QMP ownership observation."""


class QmpProtocolError(RuntimeError):
    """A malformed, truncated, or uncorrelated QMP wire message."""


class OwnershipState(str, enum.Enum):
    CONFIG_INVALID = "config_invalid"
    LISTENER_NOT_YET_READY = "listener_not_yet_ready"
    LISTENER_MISSING = "listener_missing"
    OWNERSHIP_UNKNOWN = "ownership_unknown"
    FOREIGN_LISTENER = "foreign_listener"
    UNEXPECTED_LISTENER = "unexpected_listener"
    WRONG_PORT = "wrong_port"
    OWNED_LISTENER_VERIFIED = "owned_listener_verified"


MAX_QAC_SNAPSHOT_AGE_SECONDS = 2.0


def _utc_now() -> _datetime.datetime:
    """Return an aware UTC wall clock for QAC snapshot-age checks."""
    return _datetime.datetime.now(_datetime.timezone.utc)


def _monotonic() -> float:
    """Return a monotonic clock for local elapsed-time checks."""
    return time.monotonic()


@dataclass(frozen=True)
class QmpEndpoint:
    transport: str
    host: str
    port: int
    server: bool
    wait: str | None

    @classmethod
    def from_qemu_args(
        cls, args: Iterable[str], *, requested_port: int | None = None
    ) -> "QmpEndpoint":
        values = list(args)
        switches = [i for i, value in enumerate(values) if value == "-qmp"]
        if len(switches) != 1 or switches[0] + 1 >= len(values):
            raise PreflightError("QEMU arguments must contain exactly one complete -qmp pair")
        endpoint = cls.parse(values[switches[0] + 1])
        if requested_port is not None and endpoint.port != requested_port:
            raise PreflightError(
                f"configured QMP port {endpoint.port} differs from requested port {requested_port}"
            )
        return endpoint

    @classmethod
    def parse(cls, value: str) -> "QmpEndpoint":
        if not isinstance(value, str) or not value:
            raise PreflightError("QMP endpoint is empty")
        address, *option_parts = value.split(",")
        if ":" not in address:
            raise PreflightError("QMP endpoint has no transport separator")
        transport, target = address.split(":", 1)
        if transport.lower() != "tcp":
            raise PreflightError(f"unsupported QMP transport {transport!r}; TCP is required here")
        if target.startswith("["):
            close = target.find("]")
            if close < 0 or target[close + 1 : close + 2] != ":":
                raise PreflightError("bracketed IPv6 QMP endpoint is malformed")
            host, raw_port = target[1:close], target[close + 2 :]
        else:
            host, separator, raw_port = target.rpartition(":")
            if not separator:
                raise PreflightError("TCP QMP endpoint must include an explicit host and port")
        try:
            parsed_host = ipaddress.ip_address(host.split("%", 1)[0])
        except ValueError as exc:
            raise PreflightError("QMP host must be an explicit numeric loopback address") from exc
        if not parsed_host.is_loopback:
            raise PreflightError("QMP must bind an explicit loopback address")
        try:
            port = int(raw_port, 10)
        except ValueError as exc:
            raise PreflightError("QMP port is not a decimal integer") from exc
        if not 1 <= port <= 65535:
            raise PreflightError("QMP port must be in the range 1..65535")
        options: dict[str, str] = {}
        for part in option_parts:
            if "=" not in part:
                raise PreflightError(f"malformed QMP option {part!r}")
            key, option_value = part.split("=", 1)
            if not key or key in options:
                raise PreflightError(f"duplicate or empty QMP option {key!r}")
            options[key] = option_value
        if options.get("server") != "on":
            raise PreflightError("QMP must explicitly enable server=on")
        if options.get("wait") != "off":
            raise PreflightError("QMP must explicitly set wait=off for bounded readiness")
        return cls("tcp", str(parsed_host), port, True, options.get("wait"))

    @property
    def qemu_value(self) -> str:
        host = f"[{self.host}]" if ipaddress.ip_address(self.host).version == 6 else self.host
        wait = f",wait={self.wait}" if self.wait is not None else ""
        return f"tcp:{host}:{self.port},server=on{wait}"


def parse_qmp_command_line(command_line: str) -> QmpEndpoint:
    """Extract one -qmp value from the live Win32_Process command line."""
    matches = re.findall(
        r"(?<!\S)-qmp\s+(?:\"([^\"]+)\"|'([^']+)'|(\S+))",
        command_line,
        flags=re.IGNORECASE,
    )
    if len(matches) != 1:
        raise PreflightError("live process command line must contain exactly one -qmp value")
    value = next(part for part in matches[0] if part)
    return QmpEndpoint.parse(value)


def _ip(value: Any) -> ipaddress.IPv4Address | ipaddress.IPv6Address:
    if not isinstance(value, str):
        raise PreflightError("listener address is not text")
    text = value.strip().strip("[]").split("%", 1)[0]
    address = ipaddress.ip_address(text)
    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
        return address.ipv4_mapped
    return address


def _same_path(left: Any, right: Any) -> bool:
    if not isinstance(left, str) or not isinstance(right, str) or not left or not right:
        return False
    normalize = lambda value: os.path.normcase(os.path.normpath(value)).replace("/", "\\").casefold()
    return normalize(left) == normalize(right)


def _parse_time(value: Any, *, field_name: str = "process start time") -> _datetime.datetime:
    if not isinstance(value, str):
        raise PreflightError(f"{field_name} is missing")
    match = re.fullmatch(r"/Date\((-?\d+)(?:[+-]\d+)?\)/", value)
    if match:
        return _datetime.datetime.fromtimestamp(
            int(match.group(1)) / 1000, tz=_datetime.timezone.utc
        )
    text = value.replace("Z", "+00:00")
    try:
        parsed = _datetime.datetime.fromisoformat(text)
    except ValueError as exc:
        raise PreflightError(f"{field_name} has an unsupported format") from exc
    if parsed.tzinfo is None:
        raise PreflightError(f"{field_name} must include a UTC offset")
    return parsed.astimezone(_datetime.timezone.utc)


def _owner_pid(owner: Any) -> int | None:
    match = re.search(r"\(PID\s+(\d+)\)", str(owner))
    return int(match.group(1)) if match else None


def _is_listener(row: dict[str, Any]) -> bool:
    """Recognize Windows Listen rows and the observed null-State bound-socket form."""
    state = str(row.get("State") or "").casefold()
    if state in ("listen", "listening"):
        return True
    if state:
        return False
    try:
        remote_port = int(row.get("RemotePort"))
        remote_address = _ip(row.get("RemoteAddress"))
    except (TypeError, ValueError, PreflightError) as exc:
        raise PreflightError("Windows TCP row has no usable listener state or remote endpoint") from exc
    return remote_port == 0 and remote_address.is_unspecified


def validate_qac_relationship(admission: Any) -> tuple[dict[str, Any], dict[str, Any]]:
    """Validate the current Running reservation and return (relationship, reservation)."""
    if not isinstance(admission, dict):
        raise PreflightError("QAC admission must be a JSON object")
    observed_at = _parse_time(admission.get("observedAtUtc"), field_name="QAC observation timestamp")
    qac_age = (_utc_now() - observed_at).total_seconds()
    if qac_age < -0.5 or qac_age > MAX_QAC_SNAPSHOT_AGE_SECONDS:
        raise PreflightError(f"QAC reservation observation is stale or future-dated ({qac_age:.3f} seconds)")
    return _validate_qac_relationship_identity(admission)


def _validate_qac_relationship_identity(admission: Any) -> tuple[dict[str, Any], dict[str, Any]]:
    """Check QAC identity fields without using the snapshot as fresh authorization."""
    if not isinstance(admission, dict):
        raise PreflightError("QAC admission must be a JSON object")
    relationship = admission.get("relationship")
    if not isinstance(relationship, dict):
        raise PreflightError("QAC reservation/process relationship is unavailable")
    reservation = relationship.get("reservation")
    process = relationship.get("listedProcess")
    reservation_id = admission.get("reservationId")
    pid = admission.get("qemuPid")
    if not isinstance(reservation, dict) or not isinstance(process, dict):
        raise PreflightError("QAC reservation or managed process identity is unavailable")
    expected = (
        isinstance(reservation_id, str)
        and bool(reservation_id)
        and relationship.get("reservationId") == reservation_id
        and reservation.get("id") == reservation_id
        and relationship.get("state") == "Running"
        and reservation.get("state") == "Running"
        and relationship.get("project") == "guideXOSServer"
        and reservation.get("project") == "guideXOSServer"
        and relationship.get("mode") == "acceptance"
        and reservation.get("mode") == "acceptance"
        and relationship.get("ownerPidMatches") is True
        and reservation.get("ownerLiveness") == "Verified"
        and relationship.get("childIdentityMatches") is True
        and relationship.get("running") is True
        and relationship.get("pending") is False
        and isinstance(pid, int)
        and not isinstance(pid, bool)
        and pid > 0
        and relationship.get("qemuPid") == pid
        and reservation.get("processId") == pid
        and process.get("pid") == pid
        and isinstance(relationship.get("processIdentity"), str)
        and reservation.get("processIdentity") == relationship.get("processIdentity")
        and process.get("exe") == relationship.get("processIdentity")
    )
    if not expected:
        raise PreflightError("QAC has not verified this exact Running acceptance reservation and child")
    owner_pid = _owner_pid(reservation.get("owner"))
    if owner_pid is None or owner_pid != _owner_pid(relationship.get("owner")):
        raise PreflightError("QAC owner PID is absent or inconsistent")
    try:
        listed_start = _parse_time(process.get("start"), field_name="QAC listed child start time")
        reservation_start = _parse_time(reservation.get("processStartTimeUtc"),
                                        field_name="QAC reservation child start time")
    except PreflightError as exc:
        raise PreflightError(f"QAC listed child start identity is unavailable: {exc}") from exc
    if abs((listed_start - reservation_start).total_seconds() * 1000) > 5:
        raise PreflightError("QAC listed child start time does not match the reservation identity")
    return relationship, reservation


@dataclass(frozen=True)
class OwnershipEvidence:
    state: OwnershipState
    endpoint: QmpEndpoint | None
    reservation_id: str | None
    qemu_pid: int | None
    owner_pid: int | None
    reason: str
    observation: dict[str, Any] = field(default_factory=dict)
    verified_at_monotonic: float = field(default_factory=lambda: _monotonic())
    qac_observed_at_utc: str | None = None

    @property
    def verified(self) -> bool:
        if self.state is not OwnershipState.OWNED_LISTENER_VERIFIED or not self.qac_observed_at_utc:
            return False
        age = _monotonic() - self.verified_at_monotonic
        if age < 0 or age > MAX_QAC_SNAPSHOT_AGE_SECONDS:
            return False
        try:
            qac_age = (_utc_now() - _parse_time(
                self.qac_observed_at_utc, field_name="QAC observation timestamp")).total_seconds()
        except PreflightError:
            return False
        return -0.5 <= qac_age <= MAX_QAC_SNAPSHOT_AGE_SECONDS


def _process_matches(
    process: dict[str, Any],
    relationship: dict[str, Any],
    reservation: dict[str, Any],
    expected_executable: str,
    endpoint: QmpEndpoint,
    *,
    start_tolerance_ms: int,
) -> tuple[bool, str]:
    pid = relationship.get("qemuPid")
    owner_pid = _owner_pid(reservation.get("owner"))
    if process.get("ProcessId") != pid:
        return False, "OS process PID does not match the QAC child PID"
    if process.get("ParentProcessId") != owner_pid:
        return False, "OS process parent PID does not match the verified QAC owner"
    if not _same_path(process.get("ExecutablePath"), expected_executable):
        return False, "OS executable path does not match the expected QEMU executable"
    if not _same_path(process.get("ExecutablePath"), relationship.get("processIdentity")):
        return False, "OS executable path does not match the QAC process identity"
    try:
        live_start = _parse_time(process.get("CreationTimeUtc"))
        qac_start = _parse_time(reservation.get("processStartTimeUtc"))
    except PreflightError as exc:
        return False, str(exc)
    delta_ms = abs((live_start - qac_start).total_seconds() * 1000)
    if delta_ms > start_tolerance_ms:
        return False, f"OS process start differs from QAC child identity by {delta_ms:.3f} ms"
    command_line = process.get("CommandLine")
    if not isinstance(command_line, str) or not command_line:
        return False, "OS command line is unavailable"
    try:
        live_endpoint = parse_qmp_command_line(command_line)
    except PreflightError as exc:
        return False, f"live QEMU command line has no valid QMP endpoint: {exc}"
    if live_endpoint != endpoint:
        return False, "live QEMU command-line QMP endpoint differs from the requested endpoint"
    return True, "OS process identity and QMP command-line endpoint match QAC"


def evaluate_ownership(
    admission: Any,
    endpoint: QmpEndpoint,
    observation: dict[str, Any],
    *,
    expected_executable: str,
    start_tolerance_ms: int = 5,
    _readiness_only: bool = False,
) -> OwnershipEvidence:
    """Classify one OS snapshot without converting inspection errors into ownership mismatch."""
    try:
        relationship, reservation = (
            _validate_qac_relationship_identity(admission)
            if _readiness_only else validate_qac_relationship(admission)
        )
    except PreflightError as exc:
        return OwnershipEvidence(
            OwnershipState.OWNERSHIP_UNKNOWN,
            endpoint,
            admission.get("reservationId") if isinstance(admission, dict) else None,
            admission.get("qemuPid") if isinstance(admission, dict) else None,
            None,
            str(exc),
            observation,
        )
    qpid = relationship["qemuPid"]
    owner_pid = _owner_pid(reservation.get("owner"))
    reservation_id = admission["reservationId"]
    process = observation.get("process") if isinstance(observation, dict) else None
    listeners = observation.get("listeners") if isinstance(observation, dict) else None
    if not isinstance(process, dict):
        return OwnershipEvidence(OwnershipState.OWNERSHIP_UNKNOWN, endpoint, reservation_id, qpid, owner_pid,
                                 "OS process observation is missing", observation)
    if not isinstance(listeners, list):
        return OwnershipEvidence(OwnershipState.OWNERSHIP_UNKNOWN, endpoint, reservation_id, qpid, owner_pid,
                                 "OS listener observation is missing or malformed", observation)

    process_ok, process_reason = _process_matches(
        process, relationship, reservation, expected_executable, endpoint,
        start_tolerance_ms=start_tolerance_ms,
    )
    same_port: list[dict[str, Any]] = []
    child_listeners: list[dict[str, Any]] = []
    for row in listeners:
        try:
            row_port = int(row.get("LocalPort"))
            row_pid = int(row.get("OwningProcess"))
        except (AttributeError, TypeError, ValueError):
            return OwnershipEvidence(OwnershipState.OWNERSHIP_UNKNOWN, endpoint, reservation_id, qpid, owner_pid,
                                     "OS listener row is malformed", observation)
        try:
            listener = _is_listener(row)
        except PreflightError as exc:
            return OwnershipEvidence(OwnershipState.OWNERSHIP_UNKNOWN, endpoint, reservation_id, qpid, owner_pid,
                                     str(exc), observation)
        if not listener:
            continue
        if row_port == endpoint.port:
            same_port.append(row)
        if row_pid == qpid:
            child_listeners.append(row)

    if not same_port:
        if child_listeners:
            return OwnershipEvidence(OwnershipState.WRONG_PORT, endpoint, reservation_id, qpid, owner_pid,
                                     "QAC child owns a listening socket, but not on the requested QMP port", observation)
        return OwnershipEvidence(OwnershipState.LISTENER_MISSING, endpoint, reservation_id, qpid, owner_pid,
                                 "no listener is present on the requested QMP port", observation)

    target: list[dict[str, Any]] = []
    for row in same_port:
        try:
            address = _ip(row.get("LocalAddress"))
        except (PreflightError, ValueError):
            return OwnershipEvidence(OwnershipState.OWNERSHIP_UNKNOWN, endpoint, reservation_id, qpid, owner_pid,
                                     "OS returned an unparseable listener address", observation)
        expected_address = _ip(endpoint.host)
        if address == expected_address:
            target.append(row)
    if not target:
        return OwnershipEvidence(OwnershipState.UNEXPECTED_LISTENER, endpoint, reservation_id, qpid, owner_pid,
                                 "listeners exist on the requested port but not on the configured endpoint address", observation)
    if len(target) != 1 or len(same_port) != 1:
        return OwnershipEvidence(OwnershipState.UNEXPECTED_LISTENER, endpoint, reservation_id, qpid, owner_pid,
                                 "multiple or additional listeners exist on the requested port", observation)
    row_pid = int(target[0]["OwningProcess"])
    if row_pid != qpid:
        return OwnershipEvidence(OwnershipState.FOREIGN_LISTENER, endpoint, reservation_id, qpid, owner_pid,
                                 f"requested endpoint listener belongs to PID {row_pid}, not QAC child PID {qpid}", observation)
    if not process_ok:
        return OwnershipEvidence(OwnershipState.OWNERSHIP_UNKNOWN, endpoint, reservation_id, qpid, owner_pid,
                                 process_reason, observation)
    qac_observed_at = admission.get("observedAtUtc")
    if not _readiness_only:
        try:
            qac_age = (_utc_now() - _parse_time(
                qac_observed_at, field_name="QAC observation timestamp")).total_seconds()
        except PreflightError as exc:
            return OwnershipEvidence(OwnershipState.OWNERSHIP_UNKNOWN, endpoint, reservation_id, qpid,
                                     owner_pid, str(exc), observation)
        if qac_age < -0.5 or qac_age > MAX_QAC_SNAPSHOT_AGE_SECONDS:
            return OwnershipEvidence(OwnershipState.OWNERSHIP_UNKNOWN, endpoint, reservation_id, qpid, owner_pid,
                                     f"QAC reservation snapshot expired during OS inspection ({qac_age:.3f} seconds)",
                                     observation)
    return OwnershipEvidence(OwnershipState.OWNED_LISTENER_VERIFIED, endpoint, reservation_id, qpid, owner_pid,
                             "exact endpoint listener PID, QAC child identity, and live command line agree",
                             observation, qac_observed_at_utc=qac_observed_at)


def _powershell_json(script: str, *, timeout_seconds: float = 10.0) -> dict[str, Any]:
    """Run one read-only Windows process/listener observation and preserve its errors."""
    if os.name != "nt":
        raise PreflightError("Windows listener ownership observation requires Windows")
    command = ["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script]
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise PreflightError(f"PowerShell OS inspection failed: {exc!r}") from exc
    if result.returncode != 0:
        raise PreflightError(
            "PowerShell OS inspection returned a nonzero exit code "
            f"{result.returncode}; stdout={result.stdout!r}; stderr={result.stderr!r}"
        )
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise PreflightError(
            f"PowerShell OS inspection returned invalid JSON; stdout={result.stdout!r}; "
            f"stderr={result.stderr!r}"
        ) from exc
    if not isinstance(value, dict):
        raise PreflightError("PowerShell OS inspection returned a non-object JSON value")
    value["powershell"] = {
        "command": command,
        "exitCode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }
    return value


def observe_windows_process_and_listeners(pid: int, port: int, *, timeout_seconds: float = 10.0) -> dict[str, Any]:
    """Observe the process and all same-port / same-process TCP listeners using Windows APIs."""
    if not (1 <= pid <= 0xFFFFFFFF and 1 <= port <= 65535):
        raise PreflightError("PID or port is outside the valid range")
    script = rf"""
$ErrorActionPreference = 'Stop'
$targetPid = {pid}
$targetPort = {port}
$p = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$targetPid" -ErrorAction Stop
$process = $null
if ($null -ne $p) {{
  $process = [ordered]@{{
    ProcessId = [int]$p.ProcessId
    ParentProcessId = [int]$p.ParentProcessId
    ExecutablePath = [string]$p.ExecutablePath
    CommandLine = [string]$p.CommandLine
    CreationTimeUtc = $p.CreationDate.ToUniversalTime().ToString('o')
  }}
}}
$tcpByPort = @(Get-NetTCPConnection -LocalPort $targetPort -ErrorAction Stop)
$tcpByPid = @(Get-NetTCPConnection -OwningProcess $targetPid -ErrorAction Stop)
$tcp = @(($tcpByPort + $tcpByPid) |
  Sort-Object LocalAddress, LocalPort, RemoteAddress, RemotePort, OwningProcess -Unique |
  Select-Object @{{Name='LocalAddress';Expression={{[string]$_.LocalAddress}}}},
                @{{Name='LocalPort';Expression={{[int]$_.LocalPort}}}},
                @{{Name='State';Expression={{[string]$_.State}}}},
                @{{Name='RemoteAddress';Expression={{[string]$_.RemoteAddress}}}},
                @{{Name='RemotePort';Expression={{[int]$_.RemotePort}}}},
                @{{Name='OwningProcess';Expression={{[int]$_.OwningProcess}}}},
                @{{Name='CreationTime';Expression={{if ($_.CreationTime) {{$_.CreationTime.ToUniversalTime().ToString('o')}} else {{$null}}}}}})
[ordered]@{{ process = $process; listeners = $tcp }} | ConvertTo-Json -Depth 5 -Compress
"""
    return _powershell_json(script, timeout_seconds=timeout_seconds)


def poll_ownership(
    admission: Any,
    endpoint: QmpEndpoint,
    *,
    expected_executable: str,
    timeout_seconds: float = 5.0,
    interval_seconds: float = 0.2,
    observe: Callable[[int, int], dict[str, Any]] = observe_windows_process_and_listeners,
) -> OwnershipEvidence:
    """Probe readiness, then correlate the last OS observation with a fresh QAC census.

    A callable admission is invoked once to establish the reservation identity and once
    after the listener is ready. The first snapshot is only an identity anchor during
    readiness checks; only the final snapshot can authorize QMP access.
    """
    initial_admission: Any = None
    try:
        initial_admission = admission() if callable(admission) else admission
        relationship, reservation = validate_qac_relationship(initial_admission)
    except Exception as exc:
        admission_object = initial_admission if isinstance(initial_admission, dict) else None
        return OwnershipEvidence(
            OwnershipState.OWNERSHIP_UNKNOWN, endpoint,
            admission_object.get("reservationId") if admission_object else None,
            admission_object.get("qemuPid") if admission_object else None,
            None, f"QAC admission could not establish the expected reservation identity: {exc}",
            {"admissionError": repr(exc)},
        )
    deadline = _monotonic() + max(0.0, timeout_seconds)
    attempts: list[dict[str, Any]] = []
    provider = admission if callable(admission) else None

    def with_attempts(evidence: OwnershipEvidence) -> OwnershipEvidence:
        return OwnershipEvidence(
            evidence.state, evidence.endpoint, evidence.reservation_id, evidence.qemu_pid,
            evidence.owner_pid, evidence.reason,
            {"attempts": attempts, "last": evidence.observation},
            verified_at_monotonic=evidence.verified_at_monotonic,
            qac_observed_at_utc=evidence.qac_observed_at_utc,
        )

    def same_reservation_identity(fresh_admission: Any) -> tuple[bool, str]:
        try:
            fresh_relationship, fresh_reservation = validate_qac_relationship(fresh_admission)
            expected_owner_pid = _owner_pid(reservation.get("owner"))
            fresh_owner_pid = _owner_pid(fresh_reservation.get("owner"))
            if (fresh_admission.get("reservationId") != initial_admission.get("reservationId")
                    or fresh_relationship.get("reservationId") != relationship.get("reservationId")
                    or fresh_relationship.get("qemuPid") != relationship.get("qemuPid")
                    or fresh_owner_pid != expected_owner_pid
                    or not _same_path(fresh_relationship.get("processIdentity"),
                                      relationship.get("processIdentity"))):
                return False, "QAC reservation, owner, child PID, or executable changed during readiness checks"
            initial_start = _parse_time(reservation.get("processStartTimeUtc"),
                                        field_name="initial QAC child start time")
            fresh_start = _parse_time(fresh_reservation.get("processStartTimeUtc"),
                                      field_name="fresh QAC child start time")
            delta_ms = abs((initial_start - fresh_start).total_seconds() * 1000)
            if delta_ms > 5:
                return False, f"QAC child start identity changed during readiness checks ({delta_ms:.3f} ms)"
            return True, "same QAC reservation and child identity"
        except Exception as exc:
            return False, f"fresh QAC reservation could not be validated: {exc}"

    while True:
        inspection_error: Exception | None = None
        snapshot: dict[str, Any] = {}
        try:
            snapshot = observe(relationship["qemuPid"], endpoint.port)
            readiness = evaluate_ownership(
                initial_admission, endpoint, snapshot,
                expected_executable=expected_executable, _readiness_only=True,
            )
        except Exception as exc:
            inspection_error = exc
            readiness = OwnershipEvidence(
                OwnershipState.OWNERSHIP_UNKNOWN, endpoint,
                initial_admission.get("reservationId"), relationship.get("qemuPid"),
                _owner_pid(reservation.get("owner")), f"OS ownership inspection failed: {exc}",
                {"inspectionError": repr(exc)},
            )

        if readiness.state is OwnershipState.OWNED_LISTENER_VERIFIED:
            final_admission = initial_admission
            if provider is not None:
                try:
                    final_admission = provider()
                except Exception as exc:
                    attempts.append({"state": OwnershipState.OWNERSHIP_UNKNOWN.value,
                                     "reason": f"fresh QAC census failed: {exc}",
                                     "observation": {"admissionError": repr(exc)}})
                    return with_attempts(OwnershipEvidence(
                        OwnershipState.OWNERSHIP_UNKNOWN, endpoint,
                        initial_admission.get("reservationId"), relationship.get("qemuPid"),
                        _owner_pid(reservation.get("owner")), f"fresh QAC census failed: {exc}",
                        snapshot,
                    ))
                same_identity, identity_reason = same_reservation_identity(final_admission)
                if not same_identity:
                    attempts.append({"state": OwnershipState.OWNERSHIP_UNKNOWN.value,
                                     "reason": identity_reason, "observation": snapshot})
                    return with_attempts(OwnershipEvidence(
                        OwnershipState.OWNERSHIP_UNKNOWN, endpoint,
                        final_admission.get("reservationId")
                        if isinstance(final_admission, dict) else None,
                        final_admission.get("qemuPid") if isinstance(final_admission, dict) else None,
                        None, identity_reason, snapshot,
                    ))
            verified = evaluate_ownership(final_admission, endpoint, snapshot,
                                          expected_executable=expected_executable)
            attempts.append({"state": verified.state.value, "reason": verified.reason,
                             "observation": verified.observation,
                             "qacObservedAtUtc": verified.qac_observed_at_utc})
            return with_attempts(verified)

        attempts.append({"state": readiness.state.value, "reason": readiness.reason,
                         "observation": readiness.observation})
        if readiness.state not in (OwnershipState.LISTENER_MISSING,
                                   OwnershipState.OWNERSHIP_UNKNOWN):
            return with_attempts(readiness)
        if readiness.state is OwnershipState.OWNERSHIP_UNKNOWN and inspection_error is None:
            # Identity mismatches and malformed OS rows are terminal, not readiness delays.
            return with_attempts(readiness)
        remaining = deadline - _monotonic()
        if remaining <= 0:
            if readiness.state is OwnershipState.LISTENER_MISSING:
                if provider is not None:
                    try:
                        final_admission = provider()
                        still_same, identity_reason = same_reservation_identity(final_admission)
                    except Exception as exc:
                        still_same = False
                        identity_reason = f"final QAC revalidation failed: {exc}"
                        final_admission = None
                    if not still_same:
                        attempts.append({"state": OwnershipState.OWNERSHIP_UNKNOWN.value,
                                         "reason": identity_reason,
                                         "observation": {"qacRevalidation": final_admission}})
                        return with_attempts(OwnershipEvidence(
                            OwnershipState.OWNERSHIP_UNKNOWN, endpoint,
                            final_admission.get("reservationId")
                            if isinstance(final_admission, dict) else None,
                            final_admission.get("qemuPid")
                            if isinstance(final_admission, dict) else None,
                            None, f"QAC reservation changed or was released during readiness checks: {identity_reason}",
                            {"lastOsObservation": snapshot, "qacRevalidation": final_admission},
                        ))
                readiness = OwnershipEvidence(
                    OwnershipState.LISTENER_MISSING, endpoint, initial_admission.get("reservationId"),
                    relationship.get("qemuPid"), _owner_pid(reservation.get("owner")),
                    "listener remained absent for the bounded readiness interval", snapshot,
                )
            return with_attempts(readiness)
        if readiness.state is OwnershipState.LISTENER_MISSING:
            attempts[-1]["state"] = OwnershipState.LISTENER_NOT_YET_READY.value
        time.sleep(min(max(0.0, interval_seconds), remaining))


def require_owned(evidence: OwnershipEvidence, endpoint: QmpEndpoint | None = None) -> None:
    if not isinstance(evidence, OwnershipEvidence):
        raise PreflightError("QMP command refused because listener ownership evidence is missing")
    if evidence.state is not OwnershipState.OWNED_LISTENER_VERIFIED:
        state = getattr(getattr(evidence, "state", None), "value", "missing")
        raise PreflightError(f"QMP command refused because listener ownership is not verified ({state})")
    if endpoint is not None and endpoint != evidence.endpoint:
        raise PreflightError("QMP endpoint differs from the verified ownership evidence")
    age = _monotonic() - evidence.verified_at_monotonic
    if age < 0 or age > MAX_QAC_SNAPSHOT_AGE_SECONDS:
        raise PreflightError(f"QMP ownership evidence is stale ({age:.3f} seconds old)")
    if not evidence.qac_observed_at_utc:
        raise PreflightError("QMP ownership evidence has no fresh QAC observation timestamp")
    try:
        qac_age = (_utc_now() - _parse_time(
            evidence.qac_observed_at_utc, field_name="QAC observation timestamp")).total_seconds()
    except PreflightError as exc:
        raise PreflightError(f"QMP ownership evidence has an invalid QAC timestamp: {exc}") from exc
    if qac_age < -0.5 or qac_age > MAX_QAC_SNAPSHOT_AGE_SECONDS:
        raise PreflightError(f"QMP QAC reservation evidence is stale ({qac_age:.3f} seconds old)")


def _require_same_owned_identity(
    original: OwnershipEvidence,
    fresh: OwnershipEvidence,
    endpoint: QmpEndpoint,
    operation: str,
) -> None:
    """Keep guarded QMP operations on the same reservation and exact process instance."""
    require_owned(fresh, endpoint)
    if (original.endpoint != endpoint
            or original.reservation_id != fresh.reservation_id
            or original.qemu_pid != fresh.qemu_pid
            or original.owner_pid != fresh.owner_pid):
        raise PreflightError(
            f"QMP {operation} refused because reservation, owner, or child identity changed"
        )
    original_process = _latest_os_snapshot(original).get("process")
    fresh_process = _latest_os_snapshot(fresh).get("process")
    if not isinstance(original_process, dict) or not isinstance(fresh_process, dict):
        raise PreflightError(f"QMP {operation} refused because process identity evidence is missing")
    if (original_process.get("ProcessId") != fresh_process.get("ProcessId")
            or original_process.get("ParentProcessId") != fresh_process.get("ParentProcessId")
            or not _same_path(original_process.get("ExecutablePath"),
                              fresh_process.get("ExecutablePath"))):
        raise PreflightError(f"QMP {operation} refused because QEMU process identity changed")
    try:
        original_start = _parse_time(original_process.get("CreationTimeUtc"),
                                     field_name="original OS process start time")
        fresh_start = _parse_time(fresh_process.get("CreationTimeUtc"),
                                  field_name="fresh OS process start time")
    except PreflightError as exc:
        raise PreflightError(f"QMP {operation} refused because process start identity is invalid: {exc}") from exc
    delta_ms = abs((original_start - fresh_start).total_seconds() * 1000)
    if delta_ms > 5:
        raise PreflightError(f"QMP {operation} refused because PID was reused ({delta_ms:.3f} ms start delta)")
    for process in (original_process, fresh_process):
        command_line = process.get("CommandLine")
        try:
            process_endpoint = parse_qmp_command_line(command_line)
        except PreflightError as exc:
            raise PreflightError(f"QMP {operation} refused because live QEMU command line is invalid: {exc}") from exc
        if process_endpoint != endpoint:
            raise PreflightError(f"QMP {operation} refused because live QEMU endpoint changed")


def _latest_os_snapshot(evidence: OwnershipEvidence) -> dict[str, Any]:
    snapshot = evidence.observation
    if isinstance(snapshot, dict) and isinstance(snapshot.get("last"), dict):
        return snapshot["last"]
    return snapshot


def require_connected_peer(
    evidence: OwnershipEvidence,
    endpoint: QmpEndpoint,
    client_address: tuple[str, int],
) -> None:
    """Require the accepted TCP tuple itself to be attributed to the verified QAC child."""
    require_owned(evidence, endpoint)
    rows = _latest_os_snapshot(evidence).get("listeners")
    if not isinstance(rows, list):
        raise PreflightError("OS TCP table did not return rows for the connected QMP peer")
    try:
        local_client = _ip(client_address[0])
    except (ValueError, PreflightError) as exc:
        raise PreflightError("connected QMP client address is not a numeric IP") from exc
    for row in rows:
        try:
            local_matches = (_ip(row.get("LocalAddress")) == _ip(endpoint.host)
                             and int(row.get("LocalPort")) == endpoint.port)
            remote_matches = (_ip(row.get("RemoteAddress")) == local_client
                              and int(row.get("RemotePort")) == client_address[1])
            owner_matches = int(row.get("OwningProcess")) == evidence.qemu_pid
        except (TypeError, ValueError, PreflightError):
            continue
        if local_matches and remote_matches and owner_matches:
            state = str(row.get("State") or "").casefold()
            # Some Windows builds omit State for TCP-table rows; a nonzero peer port
            # still distinguishes an accepted connection from the listening socket.
            if state in ("established", ""):
                return
    raise PreflightError(
        "the connected QMP TCP tuple is not attributed to the verified QAC child PID"
    )


class QmpClient:
    """Small CRLF-framed QMP client that can only connect after owned-listener verification."""

    def __init__(
        self,
        stream: socket.socket,
        endpoint: QmpEndpoint,
        evidence: OwnershipEvidence,
        *,
        request_prefix: str = "r3ay",
        max_frame_bytes: int = 1_048_576,
    ) -> None:
        require_owned(evidence, endpoint)
        if not re.fullmatch(r"[A-Za-z0-9_-]+", request_prefix):
            raise PreflightError("QMP request prefix contains unsupported characters")
        self.stream = stream
        self.endpoint = endpoint
        self.evidence = evidence
        self.request_prefix = request_prefix
        self.max_frame_bytes = max_frame_bytes
        self.buffer = bytearray()
        self.count = 0
        self.greeting: dict[str, Any] | None = None
        self.capabilities_ready = False
        self.initial_status: dict[str, Any] | None = None
        self.events: list[dict[str, Any]] = []

    @classmethod
    def connect_verified(
        cls,
        endpoint: QmpEndpoint,
        evidence: OwnershipEvidence,
        *,
        timeout_seconds: float = 5.0,
        request_prefix: str = "r3ay",
        expected_running: bool = True,
        reverify: Callable[[], OwnershipEvidence],
    ) -> "QmpClient":
        require_owned(evidence, endpoint)
        sock = socket.create_connection((endpoint.host, endpoint.port), timeout=timeout_seconds)
        sock.settimeout(timeout_seconds)
        client = cls(sock, endpoint, evidence, request_prefix=request_prefix)
        try:
            greeting = client._receive()
            if not isinstance(greeting, dict) or not isinstance(greeting.get("QMP"), dict):
                raise QmpProtocolError("QMP greeting is missing the QMP object")
            client.greeting = greeting
            fresh_evidence = reverify()
            _require_same_owned_identity(evidence, fresh_evidence, endpoint, "connection")
            require_connected_peer(fresh_evidence, endpoint, client.stream.getsockname())
            client.evidence = fresh_evidence
            capabilities = client.command("qmp_capabilities")
            if "error" in capabilities or capabilities.get("return") != {}:
                raise QmpProtocolError("QMP capabilities negotiation failed")
            client.capabilities_ready = True
            status = client.command("query-status")
            result = status.get("return")
            if "error" in status or not isinstance(result, dict) or not isinstance(result.get("status"), str):
                raise QmpProtocolError("correlated QMP query-status response is malformed")
            if expected_running and (result.get("running") is not True or result.get("status") != "running"):
                raise QmpProtocolError("correlated QMP query-status did not report a running guest")
            if not expected_running and result.get("running") is not False:
                raise QmpProtocolError("correlated QMP query-status did not confirm CPUs are paused")
            client.initial_status = result
            return client
        except Exception:
            client.close()
            raise

    def _receive(self) -> dict[str, Any]:
        while True:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                frame = bytes(self.buffer[: newline + 1])
                del self.buffer[: newline + 1]
                if not frame.endswith(b"\r\n"):
                    raise QmpProtocolError("QMP frame does not use CRLF termination")
                payload = frame[:-2]
                if payload.startswith(b"\xef\xbb\xbf"):
                    raise QmpProtocolError("QMP frame contains an unexpected UTF-8 BOM")
                try:
                    value = json.loads(payload.decode("utf-8", errors="strict"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise QmpProtocolError(f"QMP frame is not valid UTF-8 JSON: {exc}") from exc
                if not isinstance(value, dict):
                    raise QmpProtocolError("QMP frame must be a JSON object")
                return value
            if len(self.buffer) > self.max_frame_bytes:
                raise QmpProtocolError("QMP frame exceeds the configured maximum size")
            try:
                chunk = self.stream.recv(4096)
            except OSError as exc:
                raise QmpProtocolError(f"QMP receive failed: {exc!r}") from exc
            if not chunk:
                if self.buffer:
                    raise QmpProtocolError("QMP stream ended in a truncated frame")
                raise QmpProtocolError("QMP stream disconnected")
            self.buffer.extend(chunk)

    def _send(self, value: dict[str, Any]) -> bytes:
        wire = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\r\n"
        if wire.startswith(b"\xef\xbb\xbf"):
            raise QmpProtocolError("QMP request unexpectedly contains a UTF-8 BOM")
        try:
            self.stream.sendall(wire)
        except OSError as exc:
            raise QmpProtocolError(f"QMP send failed: {exc!r}") from exc
        return wire

    def command(self, execute: str, arguments: dict[str, Any] | None = None) -> dict[str, Any]:
        if not isinstance(execute, str) or not execute:
            raise PreflightError("QMP command name is empty")
        if execute in ("quit", "cont"):
            raise PreflightError(f"QMP {execute} must use its fresh-ownership guarded method")
        if execute not in ("qmp_capabilities", "query-status", "human-monitor-command"):
            raise PreflightError(f"QMP command {execute!r} is outside the read-only qualification allowlist")
        if execute != "qmp_capabilities" and not self.capabilities_ready:
            raise PreflightError("QMP command refused before capabilities negotiation")
        if execute == "human-monitor-command":
            if (not isinstance(arguments, dict) or set(arguments) != {"command-line"}
                    or not isinstance(arguments.get("command-line"), str)
                    or not (arguments["command-line"] == "info registers"
                            or re.fullmatch(r"xp /1gx 0x[0-9A-Fa-f]{1,16}",
                                            arguments["command-line"]) is not None)):
                raise PreflightError("QMP HMP command is outside the read-only inspection allowlist")
        return self._exchange(execute, arguments)

    def move_pointer(self, fresh_evidence: OwnershipEvidence,
                     delta_x: int, delta_y: int) -> dict[str, Any]:
        """Move the guest pointer only with fresh evidence for this QAC-owned QMP peer."""
        if (isinstance(delta_x, bool) or not isinstance(delta_x, int)
                or isinstance(delta_y, bool) or not isinstance(delta_y, int)
                or not -4096 <= delta_x <= 4096 or not -4096 <= delta_y <= 4096):
            raise PreflightError("QMP pointer movement is outside the bounded relative-input range")
        return self._send_guarded_input(fresh_evidence, [{
            "type": "rel", "data": {"axis": "x", "value": delta_x}}, {
            "type": "rel", "data": {"axis": "y", "value": delta_y}}])

    def left_button(self, fresh_evidence: OwnershipEvidence, down: bool) -> dict[str, Any]:
        """Send one left-button transition to the guest after ownership revalidation."""
        if not isinstance(down, bool):
            raise PreflightError("QMP left-button state must be a boolean")
        return self._send_guarded_input(fresh_evidence, [{
            "type": "btn", "data": {"button": "left", "down": down}}])

    def _send_guarded_input(self, fresh_evidence: OwnershipEvidence,
                            events: list[dict[str, Any]]) -> dict[str, Any]:
        if not self.capabilities_ready:
            raise PreflightError("QMP input refused before capabilities negotiation")
        if not isinstance(self.initial_status, dict) or self.initial_status.get("running") is not True:
            raise PreflightError("QMP input refused unless the guest is verified running")
        _require_same_owned_identity(self.evidence, fresh_evidence, self.endpoint, "input")
        require_connected_peer(fresh_evidence, self.endpoint, self.stream.getsockname())
        reply = self._exchange("input-send-event", {"events": events})
        if "error" in reply or reply.get("return") != {}:
            raise QmpProtocolError("QMP guest input event was rejected")
        return reply

    def _exchange(self, execute: str, arguments: dict[str, Any] | None = None) -> dict[str, Any]:
        self.count += 1
        request_id = f"{self.request_prefix}-{self.count}"
        request: dict[str, Any] = {"execute": execute, "id": request_id}
        if arguments is not None:
            request["arguments"] = arguments
        self._send(request)
        while True:
            response = self._receive()
            if "event" in response and "id" not in response:
                self.events.append(response)
                continue
            if response.get("id") != request_id:
                raise QmpProtocolError(
                    f"QMP response id {response.get('id')!r} does not match request {request_id!r}"
                )
            if "error" not in response and "return" not in response:
                raise QmpProtocolError("correlated QMP response has neither return nor error")
            if execute == "qmp_capabilities" and "error" not in response:
                self.capabilities_ready = True
            return response

    def quit(self, fresh_evidence: OwnershipEvidence) -> dict[str, Any]:
        """Send quit only with a fresh successful ownership snapshot for the same endpoint."""
        _require_same_owned_identity(self.evidence, fresh_evidence, self.endpoint, "quit")
        require_connected_peer(fresh_evidence, self.endpoint, self.stream.getsockname())
        if not self.capabilities_ready:
            raise PreflightError("QMP quit refused before capabilities negotiation")
        return self._exchange("quit")

    def continue_guest(self, fresh_evidence: OwnershipEvidence) -> dict[str, Any]:
        """Resume a -S guest only after revalidating the same current QAC-owned listener."""
        _require_same_owned_identity(self.evidence, fresh_evidence, self.endpoint, "cont")
        require_connected_peer(fresh_evidence, self.endpoint, self.stream.getsockname())
        if not self.capabilities_ready or not isinstance(self.initial_status, dict):
            raise PreflightError("QMP cont refused before capabilities and status negotiation")
        if self.initial_status.get("running") is not False:
            raise PreflightError("QMP cont refused because the guest was not verified paused")
        reply = self._exchange("cont")
        if "error" in reply:
            raise QmpProtocolError("QMP cont command was rejected")
        status = self.command("query-status")
        result = status.get("return")
        if ("error" in status or not isinstance(result, dict)
                or result.get("running") is not True or result.get("status") != "running"):
            raise QmpProtocolError("QMP query-status did not confirm guest execution after cont")
        self.initial_status = result
        return status

    def close(self) -> None:
        try:
            self.stream.close()
        except OSError:
            pass

    def __enter__(self) -> "QmpClient":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()


def _register_value(response: Any, expected_id: str, register: str) -> int:
    if register not in ("CR0", "CR3", "CR4"):
        raise PreflightError("unsupported control register")
    if not isinstance(response, dict) or response.get("id") != expected_id:
        raise PreflightError("QMP register response ID does not match the request")
    if "error" in response or not isinstance(response.get("return"), str):
        raise PreflightError("QMP register response has no text return value")
    values = re.findall(
        rf"(?<![A-Za-z0-9_]){register}=((?:0x)?(?:[0-9A-Fa-f]{{8}}|[0-9A-Fa-f]{{16}}))(?=\s|$)",
        response["return"],
        flags=re.IGNORECASE,
    )
    if len(values) != 1:
        raise PreflightError(f"QMP response must contain exactly one eight- or sixteen-digit {register}")
    value = values[0]
    if value.lower().startswith("0x"):
        value = value[2:]
    return int(value, 16)


def parse_qmp_register(response: Any, expected_id: str, register: str) -> int:
    """Parse an exact-width 8- or 16-digit control register without precision loss."""
    return _register_value(response, expected_id, register.upper())


def parse_cr3_active(record: str) -> int:
    if not isinstance(record, str):
        raise PreflightError("MAP-E record must be text")
    values = re.findall(r"(?<![A-Za-z0-9_])cr3_active=([0-9A-Fa-f]{16})(?=\s|$)", record)
    if len(values) != 1:
        raise PreflightError("MAP-E record must contain exactly one sixteen-digit cr3_active field")
    return int(values[0], 16)


def inspect_register_policy(cr0: int, cr4: int) -> dict[str, bool]:
    if any(not isinstance(value, int) or isinstance(value, bool) or not 0 <= value < (1 << 64)
           for value in (cr0, cr4)):
        raise PreflightError("CR0 and CR4 must be unsigned 64-bit integers")
    return {"cr0_wp": bool(cr0 & (1 << 16)), "cr4_la57": bool(cr4 & (1 << 12)),
            "four_level_supported": not bool(cr4 & (1 << 12))}


def require_four_level_paging(cr4: int) -> None:
    if not isinstance(cr4, int) or isinstance(cr4, bool) or not 0 <= cr4 < (1 << 64):
        raise PreflightError("CR4 must be an unsigned 64-bit integer")
    if cr4 & (1 << 12):
        raise PreflightError("CR4.LA57 enables unsupported five-level paging")
