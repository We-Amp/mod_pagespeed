#!/usr/bin/env python3
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""In-Place Resource Optimization (IPRO) tests.

Ported from: pagespeed/system/system_tests/ipro_caching.sh

These tests verify that IPRO flow uses the cache correctly.
"""

import re
import time
from typing import Dict

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
    assert_stat_delta,
    assert_stat_increased,
    assert_file_size,
)
from pagespeed_test_framework.stats import scrape_header, extract_headers


class TestIproCaching:
    """Tests for IPRO caching behavior.

    Bash original:
        start_test IPRO flow uses cache as expected.
        IPRO_HOST=http://ipro.example.com
        IPRO_ROOT=$IPRO_HOST/mod_pagespeed_test/ipro
        URL=$IPRO_ROOT/test_image_dont_reuse2.png
    """

    @pytest.mark.requires_stats
    @pytest.mark.requires_secondary
    def test_ipro_caching_flow(
        self,
        secondary_client: PageSpeedClient,
        stats_snapshot,
    ):
        """IPRO flow should use cache as expected.

        First request: resource not in cache, runs recorder flow
        Second request: resource found in cache, served optimized
        """
        ipro_root = "/mod_pagespeed_test/ipro"
        url = f"{ipro_root}/test_image_dont_reuse2.png"

        # Initial stats
        stats_0 = stats_snapshot()

        # First IPRO request
        response_1 = secondary_client.get(url)
        assert_http_status(response_1, 200)
        stats_1 = stats_snapshot()

        # Resource not in cache the first time
        assert_stat_delta(stats_0, stats_1, "cache_hits", 0)
        assert_stat_delta(stats_0, stats_1, "cache_misses", 1)
        assert_stat_delta(stats_0, stats_1, "ipro_served", 0)
        assert_stat_delta(stats_0, stats_1, "ipro_not_rewritable", 0)

        # Runs ipro recorder flow and inserts into cache
        assert_stat_delta(stats_0, stats_1, "ipro_not_in_cache", 1)
        assert_stat_delta(stats_0, stats_1, "ipro_recorder_resources", 1)
        assert_stat_delta(stats_0, stats_1, "ipro_recorder_inserted_into_cache", 1)

        # Image doesn't get rewritten the first time
        assert_stat_delta(stats_0, stats_1, "image_rewrites", 0)

        # Second IPRO request - wait for optimization
        # Original file is 15131 bytes, optimized is ~11395
        response_2 = secondary_client.fetch_until(
            url,
            condition=lambda r: len(r.body) < 12000,
            timeout=30.0,
        )
        assert_http_status(response_2, 200)
        stats_2 = stats_snapshot()

        # Resource found in cache
        assert_stat_increased(stats_1, stats_2, "cache_hits", min_increase=1)
        assert_stat_increased(stats_1, stats_2, "ipro_served", min_increase=1)
        assert_stat_delta(stats_1, stats_2, "ipro_not_rewritable", 0)

        # Doesn't run recorder flow
        assert_stat_delta(stats_1, stats_2, "ipro_not_in_cache", 0)
        assert_stat_delta(stats_1, stats_2, "ipro_recorder_resources", 0)

        # Image gets rewritten (allow 1-2 due to advisory locks)
        assert_stat_increased(stats_1, stats_2, "image_rewrites", min_increase=1)

    @pytest.mark.requires_stats
    @pytest.mark.requires_secondary
    def test_ipro_uncacheable_not_re_recorded(
        self,
        secondary_client: PageSpeedClient,
        stats_snapshot,
    ):
        """IPRO should not re-record uncacheable resources.

        Bash original:
            start_test "IPRO flow doesn't copy uncacheable resources multiple times."
            URL=$IPRO_ROOT/nocache/test_image_dont_reuse.png
        """
        ipro_root = "/mod_pagespeed_test/ipro"
        url = f"{ipro_root}/nocache/test_image_dont_reuse.png"

        # Initial stats
        stats_0 = stats_snapshot()

        # First request - resource not cacheable
        response_1 = secondary_client.get(url)
        assert_http_status(response_1, 200)
        stats_1 = stats_snapshot()

        # Resource not in cache
        assert_stat_delta(stats_0, stats_1, "cache_hits", 0)
        assert_stat_delta(stats_0, stats_1, "cache_misses", 1)
        assert_stat_delta(stats_0, stats_1, "ipro_served", 0)

        # Runs recorder but resource is not cacheable
        assert_stat_delta(stats_0, stats_1, "ipro_not_in_cache", 1)
        assert_stat_delta(stats_0, stats_1, "ipro_recorder_resources", 1)
        assert_stat_delta(stats_0, stats_1, "ipro_recorder_not_cacheable", 1)

        # Uncacheable, so no rewrites
        assert_stat_delta(stats_0, stats_1, "image_rewrites", 0)

        # Second request - should NOT run recorder again
        response_2 = secondary_client.get(url)
        assert_http_status(response_2, 200)
        stats_2 = stats_snapshot()

        assert_stat_delta(stats_1, stats_2, "cache_hits", 0)
        assert_stat_delta(stats_1, stats_2, "ipro_served", 0)
        assert_stat_delta(stats_1, stats_2, "ipro_not_rewritable", 1)

        # Important: Does NOT run recorder again
        assert_stat_delta(stats_1, stats_2, "ipro_not_in_cache", 0)
        assert_stat_delta(stats_1, stats_2, "ipro_recorder_resources", 0)

        # Third request - still doesn't run recorder
        response_3 = secondary_client.get(url)
        assert_http_status(response_3, 200)
        stats_3 = stats_snapshot()

        assert_stat_delta(stats_2, stats_3, "ipro_not_rewritable", 1)
        assert_stat_delta(stats_2, stats_3, "ipro_recorder_resources", 0)


class TestIproImplicitCacheTtl:
    """Tests that IPRO respects ImplicitCacheTtlMs.

    Bash original:
        start_test "IPRO respects ImplicitCacheTtlMs."
    """

    @pytest.mark.requires_secondary
    @pytest.mark.slow
    def test_ipro_implicit_cache_ttl(self, secondary_client: PageSpeedClient):
        """IPRO served resources should respect ImplicitCacheTtlMs directive."""
        ipro_root = "/mod_pagespeed_test/ipro"
        html_url = f"{ipro_root}/no-cache-control-header/ipro.html"
        resource_url = f"{ipro_root}/no-cache-control-header/test_image_dont_reuse.png"

        # Fetch HTML to initiate rewriting
        html_response = secondary_client.get(html_url)
        assert_http_status(html_response, 200)

        # First resource request after short wait
        time.sleep(2)
        response_1 = secondary_client.get(resource_url)
        assert_http_status(response_1, 200)

        # Not yet optimized (original is > 15000 bytes)
        assert_file_size(response_1, ">", 15000, "Should not be optimized yet")

        # Check max-age from Cache-Control header
        cache_control = response_1.header("Cache-Control")
        match = re.search(r"max-age=(\d+)", cache_control)
        assert match, f"Expected max-age in Cache-Control, got: {cache_control}"
        max_age_1 = int(match.group(1))
        assert max_age_1 == 333, f"Expected max-age=333, got {max_age_1}"

        # Second request after another short wait - should be optimized
        time.sleep(2)
        response_2 = secondary_client.fetch_until(
            resource_url,
            condition=lambda r: len(r.body) < 15000,
            timeout=30.0,
        )
        assert_http_status(response_2, 200)
        assert_file_size(response_2, "<", 15000, "Should be optimized")

        # TTL should be reduced (time has passed)
        cache_control = response_2.header("Cache-Control")
        match = re.search(r"max-age=(\d+)", cache_control)
        assert match, f"Expected max-age in Cache-Control, got: {cache_control}"
        max_age_2 = int(match.group(1))

        # Should be less than original but still reasonable
        assert max_age_2 < 333, f"max-age should be reduced, got {max_age_2}"
        assert max_age_2 > 300, f"max-age should still be > 300, got {max_age_2}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
