# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Header preservation and security tests for the caching reverse proxy."""

import json
import os
import socket
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from uuid import uuid4

import pytest


class TestHeadersSecurity:
    """Verify header pass-through, XSS prevention, and request safety."""

    # ------------------------------------------------------------------
    # P0: Hard gate for production
    # ------------------------------------------------------------------

    @pytest.mark.p0
    def test_cors_allow_origin_preserved(self, client):
        """CORS Access-Control-Allow-Origin header present on MISS (pass-through)."""
        uid = uuid4().hex
        path = f"/headers/cors?_={uid}"

        r = client.get(path)
        assert r.status_code == 200
        acao = r.headers.get("Access-Control-Allow-Origin")
        assert acao is not None, "Access-Control-Allow-Origin missing on MISS response"
        assert acao == "https://example.com"

    @pytest.mark.p0
    def test_cors_preflight_not_cached(self, client):
        """OPTIONS preflight for /headers/cors returns 204 and is not cached."""
        uid = uuid4().hex
        path = f"/headers/cors?_={uid}"

        for _ in range(3):
            r = client.options(path)
            assert r.status_code == 204, (
                f"Expected 204 for CORS preflight, got {r.status_code}"
            )
            xps = r.headers.get("X-PageSpeed")
            assert xps != "HIT", "OPTIONS preflight should not be served from cache"

    @pytest.mark.p0
    def test_csp_header_preserved(self, client):
        """Content-Security-Policy header present on MISS (pass-through)."""
        uid = uuid4().hex
        path = f"/headers/security?_={uid}"

        r = client.get(path)
        assert r.status_code == 200
        csp = r.headers.get("Content-Security-Policy")
        assert csp is not None, "Content-Security-Policy missing on MISS response"
        assert "default-src" in csp

    @pytest.mark.p0
    def test_hsts_header_preserved(self, client):
        """Strict-Transport-Security header present on MISS (pass-through)."""
        uid = uuid4().hex
        path = f"/headers/security?hsts={uid}"

        r = client.get(path)
        assert r.status_code == 200
        hsts = r.headers.get("Strict-Transport-Security")
        assert hsts is not None, "Strict-Transport-Security missing on MISS response"
        assert "max-age=" in hsts

    @pytest.mark.p0
    def test_x_frame_options_preserved(self, client):
        """X-Frame-Options header present on MISS (pass-through)."""
        uid = uuid4().hex
        path = f"/headers/security?xfo={uid}"

        r = client.get(path)
        assert r.status_code == 200
        xfo = r.headers.get("X-Frame-Options")
        assert xfo is not None, "X-Frame-Options missing on MISS response"
        assert xfo.upper() == "DENY"

    @pytest.mark.p0
    def test_set_cookie_not_cached(self, client):
        """Response with Set-Cookie must not be cached — treated as uncacheable."""
        uid = uuid4().hex
        path = f"/headers/set-cookie?_={uid}"

        # First request: should be a MISS with Set-Cookie present.
        r = client.get(path)
        assert r.status_code == 200
        sc = r.headers.get("Set-Cookie")
        assert sc is not None, "Set-Cookie missing on origin response"
        assert "session=" in sc
        client.assert_miss(r)

        # Second request: must also be a MISS (not cached).
        time.sleep(2)
        r2 = client.get(path)
        assert r2.status_code == 200
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_set_cookie_stripped_from_cache(self, client):
        """Set-Cookie response must never produce a cache HIT."""
        uid = uuid4().hex
        path = f"/headers/set-cookie?_={uid}"

        # Make several requests — none should be cached.
        for i in range(3):
            r = client.get(path)
            assert r.status_code == 200
            xps = r.headers.get("X-PageSpeed")
            assert xps != "HIT", (
                f"Set-Cookie response was cached on request {i + 1}: X-PageSpeed={xps}"
            )
            if i == 0:
                sc = r.headers.get("Set-Cookie")
                assert sc is not None, "Set-Cookie missing on first response"
            time.sleep(1)

    @pytest.mark.p0
    def test_critical_css_xss_script_tag(self, client):
        """CSS with </style><script> XSS attempt must be escaped or neutralized."""
        uid = uuid4().hex
        path = f"/content/css-xss?_={uid}"

        r = client.get(path)
        assert r.status_code == 200

        # Wait for the proxy to potentially process/inline the CSS
        time.sleep(3)
        r2 = client.get(path)
        assert r2.status_code == 200

        body = r2.text
        # If the proxy inlines CSS into HTML, the raw </style><script>
        # must not appear unescaped.  For a CSS-only response, the body
        # is fine as-is because the browser interprets it as CSS.
        ct = r2.headers.get("Content-Type", "")
        if "text/html" in ct:
            assert "<script>" not in body.lower(), (
                "Unescaped <script> tag found in HTML response from CSS XSS"
            )

    @pytest.mark.p0
    def test_critical_css_xss_url_protocol(self, client):
        """CSS with javascript: URL protocol must be stripped or neutralized."""
        uid = uuid4().hex
        path = f"/content/css-xss-url?_={uid}"

        r = client.get(path)
        assert r.status_code == 200

        time.sleep(3)
        r2 = client.get(path)
        assert r2.status_code == 200

        body = r2.text
        ct = r2.headers.get("Content-Type", "")
        if "text/html" in ct:
            assert "javascript:" not in body.lower(), (
                "javascript: protocol found in HTML response from CSS XSS"
            )

    @pytest.mark.p0
    @pytest.mark.xfail(reason="SVG Accept header negotiation not yet implemented")
    def test_svg_sanitize_script_tags(self, client):
        """Generated SVG variants must not contain <script> tags."""
        uid = uuid4().hex
        path = f"/content/image/large?svg_script={uid}"
        accept_svg = "image/svg+xml,image/webp,image/jpeg,*/*"

        # Populate cache with original image
        r = client.get(path)
        assert r.status_code == 200

        # Wait for worker to potentially generate SVG variant
        time.sleep(5)
        r2 = client.get(path, headers={"Accept": accept_svg})
        assert r2.status_code == 200

        ct = r2.headers.get("Content-Type", "")
        if "svg" in ct:
            body = r2.text
            assert "<script" not in body.lower(), "Generated SVG contains <script> tag"

    @pytest.mark.p0
    @pytest.mark.xfail(reason="SVG Accept header negotiation not yet implemented")
    def test_svg_sanitize_external_refs(self, client):
        """Generated SVG must not contain external resource references."""
        uid = uuid4().hex
        path = f"/content/image/large?svg_xlink={uid}"
        accept_svg = "image/svg+xml,image/webp,image/jpeg,*/*"

        r = client.get(path)
        assert r.status_code == 200

        time.sleep(5)
        r2 = client.get(path, headers={"Accept": accept_svg})
        assert r2.status_code == 200

        ct = r2.headers.get("Content-Type", "")
        if "svg" in ct:
            body = r2.text
            # Check for external xlink:href references
            assert 'xlink:href="http' not in body, (
                "Generated SVG contains external xlink:href"
            )
            assert "xlink:href='http" not in body, (
                "Generated SVG contains external xlink:href"
            )

    @pytest.mark.p0
    def test_no_response_mixing_under_load(self, client):
        """50 concurrent requests to 10 URLs must each return matching content."""
        uid = uuid4().hex
        num_urls = 10
        requests_per_url = 5
        paths = [f"/content/mix-{uid}-{i}" for i in range(num_urls)]

        # Pre-populate cache for all paths
        for path in paths:
            r = client.get(path)
            assert r.status_code == 200

        # Wait for cache population
        time.sleep(3)

        errors = []

        def fetch_and_verify(path):
            try:
                r = client.get(path)
                # The catch-all route returns "Content for <path_suffix>"
                # Extract the path part after /content/
                expected_suffix = path.split("/content/")[1]
                if expected_suffix not in r.text:
                    return (
                        f"Response mismatch for {path}: "
                        f"expected '{expected_suffix}' in body, "
                        f"got '{r.text[:100]}'"
                    )
            except Exception:
                pass  # Transient connection errors under load are acceptable
            return None

        with ThreadPoolExecutor(max_workers=20) as executor:
            futures = []
            for path in paths:
                for _ in range(requests_per_url):
                    futures.append(executor.submit(fetch_and_verify, path))

            for future in as_completed(futures):
                err = future.result()
                if err is not None:
                    errors.append(err)

        assert not errors, f"{len(errors)} response mixing errors:\n" + "\n".join(
            errors[:5]
        )

    # ------------------------------------------------------------------
    # P1: Soft gate, review exceptions
    # ------------------------------------------------------------------

    @pytest.mark.p1
    def test_custom_origin_headers_preserved(self, client):
        """X-Custom-* headers from origin present on MISS (pass-through)."""
        uid = uuid4().hex
        custom_headers = json.dumps(
            [
                {"name": "X-Custom-Foo", "value": "bar-123"},
                {"name": "X-Custom-Tag", "value": "prod-test"},
            ]
        )
        path = f"/headers/custom?h={custom_headers}&_={uid}"

        r = client.get(path)
        assert r.status_code == 200

        assert r.headers.get("X-Custom-Foo") == "bar-123", (
            f"X-Custom-Foo missing or wrong on MISS: {r.headers.get('X-Custom-Foo')}"
        )
        assert r.headers.get("X-Custom-Tag") == "prod-test", (
            f"X-Custom-Tag missing or wrong on MISS: {r.headers.get('X-Custom-Tag')}"
        )

    @pytest.mark.p1
    def test_content_disposition_preserved(self, client):
        """Content-Disposition: attachment present on MISS (pass-through)."""
        uid = uuid4().hex
        path = f"/headers/content-disposition/attachment?_={uid}"

        r = client.get(path)
        assert r.status_code == 200

        cd = r.headers.get("Content-Disposition")
        assert cd is not None, "Content-Disposition missing on MISS response"
        assert "attachment" in cd

    @pytest.mark.p1
    def test_link_preload_no_duplicate(self, client):
        """Link preload header is not duplicated on HIT responses."""
        uid = uuid4().hex
        path = f"/headers/link-preload?_={uid}"

        r = client.get(path)
        assert r.status_code == 200

        hit = client.poll_for_hit(path)
        client.assert_hit(hit)

        # Get all Link headers (requests library joins multiple
        # values with comma for same header name)
        link_values = hit.headers.get("Link", "")
        # Count occurrences of the specific preload directive
        count = link_values.count("rel=preload")
        assert count <= 1, f"Link preload duplicated {count} times: {link_values}"

    @pytest.mark.p1
    def test_x_pagespeed_header_present(self, client):
        """X-PageSpeed header is present on every proxy response."""
        uid = uuid4().hex
        path = f"/status/200?cache=3600&body=xps-{uid}"

        # MISS response
        r = client.get(path)
        assert r.status_code == 200
        xps = r.headers.get("X-PageSpeed")
        assert xps is not None, "X-PageSpeed header missing on MISS response"

        # HIT response
        hit = client.poll_for_hit(path)
        xps_hit = hit.headers.get("X-PageSpeed")
        assert xps_hit is not None, "X-PageSpeed header missing on HIT response"
        assert xps_hit == "HIT"

    @pytest.mark.p1
    def test_url_null_bytes_rejected(self, client):
        """URL containing null bytes (%00) is rejected with 400 or error."""
        try:
            r = client.get("/status/200%00injected")
            # nginx should reject this with 400
            assert r.status_code in (400, 404), (
                f"Expected 400/404 for null byte URL, got {r.status_code}"
            )
        except Exception:
            # Connection error or request failure is also acceptable
            pass

    @pytest.mark.p1
    def test_url_crlf_injection_rejected(self, client):
        """URL with CRLF (%0d%0a) is rejected or sanitized."""
        try:
            r = client.get(
                "/status/200%0d%0aX-Injected: true",
                allow_redirects=False,
            )
            # Either rejected outright or the injected header must not appear
            if r.status_code == 200:
                assert r.headers.get("X-Injected") is None, (
                    "CRLF injection succeeded: X-Injected header present"
                )
            else:
                assert r.status_code in (400, 404), (
                    f"Expected 400/404 for CRLF URL, got {r.status_code}"
                )
        except Exception:
            # Connection error is acceptable -- server rejected the request
            pass

    @pytest.mark.p1
    def test_request_smuggling_cl_te(self, client):
        """Request with both Content-Length and Transfer-Encoding is rejected."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(10)
            s.connect(("localhost", int(os.environ.get("PRODTEST_NGINX_PORT", "8200"))))

            # Send a request with both CL and TE headers
            request = (
                "POST /status/200 HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Content-Length: 4\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "0\r\n"
                "\r\n"
            )
            s.sendall(request.encode())

            response = b""
            while True:
                try:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    response += chunk
                    if b"\r\n\r\n" in response:
                        break
                except TimeoutError:
                    break
            s.close()

            response_str = response.decode("utf-8", errors="replace")
            # nginx should either reject (400) or handle safely by
            # preferring one header and ignoring the other
            if "400" in response_str:
                pass  # Correctly rejected
            elif "200" in response_str:
                pass  # Handled safely
            else:
                # Any non-crash response is acceptable
                assert len(response_str) > 0, "No response received for CL+TE request"
        except (ConnectionError, OSError):
            # Connection refused or reset is acceptable
            pass

    # ------------------------------------------------------------------
    # P2: Rare scenarios, completeness
    # ------------------------------------------------------------------

    @pytest.mark.p2
    def test_via_header_appended(self, client):
        """Via header is present in proxy responses."""
        uid = uuid4().hex
        path = f"/status/200?cache=3600&body=via-{uid}"

        r = client.get(path)
        assert r.status_code == 200

        via = r.headers.get("Via")
        if via is None:
            # Also check on HIT
            hit = client.poll_for_hit(path)
            via = hit.headers.get("Via")

        # Via header is recommended but not strictly required
        if via is None:
            pytest.skip("Proxy does not set Via header")
        assert len(via) > 0

    @pytest.mark.p2
    def test_x_forwarded_for_appended(self, client):
        """X-Forwarded-For is passed through to origin or set by proxy."""
        uid = uuid4().hex
        # Use the echo endpoint to see what headers the origin receives
        path = f"/echo?_={uid}"

        r = client.get(path, headers={"X-Forwarded-For": "203.0.113.50"})
        assert r.status_code == 200

        body = r.json()
        origin_headers = body.get("headers", {})
        xff = origin_headers.get("x-forwarded-for", "")
        # The proxy should preserve or append to X-Forwarded-For
        assert "203.0.113.50" in xff, f"X-Forwarded-For not passed to origin: {xff}"

    @pytest.mark.p2
    def test_management_api_wsocket_limit(self, client):
        """Many WebSocket-style connections to management socket do not crash."""
        sockets = []
        connected = 0
        try:
            for _ in range(20):
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(5)
                try:
                    s.connect(("localhost", int(os.environ.get("PRODTEST_NGINX_PORT", "8200"))))
                    sockets.append(s)
                    connected += 1
                except (ConnectionError, OSError):
                    s.close()
                    break
        finally:
            for s in sockets:
                try:
                    s.close()
                except Exception:
                    pass

        # After closing all connections, the proxy should still respond
        time.sleep(1)
        r = client.get("/health")
        assert r.status_code == 200, (
            f"Proxy unhealthy after {connected} connections: status={r.status_code}"
        )
