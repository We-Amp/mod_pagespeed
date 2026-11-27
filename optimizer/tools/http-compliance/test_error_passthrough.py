# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Error response passthrough tests.

Verifies that 4xx/5xx error responses from the origin are passed
through the proxy without modification.

RFC 9110 Section 15: Status Codes.
"""


class TestClientErrors:
    """4xx error passthrough."""

    def test_404_passed_through(self, client):
        """404 from origin passed through proxy."""
        r = client.get("/error/404")
        assert r.status_code == 404

    def test_403_passed_through(self, client):
        """403 from origin passed through proxy."""
        r = client.get("/error/403")
        assert r.status_code == 403

    def test_404_body_preserved(self, client):
        """404 response body is not empty."""
        r = client.get("/error/404")
        assert r.status_code == 404
        assert len(r.content) > 0, "Error body should not be empty"

    def test_nonexistent_file_404(self, client):
        """Request for nonexistent file returns 404."""
        r = client.get("/does-not-exist.html")
        assert r.status_code == 404


class TestServerErrors:
    """5xx error passthrough."""

    def test_500_passed_through(self, client):
        """500 from origin passed through proxy."""
        r = client.get("/error/500")
        assert r.status_code == 500

    def test_502_passed_through(self, client):
        """502 from origin passed through proxy."""
        r = client.get("/error/502")
        assert r.status_code == 502

    def test_503_passed_through(self, client):
        """503 from origin passed through proxy."""
        r = client.get("/error/503")
        assert r.status_code == 503


class TestErrorHeaders:
    """Error response header correctness."""

    def test_error_no_x_pagespeed(self, client):
        """Error responses should not have X-PageSpeed header.

        The module only adds X-PageSpeed on 2xx responses.
        """
        r = client.get("/error/404")
        # X-PageSpeed should not be present on error responses
        xs = r.headers.get("X-PageSpeed")
        assert xs is None, f"Error should not have X-PageSpeed: {xs}"

    def test_error_has_date(self, client):
        """Error response includes Date header."""
        r = client.get("/error/500")
        client.assert_has_date(r)


class TestRedirects:
    """Redirect response passthrough."""

    def test_301_passed_through(self, client):
        """301 redirect passed through with Location header."""
        r = client.get("/redirect/301", allow_redirects=False)
        assert r.status_code == 301
        assert "Location" in r.headers

    def test_302_passed_through(self, client):
        """302 redirect passed through with Location header."""
        r = client.get("/redirect/302", allow_redirects=False)
        assert r.status_code == 302
        assert "Location" in r.headers

    def test_redirect_location_preserved(self, client):
        """Redirect Location header value preserved."""
        r = client.get("/redirect/301", allow_redirects=False)
        assert r.status_code == 301
        loc = r.headers.get("Location", "")
        # Location should point to /small.html
        assert "small.html" in loc, f"Unexpected Location: {loc}"
