#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""nginx cache method tests.

The cache is consulted and filled for GET and HEAD requests only. A request
with any other method is neither answered from the cache nor leaves an entry
in it, on the proxy-mapped path and on the in-place path alike: the request
reaches the origin with the method it was sent with, and the counters of the
cache and of the in-place recorder do not move.
"""

import os

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    assert_not_contains,
)
from pagespeed_test_framework.stats import settled_stats

# The assertions read server-wide counters: this file cannot share the
# server with parallel workers.
if os.environ.get("PYTEST_XDIST_WORKER"):
    pytest.skip(
        "reads server-wide counters; run without parallel workers",
        allow_module_level=True,
    )

CACHE_COUNTERS = (
    "cache_hits",
    "cache_misses",
    "cache_inserts",
    "http_cache_hits",
    "http_cache_misses",
    "ipro_not_in_cache",
    "ipro_recorder_resources",
    "ipro_recorder_inserted_into_cache",
)


def _snapshot(client: PageSpeedClient, server_config) -> dict:
    stats = client.get_statistics(
        stats_path=server_config.stats_path, disable_pagespeed=False
    )
    return {name: stats.get(name, 0) for name in CACHE_COUNTERS}


def _settled(client: PageSpeedClient, server_config) -> dict:
    """A snapshot taken once work left over from earlier tests has finished."""
    return settled_stats(
        lambda: _snapshot(client, server_config), list(CACHE_COUNTERS)
    )


@pytest.mark.nginx_only
class TestNginxCacheMethods:
    """Only GET and HEAD reach the cache."""

    @pytest.mark.requires_module
    @pytest.mark.requires_stats
    def test_proxy_mapped_url_serves_only_get_and_head_from_cache(
        self, client: PageSpeedClient, test_root: str, server_config
    ):
        """POSTs to a proxy-mapped URL reach the origin and touch no cache."""
        url = f"{test_root}/method_proxy/echo.css"

        before = _settled(client, server_config)

        for _ in range(3):
            response = client.post(url, data="x=1")
            assert_http_status(response, 200)
            # The origin saw a POST, not the GET the request defaulted to.
            assert_contains(response, "post-body-marker")
            assert_not_contains(response, "get-body-marker")

        after = _snapshot(client, server_config)
        assert after == before, (
            f"a POST moved a cache counter: before={before} after={after}"
        )

        # A GET afterwards is answered with the GET body only.
        response = client.get(url)
        assert_http_status(response, 200)
        assert_contains(response, "get-body-marker")
        assert_not_contains(response, "post-body-marker")

    @pytest.mark.requires_module
    @pytest.mark.requires_stats
    def test_in_place_url_serves_only_get_and_head_from_cache(
        self, client: PageSpeedClient, test_root: str, server_config
    ):
        """POSTs to an in-place URL reach the origin and touch no cache."""
        url = f"{test_root}/inplace_methods/counters.css"

        before = _settled(client, server_config)

        for _ in range(3):
            response = client.post(url, data="x=1")
            assert_http_status(response, 200)
            assert_contains(response, "post-body-marker")

        after = _snapshot(client, server_config)
        assert after == before, (
            f"a POST moved a cache counter: before={before} after={after}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
