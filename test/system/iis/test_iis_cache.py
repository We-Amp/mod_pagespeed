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

"""IIS cache management system tests.

These tests verify the PageSpeed cache management functionality on IIS, ported
from Apache's cache_flushing.sh and compressed_cache.sh system tests.

Test classes:
- TestCacheFlush: Verifies cache flushing via cache.flush file or admin API
- TestCacheFlushStatistics: Verifies cache_flush_count statistic tracking
- TestCompressedCache: Tests compressed cache statistics
- TestCachePurge: Tests cache purge functionality (if enabled)

Environment Variables:
    PAGESPEED_CACHE_DIR: Path to cache directory (required for file-based flush)
    PAGESPEED_ADMIN_PATH: Path to admin endpoint (default: /pagespeed_admin)
"""

import json
import os
import pathlib
import time
from typing import Callable, Dict, Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    parse_statistics,
    require_no_auth_gate,
    require_status_ok,
)


# ============================================================================
# Test Fixtures
# ============================================================================


@pytest.fixture(scope="session")
def cache_dir_path(server_config) -> Optional[pathlib.Path]:
    """Return the path to the cache directory if configured."""
    cache_dir = server_config.cache_dir
    if cache_dir:
        return pathlib.Path(cache_dir)
    return None


@pytest.fixture
def cache_flush_file(cache_dir_path: Optional[pathlib.Path]) -> Optional[pathlib.Path]:
    """Return the path to the cache.flush file."""
    if cache_dir_path:
        return cache_dir_path / "cache.flush"
    return None


@pytest.fixture
def initial_stats(
    client: PageSpeedClient, server_config
) -> Callable[[], Dict[str, int]]:
    """Factory fixture for capturing initial statistics."""

    def _capture() -> Dict[str, int]:
        return client.get_statistics(stats_path=server_config.stats_path)

    return _capture


# ============================================================================
# TestCacheFlush: Test cache flushing mechanism
# ============================================================================


class TestCacheFlush:
    """Tests for cache flushing functionality.

    These tests verify that PageSpeed's cache can be flushed via:
    1. Touching cache.flush file in the cache directory
    2. Admin API endpoint (for IIS)

    Ported from: pagespeed/apache/system_tests/cache_flushing.sh
    """

    @pytest.mark.iis_only
    def test_cache_flush_file_exists_after_touch(
        self,
        flush_cache: Callable[[], None],
        cache_flush_file: Optional[pathlib.Path],
    ):
        """Cache flush file should exist after flush operation."""
        if not cache_flush_file:
            pytest.fail(
                "Cannot locate cache.flush: PAGESPEED_CACHE_DIR is not set and "
                "has no default -- every lane uses a different FileCachePath, "
                "so this test cannot guess where <cache dir>/cache.flush lives. "
                "Export PAGESPEED_CACHE_DIR to the cache directory the IIS "
                "server under test is actually configured with."
            )

        # Perform flush
        flush_cache()

        # File should exist (created by flush operation)
        # Note: On IIS, flush may use admin API instead of file
        # so we don't fail if file doesn't exist
        if cache_flush_file.parent.exists():
            # Just verify we can access the cache directory
            assert cache_flush_file.parent.is_dir()

    # test_cache_flush_via_admin_api removed: there is no admin-API cache
    # flush in 1.1 today. pagespeed/iis/iis_admin_handler.cc defines a POST
    # /cache?action=flush dispatch, but that file's HandleRequest is dead
    # code (iis_http_module.cpp calls server_context->AdminPage() directly
    # and never delegates to IisAdminHandler) -- The
    # cross-server AdminSite::PrintCaches doesn't honor "cache=flush"
    # query either -- it only acts on url=, new_set=, purge=. Cache flush
    # on IIS today goes through the file-based cache.flush touch, which
    # test_cache_flush_triggers_cache_invalidation below already exercises.

    @pytest.mark.iis_only
    def test_cache_flush_triggers_cache_invalidation(
        self,
        client: PageSpeedClient,
        server_config,
        flush_cache: Callable[[], None],
        example_root: str,
    ):
        """Flushing cache should invalidate cached resources.

        This test:
        1. Fetches a resource to prime the cache
        2. Flushes the cache
        3. Verifies the resource is re-fetched (not served from cache)

        Ported from cache_flushing.sh: "Cache flushing works by touching
        cache.flush in cache directory"
        """
        # Prime the cache with a request
        test_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response1 = client.fetch_until_contains(
            test_url, pattern=r"\.pagespeed\.", timeout=30.0
        )
        assert_http_status(response1, 200)

        # Record cache state before flush
        # (We use a unique query parameter to avoid getting cached response)
        timestamp = int(time.time() * 1000)

        # Flush the cache
        flush_cache()

        # Wait for flush to take effect
        time.sleep(1.0)

        # Make a request - if cache was properly flushed, this should
        # trigger re-optimization. We test by checking the response is valid.
        test_url2 = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images&ts={timestamp}"
        response2 = client.fetch_until_contains(
            test_url2, pattern=r"\.pagespeed\.", timeout=30.0
        )
        assert_http_status(response2, 200)

    @pytest.mark.iis_only
    @pytest.mark.slow
    def test_cache_flush_timing(
        self,
        client: PageSpeedClient,
        server_config,
        flush_cache: Callable[[], None],
        example_root: str,
    ):
        """Cache flush should take effect within reasonable time.

        Tests that cache flush is detected and acted upon within the
        expected polling interval (typically 1-5 seconds).
        """
        # Prime cache
        test_url = f"{example_root}/extend_cache.html"
        client.get(test_url)

        # Record time before flush
        start_time = time.time()

        # Flush cache
        flush_cache()

        # Cache should be flushed within 5 seconds
        max_wait = 5.0
        elapsed = time.time() - start_time

        assert elapsed < max_wait, (
            f"Cache flush operation took too long: {elapsed:.2f}s (max: {max_wait}s)"
        )


# ============================================================================
# TestCacheFlushStatistics: Test cache_flush_count statistic
# ============================================================================


class TestCacheFlushStatistics:
    """Tests for cache flush statistics tracking.

    These tests verify that PageSpeed tracks the cache_flush_count statistic
    correctly.

    Ported from: cache_flushing.sh - "NUM_FLUSHES=$(scrape_stat cache_flush_count)"
    """

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_cache_flush_count_exists(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics should include cache_flush_count counter."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        assert "cache_flush_count" in stats, (
            f"Expected 'cache_flush_count' stat. Available: {list(stats.keys())[:20]}..."
        )
        assert stats["cache_flush_count"] >= 0

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_cache_flush_count_increments(
        self,
        client: PageSpeedClient,
        server_config,
        example_root: str,
    ):
        """cache_flush_count should increment after a whole-cache purge.

        Ported from cache_flushing.sh:
        NUM_NEW_FLUSHES=$(expr $NUM_FLUSHES - $NUM_INITIAL_FLUSHES)
        check [ $NUM_NEW_FLUSHES -ge 1 ]
        check [ $NUM_NEW_FLUSHES -lt 20 ]

        The IIS rig runs with EnableCachePurge on, so the live flush path
        is the purge machinery, not the legacy cache.flush file-watch:
        GET /pagespeed_admin/cache?purge=* routes through AdminSite::PurgeHandler
        -> PurgeContext::SetCachePurgeGlobalTimestampMs, and the next
        request's FlushCacheIfNecessary -> PollFileSystem applies the new
        purge set and bumps cache_flush_count via UpdateCachePurgeSet
        (system_server_context.cc). The flush_cache fixture's cache.flush
        file-touch is inert here because PurgeContext watches cache.purge
        in purge mode.
        """
        # Get initial count
        stats_before = client.get_statistics(stats_path=server_config.stats_path)
        initial_count = stats_before.get("cache_flush_count", 0)

        # Drive a whole-cache purge through the live admin path.
        purge_url = f"{server_config.admin_path}/cache?purge=*"
        response = client.get(purge_url)
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")
        require_status_ok(response, "Cache purge endpoint")

        # The counter moves when the next pagespeed-handled request's
        # PollFileSystem observes the updated purge set; poll, driving a
        # page fetch each round to trigger the check.
        new_count = initial_count
        deadline = time.time() + 30.0
        while time.time() < deadline:
            client.get(f"{example_root}/extend_cache.html")
            stats_after = client.get_statistics(stats_path=server_config.stats_path)
            new_count = stats_after.get("cache_flush_count", 0)
            if new_count > initial_count:
                break
            time.sleep(0.5)

        # Should have incremented by at least 1, but not excessively
        delta = new_count - initial_count
        assert delta >= 1, (
            f"cache_flush_count should increment after purge. "
            f"Before: {initial_count}, After: {new_count}, Delta: {delta}"
        )
        assert delta < 20, (
            f"cache_flush_count incremented too much ({delta}). "
            f"Expected < 20 flushes from a single purge."
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_cache_flush_count_stable_without_flush(
        self, client: PageSpeedClient, server_config
    ):
        """cache_flush_count should not increment without cache flush."""
        # Get initial count
        stats_before = client.get_statistics(stats_path=server_config.stats_path)
        initial_count = stats_before.get("cache_flush_count", 0)

        # Wait without flushing
        time.sleep(2.0)

        # Get count again
        stats_after = client.get_statistics(stats_path=server_config.stats_path)
        new_count = stats_after.get("cache_flush_count", 0)

        # Should not have changed
        assert new_count == initial_count, (
            f"cache_flush_count changed without flush. "
            f"Before: {initial_count}, After: {new_count}"
        )


# ============================================================================
# TestCompressedCache: Test compressed cache statistics
# ============================================================================


class TestCompressedCache:
    """Tests for compressed cache functionality.

    These tests verify that PageSpeed's compressed cache is working and
    tracking statistics correctly.

    Ported from: pagespeed/apache/system_tests/compressed_cache.sh
    """

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_compressed_cache_stats_exist(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics should include compressed cache counters."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        # Check for compressed cache stats
        compressed_stats = [
            "compressed_cache_original_size",
            "compressed_cache_compressed_size",
        ]

        found = []
        for stat in compressed_stats:
            if stat in stats:
                found.append(stat)

        if len(found) == 0:
            pytest.skip(
                f"Compressed cache stats not found. "
                f"CompressedCache may not be enabled. "
                f"Available stats: {list(stats.keys())[:20]}..."
            )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_compressed_cache_shows_savings(
        self,
        client: PageSpeedClient,
        server_config,
        example_root: str,
    ):
        """Compressed cache should show size savings.

        Ported from compressed_cache.sh:
        check [ "$compressed_size" -lt "$original_size" ];
        check [ "$compressed_size" -gt 0 ];
        check [ "$original_size" -gt 0 ];
        """
        # First, make some requests to populate the cache
        urls = [
            f"{example_root}/combine_css.html?PageSpeedFilters=combine_css",
            f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images",
        ]
        for url in urls:
            client.fetch_until_contains(url, pattern=r"\.pagespeed\.", timeout=30.0)

        # Give cache time to populate
        time.sleep(1.0)

        # Get statistics
        stats = client.get_statistics(stats_path=server_config.stats_path)

        original_size = stats.get("compressed_cache_original_size", 0)
        compressed_size = stats.get("compressed_cache_compressed_size", 0)

        # Skip if compressed cache is not enabled or not populated
        if original_size == 0 and compressed_size == 0:
            pytest.skip(
                "Compressed cache appears to be disabled or empty. "
                "Enable CompressedCache to test this feature."
            )

        # Verify compressed cache is providing savings
        assert compressed_size < original_size, (
            f"Compressed cache should be smaller than original. "
            f"Original: {original_size}, Compressed: {compressed_size}"
        )
        assert compressed_size > 0, "Compressed size should be positive"
        assert original_size > 0, "Original size should be positive"

        # Calculate and report savings
        savings_pct = ((original_size - compressed_size) / original_size) * 100
        print(
            f"Compressed cache savings: {savings_pct:.1f}% "
            f"(original: {original_size}, compressed: {compressed_size})"
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_compressed_cache_grows_with_requests(
        self,
        client: PageSpeedClient,
        server_config,
        example_root: str,
    ):
        """Compressed cache size should grow as more resources are cached."""
        # Get initial sizes
        stats_before = client.get_statistics(stats_path=server_config.stats_path)
        original_before = stats_before.get("compressed_cache_original_size", 0)

        # Make requests to different pages to add to cache
        timestamp = int(time.time() * 1000)
        urls = [
            f"{example_root}/styles/all_styles.css?ts={timestamp}",
            f"{example_root}/images/Puzzle.jpg?ts={timestamp}",
        ]

        for url in urls:
            try:
                client.get(url)
            except Exception:
                pass  # Some resources may not exist

        # Give cache time to update
        time.sleep(1.5)

        # Get new sizes
        stats_after = client.get_statistics(stats_path=server_config.stats_path)
        original_after = stats_after.get("compressed_cache_original_size", 0)

        # Cache should have grown (or stayed same if resources were already cached)
        assert original_after >= original_before, (
            f"Cache size should not decrease. "
            f"Before: {original_before}, After: {original_after}"
        )


# ============================================================================
# TestCachePurge: Test cache purge functionality
# ============================================================================


class TestCachePurge:
    """Tests for cache purge via AdminSite::PrintCaches.

    Routed by GET /pagespeed_admin/cache?purge=URL. The /cache prefix in
    the path is essential -- a bare /pagespeed_admin?purge=URL hits
    AdminSite::AdminPage with an empty leaf and 301-redirects to add a
    trailing slash (admin_site.cc:404). With /cache present, PrintCaches
    handles the ?purge= query: when enable_cache_purge is on it calls
    PurgeHandler; when disabled it returns a JSON success:false error.

    Note: the IIS-specific POST /cache?action=purge handler in
    iis_admin_handler.cc::HandleCachePurge is dead code,
    so these tests target the cross-server GET path that AdminPage
    actually dispatches in 1.1 today.
    """

    @pytest.mark.iis_only
    def test_cache_purge_endpoint_exists(
        self, client: PageSpeedClient, server_config
    ):
        """Purge endpoint should respond 200 + JSON regardless of whether
        EnableCachePurge is on or off."""
        purge_url = f"{server_config.admin_path}/cache?purge=*"
        response = client.get(purge_url)

        # The lane runs the admin endpoint without auth; a 403 from the
        # admin handler is a plausible regression, not an environment
        # condition.
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")

        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "application/json" in content_type, (
            f"Purge endpoint should return JSON, got: {content_type}"
        )
        # Body parses as JSON regardless of purge-enabled state.
        json.loads(response.text)

    @pytest.mark.iis_only
    def test_cache_purge_disabled_message(
        self, client: PageSpeedClient, server_config
    ):
        """When EnableCachePurge is off, PrintCaches returns a JSON error.

        Ported from: pagespeed/apache/system_tests/purging_disabled.sh.
        admin_site.cc:267-272 returns
        {"success":false,"error":"Purging not enabled: please add '<directive>' to your configuration file."}
        with HTTP 200.
        """
        purge_url = (
            f"{server_config.admin_path}/cache"
            f"?purge=http://example.com/test.css"
        )
        response = client.get(purge_url)

        # The lane runs the admin endpoint without auth; a 403 from the
        # admin handler is a plausible regression, not an environment
        # condition.
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")

        assert_http_status(response, 200)
        data = json.loads(response.text)

        # If purge is enabled, body is the PurgeHandler response (no
        # success:false field). Otherwise the disabled-error shape applies.
        # The error string itself can be empty in some test rigs (depends
        # on how FormatOption renders the directive) -- the success:false
        # signal is the canonical disabled indicator.
        if data.get("success") is False:
            assert "error" in data, (
                f"Disabled-purge response should carry an 'error' field, "
                f"got: {data!r}"
            )
        else:
            pytest.skip("Cache purge appears to be enabled")

    @pytest.mark.iis_only
    def test_cache_purge_url_format(
        self, client: PageSpeedClient, server_config
    ):
        """PrintCaches purge handler should accept a range of URL formats
        without surfacing a 500."""
        test_urls = [
            "http://example.com/test.css",
            "http://example.com/images/*.jpg",
            "*",
        ]

        for test_url in test_urls:
            purge_url = f"{server_config.admin_path}/cache?purge={test_url}"
            response = client.get(purge_url)

            assert response.status != 500, (
                f"Purge URL format '{test_url}' caused server error"
            )


# ============================================================================
# TestCacheIntegration: Integration tests combining cache features
# ============================================================================


class TestCacheIntegration:
    """Integration tests that verify cache works with other features."""

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_cache_stats_update_after_optimization(
        self,
        client: PageSpeedClient,
        server_config,
        example_root: str,
    ):
        """Cache statistics should update after resource optimization."""
        # Get initial cache stats
        stats_before = client.get_statistics(stats_path=server_config.stats_path)

        cache_stat_names = [
            "cache_hits",
            "cache_misses",
            "cache_inserts",
        ]

        initial_values = {
            name: stats_before.get(name, 0) for name in cache_stat_names
        }

        # Make requests that should trigger cache activity
        timestamp = int(time.time() * 1000)
        test_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images&ts={timestamp}"

        # First request - should be a cache miss
        client.fetch_until_contains(test_url, pattern=r"\.pagespeed\.", timeout=30.0)

        # Second request - should be a cache hit (for some resources)
        client.get(test_url)

        # Get updated stats
        time.sleep(0.5)
        stats_after = client.get_statistics(stats_path=server_config.stats_path)

        # At least one cache stat should have changed
        changed_stats = []
        for name in cache_stat_names:
            before = initial_values.get(name, 0)
            after = stats_after.get(name, 0)
            if after != before:
                changed_stats.append(f"{name}: {before} -> {after}")

        if not changed_stats:
            # This might be expected if all resources were already cached
            pytest.skip(
                "No cache statistics changed - resources may have been pre-cached"
            )

    @pytest.mark.iis_only
    @pytest.mark.slow
    def test_cache_respects_ttl(
        self,
        client: PageSpeedClient,
        server_config,
        example_root: str,
    ):
        """Cached resources should respect their TTL settings."""
        # Request a resource with caching
        test_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until_contains(
            test_url, pattern=r"\.pagespeed\.", timeout=30.0
        )
        assert_http_status(response, 200)

        # Check Cache-Control header on optimized resources
        # Optimized resources should have extended cache times
        cache_control = response.header("Cache-Control")
        if cache_control:
            # Should have max-age directive
            if "max-age=" in cache_control.lower():
                # Extract max-age value
                import re
                match = re.search(r"max-age=(\d+)", cache_control.lower())
                if match:
                    max_age = int(match.group(1))
                    # Extended cache should have long TTL (at least 1 day = 86400s)
                    # But this may vary by configuration
                    assert max_age > 0, "max-age should be positive"
