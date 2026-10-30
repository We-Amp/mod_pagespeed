# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""IIS system test: an unusable optimizer service is reported locally only.

With the configured optimizer service unusable, the module serves normally
with in-place optimization off and marks its responses to local requests
with ``X-Pagespeed-Init-Status: daemon-unavailable``. The marked response is
kept out of the IIS kernel and user caches, so a remote client never sees it,
even after the local requests have primed what IIS would otherwise cache.

Tests:

* ``test_unusable_service_reported_locally_never_remotely`` runs in the
  standard IIS lane. It needs no optimizer service: it points the test
  site's DaemonSocketPath at a pipe nothing listens on and its
  DaemonVolumePath at an empty temporary folder, which is an unusable
  service by definition, recycles the application pool, checks the local
  header and its absence for remote clients, then restores the site's
  pagespeed.config and recycles again.
* The same test also reads the Application event log entry the module
  writes for the unusable service and asserts that it names the configured
  pipe; ``test_half_configured_service_event_log_says_not_set`` asserts
  that an empty volume path prints "(not set)".
* ``test_recovered_service_turns_the_header_off`` needs a real optimizer
  service. It runs against either a Windows service installed and already
  named by the site's pagespeed.config, or the optimizer worker the lane
  starts (tools/ci/Start-OptimizerWorker.ps1) and the extra site the lane
  points at it (PAGESPEED_IIS_DAEMON_SITE_URL). It stops the service or
  worker, waits for the header, starts it again, and checks that the
  30 second recheck turns the header off and writes one informational
  event-log entry. It is skipped, with the reason, where neither exists.

The remote-client checks need a way to make a request that counts as remote:
the module treats a client as local when its address is loopback or equals
the address it connected to, so a "remote" request is one made from one local
address to another, or from a WSL guest. A machine with neither still runs
every local check; it prints one note saying why the remote half was not run.
The recovery test is marked ``slow_recovery`` (about a minute, mostly the
30 second recheck) so the per-merge lane can deselect it.
The site is restored and the application pool recycled on every exit path.

Environment variables (all optional; the defaults match the standard lane):
    PAGESPEED_IIS_OPTIMIZER_SERVICE: service name (WeAmpPageSpeedOptimizer)
    PAGESPEED_IIS_APPPOOL: application pool of the site (PageSpeedTestPool)
    PAGESPEED_IIS_SITE_CONFIG: the pagespeed.config the site reads
        (C:\\inetpub\\pagespeed_test\\pagespeed.config)
    PAGESPEED_IIS_DAEMON_SITE_URL: base URL of the site that uses the lane's
        optimizer worker (set by the lane's setup script)
    PAGESPEED_IIS_DAEMON_SITE_CONFIG: that site's pagespeed.config
        (C:\\inetpub\\pagespeed_test_daemon\\pagespeed.config)
    PAGESPEED_OPTIMIZER_DIR / _VOLUME / _SOCKET / _API_TOKEN: the lane's
        optimizer worker (set by tools/ci/Start-OptimizerWorker.ps1)
    PAGESPEED_OPTIMIZER_ROOT: where that script keeps the worker's pid file
        (C:\\pagespeed_optimizer)
"""

import contextlib
import http.client
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.parse
import uuid
from typing import List, Optional

import pytest

STATUS_HEADER = "X-Pagespeed-Init-Status"
STATUS_VALUE = "daemon-unavailable"
# The module rechecks a service that was not running every 30 seconds.
RECHECK_SECONDS = 30
SERVICE = os.environ.get("PAGESPEED_IIS_OPTIMIZER_SERVICE", "WeAmpPageSpeedOptimizer")
APPPOOL = os.environ.get("PAGESPEED_IIS_APPPOOL", "PageSpeedTestPool")
SITE_CONFIG = os.environ.get(
    "PAGESPEED_IIS_SITE_CONFIG", r"C:\inetpub\pagespeed_test\pagespeed.config"
)
APPCMD = os.path.join(
    os.environ.get("SystemRoot", r"C:\Windows"), "system32", "inetsrv", "appcmd.exe"
)


def _scaled(seconds: float) -> float:
    """Scale a wait by PAGESPEED_TEST_TIMEOUT_MULTIPLIER, as the harness does."""
    try:
        return seconds * max(1.0, float(os.environ.get("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", "1")))
    except ValueError:
        return seconds


def _run(args: List[str]) -> subprocess.CompletedProcess:
    return subprocess.run(args, capture_output=True, text=True, timeout=120)


def _service_state() -> Optional[str]:
    """RUNNING, STOPPED, ... from ``sc query``, or None when not installed."""
    out = _run(["sc.exe", "query", SERVICE])
    if out.returncode != 0:
        return None
    match = re.search(r"STATE\s*:\s*\d+\s+(\w+)", out.stdout)
    return match.group(1) if match else None


def _wait_for_service(state: str, timeout: float = 60.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _service_state() == state:
            return
        time.sleep(1)
    pytest.fail(f"service {SERVICE} did not reach {state}: {_service_state()}")


def _restore_service() -> None:
    """Start the service again if needed; never raises (used in cleanup)."""
    if _service_state() == "RUNNING":
        return
    _run(["sc.exe", "start", SERVICE])
    deadline = time.time() + 60
    while time.time() < deadline and _service_state() != "RUNNING":
        time.sleep(1)


def _recycle_pool() -> None:
    _run([APPCMD, "recycle", "apppool", f"/apppool.name:{APPPOOL}"])
    time.sleep(3)


def _non_loopback_addresses() -> List[str]:
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    except OSError:
        return []
    found = []
    for info in infos:
        address = info[4][0]
        if not address.startswith("127.") and address not in found:
            found.append(address)
    return found


def _get(host: str, port: int, path: str, source: Optional[str] = None):
    """GET path from host:port, optionally from a given local address."""
    conn = http.client.HTTPConnection(
        host, port, timeout=30, source_address=(source, 0) if source else None
    )
    try:
        conn.request("GET", path, headers={"Host": "localhost"})
        response = conn.getresponse()
        response.read()
        return response
    finally:
        conn.close()


def _event_messages_since(since: float) -> List[str]:
    """Text of the Application event log entries from the PageSpeed source.

    The source is not always registered with a message file (a lane that did
    not run the installer), and then Windows can neither filter by its name
    nor format its message; the text is the entry's first insertion string.
    """
    script = (
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
        "$s = [DateTime]::Now.AddSeconds(-%d); "
        "$m = @(Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$s} "
        "-ErrorAction SilentlyContinue | Where-Object { $_.ProviderName -eq 'PageSpeed' } "
        "| ForEach-Object { ($_.Properties | ForEach-Object { $_.Value }) -join ' ' }); "
        "[Console]::Out.Write(($m -join [string][char]30))"
        % (time.time() - since + 5)
    )
    out = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", script],
        capture_output=True, text=True, encoding="utf-8", timeout=120,
    )
    assert out.returncode == 0, f"event log query failed: {out.stderr!r}"
    return [m for m in out.stdout.split("\x1e") if m]


def _wait_for_event(since: float, needle: str, timeout: float = 60.0) -> List[str]:
    """Event log messages since a time that contain needle (polls briefly)."""
    deadline = time.time() + _scaled(timeout)
    while True:
        found = [m for m in _event_messages_since(since) if needle in m]
        if found or time.time() >= deadline:
            return found
        time.sleep(2)


def _recovery_entry_since(since: float) -> bool:
    """True when the recovery note is in the Application event log."""
    return any("answered a later check" in m for m in _event_messages_since(since))


def _connects(target: str, source: str, port: int) -> bool:
    """True when a TCP connect to target:port from source succeeds."""
    try:
        probe = socket.create_connection((target, port), 2, source_address=(source, 0))
    except OSError:
        return False
    probe.close()
    return True


def _wsl_available() -> bool:
    if shutil.which("wsl.exe") is None:
        return False
    try:
        return _run(["wsl.exe", "-e", "curl", "--version"]).returncode == 0
    except OSError:
        return False


def _find_remote_route(port: int):
    """((target, remote_source), None) for a request that counts as remote.

    Windows refuses to send from one interface's address to another
    interface's address (strong host model, WinError 10051), so pairs are
    probed and the first that connects is used. With no routable pair the
    request is made from a WSL guest (remote_source None), which has its own
    network stack. Never skips: with no way to make a remote request the
    result is (None, reason), and the caller runs its local checks anyway.
    """
    addresses = _non_loopback_addresses()
    for target in addresses:
        for source in addresses:
            if source != target and _connects(target, source, port):
                return (target, source), None
    if not addresses:
        return None, f"no non-loopback IPv4 address found for port {port}"
    if not _wsl_available():
        return None, (
            "no routable pair of non-loopback IPv4 addresses to make a remote "
            f"request on port {port}, and no WSL guest to make it from; found: {addresses}"
        )
    for target in addresses:
        probe = _run(["wsl.exe", "-e", "curl", "-s", "-o", "/dev/null",
                      "--max-time", "5", f"http://{target}:{port}/"])
        if probe.returncode == 0:
            return (target, None), None
    return None, (
        f"a WSL guest cannot reach the site on any of {addresses}, port {port}; "
        "no routable pair of host addresses either"
    )


class _WslResponse:
    def __init__(self, status: int, headers):
        self.status = status
        self._headers = headers

    def getheader(self, name: str):
        return self._headers.get(name.lower())


def _remote_get(target: str, port: int, path: str, source: Optional[str]):
    """GET from the remote source, or from a WSL guest when source is None."""
    if source is not None:
        return _get(target, port, path, source=source)
    out = _run(
        ["wsl.exe", "-e", "curl", "-s", "-D", "-", "-o", "/dev/null",
         "--max-time", "30", "-H", "Host: localhost",
         f"http://{target}:{port}{path}"]
    )
    lines = out.stdout.replace("\r", "").splitlines()
    match = re.match(r"HTTP/\S+ (\d+)", lines[0]) if lines else None
    assert match, f"no status line from wsl curl: {out.stdout!r} {out.stderr!r}"
    headers = {}
    for line in lines[1:]:
        name, sep, value = line.partition(":")
        if sep:
            headers[name.strip().lower()] = value.strip()
    return _WslResponse(int(match.group(1)), headers)


def _read_config(config_path: str) -> str:
    try:
        with open(config_path, encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        pytest.skip(f"site config {config_path} not readable")


def _read_site_config() -> str:
    return _read_config(SITE_CONFIG)


def _sets_daemon_paths(config: str) -> bool:
    return bool(
        re.search(r"(?mi)^\s*pagespeed\s+DaemonSocketPath\s+\S", config)
        and re.search(r"(?mi)^\s*pagespeed\s+DaemonVolumePath\s+\S", config)
    )


def _wait_for_local_header(port: int, path: str, want: bool, timeout: float = 60.0):
    """Poll a local request until the status header is (or is not) present."""
    deadline = time.time() + timeout
    response = _get("127.0.0.1", port, path)
    while (response.getheader(STATUS_HEADER) == STATUS_VALUE) != want and (
        time.time() < deadline
    ):
        time.sleep(2)
        response = _get("127.0.0.1", port, path)
    return response


def _assert_local_only(route, route_reason, port: int, path: str) -> None:
    """The header is on local requests and never reaches a remote one.

    The local half always runs. The remote half runs when |route| (from
    _find_remote_route) is not None; otherwise a note says why it did not.
    """
    local = _wait_for_local_header(port, path, want=True)
    assert local.getheader(STATUS_HEADER) == STATUS_VALUE, (
        f"local request lacks {STATUS_HEADER}: {local.getheader(STATUS_HEADER)}"
    )
    if route is None:
        print(f"NOTE: remote half not run: {route_reason}")
        return
    target, remote_source = route
    # Local requests for the same file inside the cache window would prime an
    # IIS cache if the response were cacheable. They go to the same address
    # the remote ones do (source == target is local by the module's rule), so
    # a cache keyed on the destination address would be primed too.
    for _ in range(3):
        primed = _get(target, port, path, source=target)
        assert primed.getheader(STATUS_HEADER) == STATUS_VALUE
    for _ in range(3):
        remote = _remote_get(target, port, path, remote_source)
        assert remote.status == 200
        assert remote.getheader(STATUS_HEADER) is None, (
            f"a remote request carried {STATUS_HEADER}"
        )


@contextlib.contextmanager
def _site_config_with(config_path: str, lines: str):
    """Prepend directives to a site's pagespeed.config; restore it on exit.

    Prepended, not appended: the lane's config ends in "host:" match blocks,
    and directives appended there fall inside the last block (where the daemon
    options are refused) instead of the site's own scope. The pool is
    recycled on the way in and on the way out.
    """
    with open(config_path, "rb") as handle:
        raw_original = handle.read()
    original = raw_original.decode("utf-8", errors="replace")
    try:
        with open(config_path, "w", encoding="utf-8", newline="") as handle:
            handle.write(lines + original)
        _recycle_pool()
        yield
    finally:
        with open(config_path, "wb") as handle:
            handle.write(raw_original)
        _recycle_pool()


@pytest.mark.iis_only
@pytest.mark.slow
def test_unusable_service_reported_locally_never_remotely(server_config, example_root):
    """Local-only header, and an event-log entry that names the cause."""
    original = _read_site_config()
    if _sets_daemon_paths(original):
        pytest.skip(
            f"{SITE_CONFIG} already names an optimizer service; "
            "test_recovered_service_turns_the_header_off covers that setup"
        )
    port = server_config.port
    route, route_reason = _find_remote_route(port)
    path = f"{example_root}/combine_javascript1.js"

    # A pipe nothing listens on and a folder that holds no cache volume: the
    # service is configured and cannot be used.
    started = time.time()
    empty_volume = tempfile.mkdtemp(prefix="pagespeed-unusable-")
    pipe = rf"\\.\pipe\pagespeed-unusable-{uuid.uuid4().hex}"
    try:
        with _site_config_with(
            SITE_CONFIG,
            f"pagespeed DaemonSocketPath {pipe}\n"
            f"pagespeed DaemonVolumePath {empty_volume}\n",
        ):
            _assert_local_only(route, route_reason, port, path)
            found = _wait_for_event(started, pipe)
            assert found, (
                "no event-log entry names the configured pipe; got: "
                f"{_event_messages_since(started)!r}"
            )
            message = found[0]
            assert "optimizer service" in message, message
            assert "DaemonSocketPath" in message, message
            assert "not set" not in message, message
    finally:
        shutil.rmtree(empty_volume, ignore_errors=True)


@pytest.mark.iis_only
@pytest.mark.slow
def test_half_configured_service_event_log_says_not_set(server_config, example_root):
    """An empty volume path is printed as "(not set)" in the entry."""
    original = _read_site_config()
    if _sets_daemon_paths(original):
        pytest.skip(f"{SITE_CONFIG} already names an optimizer service")
    started = time.time()
    pipe = rf"\\.\pipe\pagespeed-halfset-{uuid.uuid4().hex}"
    with _site_config_with(SITE_CONFIG, f"pagespeed DaemonSocketPath {pipe}\n"):
        _get("127.0.0.1", server_config.port, f"{example_root}/combine_javascript1.js")
        found = _wait_for_event(started, pipe)
        assert found, (
            "no event-log entry names the configured pipe; got: "
            f"{_event_messages_since(started)!r}"
        )
        assert "DaemonVolumePath (not set)" in found[0], found[0]


class _WindowsServiceUnderTest:
    """A Windows service the site's own pagespeed.config names."""

    def __init__(self, port: int):
        self.port = port
        self.config_path = SITE_CONFIG

    @staticmethod
    def available() -> Optional[str]:
        """None when usable, otherwise the reason to skip."""
        if _service_state() != "RUNNING":
            return f"optimizer service {SERVICE} is not installed and running"
        if not _sets_daemon_paths(_read_site_config()):
            return f"{SITE_CONFIG} does not set DaemonSocketPath and DaemonVolumePath"
        return None

    def stop(self) -> None:
        _run(["sc.exe", "stop", SERVICE])
        _wait_for_service("STOPPED")

    def start(self) -> None:
        _run(["sc.exe", "start", SERVICE])
        _wait_for_service("RUNNING")

    def restore(self) -> None:
        _restore_service()


class _LaneWorkerUnderTest:
    """The optimizer worker the lane starts as a background process.

    tools/ci/Start-OptimizerWorker.ps1 starts factory_worker.exe, records its
    pid, and exports where it lives; the lane's setup script points an extra
    site at it. It is stopped by its recorded pid (checked to still be a
    factory_worker.exe) and started again with the same arguments, volume and
    management token, so the other tests see the worker they started with.
    """

    def __init__(self, port: int):
        self.port = port
        self.config_path = os.environ.get(
            "PAGESPEED_IIS_DAEMON_SITE_CONFIG",
            r"C:\inetpub\pagespeed_test_daemon\pagespeed.config",
        )
        self.root = os.environ.get("PAGESPEED_OPTIMIZER_ROOT", r"C:\pagespeed_optimizer")
        self.pid_file = os.path.join(self.root, "worker.pid")
        self.release = os.environ.get("PAGESPEED_OPTIMIZER_DIR", "")
        self.volume_dir = os.path.dirname(os.environ.get("PAGESPEED_OPTIMIZER_VOLUME", ""))
        self.socket_name = os.environ.get("PAGESPEED_OPTIMIZER_SOCKET", "pagespeed-optimizer")
        # tools/ci/Start-OptimizerWorker.ps1 exports host:port of the
        # management API; the other worker arguments below are that script's.
        self.api_port = int(os.environ.get("PAGESPEED_OPTIMIZER_API", "127.0.0.1:18765").rsplit(":", 1)[1])
        # Logs go where the lane's failure upload collects the worker's own.
        self.log_dir = os.environ.get("PAGESPEED_EVIDENCE_DIR", self.root)

    @classmethod
    def available(cls) -> Optional[str]:
        needed = ("PAGESPEED_IIS_DAEMON_SITE_URL", "PAGESPEED_OPTIMIZER_DIR",
                  "PAGESPEED_OPTIMIZER_VOLUME", "PAGESPEED_OPTIMIZER_API_TOKEN")
        missing = [name for name in needed if not os.environ.get(name)]
        if missing:
            return "the lane's optimizer worker is not set up (" + ", ".join(missing) + ")"
        return None

    @staticmethod
    def port_from_env() -> int:
        url = urllib.parse.urlparse(os.environ["PAGESPEED_IIS_DAEMON_SITE_URL"])
        return url.port or 80

    def _pid(self) -> Optional[int]:
        try:
            with open(self.pid_file, encoding="ascii") as handle:
                return int(handle.read().split()[0])
        except (OSError, ValueError, IndexError):
            return None

    def _is_worker(self, pid: int) -> bool:
        out = _run(["tasklist.exe", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"])
        return out.returncode == 0 and "factory_worker.exe" in out.stdout.lower().replace('"', "")

    def _running(self) -> bool:
        pid = self._pid()
        return pid is not None and self._is_worker(pid)

    def stop(self) -> None:
        pid = self._pid()
        assert pid is not None and self._is_worker(pid), "no optimizer worker of the lane is running"
        _run(["taskkill.exe", "/PID", str(pid), "/F"])
        deadline = time.time() + 30
        while time.time() < deadline and self._is_worker(pid):
            time.sleep(1)
        assert not self._is_worker(pid), "the optimizer worker did not stop"

    def start(self) -> None:
        env = dict(os.environ)
        env["PAGESPEED_API_TOKEN"] = os.environ["PAGESPEED_OPTIMIZER_API_TOKEN"]
        worker = os.path.join(self.release, "factory_worker.exe")
        os.makedirs(self.log_dir, exist_ok=True)
        with open(os.path.join(self.log_dir, "optimizer-worker-restart.out.log"), "ab") as out_log, \
                open(os.path.join(self.log_dir, "optimizer-worker-restart.err.log"), "ab") as err_log:
            proc = subprocess.Popen(
                [worker, "--socket", self.socket_name, "--cache-dir", self.volume_dir,
                 "--cache-size", "268435456", "--api-bind", "127.0.0.1",
                 "--api-port", str(self.api_port), "--log-level", "info"],
                env=env, stdout=out_log, stderr=err_log, stdin=subprocess.DEVNULL,
                creationflags=0x08000000 | 0x00000008,  # CREATE_NO_WINDOW | DETACHED_PROCESS
            )
        with open(self.pid_file, "w", encoding="ascii") as handle:
            handle.write(f"{proc.pid}\n")
        deadline = time.time() + _scaled(60)
        while time.time() < deadline:
            try:
                conn = http.client.HTTPConnection("127.0.0.1", self.api_port, timeout=2)
                conn.request("GET", "/v1/health", headers={
                    "Authorization": "Bearer " + os.environ["PAGESPEED_OPTIMIZER_API_TOKEN"]})
                if conn.getresponse().status == 200:
                    return
            except OSError:
                pass
            time.sleep(1)
        pytest.fail("the restarted optimizer worker did not answer /v1/health")

    def restore(self) -> None:
        if not self._running():
            self.start()


@pytest.mark.iis_only
@pytest.mark.slow
@pytest.mark.slow_recovery
def test_recovered_service_turns_the_header_off(server_config, example_root):
    service_reason = _WindowsServiceUnderTest.available()
    if service_reason is None:
        under_test = _WindowsServiceUnderTest(server_config.port)
    else:
        worker_reason = _LaneWorkerUnderTest.available()
        if worker_reason is not None:
            pytest.skip(f"{service_reason}; {worker_reason}")
        under_test = _LaneWorkerUnderTest(_LaneWorkerUnderTest.port_from_env())
        if not _sets_daemon_paths(_read_config(under_test.config_path)):
            pytest.skip(f"{under_test.config_path} does not set DaemonSocketPath and DaemonVolumePath")
    port = under_test.port
    route, route_reason = _find_remote_route(port)
    path = f"{example_root}/combine_javascript1.js"

    try:
        under_test.stop()
        # A worker starts the check afresh after a recycle.
        _recycle_pool()
        _assert_local_only(route, route_reason, port, path)

        # Recovery: start the service, let the 30 second recheck pass.
        started = time.time()
        under_test.start()
        time.sleep(RECHECK_SECONDS)
        recovered = _wait_for_local_header(port, path, want=False, timeout=90.0)
        assert recovered.getheader(STATUS_HEADER) is None, (
            f"{STATUS_HEADER} still present after the service recovered"
        )
        assert _recovery_entry_since(started), "no recovery entry in the event log"
    finally:
        # Cleanup must not mask the failure above: restore without raising.
        try:
            under_test.restore()
        finally:
            _recycle_pool()


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite --
    # vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
