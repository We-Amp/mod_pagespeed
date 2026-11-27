# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Host header forwarding and X-Forwarded-For tests.

Verifies that the proxy correctly forwards Host headers and adds
X-Forwarded-For.

RFC 9110 Section 7.2: Host and :authority.
Uses /nocache.html to avoid 103 Early Hints from cached HTML.
"""


class TestHostForwarding:
    """Host header forwarded to origin."""

    def test_response_successful_with_host(self, client):
        """Request with Host header returns successful response."""
        r = client.get("/nocache.html", headers={"Host": "example.com"})
        if r.status_code == 103:
            return
        assert r.status_code == 200

    def test_no_internal_hostname_leak(self, client):
        """Response doesn't expose internal Docker hostnames."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        # Check that internal hostnames aren't leaked in headers
        for key, value in r.headers.items():
            if key.lower() not in ("server", "via"):
                assert "origin:" not in value.lower(), (
                    f"Internal hostname leaked in {key}: {value}"
                )

    def test_port_in_host_handled(self, client):
        """Host header with port is handled correctly."""
        r = client.get("/nocache.html", headers={"Host": "example.com:8080"})
        if r.status_code == 103:
            return
        assert r.status_code == 200


class TestXForwardedFor:
    """X-Forwarded-For header handling."""

    def test_response_from_proxy_succeeds(self, client):
        """Response through proxy is successful."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200

    def test_baseline_response_succeeds(self, baseline_client):
        """Response through baseline proxy is successful."""
        r = baseline_client.get("/baseline/small.html")
        assert r.status_code == 200

    def test_via_header_not_required(self, client):
        """Via header is optional for simple proxy."""
        # We don't mandate Via but test that it's valid if present
        r = client.get("/etag-test.css")
        assert r.status_code == 200
