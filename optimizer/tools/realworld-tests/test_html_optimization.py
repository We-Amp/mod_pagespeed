# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""HTML optimization tests against real-world sites.

Verifies HTML validity, structure preservation, Content-Type,
and worker notification processing.
"""

import pytest
from conftest import _poll_stat


def test_cached_html_has_valid_markup(nginx):
    """Cached HTML retains valid markup (doctype and closing tags)."""
    resp = nginx.poll_for_hit("/example/", timeout=20)
    ps = resp.headers.get("X-PageSpeed")
    if ps != "HIT":
        pytest.skip("Cache not warm — could not get HIT for /example/")
    body = resp.text.lower()
    assert "<!doctype html" in body or "<html" in body, (
        "Cached HTML missing doctype or html tag"
    )
    assert "</html>" in body, "Cached HTML missing closing </html>"


def test_html_content_type_preserved_on_hit(nginx):
    """HTML Content-Type is preserved on HIT."""
    resp = nginx.poll_for_hit("/example/", timeout=20)
    ps = resp.headers.get("X-PageSpeed")
    if ps != "HIT":
        pytest.skip("Cache not warm — could not get HIT for /example/")
    ct = resp.headers.get("Content-Type", "")
    assert "text/html" in ct, f"Expected text/html on HIT, got: {ct}"


def test_worker_receives_notifications(nginx, worker_api):
    """Worker stats show notifications received after proxied requests."""
    # Make several requests to trigger notifications
    nginx.get("/example/")
    nginx.get("/httpbin/get")

    count = _poll_stat(
        worker_api, ["notifications", "received"], min_value=1, timeout=30
    )
    assert count >= 1, "Worker received no notifications"
