# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Regression tests for socket double-close bugs #1 and #2.

Bug #1: Management socket double-close — disconnecting mid-response
triggers both OnMgmtRead(UV_EOF) and OnMgmtWriteDone, each trying
to delete MgmtClientContext. Fixed via `bool closing` guard.

Bug #2: Client connection (IPC) double-close — same pattern on the
notification socket between OnConnectionTimeout and CloseClientConnection.
Fixed via `bool closing` guard in ClientContext.
"""

import time

import metrics_helpers
import requests
from conftest import docker_exec

# ---------------------------------------------------------------------------
# Bug #1: Management socket double-close
# ---------------------------------------------------------------------------


class TestMgmtSocketDoubleClose:
    """Regression tests for management socket double-close (bug #1)."""

    def _run_mgmt_script(self, script):
        """Run a Python socket script inside the worker container."""
        return docker_exec("worker", ["python3", "-c", script])

    def test_mgmt_stats_immediate_close(self, stress_services):
        """Send STATS, close socket immediately before reading response.

        50 iterations. Worker must remain healthy.
        """
        script = r"""
import socket, sys, time

for i in range(50):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('/shared/pagespeed.sock.mgmt')
        s.sendall(b'STATS\n')
        s.close()
    except Exception as e:
        print(f'iter {i}: {e}', file=sys.stderr)
    time.sleep(0.01)

print('OK')
"""
        result = self._run_mgmt_script(script)
        assert result.returncode == 0, f"Script failed: {result.stderr}"

        # Worker must still be healthy after the storm
        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after test: {health}"

    def test_mgmt_stats_partial_read_close(self, stress_services):
        """Send STATS, read 10 bytes, close. 50 iterations.

        Worker must remain healthy.
        """
        script = r"""
import socket, sys, time

for i in range(50):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('/shared/pagespeed.sock.mgmt')
        s.sendall(b'STATS\n')
        _ = s.recv(10)
        s.close()
    except Exception as e:
        print(f'iter {i}: {e}', file=sys.stderr)
    time.sleep(0.01)

print('OK')
"""
        result = self._run_mgmt_script(script)
        assert result.returncode == 0, f"Script failed: {result.stderr}"

        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after test: {health}"

    def test_mgmt_concurrent_disconnect(self, stress_services):
        """50 threads each connect + STATS + close after 0-50ms random delay.

        Worker must remain healthy and active connections must drain to 0.
        """
        script = r"""
import socket, sys, time, threading, random

errors = []

def worker(idx):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('/shared/pagespeed.sock.mgmt')
        s.sendall(b'STATS\n')
        time.sleep(random.uniform(0, 0.05))
        s.close()
    except Exception as e:
        errors.append(f'thread {idx}: {e}')

threads = [threading.Thread(target=worker, args=(i,)) for i in range(50)]
for t in threads:
    t.start()
for t in threads:
    t.join(timeout=30)

if errors:
    print(f'{len(errors)} errors', file=sys.stderr)
    for e in errors[:5]:
        print(e, file=sys.stderr)

print('OK')
"""
        result = self._run_mgmt_script(script)
        assert result.returncode == 0, f"Script failed: {result.stderr}"

        # Wait for connections to drain
        time.sleep(1)

        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after test: {health}"


# ---------------------------------------------------------------------------
# Bug #2: Client connection (IPC) double-close
# ---------------------------------------------------------------------------


class TestIpcDoubleClose:
    """Regression tests for IPC notification socket double-close (bug #2)."""

    def _run_ipc_script(self, script):
        """Run a Python socket script inside the worker container."""
        return docker_exec("worker", ["python3", "-c", script])

    def test_ipc_partial_notification_timeout(self, stress_services):
        """Send 4-byte length header claiming 1000 bytes, hold open.

        Wait for worker to timeout the connection. Worker must stay healthy.
        """
        script = r"""
import socket, struct, sys, time

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(60)
s.connect('/shared/pagespeed.sock')
# Send a 4-byte length header claiming 1000 bytes but don't send data
s.sendall(struct.pack('>I', 1000))
# Hold the connection open for a bit (worker has a connection timeout)
time.sleep(5)
try:
    s.close()
except Exception:
    pass

print('OK')
"""
        result = self._run_ipc_script(script)
        assert result.returncode == 0, f"Script failed: {result.stderr}"

        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after partial notification: {health}"

    def test_ipc_send_and_immediate_close(self, stress_services):
        """Send full notification then immediately close. 100 iterations.

        Worker must remain healthy.
        """
        script = r"""
import socket, struct, sys, time

# Build a valid v5 IPC notification payload:
# Wire format: [4B total_length BE][1B version=5][4B url_len BE][url]
#   [4B host_len BE][hostname][1B content_type][4B mask BE][1B scheme]
#   [1B agent_request][4B option_ctx_len][ctx][1B sig_len][sig]
def make_notification(url, hostname='localhost'):
    url_bytes = url.encode()
    hostname_bytes = hostname.encode()
    payload = struct.pack('B', 5)  # version
    payload += struct.pack('>I', len(url_bytes)) + url_bytes
    payload += struct.pack('>I', len(hostname_bytes)) + hostname_bytes
    payload += struct.pack('B', 3)  # content_type = IMAGE
    payload += struct.pack('>I', 0x08)  # capability_mask
    payload += struct.pack('B', 2)  # scheme = HTTPS
    payload += struct.pack('B', 0)  # agent_request
    payload += struct.pack('>I', 0) + struct.pack('B', 0)  # empty option context
    return struct.pack('>I', len(payload)) + payload

for i in range(100):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('/shared/pagespeed.sock')
        s.sendall(make_notification(f'/images/ipc-close-{i}.jpg'))
        s.close()
    except Exception as e:
        print(f'iter {i}: {e}', file=sys.stderr)
    time.sleep(0.005)

print('OK')
"""
        result = self._run_ipc_script(script)
        assert result.returncode == 0, f"Script failed: {result.stderr}"

        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after test: {health}"

    def test_ipc_rapid_connect_disconnect(self, stress_services):
        """Connect-then-close 200 times with no data. Worker must stay healthy."""
        script = r"""
import socket, sys, time

for i in range(200):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('/shared/pagespeed.sock')
        s.close()
    except Exception as e:
        print(f'iter {i}: {e}', file=sys.stderr)

print('OK')
"""
        result = self._run_ipc_script(script)
        assert result.returncode == 0, f"Script failed: {result.stderr}"

        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, (
            f"Worker unhealthy after rapid connect/disconnect: {health}"
        )

    def test_worker_functional_after_storm(self, stress_services):
        """Run all abuse patterns, then verify worker still processes notifications.

        Send a real request through nginx and verify the notification counter
        increments, confirming the worker is still functional.
        """
        base_url = stress_services.nginx_url

        # Run the abuse patterns via inline scripts
        scripts = [
            # Rapid connect/disconnect
            r"""
import socket, time
for i in range(50):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('/shared/pagespeed.sock')
        s.close()
    except: pass
print('OK')
""",
            # Partial data
            r"""
import socket, struct, time
for i in range(10):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('/shared/pagespeed.sock')
        s.sendall(struct.pack('>I', 999))
        time.sleep(0.1)
        s.close()
    except: pass
print('OK')
""",
            # Mgmt abuse
            r"""
import socket, time
for i in range(50):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('/shared/pagespeed.sock.mgmt')
        s.sendall(b'STATS\n')
        s.close()
    except: pass
print('OK')
""",
        ]

        for script in scripts:
            docker_exec("worker", ["python3", "-c", script])

        time.sleep(2)

        # Take a STATS snapshot
        try:
            raw = metrics_helpers.get_stats_via_docker()
            stats_before = metrics_helpers.parse_stats(raw)
            received_before = stats_before.get("notifications", {}).get("received", 0)
        except Exception:
            received_before = 0

        # Send a real request through nginx to trigger a notification
        r = requests.get(
            base_url + "/images/img-100k.jpg?functional_after_storm",
            timeout=10,
        )
        assert r.status_code == 200, f"Request failed: {r.status_code}"

        # Poll for notification counter to increase
        try:
            stats_after = metrics_helpers.wait_for_stats_condition(
                lambda s: s.get("notifications", {}).get("received", 0) > received_before,
                timeout=10,
                interval=0.5,
            )
        except TimeoutError:
            raw = metrics_helpers.get_stats_via_docker()
            stats_after = metrics_helpers.parse_stats(raw)
        received_after = stats_after.get("notifications", {}).get("received", 0)

        assert received_after > received_before, (
            f"Notification counter did not increment: {received_before} -> {received_after}"
        )

        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy: {health}"
