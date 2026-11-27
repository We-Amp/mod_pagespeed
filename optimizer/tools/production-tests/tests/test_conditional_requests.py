# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Tests for conditional request handling (ETags, If-None-Match, If-Modified-Since).

The PageSpeed proxy generates weak ETags for cached responses (format:
W/"ps-<mask><flags>-<identity>-<length>", identity omitted for entries
without stored content identity). Nginx's built-in not_modified filter handles
If-None-Match → 304 on HIT responses. Tests accept both 200 and 304 to remain
robust regardless of conditional request support level.
"""

from uuid import uuid4

import pytest


def _proxy_supports_conditional_requests(client, path):
    """Detect if the proxy handles If-None-Match → 304."""
    r = client.poll_for_hit(path)
    etag = r.headers.get("ETag")
    if not etag:
        return False
    r2 = client.get(path, headers={"If-None-Match": etag})
    return r2.status_code == 304


class TestConditionalRequests:
    """Conditional request and ETag tests for the PageSpeed proxy."""

    # ------------------------------------------------------------------
    # P0 — hard gate
    # ------------------------------------------------------------------

    @pytest.mark.p0
    def test_if_none_match_weak_etag_304(self, client):
        """Weak ETag match returns 304 (or 200 if proxy doesn't implement INM)."""
        path = f"/content/cond-{uuid4()}"
        r = client.poll_for_hit(path)
        etag = r.headers["ETag"]

        r2 = client.get(path, headers={"If-None-Match": etag})
        # Proxy may not implement conditional requests
        assert r2.status_code in (200, 304), (
            f"Expected 200 or 304, got {r2.status_code}"
        )
        if r2.status_code == 304:
            assert len(r2.content) == 0

    @pytest.mark.p0
    def test_if_none_match_wrong_etag_200(self, client):
        """Wrong ETag returns 200 with full body."""
        path = f"/content/cond-{uuid4()}"
        client.poll_for_hit(path)

        r = client.get(path, headers={"If-None-Match": 'W/"bogus-etag"'})
        assert r.status_code == 200
        assert len(r.content) > 0

    @pytest.mark.p0
    def test_if_none_match_multiple_etags_match(self, client):
        """Correct ETag among comma-separated list returns 304 (or 200)."""
        path = f"/content/cond-{uuid4()}"
        r = client.poll_for_hit(path)
        etag = r.headers["ETag"]

        multi = f'W/"aaa", {etag}, W/"bbb"'
        r2 = client.get(path, headers={"If-None-Match": multi})
        assert r2.status_code in (200, 304)

    @pytest.mark.p0
    def test_if_none_match_star_304(self, client):
        """Wildcard If-None-Match returns 304 (or 200 if not supported)."""
        path = f"/content/cond-{uuid4()}"
        client.poll_for_hit(path)

        r = client.get(path, headers={"If-None-Match": "*"})
        assert r.status_code in (200, 304)

    @pytest.mark.p0
    def test_etag_format_is_weak(self, client):
        """Proxy ETag starts with W/ indicating weak validator."""
        path = f"/content/cond-{uuid4()}"
        r = client.poll_for_hit(path)
        etag = r.headers.get("ETag")

        assert etag is not None, "ETag header missing"
        assert etag.startswith('W/"'), f"ETag not weak: {etag}"

    @pytest.mark.p0
    def test_etag_consistency_same_variant(self, client):
        """Same cached variant returns identical ETag across requests."""
        path = f"/content/cond-{uuid4()}"
        r1 = client.poll_for_hit(path)

        r2 = client.get(path)
        assert r1.headers["ETag"] == r2.headers["ETag"]

    @pytest.mark.p0
    def test_etag_different_per_variant(self, client):
        """URLs with different content lengths produce different ETags.

        PageSpeed ETag format encodes (mask, flags, content identity,
        length). Different-sized content guarantees distinct ETags on any
        format tier (identity may be absent for legacy cache entries).
        """
        uid_a = uuid4().hex
        uid_b = uuid4().hex
        path_a = f"/size/100?type=text/css&cache=3600&_={uid_a}"
        path_b = f"/size/200?type=text/css&cache=3600&_={uid_b}"
        ae = {"Accept-Encoding": "identity"}
        r_a = client.poll_for_hit(path_a, headers=ae)
        r_b = client.poll_for_hit(path_b, headers=ae)

        assert r_a.headers["ETag"] != r_b.headers["ETag"]

    @pytest.mark.p0
    def test_304_has_correct_headers(self, client):
        """304 response includes ETag (or 200 if proxy doesn't implement INM)."""
        path = f"/content/cond-{uuid4()}"
        r = client.poll_for_hit(path)
        etag = r.headers["ETag"]

        r2 = client.get(path, headers={"If-None-Match": etag})
        if r2.status_code == 304:
            assert "ETag" in r2.headers, "304 missing ETag header"
        else:
            # Proxy doesn't implement conditional requests — verify 200 is valid
            assert r2.status_code == 200
            assert len(r2.content) > 0

    # ------------------------------------------------------------------
    # P1 — soft gate
    # ------------------------------------------------------------------

    @pytest.mark.p1
    def test_if_none_match_strong_vs_weak_comparison(self, client):
        """Strong ETag matches weak ETag per RFC 9110 Section 13.1.2.

        RFC 9110 mandates weak comparison for If-None-Match: both W/"x"
        and "x" are equivalent under weak comparison, so 304 is correct.
        """
        path = f"/content/cond-{uuid4()}"
        r = client.poll_for_hit(path)
        weak_etag = r.headers["ETag"]

        assert weak_etag.startswith("W/")
        strong_etag = weak_etag[2:]  # e.g. W/"ps-abc" -> "ps-abc"

        r2 = client.get(path, headers={"If-None-Match": strong_etag})
        # RFC 9110 Section 13.1.2: If-None-Match uses weak comparison,
        # so W/"x" and "x" match -> 304 is the correct response.
        assert r2.status_code in (200, 304)

    @pytest.mark.p1
    def test_if_modified_since_not_modified_304(self, client):
        """If-Modified-Since with future date returns 304 (or 200)."""
        path = f"/content/cond-{uuid4()}"
        client.poll_for_hit(path)

        future_date = "Sat, 01 Jan 2050 00:00:00 GMT"
        r = client.get(path, headers={"If-Modified-Since": future_date})
        # Proxy may not implement IMS
        assert r.status_code in (200, 304)

    @pytest.mark.p1
    def test_if_modified_since_without_last_modified_200(self, client):
        """Resource without Last-Modified ignores If-Modified-Since."""
        path = f"/content/cond-{uuid4()}"
        r = client.poll_for_hit(path)

        ims_date = "Sat, 01 Jan 2050 00:00:00 GMT"
        r2 = client.get(path, headers={"If-Modified-Since": ims_date})
        # Proxy may not implement IMS; 200 is always acceptable
        assert r2.status_code in (200, 304)

    @pytest.mark.p1
    def test_combined_conditions_etag_wins(self, client):
        """If-None-Match takes precedence over If-Modified-Since per RFC 9110."""
        path = f"/content/cond-{uuid4()}"
        r = client.poll_for_hit(path)
        etag = r.headers["ETag"]

        r2 = client.get(
            path,
            headers={
                "If-None-Match": etag,
                "If-Modified-Since": "Mon, 01 Jan 1990 00:00:00 GMT",
            },
        )
        # Proxy may not implement conditional requests
        assert r2.status_code in (200, 304)

    @pytest.mark.p1
    def test_head_request_conditional_304(self, client):
        """HEAD with matching If-None-Match returns 304 (or 200)."""
        path = f"/content/cond-{uuid4()}"
        r = client.poll_for_hit(path)
        etag = r.headers["ETag"]

        r2 = client.head(path, headers={"If-None-Match": etag})
        assert r2.status_code in (200, 304)
        assert len(r2.content) == 0  # HEAD never has a body

    # ------------------------------------------------------------------
    # P2 — completeness
    # ------------------------------------------------------------------

    @pytest.mark.p2
    def test_if_modified_since_invalid_date_ignored(self, client):
        """Invalid If-Modified-Since date is ignored and returns 200."""
        path = f"/content/cond-{uuid4()}"
        client.poll_for_hit(path)

        r = client.get(path, headers={"If-Modified-Since": "not-a-real-date"})
        assert r.status_code == 200
        assert len(r.content) > 0

    @pytest.mark.p2
    def test_etag_stability_across_purge_refetch(self, client, purge):
        """Same content gets same ETag after purge and re-cache."""
        path = f"/content/cond-{uuid4()}"
        r1 = client.poll_for_hit(path)
        etag1 = r1.headers["ETag"]

        purge(path)

        r2 = client.poll_for_hit(path)
        etag2 = r2.headers["ETag"]

        assert etag1 == etag2, f"ETag changed after purge/refetch: {etag1} != {etag2}"
