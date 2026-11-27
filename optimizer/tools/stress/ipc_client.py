# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Python IPC client for the PageSpeed worker Unix socket protocol.

Implements the v6 wire format from src/proto/worker_ipc.h:
  [4B: total_length (big-endian, not including this field)]
  [1B: version = IPC_VERSION]
  [4B: url_length]
  [url_length bytes: url]
  [4B: hostname_length]
  [hostname_length bytes: hostname]
  [1B: content_type]
  [4B: capability_mask (big-endian)]
  [1B: scheme (0x01=http, 0x02=https)]
  [1B: agent_request (0/1)]
  [4B: option_context_length][option_context]
  [1B: option_signature_length][option_signature]
  [1B: site_length (0 or 64)][site, lowercase hex]
  [1B: flags (bit0 host vouched, bit1 from module, bit2 legacy caller)]

The version is checked before any field is read and the daemon accepts
exactly one value, so a harness left behind on an older version does not
degrade -- every notification it sends is refused whole, and a test that
measures notifications by counter sees nothing arrive at all.
"""

import socket
import struct
import time
from enum import IntEnum


class ContentType(IntEnum):
    HTML = 0
    CSS = 1
    JAVASCRIPT = 2
    IMAGE = 3
    OTHER = 4


class Scheme(IntEnum):
    HTTP = 1
    HTTPS = 2


# Default capability mask: Desktop + 4G+ = 0xC8
DEFAULT_MASK = 0xC8

# Notify wire version this harness speaks. Must equal kIpcVersion in
# src/proto/worker_ipc.h.
IPC_VERSION = 6

# The tail after agent_request: an option context (4-byte length prefix) and
# its signature (1-byte length prefix), both empty here (v5), then no site
# (1-byte length 0) and no flags (v6). The receiver refuses a frame carrying
# a context without its signature, so they are emitted together.
EMPTY_OPTION_CONTEXT = (
    struct.pack(">I", 0) + struct.pack("B", 0) + struct.pack("B", 0) + struct.pack("B", 0)
)

# The same tail as source text, for the notification scripts that are
# exec'd inside the worker container and cannot import this module.
EMPTY_OPTION_CONTEXT_EXPR = (
    "struct.pack('>I', 0) + struct.pack('B', 0) + struct.pack('B', 0) + struct.pack('B', 0)"
)

# Sentinel values
WARMUP_SENTINEL = 0xFFFFFFFE
EARLY_HINTS_SENTINEL = 0xFFFFFFFF


class IpcClient:
    """Client for communicating with the PageSpeed worker via Unix socket."""

    def __init__(self, socket_path="/shared/pagespeed.sock"):
        self.socket_path = socket_path
        self._sock = None

    def connect(self, timeout=5.0):
        """Connect to the worker Unix socket."""
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        self._sock.connect(self.socket_path)
        return self

    def close(self):
        """Close the connection."""
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *args):
        self.close()

    @staticmethod
    def serialize_notification(
        url,
        content_type,
        mask=DEFAULT_MASK,
        hostname="localhost",
        scheme=2,
        agent_request=False,
    ):
        """Serialize a CacheNotification to v6 wire format bytes."""
        url_bytes = url.encode("utf-8")
        hostname_bytes = hostname.encode("utf-8")
        # Payload: [1B version][4B url_len][url][4B host_len][hostname][1B ct]
        #          [4B mask][1B scheme][1B agent_request][option context]
        payload = struct.pack("B", IPC_VERSION)  # version
        payload += struct.pack(">I", len(url_bytes)) + url_bytes
        payload += struct.pack(">I", len(hostname_bytes)) + hostname_bytes
        payload += struct.pack("B", int(content_type))
        payload += struct.pack(">I", mask)
        payload += struct.pack("B", int(scheme))
        payload += struct.pack("B", 1 if agent_request else 0)  # agent_request (v4)
        payload += EMPTY_OPTION_CONTEXT  # option context + signature (v5), site + flags (v6)
        # Prefix with total length (not including the 4B length field itself)
        return struct.pack(">I", len(payload)) + payload

    def send_notification(
        self,
        url,
        content_type,
        mask=DEFAULT_MASK,
        hostname="localhost",
        scheme=2,
        agent_request=False,
    ):
        """Send a single notification on the current connection."""
        data = self.serialize_notification(
            url, content_type, mask, hostname, scheme, agent_request
        )
        self._sock.sendall(data)

    def send_raw(self, data):
        """Send raw bytes (for malformed message testing)."""
        self._sock.sendall(data)

    def send_bytes_slowly(self, data, bytes_per_chunk=1, delay=0.5):
        """Send data one chunk at a time with delays (slow client simulation)."""
        for i in range(0, len(data), bytes_per_chunk):
            chunk = data[i : i + bytes_per_chunk]
            try:
                self._sock.sendall(chunk)
            except (BrokenPipeError, ConnectionResetError):
                return False
            time.sleep(delay)
        return True

    @property
    def socket(self):
        """Access the underlying socket for advanced testing."""
        return self._sock


class MgmtClient:
    """Client for the worker management socket (.mgmt)."""

    def __init__(self, socket_path="/shared/pagespeed.sock.mgmt"):
        self.socket_path = socket_path

    def send_command(self, command, timeout=5.0):
        """Send a command and return the response."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect(self.socket_path)
            sock.sendall((command + "\n").encode("utf-8"))
            # Read response
            chunks = []
            while True:
                try:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    # Responses are newline-terminated
                    if b"\n" in chunk:
                        break
                except TimeoutError:
                    break
            return b"".join(chunks).decode("utf-8").strip()
        finally:
            sock.close()

    def stats(self, timeout=5.0):
        """Get STATS as a string."""
        return self.send_command("STATS", timeout)

    def metrics(self, timeout=5.0):
        """Get METRICS (Prometheus format) as a string."""
        return self.send_command("METRICS", timeout)

    def purge(self, url, hostname="localhost", token=None, timeout=5.0):
        """PURGE a URL with AUTH + hostname on the same connection."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect(self.socket_path)
            if token:
                sock.sendall(f"AUTH {token}\n".encode())
                auth_resp = self._recv_line(sock)
                if not auth_resp.startswith("OK"):
                    return auth_resp
            sock.sendall(f"PURGE {hostname} {url}\n".encode())
            return self._recv_line(sock)
        finally:
            sock.close()

    @staticmethod
    def _recv_line(sock):
        """Read a newline-terminated response."""
        chunks = []
        while True:
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
                if b"\n" in chunk:
                    break
            except TimeoutError:
                break
        return b"".join(chunks).decode("utf-8").strip()


class HealthClient:
    """Client for the worker health socket (.health)."""

    def __init__(self, socket_path="/shared/pagespeed.sock.health"):
        self.socket_path = socket_path

    def check(self, timeout=5.0):
        """Get health status string."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect(self.socket_path)
            chunks = []
            while True:
                try:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    if b"\n" in chunk:
                        break
                except TimeoutError:
                    break
            return b"".join(chunks).decode("utf-8").strip()
        finally:
            sock.close()
