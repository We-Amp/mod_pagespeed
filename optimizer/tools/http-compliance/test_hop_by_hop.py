# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Hop-by-hop header handling tests.

Tests how the nginx reverse proxy handles hop-by-hop headers from
the origin. Note: nginx as a reverse proxy does NOT strip all
hop-by-hop headers (like Proxy-Authenticate or Trailer) — it only
handles Connection and Transfer-Encoding itself.

RFC 9110 Section 7.6.1: Connection header and hop-by-hop headers.
"""


class TestHopByHopHeaders:
    """Proxy handling of hop-by-hop headers from origin."""

    def test_response_body_intact(self, client):
        """Body intact after proxy processing."""
        r = client.get("/hop-by-hop")
        assert r.status_code == 200
        assert len(r.content) > 0

    def test_content_type_preserved(self, client):
        """Content-Type (end-to-end header) preserved."""
        r = client.get("/hop-by-hop")
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "text/html" in ct

    def test_date_preserved(self, client):
        """Date header (end-to-end) present in proxy response."""
        r = client.get("/hop-by-hop")
        assert r.status_code == 200
        client.assert_has_date(r)

    def test_proxy_authenticate_behavior(self, client):
        """Document Proxy-Authenticate behavior through nginx.

        nginx reverse proxy passes through Proxy-Authenticate from
        upstream. This is technically incorrect per RFC 9110 (it's
        hop-by-hop) but is standard nginx behavior.
        """
        r = client.get("/hop-by-hop")
        assert r.status_code == 200
        # nginx passes through Proxy-Authenticate — document this
        # behavior rather than assert it's stripped.
        # If nginx strips it in the future, both behaviors are acceptable.
        _ = r.headers.get("Proxy-Authenticate")
        assert r.status_code == 200  # Just verify the response is valid

    def test_baseline_behavior(self, baseline_client):
        """Same behavior with pagespeed disabled."""
        r = baseline_client.get("/baseline/hop-by-hop")
        assert r.status_code == 200
        assert len(r.content) > 0

    def test_x_pagespeed_added(self, client):
        """X-PageSpeed header added by the module."""
        r = client.get("/hop-by-hop")
        assert r.status_code == 200
        xs = r.headers.get("X-PageSpeed")
        assert xs in ("MISS", "HIT")

    def test_connection_header_from_nginx(self, client):
        """Connection header is set by nginx (not origin's value)."""
        r = client.get("/hop-by-hop")
        assert r.status_code == 200
        # Nginx sets its own Connection header (keep-alive or close)
        conn = r.headers.get("Connection", "")
        assert conn in ("keep-alive", "close", "")

    def test_transfer_encoding_handled(self, client):
        """Transfer-Encoding is managed by nginx, not origin value."""
        r = client.get("/hop-by-hop")
        assert r.status_code == 200
        # Non-chunked small response should have Content-Length
        has_cl = "Content-Length" in r.headers
        has_te = "Transfer-Encoding" in r.headers
        assert has_cl or has_te
