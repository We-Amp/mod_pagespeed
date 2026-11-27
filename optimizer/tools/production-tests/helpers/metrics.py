# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Metrics and IPC helpers for production tests.

Queries worker stats, health, and management sockets via docker exec.
Adapted from tools/stress/metrics_helpers.py.
"""

import json
import re
import subprocess
import time

COMPOSE_DIR = None  # Set by conftest


def docker_exec(service, command, timeout=30):
    """Run a command inside a Docker Compose service container."""
    result = subprocess.run(
        ["docker", "compose", "exec", "-T", service, "bash", "-c", command],
        capture_output=True,
        text=True,
        timeout=timeout,
        cwd=COMPOSE_DIR,
    )
    return result.stdout.strip()


def _run_socket_script(socket_path, send_command=None, timeout=10, service="worker"):
    """Run a Python socket script inside a Docker Compose service container.

    Uses subprocess list form to avoid shell quoting issues.
    """
    if send_command:
        send_line = f's.sendall(b"{send_command}\\n")'
    else:
        send_line = ""

    script = f"""import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout({timeout})
s.connect("{socket_path}")
{send_line}
data = b""
while True:
    try:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
        if b"\\n" in data:
            break
    except socket.timeout:
        break
s.close()
print(data.decode("utf-8", errors="replace").strip())
"""
    result = subprocess.run(
        ["docker", "compose", "exec", "-T", service, "python3", "-c", script],
        capture_output=True,
        text=True,
        timeout=timeout + 5,
        cwd=COMPOSE_DIR,
    )
    return result.stdout.strip()


def get_stats(timeout=60):
    """Get worker stats as parsed JSON dict."""
    raw = _run_socket_script("/shared/pagespeed.sock.mgmt", "STATS", timeout=timeout)
    return json.loads(raw)


def get_health(timeout=10):
    """Get worker health status string."""
    return _run_socket_script("/shared/pagespeed.sock.health", timeout=timeout)


def get_metrics(timeout=10):
    """Get Prometheus metrics text."""
    return _run_socket_script("/shared/pagespeed.sock.mgmt", "METRICS", timeout=timeout)


def purge(url, hostname="localhost", token="", timeout=10):
    """Purge a URL from cache via management socket."""
    script = f"""import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout({timeout})
s.connect("/shared/pagespeed.sock.mgmt")
s.sendall(b"PURGE {hostname} {url}\\n")
data = b""
while True:
    try:
        chunk = s.recv(1024)
        if not chunk:
            break
        data += chunk
        if b"\\n" in data:
            break
    except socket.timeout:
        break
s.close()
print(data.decode().strip())
"""
    result = subprocess.run(
        ["docker", "compose", "exec", "-T", "worker", "python3", "-c", script],
        capture_output=True,
        text=True,
        timeout=timeout + 5,
        cwd=COMPOSE_DIR,
    )
    return result.stdout.strip()


def parse_prometheus_metrics(text):
    """Parse Prometheus exposition format into dict."""
    metrics = {}
    for line in text.strip().split("\n"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2:
            metrics[parts[0]] = float(parts[1])
    return metrics


def parse_health(text):
    """Parse health status string into dict."""
    result = {"raw": text}
    match = re.match(r"(\w+)\s+(\d+)/(\d+)", text)
    if match:
        result["status"] = match.group(1)
        result["active"] = int(match.group(2))
        result["max"] = int(match.group(3))
    for kv in re.finditer(r"(\w+)=(\d+)", text):
        result[kv.group(1)] = int(kv.group(2))
    return result


def wait_for_stats_condition(predicate, timeout=30.0, interval=1.0):
    """Poll STATS until predicate(stats_dict) returns True."""
    deadline = time.time() + timeout
    last_stats = None
    while time.time() < deadline:
        try:
            last_stats = get_stats()
            if predicate(last_stats):
                return last_stats
        except Exception:
            pass
        time.sleep(interval)
    raise TimeoutError(f"Stats condition not met within {timeout}s. Last: {last_stats}")
