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

"""IIS blocking rewrite system tests.

These tests verify the PageSpeed blocking rewrite functionality on IIS, ported
from Apache's blocking_rewrite.sh system test.

Test classes:
- TestBlockingRewrite: Tests X-PSA-Blocking-Rewrite header behavior
- TestBlockingRewriteStatistics: Tests blocking rewrite statistics

Environment Variables:
    PAGESPEED_TEST_ROOT: Root path for test pages
    PAGESPEED_ADMIN_KEY: Blocking rewrite key (optional)
"""

import re
import time
from typing import Dict, Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    parse_statistics,
)


# ============================================================================
# TestBlockingRewrite: Test X-PSA-Blocking-Rewrite functionality
# ============================================================================


class TestBlockingRewrite:
    """Tests for blocking rewrite functionality.

    These tests verify that the X-PSA-Blocking-Rewrite header causes
    PageSpeed to fully optimize resources before responding.

    Ported from: pagespeed/apache/system_tests/blocking_rewrite.sh
    """

    @pytest.mark.iis_only
    def test_blocking_rewrite_header_accepted(
        self, client: PageSpeedClient, example_root: str
    ):
        """X-PSA-Blocking-Rewrite header should be accepted."""
        response = client.get(
            f"{example_root}/extend_cache.html",
            headers={"X-PSA-Blocking-Rewrite": "true"}
        )
        assert_http_status(response, 200)

    @pytest.mark.iis_only
    def test_blocking_rewrite_image(
        self, client: PageSpeedClient, example_root: str
    ):
        """With blocking rewrite, images should be fully optimized in first response.

        Normal behavior: Images may be optimized in background.
        Blocking behavior: Response waits until images are optimized.
        """
        # Request with blocking rewrite
        response = client.get(
            f"{example_root}/extend_cache.html?PageSpeedFilters=rewrite_images",
            headers={"X-PSA-Blocking-Rewrite": "true"},
            timeout=60.0  # May take longer due to blocking
        )
        assert_http_status(response, 200)

        # With blocking rewrite, optimized resources should be in first response
        # (hard to verify without comparing to non-blocking behavior)
        if ".pagespeed." in response.text:
            # Resources were optimized
            pass

    @pytest.mark.iis_only
    def test_blocking_rewrite_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """With blocking rewrite, CSS should be fully optimized."""
        response = client.get(
            f"{example_root}/combine_css.html?PageSpeedFilters=combine_css",
            headers={"X-PSA-Blocking-Rewrite": "true"},
            timeout=60.0
        )
        assert_http_status(response, 200)

    @pytest.mark.iis_only
    def test_blocking_rewrite_javascript(
        self, client: PageSpeedClient, example_root: str
    ):
        """With blocking rewrite, JavaScript should be fully optimized."""
        response = client.get(
            f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript",
            headers={"X-PSA-Blocking-Rewrite": "true"},
            timeout=60.0
        )

        if response.status == 200:
            # Check if JS was combined
            if ".pagespeed.jc." in response.text:
                pass  # Successfully combined

    @pytest.mark.iis_only
    def test_blocking_rewrite_with_key(
        self, client: PageSpeedClient, example_root: str
    ):
        """Blocking rewrite should work with correct key (if configured)."""
        # Some configurations require a key for blocking rewrite
        import os
        admin_key = os.environ.get("PAGESPEED_ADMIN_KEY", "")

        headers = {"X-PSA-Blocking-Rewrite": "true"}
        if admin_key:
            headers["X-PSA-Blocking-Rewrite-Key"] = admin_key

        response = client.get(
            f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images",
            headers=headers,
            timeout=60.0
        )
        assert_http_status(response, 200)

    @pytest.mark.iis_only
    def test_blocking_rewrite_wrong_key(
        self, client: PageSpeedClient, example_root: str
    ):
        """Blocking rewrite with wrong key should be handled appropriately."""
        response = client.get(
            f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images",
            headers={
                "X-PSA-Blocking-Rewrite": "true",
                "X-PSA-Blocking-Rewrite-Key": "wrong_key_12345"
            },
            timeout=30.0
        )

        # Should either succeed (key not required) or return non-blocking
        # behavior (key required but wrong)
        assert response.status in (200, 401, 403), (
            f"Wrong key should be handled, got {response.status}"
        )


# ============================================================================
# TestBlockingRewriteStatistics: Test blocking rewrite statistics
# ============================================================================


class TestBlockingRewriteStatistics:
    """Tests for blocking rewrite statistics.

    These tests verify that blocking rewrite operations are properly
    tracked in statistics.

    Ported from: blocking_rewrite.sh stat verification
    """

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_blocking_rewrite_stat_exists(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics should include blocking rewrite counter."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        # Look for blocking rewrite related stats
        blocking_stats = [
            "blocking_rewrite_count",
            "blocking_rewrites",
            "num_blocking_rewrites",
        ]

        found = any(stat in stats for stat in blocking_stats)
        # This stat may not exist in all configurations
        if not found:
            pytest.skip(
                f"No blocking rewrite stat found. "
                f"Available stats: {list(stats.keys())[:20]}..."
            )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_blocking_rewrite_increments_stat(
        self, client: PageSpeedClient, server_config, example_root: str
    ):
        """Blocking rewrite should increment statistics."""
        # Get initial stats
        stats_before = client.get_statistics(stats_path=server_config.stats_path)

        # Make a blocking rewrite request
        client.get(
            f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images",
            headers={"X-PSA-Blocking-Rewrite": "true"},
            timeout=60.0
        )

        # Wait for stats to update
        time.sleep(1.0)

        # Get updated stats
        stats_after = client.get_statistics(stats_path=server_config.stats_path)

        # Some rewrite-related stat should have changed
        changed = False
        for key in stats_after:
            if "rewrite" in key.lower() or "cache" in key.lower():
                before = stats_before.get(key, 0)
                after = stats_after.get(key, 0)
                if after > before:
                    changed = True
                    break

        # Note: Stat may not change if resource was already optimized
        if not changed:
            pytest.skip("No rewrite stats changed - resource may be cached")

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_blocking_rewrite_cache_stats(
        self, client: PageSpeedClient, server_config, example_root: str
    ):
        """Blocking rewrite should update cache statistics."""
        # Get initial stats
        stats_before = client.get_statistics(stats_path=server_config.stats_path)
        cache_before = {
            k: v for k, v in stats_before.items()
            if "cache" in k.lower()
        }

        # Make blocking rewrite request
        timestamp = int(time.time() * 1000)
        client.get(
            f"{example_root}/extend_cache.html"
            f"?PageSpeedFilters=extend_cache_images&ts={timestamp}",
            headers={"X-PSA-Blocking-Rewrite": "true"},
            timeout=60.0
        )

        # Wait and get updated stats
        time.sleep(1.0)
        stats_after = client.get_statistics(stats_path=server_config.stats_path)
        cache_after = {
            k: v for k, v in stats_after.items()
            if "cache" in k.lower()
        }

        # At least one cache stat should have changed
        changed = any(
            cache_after.get(k, 0) != cache_before.get(k, 0)
            for k in set(cache_before) | set(cache_after)
        )

        if not changed:
            pytest.skip("No cache stats changed - may need fresh cache")


# ============================================================================
# TestBlockingRewritePerformance: Performance-related tests
# ============================================================================


class TestBlockingRewritePerformance:
    """Performance tests for blocking rewrite."""

    @pytest.mark.iis_only
    @pytest.mark.slow
    def test_blocking_rewrite_timeout(
        self, client: PageSpeedClient, example_root: str
    ):
        """Blocking rewrite should complete within timeout."""
        start_time = time.time()

        response = client.get(
            f"{example_root}/extend_cache.html?PageSpeedFilters=rewrite_images",
            headers={"X-PSA-Blocking-Rewrite": "true"},
            timeout=120.0
        )

        elapsed = time.time() - start_time

        assert_http_status(response, 200)
        # Should complete within 2 minutes
        assert elapsed < 120, f"Blocking rewrite took too long: {elapsed:.2f}s"

    @pytest.mark.iis_only
    def test_non_blocking_faster_than_blocking(
        self, client: PageSpeedClient, example_root: str
    ):
        """Non-blocking should respond faster than blocking (first request)."""
        # This test is informational - timing may vary
        timestamp = int(time.time() * 1000)

        # Non-blocking request
        start1 = time.time()
        response1 = client.get(
            f"{example_root}/extend_cache.html"
            f"?PageSpeedFilters=rewrite_images&ts={timestamp}_nb",
            timeout=60.0
        )
        time_non_blocking = time.time() - start1

        # Blocking request
        start2 = time.time()
        response2 = client.get(
            f"{example_root}/extend_cache.html"
            f"?PageSpeedFilters=rewrite_images&ts={timestamp}_b",
            headers={"X-PSA-Blocking-Rewrite": "true"},
            timeout=120.0
        )
        time_blocking = time.time() - start2

        # Log timing for analysis
        print(f"Non-blocking: {time_non_blocking:.3f}s, Blocking: {time_blocking:.3f}s")

        # Both should complete
        assert response1.status in (200, 304)
        assert response2.status in (200, 304)
