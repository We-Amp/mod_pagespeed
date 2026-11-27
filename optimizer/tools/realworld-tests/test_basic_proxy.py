# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Basic proxy connectivity and PageSpeed header tests.

Verifies that each upstream is reachable through the nginx proxy,
content is recognizable, and X-PageSpeed headers are present.
"""

import time

import pytest

VALID_PS_VALUES = ("MISS", "HIT", "REVALIDATED")


@pytest.mark.parametrize(
    "path,expect_status",
    [
        ("/example/", 200),
        ("/httpbin/get", 200),
    ],
)
def test_upstream_reachable(nginx, path, expect_status):
    """Each configured upstream is reachable through the proxy."""
    resp = nginx.poll_for_response(path)
    assert resp.status_code == expect_status, (
        f"{path} returned {resp.status_code}, expected {expect_status}"
    )


def test_example_com_content(nginx):
    """example.com returns recognizable content."""
    resp = nginx.get("/example/")
    assert resp.status_code == 200
    assert "Example Domain" in resp.text


def test_x_pagespeed_header_present(nginx):
    """X-PageSpeed header is present on all proxied responses."""
    resp = nginx.get("/example/")
    assert resp.status_code == 200
    ps = resp.headers.get("X-PageSpeed")
    assert ps is not None, "X-PageSpeed header missing"
    assert ps in VALID_PS_VALUES, f"Unexpected X-PageSpeed value: {ps}"


def test_first_request_is_miss(nginx):
    """First request to an uncached path is always MISS."""
    unique = f"/httpbin/get?t={int(time.time() * 1000)}"
    resp = nginx.get(unique)
    assert resp.status_code == 200
    assert resp.headers.get("X-PageSpeed") == "MISS"
