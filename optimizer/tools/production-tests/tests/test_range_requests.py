# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Range request handling tests for the caching reverse proxy.

Range requests on cache HITs are served by nginx's built-in range filter.
All requests pin Accept-Encoding: identity to avoid interference from
gzip/brotli variants (byte ranges of compressed content are not valid
compressed data, causing decompression failures in the requests library).

Content is sourced from /size/1024 (1024 bytes of deterministic data) to
ensure content is large enough for all tested range offsets.
"""

import re
from uuid import uuid4

import pytest

# All range-test requests use identity encoding to avoid getting
# byte slices of gzip/brotli content that the requests library
# cannot decompress.
_AE_IDENTITY = {"Accept-Encoding": "identity"}
_CONTENT_SIZE = 1024


class TestRangeRequests:
    """Verify correct handling of HTTP Range requests on cached content."""

    def _populate(self, client):
        """Populate cache with deterministic 1024-byte CSS content."""
        uid = uuid4().hex
        path = f"/size/{_CONTENT_SIZE}?type=text/css&cache=3600&_={uid}"
        hit = client.poll_for_hit(path, headers=_AE_IDENTITY)
        return path, hit

    # ------------------------------------------------------------------
    # P0 tests
    # ------------------------------------------------------------------

    @pytest.mark.p0
    def test_single_byte_range_206(self, client):
        """Basic Range request returns 206 Partial Content."""
        path, _ = self._populate(client)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=0-99"})
        assert r.status_code == 206
        assert len(r.content) == 100

    @pytest.mark.p0
    def test_unsatisfiable_range_416(self, client):
        """Range beyond EOF returns 416 Range Not Satisfiable."""
        path, _ = self._populate(client)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=999999-9999999"})
        assert r.status_code == 416

    @pytest.mark.p0
    def test_range_with_if_none_match_304(self, client):
        """If-None-Match takes precedence over Range, returning 304."""
        path, hit = self._populate(client)
        etag = hit.headers.get("ETag")
        assert etag is not None, "Cached response did not include ETag"
        r = client.get(
            path,
            headers={
                **_AE_IDENTITY,
                "Range": "bytes=0-99",
                "If-None-Match": etag,
            },
        )
        assert r.status_code == 304

    @pytest.mark.p0
    def test_content_range_header_correct(self, client):
        """Content-Range header has correct bytes X-Y/Z format on 206."""
        path, hit = self._populate(client)
        total = len(hit.content)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=0-99"})
        assert r.status_code == 206
        cr = r.headers.get("Content-Range")
        assert cr is not None, "Content-Range header missing from 206 response"
        m = re.match(r"^bytes (\d+)-(\d+)/(\d+)$", cr)
        assert m is not None, f"Content-Range format invalid: {cr}"
        start, end, size = int(m.group(1)), int(m.group(2)), int(m.group(3))
        assert start == 0
        assert end == 99
        assert size == total

    # ------------------------------------------------------------------
    # P1 tests
    # ------------------------------------------------------------------

    @pytest.mark.p1
    def test_suffix_range(self, client):
        """Suffix range bytes=-512 returns the last 512 bytes."""
        path, hit = self._populate(client)
        total = len(hit.content)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=-512"})
        assert r.status_code == 206
        expected_len = min(512, total)
        assert len(r.content) == expected_len

    @pytest.mark.p1
    def test_open_ended_range(self, client):
        """Open-ended range bytes=100- returns from byte 100 to end."""
        path, hit = self._populate(client)
        total = len(hit.content)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=100-"})
        assert r.status_code == 206
        assert len(r.content) == total - 100

    @pytest.mark.p1
    def test_middle_range(self, client):
        """Middle range bytes=100-200 returns exactly 101 bytes."""
        path, _ = self._populate(client)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=100-200"})
        assert r.status_code == 206
        assert len(r.content) == 101

    @pytest.mark.p1
    def test_range_with_weak_if_range(self, client):
        """Weak ETag in If-Range is ignored per RFC 9110, returning full body."""
        path, hit = self._populate(client)
        total = len(hit.content)
        uid = uuid4().hex
        # Construct a weak ETag; even if it matches, If-Range requires
        # strong comparison so the Range should be ignored.
        weak_etag = f'W/"fake-weak-{uid}"'
        r = client.get(
            path,
            headers={
                **_AE_IDENTITY,
                "Range": "bytes=0-99",
                "If-Range": weak_etag,
            },
        )
        # If-Range with a non-matching or weak ETag should yield full body.
        assert r.status_code == 200
        assert len(r.content) == total

    @pytest.mark.p1
    def test_range_on_miss_proxied(self, client):
        """Range request on uncached content is proxied to origin."""
        uid = uuid4().hex
        path = f"/range/{uid}"
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=0-99"})
        # Origin supports Range, so we expect either 206 (proxied range)
        # or 200 (proxy strips Range and returns full body).
        assert r.status_code in (200, 206)

    # ------------------------------------------------------------------
    # P2 tests
    # ------------------------------------------------------------------

    @pytest.mark.p2
    def test_invalid_range_syntax(self, client):
        """Invalid Range syntax like bytes=abc returns 200 or 416.

        RFC 9110 Section 14.1.1: a server MAY treat an invalid range
        as unsatisfiable (416) or ignore the Range and return 200.
        """
        path, _ = self._populate(client)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=abc"})
        assert r.status_code in (200, 416)

    @pytest.mark.p2
    def test_range_start_greater_than_end_200(self, client):
        """Range with start > end returns full body 200 or 416."""
        path, _ = self._populate(client)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=200-100"})
        assert r.status_code in (200, 416)

    @pytest.mark.p2
    def test_multipart_byte_ranges(self, client):
        """Multiple ranges return multipart/byteranges response."""
        path, _ = self._populate(client)
        r = client.get(path, headers={**_AE_IDENTITY, "Range": "bytes=0-10,20-30"})
        assert r.status_code == 206
        ct = r.headers.get("Content-Type", "")
        assert "multipart/byteranges" in ct
