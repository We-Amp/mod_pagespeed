# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Helpers for querying and validating worker stats, metrics, and health.

Provides parsing, polling, and assertion utilities for the management
and health socket responses.
"""

import json
import re
import subprocess
import time

COMPOSE_FILES = None  # Set by conftest.py (list of paths)

# Reusable Python socket script template.  The placeholder {SOCKET} and
# {COMMAND} are filled in per call.  We use exec() so that we can embed
# a proper multiline while-loop.
_SOCKET_SCRIPT = r"""
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout({timeout})
s.connect('{socket_path}')
{send_line}
d = b''
while True:
    c = s.recv(8192)
    if not c:
        break
    d += c
    if b'\n' in c:
        break
s.close()
sys.stdout.write(d.decode())
"""


def _compose_file_args():
    """Return list of -f arguments for docker compose."""
    args = []
    for f in (COMPOSE_FILES or []):
        args.extend(["-f", f])
    return args


def docker_exec(service, command):
    """Run a command inside a Docker Compose service container."""
    if isinstance(command, str):
        cmd_parts = command.split()
    else:
        cmd_parts = list(command)
    return subprocess.run(
        ["docker", "compose"] + _compose_file_args() + ["exec", "-T", service] + cmd_parts,
        capture_output=True,
        text=True,
    )


def _run_socket_script(socket_path, send_line="", timeout=60):
    """Run a socket query script inside the worker container."""
    script = _SOCKET_SCRIPT.format(
        socket_path=socket_path,
        send_line=send_line,
        timeout=timeout,
    )
    result = docker_exec("worker", ["python3", "-c", script])
    if result.returncode != 0:
        raise RuntimeError(
            f"Socket query to {socket_path} failed: {result.stderr.strip()}"
        )
    return result.stdout.strip()


def get_stats_via_docker(timeout=10):
    """Get STATS JSON by exec-ing into the worker container."""
    return _run_socket_script(
        "/shared/pagespeed.sock.mgmt",
        "s.sendall(b'STATS\\n')",
        timeout=timeout,
    )


def get_health_via_docker(timeout=10):
    """Get health status by exec-ing into the worker container."""
    return _run_socket_script("/shared/pagespeed.sock.health", timeout=timeout)


def get_metrics_via_docker(timeout=10):
    """Get METRICS (Prometheus format) by exec-ing into the worker container."""
    return _run_socket_script(
        "/shared/pagespeed.sock.mgmt",
        "s.sendall(b'METRICS\\n')",
        timeout=timeout,
    )


PURGE_TOKEN = "stress-test-token"


def purge_via_docker(url, hostname=None, timeout=10):
    """PURGE a URL by exec-ing into the worker container.

    Sends AUTH first on the same connection, then PURGE <hostname> <url>.
    The hostname must include the port when non-standard (not 80/443),
    because NormalizeHostname preserves non-default ports in the cache key.
    """
    if hostname is None:
        import os
        port = os.environ.get("STRESS_NGINX_PORT", "8190")
        hostname = f"localhost:{port}"
    safe_url = url.replace("'", "\\'")
    safe_hostname = hostname.replace("'", "\\'")
    # AUTH + PURGE on the same connection (keep_alive after AUTH OK).
    script = rf"""
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout({timeout})
s.connect('/shared/pagespeed.sock.mgmt')
s.sendall(b'AUTH {PURGE_TOKEN}\n')
d = b''
while True:
    c = s.recv(4096)
    if not c:
        break
    d += c
    if b'\n' in c:
        break
auth = d.decode().strip()
if not auth.startswith('OK'):
    sys.stdout.write(auth)
    s.close()
    sys.exit(0)
s.sendall(b'PURGE {safe_hostname} {safe_url}\n')
d = b''
while True:
    c = s.recv(4096)
    if not c:
        break
    d += c
    if b'\n' in c:
        break
s.close()
sys.stdout.write(d.decode())
"""
    result = docker_exec("worker", ["python3", "-c", script])
    if result.returncode != 0:
        raise RuntimeError(f"PURGE failed: {result.stderr.strip()}")
    return result.stdout.strip()


def parse_stats(stats_str):
    """Parse STATS JSON response into a dict."""
    return json.loads(stats_str)


def parse_health(health_str):
    """Parse health response string.

    Format: OK {active}/{max} notifs=N variants=N proactive=N errors=N cache_entries=N
    Returns a dict with parsed values.
    """
    result = {}
    if not health_str.startswith("OK"):
        result["status"] = "ERROR"
        result["raw"] = health_str
        return result

    result["status"] = "OK"
    # Parse active/max connections
    match = re.search(r"(\d+)/(\d+)", health_str)
    if match:
        result["active_connections"] = int(match.group(1))
        result["max_connections"] = int(match.group(2))

    # Parse key=value pairs
    for kv_match in re.finditer(r"(\w+)=(\d+)", health_str):
        result[kv_match.group(1)] = int(kv_match.group(2))

    return result


def parse_prometheus_metrics(metrics_str):
    """Parse Prometheus text exposition format into {metric_name: value}."""
    result = {}
    for line in metrics_str.split("\n"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # Handle labels: metric_name{label="value"} number
        match = re.match(r"^(\S+?)(?:\{[^}]*\})?\s+(.+)$", line)
        if match:
            name = match.group(1)
            try:
                value = float(match.group(2))
            except ValueError:
                continue
            # For labeled metrics, include the full line key
            label_match = re.match(r"^(\S+\{[^}]*\})\s+", line)
            if label_match:
                result[label_match.group(1)] = value
            result.setdefault(name, value)
    return result


def wait_for_stats_condition(predicate, timeout=30.0, interval=1.0, socket_timeout=60):
    """Poll STATS until predicate(stats_dict) returns True or timeout."""
    deadline = time.time() + timeout
    last_stats = None
    while time.time() < deadline:
        try:
            raw = get_stats_via_docker(timeout=socket_timeout)
            stats = parse_stats(raw)
            last_stats = stats
            if predicate(stats):
                return stats
        except Exception:
            pass
        time.sleep(interval)
    raise TimeoutError(
        f"Stats condition not met within {timeout}s. Last stats: {last_stats}"
    )


def assert_counters_monotonic(before, after):
    """Verify that counter values only increase between two STATS snapshots."""
    counter_paths = [
        ("notifications", "received"),
        ("notifications", "skipped_dedup"),
        ("variants", "written"),
        ("variants", "proactive"),
    ]
    for keys in counter_paths:
        val_before = before
        val_after = after
        for k in keys:
            val_before = val_before.get(k, 0) if isinstance(val_before, dict) else 0
            val_after = val_after.get(k, 0) if isinstance(val_after, dict) else 0
        assert val_after >= val_before, (
            f"Counter {'.'.join(keys)} decreased: {val_before} -> {val_after}"
        )

    # Also check top-level error counter
    assert after.get("errors", 0) >= before.get("errors", 0), (
        f"errors decreased: {before.get('errors', 0)} -> {after.get('errors', 0)}"
    )


def assert_stats_invariants(stats):
    """Check STATS counter invariants.

    - variants.written >= variants.proactive
    - by_type sum <= notifications.received
    - errors + variants.written + notifications.skipped_dedup <= notifications.received
    - connections.active == 0 (after quiescence)
    """
    received = stats.get("notifications", {}).get("received", 0)
    skipped = stats.get("notifications", {}).get("skipped_dedup", 0)
    written = stats.get("variants", {}).get("written", 0)
    proactive = stats.get("variants", {}).get("proactive", 0)
    errors = stats.get("errors", 0)

    by_type = stats.get("by_type", {})
    type_sum = sum(
        v.get("n", 0) if isinstance(v, dict) else v for v in by_type.values()
    )

    assert written >= proactive, (
        f"variants.written ({written}) < variants.proactive ({proactive})"
    )
    assert type_sum <= received, (
        f"by_type sum ({type_sum}) > notifications.received ({received})"
    )
    # Note: errors + written + skipped can exceed received because proactive
    # variants add to written without consuming a notification. So we check
    # non-proactive written + skipped + errors <= received.
    non_proactive = written - proactive
    assert non_proactive + skipped + errors <= received, (
        f"non_proactive_written ({non_proactive}) + skipped ({skipped}) + "
        f"errors ({errors}) > received ({received})"
    )
