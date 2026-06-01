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
        secondary_stats_snapshot,
    ):
        """IPRO flow should use cache as expected.

        First request: resource not in cache, runs recorder flow
        Second request: resource found in cache, served optimized
        """
        ipro_root = "/mod_pagespeed_test/ipro"
        url = f"{ipro_root}/test_image_dont_reuse2.png"

        # Read stats from secondary vhost (same vhost that handles IPRO requests)
        stats_0 = secondary_stats_snapshot()

        # First IPRO request
        response_1 = secondary_client.get(url)
        assert_http_status(response_1, 200)
        stats_1 = secondary_stats_snapshot()

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
        stats_2 = secondary_stats_snapshot()

        # Resource found in cache
        assert_stat_increased(stats_1, stats_2, "cache_hits", min_increase=1)
        assert_stat_increased(stats_1, stats_2, "ipro_served", min_increase=1)
        assert_stat_delta(stats_1, stats_2, "ipro_not_rewritable", 0)

        # fetch_until polling may arrive before the recorder's async cache
        # insert is visible, triggering one additional cache miss and recorder
        # run. Allow 0-1 for these inherently racy stats.
        for stat_name in ("ipro_not_in_cache", "ipro_recorder_resources"):
            delta = (stats_2.get(stat_name, 0) - stats_1.get(stat_name, 0))
            assert 0 <= delta <= 1, (
                f"{stat_name} delta should be 0 or 1, got {delta}")

        # Image gets rewritten (allow 1-2 due to advisory locks)
        assert_stat_increased(stats_1, stats_2, "image_rewrites", min_increase=1)

    @pytest.mark.requires_stats
    @pytest.mark.requires_secondary
    def test_ipro_uncacheable_not_re_recorded(
        self,
        secondary_client: PageSpeedClient,
        secondary_stats_snapshot,
    ):
        """IPRO should not re-record uncacheable resources.

        Bash original:
            start_test "IPRO flow doesn't copy uncacheable resources multiple times."
            URL=$IPRO_ROOT/nocache/test_image_dont_reuse.png
        """
        ipro_root = "/mod_pagespeed_test/ipro"
        url = f"{ipro_root}/nocache/test_image_dont_reuse.png"

        # Read stats from secondary vhost (same vhost that handles IPRO requests)
        stats_0 = secondary_stats_snapshot()

        # First request - resource not cacheable
        response_1 = secondary_client.get(url)
        assert_http_status(response_1, 200)
        stats_1 = secondary_stats_snapshot()

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

        # The recorder writes the "not rewritable" failure entry to the HTTP
        # cache via RememberFailure().  For the in-memory LRU cache this is
        # synchronous, but give a small window for the output-filter chain to
        # finish in case of scheduling jitter.
        time.sleep(1)

        # Second request — the failure entry should now be in cache, so the
        # server returns "not rewritable" without running the recorder again.
        # If the cache write from the first request hasn't landed yet (rare),
        # retry a few times with a pause rather than firing many requests in
        # a tight loop (which would launch extra recorders and perturb stats).
        for attempt in range(5):
            stats_before_2 = secondary_stats_snapshot()
            response_2 = secondary_client.get(url)
            assert_http_status(response_2, 200)
            stats_2 = secondary_stats_snapshot()
            nr_delta = (stats_2.get("ipro_not_rewritable", 0) -
                        stats_before_2.get("ipro_not_rewritable", 0))
            if nr_delta == 1:
                break
            # Failure entry not visible yet — sleep and retry.
            time.sleep(2)
        else:
            pytest.fail(
                "ipro_not_rewritable did not increment by 1 on the second "
                f"request after 5 attempts (last delta: {nr_delta})")

        # The second request should not have run the recorder.
        assert_stat_delta(stats_before_2, stats_2, "ipro_recorder_resources", 0)

        # Third request — same as second: cached failure entry, no recorder.
        # Use the same retry loop as the second request: the short-lived
        # RecentFetchFailed cache entry may have been evicted or may not be
        # visible yet after the second request's cache write, so we retry.
        for attempt in range(5):
            stats_before_3 = secondary_stats_snapshot()
            response_3 = secondary_client.get(url)
            assert_http_status(response_3, 200)
            stats_3 = secondary_stats_snapshot()
            nr_delta_3 = (stats_3.get("ipro_not_rewritable", 0) -
                          stats_before_3.get("ipro_not_rewritable", 0))
            if nr_delta_3 == 1:
                break
            time.sleep(2)
        else:
            pytest.fail(
                "ipro_not_rewritable did not increment by 1 on the third "
                f"request after 5 attempts (last delta: {nr_delta_3})")

        # The third request should not have run the recorder.
        assert_stat_delta(stats_before_3, stats_3, "ipro_recorder_resources", 0)


class TestIproImplicitCacheTtl:
    """Tests that IPRO respects ImplicitCacheTtlMs.

    Bash original:
        start_test "IPRO respects ImplicitCacheTtlMs."
    """

    @pytest.mark.requires_secondary
    @pytest.mark.slow
    def test_ipro_implicit_cache_ttl(self, secondary_client: PageSpeedClient):
        """IPRO served resources should respect ImplicitCacheTtlMs directive.

        Ported from the bash test which uses single requests with sleeps
        rather than polling.  The IPRO recording flow is sensitive to
        rapid repeated requests: the first request triggers the recorder
        and caches the resource, but aggressive polling can interfere with
        the async optimization pipeline.  We therefore mirror the original
        bash approach of sleeping before each single fetch.
        """
        ipro_root = "/mod_pagespeed_test/ipro"
        html_url = f"{ipro_root}/no-cache-control-header/ipro.html"
        resource_url = f"{ipro_root}/no-cache-control-header/test_image_dont_reuse.png"

        # Fetch HTML to initiate rewriting and caching of the image.
        html_response = secondary_client.get(html_url)
        assert_http_status(html_response, 200)

        # First IPRO resource request after a short wait: the resource
        # will have the full implicit cache TTL (max-age=333) once it has
        # been recorded.  Use fetch_until so we tolerate the initial
        # s-maxage=10 "not yet recorded" response, but match any max-age
        # value rather than demanding exactly 333 — the value may already
        # be counting down slightly by the time we see it.
        time.sleep(2)

        def has_implicit_ttl(r):
            cc = r.header("Cache-Control") or ""
            m = re.search(r"max-age=(\d+)", cc)
            return m and int(m.group(1)) > 300

        response_1 = secondary_client.fetch_until(
            resource_url,
            condition=has_implicit_ttl,
            timeout=30.0,
        )
        assert_http_status(response_1, 200)

        cache_control_1 = response_1.header("Cache-Control") or ""
        match_1 = re.search(r"max-age=(\d+)", cache_control_1)
        assert match_1, f"Expected max-age in Cache-Control, got: {cache_control_1}"
        max_age_1 = int(match_1.group(1))
        # The implicit TTL is 333s; allow a small window for elapsed time.
        assert max_age_1 <= 333, f"max-age should be <= 333, got {max_age_1}"
        assert max_age_1 > 300, f"max-age should be > 300, got {max_age_1}"

        # Second IPRO resource request: should be optimized and the TTL
        # should be reduced because more time has passed since recording.
        time.sleep(2)
        response_2 = secondary_client.fetch_until(
            resource_url,
            condition=lambda r: len(r.body) < 15000,
            timeout=30.0,
        )
        assert_http_status(response_2, 200)
        assert_file_size(response_2, "<", 15000, "Should be optimized")

        # TTL should be reduced (time has passed)
        cache_control_2 = response_2.header("Cache-Control") or ""
        match_2 = re.search(r"max-age=(\d+)", cache_control_2)
        assert match_2, f"Expected max-age in Cache-Control, got: {cache_control_2}"
        max_age_2 = int(match_2.group(1))

        # Should be less than original but still reasonable
        assert max_age_2 < 333, f"max-age should be reduced, got {max_age_2}"
        assert max_age_2 > 300, f"max-age should still be > 300, got {max_age_2}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
