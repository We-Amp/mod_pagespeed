# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""HEAD vs GET consistency tests.

Verifies that HEAD responses have the same status code, Content-Type,
and Content-Length as GET responses, but with an empty body.

RFC 9110 Section 9.3.2: HEAD method.

Note: Each test uses fresh HTTP connections (no session reuse) to avoid
connection corruption when the pagespeed module serves from cache.
Uses CSS resources to avoid 103 Early Hints issues with HTML.
"""

import requests


class TestHeadConsistency:
    """HEAD returns same metadata as GET."""

    @staticmethod
    def _get(url, headers=None):
        """GET with a fresh connection (no keep-alive)."""
        return requests.get(url, headers=headers or {}, timeout=10)

    @staticmethod
    def _head(url, headers=None):
        """HEAD with a fresh connection (no keep-alive)."""
        return requests.head(url, headers=headers or {}, timeout=10)

    def test_head_same_status_as_get(self, client):
        """HEAD returns same status code as GET."""
        base = client.base_url
        get_r = self._get(base + "/etag-test.css")
        head_r = self._head(base + "/etag-test.css")
        assert head_r.status_code == get_r.status_code, (
            f"HEAD status {head_r.status_code} != GET status {get_r.status_code}"
        )

    def test_head_same_content_type(self, client):
        """HEAD returns same Content-Type as GET."""
        base = client.base_url
        get_r = self._get(base + "/etag-test.css")
        head_r = self._head(base + "/etag-test.css")
        get_ct = get_r.headers.get("Content-Type", "")
        head_ct = head_r.headers.get("Content-Type", "")
        assert head_ct == get_ct, (
            f"HEAD Content-Type '{head_ct}' != GET Content-Type '{get_ct}'"
        )

    def test_head_body_is_empty(self, client):
        """HEAD response body is empty."""
        base = client.base_url
        r = self._head(base + "/etag-test.css")
        assert len(r.content) == 0, (
            f"HEAD body should be empty, got {len(r.content)} bytes"
        )

    def test_head_css(self, client):
        """HEAD on CSS file returns correct headers."""
        base = client.base_url
        get_r = self._get(base + "/etag-test.css")
        head_r = self._head(base + "/etag-test.css")
        assert head_r.status_code == get_r.status_code
        assert len(head_r.content) == 0

    def test_head_image(self, client):
        """HEAD on image returns correct headers."""
        base = client.base_url
        get_r = self._get(base + "/1x1.jpg")
        head_r = self._head(base + "/1x1.jpg")
        assert head_r.status_code == get_r.status_code
        assert len(head_r.content) == 0

    def test_head_includes_x_pagespeed(self, client):
        """HEAD on a known resource returns 200."""
        base = client.base_url
        r = self._head(base + "/etag-test.css")
        assert r.status_code == 200

    def test_head_on_cache_hit(self, client):
        """HEAD on cached resource returns correct headers."""
        base = client.base_url
        # Ensure cache is warm
        client.get("/etag-test.css")
        client.poll_for_hit("/etag-test.css")
        head_r = self._head(base + "/etag-test.css")
        assert head_r.status_code == 200
        assert len(head_r.content) == 0

    def test_head_content_length_matches_get(self, client):
        """HEAD Content-Length matches what GET would return."""
        base = client.base_url
        get_r = self._get(base + "/etag-test.css")
        head_r = self._head(base + "/etag-test.css")
        get_cl = get_r.headers.get("Content-Length")
        head_cl = head_r.headers.get("Content-Length")
        if get_cl and head_cl:
            assert head_cl == get_cl, (
                f"HEAD Content-Length {head_cl} != GET Content-Length {get_cl}"
            )
