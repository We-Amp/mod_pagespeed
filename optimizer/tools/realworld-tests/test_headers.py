# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""HTTP header preservation and augmentation tests.

Verifies Vary, Content-Type, ETag, and Cache-Control behavior
on responses proxied through PageSpeed.
"""

import re

import pytest


def test_html_vary_includes_accept_encoding(nginx):
    """HTML Vary header includes Accept-Encoding on HIT."""
    resp = nginx.poll_for_hit("/example/", timeout=20)
    ps = resp.headers.get("X-PageSpeed")
    if ps != "HIT":
        pytest.skip("Cache not warm — could not get HIT for /example/")
    vary = resp.headers.get("Vary", "")
    assert "accept-encoding" in vary.lower(), (
        f"Vary header missing Accept-Encoding: {vary}"
    )


def test_content_type_preserved_html(nginx):
    """HTML Content-Type is preserved through the proxy."""
    resp = nginx.get("/example/")
    ct = resp.headers.get("Content-Type", "")
    assert "text/html" in ct, f"Expected text/html, got: {ct}"


def test_content_type_preserved_json(nginx):
    """JSON Content-Type is preserved through the proxy."""
    import time

    unique = f"/httpbin/get?t={int(time.time() * 1000)}"
    resp = nginx.get(unique)
    assert resp.status_code == 200
    ct = resp.headers.get("Content-Type", "")
    assert "application/json" in ct, f"Expected application/json, got: {ct}"


def test_hit_has_etag(nginx):
    """HIT responses have a PageSpeed ETag (W/"ps-..." format)."""
    resp = nginx.poll_for_hit("/example/", timeout=20)
    ps = resp.headers.get("X-PageSpeed")
    if ps != "HIT":
        pytest.skip("Cache not warm — could not get HIT for /example/")
    etag = resp.headers.get("ETag")
    assert etag is not None, "ETag header missing on HIT"
    # PageSpeed ETag format: W/"ps-<mask_hex><flags_hex>-<identity_hex>-
    # <length_hex>" — the identity section is absent for cache entries
    # without stored content identity (legacy two-section form).
    assert re.match(r'W/"ps-[0-9a-f]+(?:-[0-9a-f]+){1,2}"', etag), (
        f"ETag does not match PageSpeed format: {etag}"
    )


def test_hit_has_cache_control(nginx):
    """HIT responses have a Cache-Control header."""
    resp = nginx.poll_for_hit("/example/", timeout=20)
    ps = resp.headers.get("X-PageSpeed")
    if ps != "HIT":
        pytest.skip("Cache not warm — could not get HIT for /example/")
    cc = resp.headers.get("Cache-Control")
    assert cc is not None, "Cache-Control header missing on HIT"
