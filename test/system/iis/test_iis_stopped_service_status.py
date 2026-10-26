# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""IIS system test: an unusable optimizer service is reported locally only.

With the configured optimizer service unusable, the module serves normally
with in-place optimization off and marks its responses to local requests
with ``X-Pagespeed-Init-Status: daemon-unavailable``. The marked response is
kept out of the IIS kernel and user caches, so a remote client never sees it,
even after the local requests have primed what IIS would otherwise cache.

Two tests:

* ``test_unusable_service_reported_locally_never_remotely`` runs in the
  standard IIS lane. It needs no optimizer service: it points the test
  site's DaemonSocketPath at a pipe nothing listens on and its
  DaemonVolumePath at an empty temporary folder, which is an unusable
  service by definition, recycles the application pool, checks the local
  header and its absence for remote clients, then restores the site's
  pagespeed.config and recycles again.
* ``test_recovered_service_turns_the_header_off`` needs a real optimizer
  service installed and already named by the site's pagespeed.config. It
  stops the service, waits for the header, restarts it, and checks that the
  30 second recheck turns the header off and writes one informational
  event-log entry. It is skipped, with the reason, everywhere else.

Both need two non-loopback IPv4 addresses on the machine: the module treats
a client as local when its address is loopback or equals the address it
connected to, so a "remote" request is one made from one local address to
another. A machine with fewer, or whose addresses cannot reach
each other, is a skip that lists the addresses it found and says no
routable pair exists (unless a WSL guest can make the remote request).
The site is restored and the application pool recycled on every exit path.

Environment variables (all optional; the defaults match the standard lane):
    PAGESPEED_IIS_OPTIMIZER_SERVICE: service name (WeAmpPageSpeedOptimizer)
    PAGESPEED_IIS_APPPOOL: application pool of the site (PageSpeedTestPool)
    PAGESPEED_IIS_SITE_CONFIG: the pagespeed.config the site reads
        (C:\\inetpub\\pagespeed_test\\pagespeed.config)
"""

import http.client
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
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


def _recovery_entry_since(since: float) -> bool:
    """True when the recovery note is in the Application event log."""
    script = (
        "$s = [DateTime]::Now.AddSeconds(-%d); "
        "@(Get-WinEvent -FilterHashtable @{LogName='Application'; "
        "ProviderName='PageSpeed'; StartTime=$s} -ErrorAction SilentlyContinue "
        "| Where-Object { $_.Message -match 'answered a later check' }).Count"
        % (time.time() - since + 5)
    )
    out = _run(["powershell.exe", "-NoProfile", "-Command", script])
    return out.returncode == 0 and out.stdout.strip() not in ("", "0")


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


def _remote_route(port: int):
    """(target, remote_source) for a request that counts as remote.

    Windows refuses to send from one interface's address to another
    interface's address (strong host model, WinError 10051), so pairs are
    probed and the first that connects is used. With no routable pair the
    request is made from a WSL guest (remote_source None), which has its own
    network stack. Otherwise a skip that lists the addresses it found.
    """
    addresses = _non_loopback_addresses()
    for target in addresses:
        for source in addresses:
            if source != target and _connects(target, source, port):
                return target, source
    if addresses and _wsl_available():
        return addresses[0], None
    pytest.skip(
        "no routable pair of non-loopback IPv4 addresses to make a remote "
        f"request on port {port}; found: {addresses or 'none'}"
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


def _read_site_config() -> str:
    try:
        with open(SITE_CONFIG, encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        pytest.skip(f"site config {SITE_CONFIG} not readable")


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


def _assert_local_only(target: str, remote_source: str, port: int, path: str) -> None:
    """The header is on local requests and never reaches a remote one."""
    local = _wait_for_local_header(port, path, want=True)
    assert local.getheader(STATUS_HEADER) == STATUS_VALUE, (
        f"local request lacks {STATUS_HEADER}: {local.getheader(STATUS_HEADER)}"
    )
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


@pytest.mark.iis_only
@pytest.mark.slow
def test_unusable_service_reported_locally_never_remotely(server_config, example_root):
    original = _read_site_config()
    if _sets_daemon_paths(original):
        pytest.skip(
            f"{SITE_CONFIG} already names an optimizer service; "
            "test_recovered_service_turns_the_header_off covers that setup"
        )
    port = server_config.port
    target, remote_source = _remote_route(port)
    path = f"{example_root}/combine_javascript1.js"

    # A pipe nothing listens on and a folder that holds no cache volume: the
    # service is configured and cannot be used.
    with open(SITE_CONFIG, "rb") as handle:
        raw_original = handle.read()
    empty_volume = tempfile.mkdtemp(prefix="pagespeed-unusable-")
    pipe = rf"\\.\pipe\pagespeed-unusable-{uuid.uuid4().hex}"
    # Prepended, not appended: the lane's config ends in "host:" match blocks,
    # and directives appended there fall inside the last block (where the
    # daemon options are refused) instead of the site's own scope.
    injected = (
        f"pagespeed DaemonSocketPath {pipe}\n"
        + f"pagespeed DaemonVolumePath {empty_volume}\n"
        + original
    )
    try:
        with open(SITE_CONFIG, "w", encoding="utf-8", newline="") as handle:
            handle.write(injected)
        _recycle_pool()
        _assert_local_only(target, remote_source, port, path)
    finally:
        with open(SITE_CONFIG, "wb") as handle:
            handle.write(raw_original)
        _recycle_pool()
        shutil.rmtree(empty_volume, ignore_errors=True)


@pytest.mark.iis_only
@pytest.mark.slow
def test_recovered_service_turns_the_header_off(server_config, example_root):
    if _service_state() != "RUNNING":
        pytest.skip(f"optimizer service {SERVICE} is not installed and running")
    if not _sets_daemon_paths(_read_site_config()):
        pytest.skip(f"{SITE_CONFIG} does not set DaemonSocketPath and DaemonVolumePath")
    port = server_config.port
    target, remote_source = _remote_route(port)
    path = f"{example_root}/combine_javascript1.js"

    try:
        _run(["sc.exe", "stop", SERVICE])
        _wait_for_service("STOPPED")
        # A worker starts the check afresh after a recycle.
        _recycle_pool()
        _assert_local_only(target, remote_source, port, path)

        # Recovery: start the service, let the 30 second recheck pass.
        started = time.time()
        _run(["sc.exe", "start", SERVICE])
        _wait_for_service("RUNNING")
        time.sleep(RECHECK_SECONDS)
        recovered = _wait_for_local_header(port, path, want=False, timeout=90.0)
        assert recovered.getheader(STATUS_HEADER) is None, (
            f"{STATUS_HEADER} still present after the service recovered"
        )
        assert _recovery_entry_since(started), "no recovery entry in the event log"
    finally:
        # Cleanup must not mask the failure above: restore without raising.
        _restore_service()
        _recycle_pool()


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite --
    # vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
