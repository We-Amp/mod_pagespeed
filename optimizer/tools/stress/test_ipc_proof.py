# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Proof tests for IPC client wire format, content type enum, and purge protocol.

These tests run locally (no Docker needed) and verify that the Python IPC
client produces bytes matching the C++ v5 wire format defined in
src/proto/worker_ipc.h.
"""

import struct
from unittest.mock import patch

from ipc_client import (
    IPC_VERSION,
    ContentType,
    IpcClient,
    MgmtClient,
)


class TestWireFormat:
    """Verify serialize_notification produces the current wire format."""

    def test_serialize_matches_current_wire_format(self):
        """Parse serialized bytes against the spec and validate fields."""
        url = "/images/test.jpg"
        hostname = "example.com"
        ct = ContentType.IMAGE
        mask = 0xC8

        data = IpcClient.serialize_notification(url, ct, mask=mask, hostname=hostname)

        # Parse: [4B total_len][1B version][4B url_len][url][4B host_len][host]
        #        [1B ct][4B mask][1B scheme][1B agent_request]
        #        [4B option_ctx_len][ctx][1B option_sig_len][sig]
        offset = 0
        total_len = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        assert total_len == len(data) - 4

        version = data[offset]
        offset += 1
        # Pinned to the client's own constant rather than a literal: the point
        # of this test is that the serializer and the version it claims agree,
        # and a literal here only records which version someone typed last.
        assert version == IPC_VERSION, f"Expected version {IPC_VERSION}, got {version}"

        url_len = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        parsed_url = data[offset : offset + url_len].decode("utf-8")
        offset += url_len
        assert parsed_url == url

        host_len = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        assert host_len < 512, f"hostname_len {host_len} >= kMaxHostnameLength"
        parsed_host = data[offset : offset + host_len].decode("utf-8")
        offset += host_len
        assert parsed_host == hostname

        ct_byte = data[offset]
        offset += 1
        assert ct_byte == int(ct)
        assert ct_byte <= 4, f"content_type {ct_byte} out of range"

        parsed_mask = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        assert parsed_mask == mask

        scheme = data[offset]
        offset += 1
        assert scheme == 2, f"Expected scheme 2 (HTTPS), got {scheme}"

        agent_request = data[offset]
        offset += 1
        assert agent_request == 0, f"Expected agent_request 0, got {agent_request}"

        # Option context and signature: both empty, and the receiver refuses
        # one without the other, so both length prefixes must be present.
        ctx_len = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        assert ctx_len == 0, f"Expected empty option context, got {ctx_len} bytes"

        sig_len = data[offset]
        offset += 1
        assert sig_len == 0, f"Expected empty option signature, got {sig_len} bytes"

        # v6 appends the site length and a flags byte after the signature.
        site_len = data[offset]
        offset += 1
        assert site_len == 0, f"Expected empty site, got {site_len} bytes"

        flags = data[offset]
        offset += 1
        assert flags == 0, f"Expected zero flags, got {flags}"

        # Should have consumed all bytes
        assert offset == len(data)

    def test_serialize_default_hostname(self):
        """Default hostname is 'localhost'."""
        data = IpcClient.serialize_notification("/test", ContentType.HTML, mask=0xC8)
        # Skip total_len(4) + version(1) + url_len(4) + url(5="/test")
        offset = 4 + 1 + 4 + 5
        host_len = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        parsed_host = data[offset : offset + host_len].decode("utf-8")
        assert parsed_host == "localhost"


class TestContentTypeEnum:
    """Verify Python ContentType matches C++ lib/classify/content_type.h."""

    def test_html_is_zero(self):
        assert ContentType.HTML == 0

    def test_css_is_one(self):
        assert ContentType.CSS == 1

    def test_javascript_is_two(self):
        assert ContentType.JAVASCRIPT == 2

    def test_image_is_three(self):
        assert ContentType.IMAGE == 3

    def test_other_is_four(self):
        assert ContentType.OTHER == 4

    def test_no_unknown_member(self):
        """UNKNOWN should not exist — it was a v1 Python-only artifact."""
        assert not hasattr(ContentType, "UNKNOWN")


class TestPurgeProtocol:
    """Verify MgmtClient.purge sends AUTH before PURGE with hostname."""

    def test_purge_sends_auth_then_purge(self):
        """Capture bytes sent by purge() and verify AUTH + PURGE sequence."""
        sent_data = []

        class FakeSocket:
            def settimeout(self, t):
                pass

            def connect(self, path):
                pass

            def sendall(self, data):
                sent_data.append(data)

            def recv(self, size):
                # Return OK for both AUTH and PURGE responses
                return b"OK\n"

            def close(self):
                pass

        with patch("ipc_client.socket.socket", return_value=FakeSocket()):
            client = MgmtClient("/fake/path")
            resp = client.purge("/test.jpg", hostname="example.com", token="my-token")

        assert len(sent_data) == 2, f"Expected 2 sends, got {len(sent_data)}"
        auth_cmd = sent_data[0].decode("utf-8")
        purge_cmd = sent_data[1].decode("utf-8")

        assert auth_cmd == "AUTH my-token\n"
        assert purge_cmd == "PURGE example.com /test.jpg\n"

    def test_purge_without_token_skips_auth(self):
        """When no token, AUTH is not sent."""
        sent_data = []

        class FakeSocket:
            def settimeout(self, t):
                pass

            def connect(self, path):
                pass

            def sendall(self, data):
                sent_data.append(data)

            def recv(self, size):
                return b"OK 0 entries deleted\n"

            def close(self):
                pass

        with patch("ipc_client.socket.socket", return_value=FakeSocket()):
            client = MgmtClient("/fake/path")
            resp = client.purge("/test.jpg", hostname="localhost")

        assert len(sent_data) == 1
        purge_cmd = sent_data[0].decode("utf-8")
        assert purge_cmd == "PURGE localhost /test.jpg\n"

    def test_purge_hostname_default(self):
        """Default hostname is 'localhost'."""
        sent_data = []

        class FakeSocket:
            def settimeout(self, t):
                pass

            def connect(self, path):
                pass

            def sendall(self, data):
                sent_data.append(data)

            def recv(self, size):
                return b"OK 0 entries deleted\n"

            def close(self):
                pass

        with patch("ipc_client.socket.socket", return_value=FakeSocket()):
            client = MgmtClient("/fake/path")
            client.purge("/test.jpg")

        purge_cmd = sent_data[0].decode("utf-8")
        assert "PURGE localhost /test.jpg" in purge_cmd
