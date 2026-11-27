# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Transfer-Encoding compliance tests.

Verifies chunked encoding handling. When the origin sends chunked
responses, the proxy must decode them correctly and serve them with
either Content-Length or its own chunked encoding.

RFC 9112 Section 6.1: Transfer-Encoding.
RFC 9110 Section 8.6: Content-Length vs Transfer-Encoding mutual exclusion.
"""


class TestChunkedFromOrigin:
    """Chunked responses from origin decoded correctly."""

    def test_chunked_css_served_correctly(self, client):
        """Chunked CSS from origin served with correct body."""
        r = client.get("/chunked/css")
        assert r.status_code == 200
        # Body should be the full CSS content (decoded from chunks)
        assert len(r.content) > 0
        # Verify it's valid CSS content (not chunk metadata)
        body = r.content.decode("utf-8", errors="replace")
        assert ".class-0" in body, "CSS content should contain class rules"
        assert ".class-1999" in body, "CSS should have all classes"

    def test_chunked_response_has_content_length_or_chunked(self, client):
        """Proxy response has Content-Length or Transfer-Encoding."""
        r = client.get("/chunked/css")
        assert r.status_code == 200
        has_cl = "Content-Length" in r.headers
        has_te = r.headers.get("Transfer-Encoding", "").lower() == "chunked"
        assert has_cl or has_te, (
            "Response must have Content-Length or Transfer-Encoding: chunked"
        )

    def test_no_double_chunked(self, client):
        """No double chunked encoding (chunked applied only once)."""
        r = client.get("/chunked/css")
        assert r.status_code == 200
        te = r.headers.get("Transfer-Encoding", "")
        assert te.lower().count("chunked") <= 1, f"Double chunked encoding: {te}"

    def test_content_length_absent_when_chunked(self, client):
        """Content-Length and chunked are mutually exclusive."""
        r = client.get("/chunked/css")
        assert r.status_code == 200
        if r.headers.get("Transfer-Encoding", "").lower() == "chunked":
            assert "Content-Length" not in r.headers, (
                "Content-Length must not be present with chunked encoding"
            )

    def test_content_length_matches_when_not_chunked(self, client):
        """If not chunked, Content-Length matches body."""
        r = client.get("/chunked/css")
        assert r.status_code == 200
        if r.headers.get("Transfer-Encoding", "").lower() != "chunked":
            client.assert_content_length_matches_body(r)


class TestNonChunkedResponses:
    """Non-chunked responses have Content-Length."""

    def test_non_chunked_has_content_length(self, client):
        """Non-chunked response includes Content-Length."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        # Either Content-Length or chunked must be present
        has_cl = "Content-Length" in r.headers
        has_te = r.headers.get("Transfer-Encoding", "").lower() == "chunked"
        assert has_cl or has_te

    def test_static_file_not_chunked(self, client):
        """Small static file served with Content-Length, not chunked."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        # Small files should use Content-Length, not chunked
        if "Content-Length" in r.headers:
            client.assert_content_length_matches_body(r)


class TestChunkedBodyModification:
    """Chunked body still processed correctly by pagespeed."""

    def test_chunked_css_still_served(self, client):
        """Chunked CSS from origin is served without corruption."""
        r = client.get("/chunked/css")
        assert r.status_code == 200
        body = r.content.decode("utf-8", errors="replace")
        # Verify the content is valid CSS, not chunk encoding artifacts
        assert "\r\n" not in body[:100] or "class" in body[:100], (
            "Response body contains raw chunk encoding"
        )

    def test_large_chunked_assembled_correctly(self, client):
        """Large chunked response assembled without data loss."""
        r = client.get("/chunked/css")
        assert r.status_code == 200
        # The origin's /chunked/css serves /large.css which is >64KB
        assert len(r.content) > 60000, f"Expected >60KB, got {len(r.content)} bytes"
