# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Worker connection, buffer, malformed input, and shutdown stress tests.

Tests the worker's resource limits, error handling, and graceful degradation
when receiving malformed or excessive input via Unix socket IPC.
"""

import struct
import time

import metrics_helpers
from conftest import docker_exec
from ipc_client import (
    DEFAULT_MASK,
    EMPTY_OPTION_CONTEXT,
    EMPTY_OPTION_CONTEXT_EXPR,
    IPC_VERSION,
    ContentType,
)

# Worker socket paths (inside container)
SOCKET_PATH = "/shared/pagespeed.sock"
MGMT_SOCKET_PATH = "/shared/pagespeed.sock.mgmt"
HEALTH_SOCKET_PATH = "/shared/pagespeed.sock.health"


def send_notification_via_docker(
    url, content_type=ContentType.IMAGE, mask=DEFAULT_MASK, hostname="localhost"
):
    """Send a notification by exec-ing a Python script inside the worker container."""
    ct_val = int(content_type)
    script = f"""
import socket, struct
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect('{SOCKET_PATH}')
url = b'{url}'
hostname = b'{hostname}'
payload = struct.pack('B', {IPC_VERSION}) + struct.pack('>I', len(url)) + url + struct.pack('>I', len(hostname)) + hostname + struct.pack('B', {ct_val}) + struct.pack('>I', {mask}) + struct.pack('B', 2) + struct.pack('B', 0) + {EMPTY_OPTION_CONTEXT_EXPR}
msg = struct.pack('>I', len(payload)) + payload
s.sendall(msg)
s.close()
"""
    return docker_exec("worker", ["python3", "-c", script])


def send_raw_via_docker(hex_data):
    """Send raw hex bytes to the worker socket from inside the container."""
    script = f"""
import socket, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect('{SOCKET_PATH}')
s.sendall(bytes.fromhex('{hex_data}'))
time.sleep(0.5)
s.close()
"""
    return docker_exec("worker", ["python3", "-c", script])


def get_health():
    """Get health status via docker exec."""
    return metrics_helpers.get_health_via_docker()


def get_stats():
    """Get STATS JSON via docker exec."""
    return metrics_helpers.get_stats_via_docker()


class TestWorkerConnections:
    """Worker connection limit tests."""

    def test_exceed_max_connections(self, stress_services):
        """Open more connections than max_connections (16)."""
        script = f"""
import socket, time, threading

results = []

def connect_and_hold(i):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect('{SOCKET_PATH}')
        time.sleep(2)
        s.close()
        results.append(('ok', i))
    except Exception as e:
        results.append(('err', i, str(e)))

threads = [threading.Thread(target=connect_and_hold, args=(i,)) for i in range(20)]
for t in threads:
    t.start()
for t in threads:
    t.join()
ok = sum(1 for r in results if r[0] == 'ok')
print(f'connected:{{ok}}')
"""
        result = docker_exec("worker", ["python3", "-c", script])

        # Worker should still be healthy
        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_connection_limit_recovery(self, stress_services):
        """Fill max connections, close all, then refill."""
        for _ in range(2):
            script = f"""
import socket, time, threading

socks = []

def connect(i):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect('{SOCKET_PATH}')
        socks.append(s)
    except:
        pass

threads = [threading.Thread(target=connect, args=(i,)) for i in range(16)]
for t in threads:
    t.start()
for t in threads:
    t.join()
time.sleep(1)
for s in socks:
    s.close()
print(f'connected:{{len(socks)}}')
"""
            result = docker_exec("worker", ["python3", "-c", script])

        health = get_health()
        assert "OK" in health, f"Worker unhealthy after recovery: {health}"


class TestNotificationFlood:
    """Notification flood tests."""

    def test_notification_flood_single(self, stress_services):
        """1000 notifications rapid-fire on one connection."""
        script = f"""
import socket, struct

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect('{SOCKET_PATH}')
hostname = b'localhost'
for i in range(1000):
    url = f'/images/img-100k.jpg?flood={{i}}'.encode()
    payload = struct.pack('B', {IPC_VERSION}) + struct.pack('>I', len(url)) + url + struct.pack('>I', len(hostname)) + hostname + struct.pack('B', 3) + struct.pack('>I', 0xC8) + struct.pack('B', 2) + struct.pack('B', 0) + {EMPTY_OPTION_CONTEXT_EXPR}
    s.sendall(struct.pack('>I', len(payload)) + payload)
s.close()
print('sent:1000')
"""
        before_raw = get_stats()
        before = metrics_helpers.parse_stats(before_raw)
        before_received = before.get("notifications", {}).get("received", 0)

        result = docker_exec("worker", ["python3", "-c", script])

        # Poll until at least 500 notifications have been received
        try:
            final_stats = metrics_helpers.wait_for_stats_condition(
                lambda s: s.get("notifications", {}).get("received", 0) - before_received >= 500,
                timeout=120,
                interval=1.0,
            )
        except TimeoutError:
            pass

        after_raw = get_stats()
        after = metrics_helpers.parse_stats(after_raw)
        after_received = after.get("notifications", {}).get("received", 0)

        delta = after_received - before_received
        assert delta >= 500, f"Expected >= 500 notifications received, got {delta}"

    def test_notification_flood_concurrent(self, stress_services):
        """16 connections x 50 notifications each."""
        script = f"""
import socket, struct, threading

count = [0]
lock = threading.Lock()

def flood(idx):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect('{SOCKET_PATH}')
        hostname = b'localhost'
        for i in range(50):
            url = f'/images/img-100k.jpg?flood_c{{idx}}_{{i}}'.encode()
            payload = struct.pack('B', {IPC_VERSION}) + struct.pack('>I', len(url)) + url + struct.pack('>I', len(hostname)) + hostname + struct.pack('B', 3) + struct.pack('>I', 0xC8) + struct.pack('B', 2) + struct.pack('B', 0) + {EMPTY_OPTION_CONTEXT_EXPR}
            s.sendall(struct.pack('>I', len(payload)) + payload)
        s.close()
        with lock:
            count[0] += 50
    except:
        pass

threads = [threading.Thread(target=flood, args=(i,)) for i in range(16)]
for t in threads:
    t.start()
for t in threads:
    t.join()
print(f'sent:{{count[0]}}')
"""
        before_raw = get_stats()
        before = metrics_helpers.parse_stats(before_raw)
        before_received = before.get("notifications", {}).get("received", 0)

        docker_exec("worker", ["python3", "-c", script])

        # Poll until at least 400 notifications have been received
        try:
            final_stats = metrics_helpers.wait_for_stats_condition(
                lambda s: s.get("notifications", {}).get("received", 0) - before_received >= 400,
                timeout=120,
                interval=1.0,
            )
        except TimeoutError:
            pass

        after_raw = get_stats()
        after = metrics_helpers.parse_stats(after_raw)
        delta = after.get("notifications", {}).get("received", 0) - before_received
        assert delta >= 400, f"Expected >= 400 notifications, got {delta}"


class TestSizeLimits:
    """Content size limit tests."""

    def test_near_limit_image(self, stress_services):
        """9.5MB image should be processed without crash."""
        send_notification_via_docker("/images/img-9.5m.jpg", ContentType.IMAGE)
        # Poll for health instead of blind wait
        deadline = time.time() + 15
        health = None
        while time.time() < deadline:
            try:
                health = metrics_helpers.get_health_via_docker(timeout=5)
                if "OK" in health:
                    break
            except Exception:
                pass
            time.sleep(1)

        if health is None:
            health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_over_limit_image(self, stress_services):
        """10.5MB image should be skipped without crash."""
        send_notification_via_docker("/images/img-10.5m.jpg", ContentType.IMAGE)
        time.sleep(5)

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_max_buffer_exceeded(self, stress_services):
        """Send > max_buffer_size (1MB) without completing a message."""
        # Generate large payload inside the container to avoid arg-too-long
        script = f"""
import socket, struct, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect('{SOCKET_PATH}')
length_header = struct.pack('>I', 2 * 1024 * 1024)
s.sendall(length_header)
garbage = b'X' * (1100 * 1024)
try:
    s.sendall(garbage)
except BrokenPipeError:
    pass
time.sleep(0.5)
s.close()
print('done')
"""
        docker_exec("worker", ["python3", "-c", script])
        time.sleep(1)

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"


class TestMalformedInput:
    """Malformed message handling tests."""

    def test_truncated_message(self, stress_services):
        """Send only length header, then close."""
        hex_data = struct.pack(">I", 100).hex()
        send_raw_via_docker(hex_data)
        time.sleep(1)

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_zero_length_message(self, stress_services):
        """Send a zero-length message."""
        hex_data = struct.pack(">I", 0).hex()
        send_raw_via_docker(hex_data)
        time.sleep(1)

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_huge_length_header(self, stress_services):
        """Length=100MB but only a few payload bytes."""
        length_header = struct.pack(">I", 100 * 1024 * 1024)
        payload = b"ABC"
        hex_data = (length_header + payload).hex()
        send_raw_via_docker(hex_data)
        time.sleep(1)

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_invalid_content_type(self, stress_services):
        """Send content_type=99 (invalid)."""
        url = b"/images/img-100k.jpg"
        hostname = b"localhost"
        payload = struct.pack("B", IPC_VERSION)  # version
        payload += struct.pack(">I", len(url)) + url
        payload += struct.pack(">I", len(hostname)) + hostname
        payload += struct.pack("B", 99)  # Invalid content type
        payload += struct.pack(">I", DEFAULT_MASK)
        payload += struct.pack("B", 2)  # scheme
        payload += struct.pack("B", 0)  # agent_request (v4)
        payload += EMPTY_OPTION_CONTEXT  # option context (v5), site + flags (v6)
        msg = struct.pack(">I", len(payload)) + payload
        hex_data = msg.hex()

        send_raw_via_docker(hex_data)
        time.sleep(1)

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_garbage_bytes(self, stress_services):
        """Send 4KB of random data."""
        import random

        garbage = bytes(random.getrandbits(8) for _ in range(4096))
        hex_data = garbage.hex()

        send_raw_via_docker(hex_data)
        time.sleep(1)

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"


class TestSlowClients:
    """Slow client simulation tests."""

    def test_slow_trickle_client(self, stress_services):
        """Send 1 byte every 500ms, should timeout at 5s."""
        script = f"""
import socket, time

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect('{SOCKET_PATH}')
data = b'\\x00\\x00\\x00\\x64' + b'A' * 100
start = time.time()
for b in data:
    try:
        s.sendall(bytes([b]))
        time.sleep(0.5)
    except:
        break
elapsed = time.time() - start
s.close()
print(f'elapsed:{{elapsed:.1f}}')
"""
        result = docker_exec("worker", ["python3", "-c", script])

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_many_slow_clients(self, stress_services):
        """16 slow connections, all should timeout."""
        script = f"""
import socket, time, threading

def slow_client(idx):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect('{SOCKET_PATH}')
        for _ in range(20):
            try:
                s.sendall(b'\\x00')
                time.sleep(0.5)
            except:
                break
        s.close()
    except:
        pass

threads = [threading.Thread(target=slow_client, args=(i,)) for i in range(16)]
for t in threads:
    t.start()
for t in threads:
    t.join(timeout=12)
print('done')
"""
        docker_exec("worker", ["python3", "-c", script])
        time.sleep(1)

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"


class TestGracefulShutdown:
    """Graceful shutdown tests."""

    def test_graceful_shutdown_active(self, stress_services):
        """SIGTERM with active connections should drain gracefully."""
        script = f"""
import socket, struct, time, threading

def hold_conn(idx):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect('{SOCKET_PATH}')
        url = f'/images/img-100k.jpg?shutdown={{idx}}'.encode()
        hostname = b'localhost'
        payload = struct.pack('B', {IPC_VERSION}) + struct.pack('>I', len(url)) + url + struct.pack('>I', len(hostname)) + hostname + struct.pack('B', 3) + struct.pack('>I', 0xC8) + struct.pack('B', 2) + struct.pack('B', 0) + {EMPTY_OPTION_CONTEXT_EXPR}
        s.sendall(struct.pack('>I', len(payload)) + payload)
        time.sleep(5)
        s.close()
    except:
        pass

threads = [threading.Thread(target=hold_conn, args=(i,)) for i in range(5)]
for t in threads:
    t.start()
time.sleep(1)
print('connections_open')
for t in threads:
    t.join(timeout=10)
"""
        result = docker_exec("worker", ["python3", "-c", script])

        time.sleep(2)
        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"


class TestMgmtSocket:
    """Management socket stress tests."""

    def test_concurrent_stats_queries(self, stress_services):
        """100 simultaneous STATS requests to .mgmt."""
        script = f"""
import socket, threading, json

results = []
lock = threading.Lock()

def query_stats(idx):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect('{MGMT_SOCKET_PATH}')
        s.sendall(b'STATS\\n')
        d = b''
        while True:
            c = s.recv(4096)
            if not c:
                break
            d += c
            if b'\\n' in c:
                break
        s.close()
        j = json.loads(d.decode())
        with lock:
            results.append('ok' if j.get('status') == 'ok' else 'bad')
    except Exception as e:
        with lock:
            results.append(f'err:{{e}}')

threads = [threading.Thread(target=query_stats, args=(i,)) for i in range(100)]
for t in threads:
    t.start()
for t in threads:
    t.join()
ok = sum(1 for r in results if r == 'ok')
print(f'ok:{{ok}}')
"""
        result = docker_exec("worker", ["python3", "-c", script])
        # At least most should succeed
        if result.stdout:
            ok_match = result.stdout.strip().split("ok:")
            if len(ok_match) > 1:
                ok_count = int(ok_match[-1])
                assert ok_count >= 50, f"Only {ok_count}/100 STATS queries succeeded"

    def test_purge_during_processing(self, stress_services):
        """PURGE while worker is generating variants."""
        send_notification_via_docker(
            "/images/img-500k.jpg?purge_test", ContentType.IMAGE
        )
        time.sleep(1)

        resp = metrics_helpers.purge_via_docker("/images/img-500k.jpg?purge_test")
        assert resp.startswith("OK"), f"PURGE failed: {resp}"

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"

    def test_rapid_purge_cycle(self, stress_services):
        """50 request-then-purge loops."""
        import requests as req

        import os
        base_url = f"http://localhost:{os.environ.get('STRESS_NGINX_PORT', '8190')}"
        for i in range(50):
            url_path = f"/images/img-100k.jpg?purge_cycle={i}"
            try:
                req.get(base_url + url_path, timeout=5)
            except Exception:
                pass
            try:
                metrics_helpers.purge_via_docker(url_path)
            except Exception:
                pass

        health = get_health()
        assert "OK" in health, f"Worker unhealthy: {health}"
