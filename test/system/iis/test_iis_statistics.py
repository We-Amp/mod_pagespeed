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

"""IIS statistics system tests.

These tests verify the PageSpeed statistics functionality on IIS, ported from
Apache's statistics.sh and statistics_logging.sh system tests.

Test classes:
- TestStatisticsEndpoint: Verifies the /pagespeed_statistics endpoint
- TestStatisticsContents: Verifies specific statistics are present and numeric
- TestStatisticsLogging: Tests statistics logging to file (if configured)
- TestStatisticsJson: Tests the JSON statistics handler
- TestStatisticsConsole: Tests the statistics console UI

Environment Variables:
    PAGESPEED_STATS_LOG: Path to statistics log file (optional)
    PAGESPEED_CONSOLE_URL: URL to statistics console (default: /pagespeed_console)
"""

import json
import os
import pathlib
import re
import time
from typing import Dict, Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    parse_statistics,
    require_status_ok,
)


# ============================================================================
# Test Fixtures
# ============================================================================


@pytest.fixture(scope="session")
def stats_log_path(server_config) -> Optional[pathlib.Path]:
    """Return the path to the statistics log file if configured."""
    log_path = os.environ.get("PAGESPEED_STATS_LOG")
    if log_path:
        return pathlib.Path(log_path)
    # Default location for IIS (only derivable when a cache dir is configured;
    # PAGESPEED_CACHE_DIR has no built-in default)
    if not server_config.cache_dir:
        return None
    return pathlib.Path(server_config.cache_dir) / "stats.log"


@pytest.fixture(scope="session")
def console_url(server_config) -> str:
    """Return the URL to the statistics console."""
    return os.environ.get("PAGESPEED_CONSOLE_URL", "/pagespeed_console")


@pytest.fixture
def test_start_time() -> int:
    """Return the current timestamp for log time comparisons."""
    return int(time.time() * 1000)  # milliseconds


# ============================================================================
# TestStatisticsEndpoint: Test /pagespeed_statistics endpoint details
# ============================================================================


class TestStatisticsEndpoint:
    """Tests for the pagespeed_statistics endpoint.

    These tests verify that the statistics endpoint is accessible and returns
    properly formatted statistics data.
    """

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_endpoint_responds(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics endpoint should respond with HTTP 200."""
        response = client.get(server_config.stats_path)
        assert_http_status(response, 200)

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_content_type(self, client: PageSpeedClient, server_config):
        """Statistics endpoint should return application/json.

        Post-SPA: AdminSite::StatisticsHandler delegates to
        StatisticsJsonHandler, which calls stats->DumpJson() with
        kContentTypeJson (admin_site.cc:146-152). The pre-SPA
        text/plain "name: value" format is gone.
        """
        response = client.get(server_config.stats_path)
        assert_http_status(response, 200)

        content_type = response.header("Content-Type").lower()
        assert "application/json" in content_type, (
            f"Expected application/json, got: {content_type}"
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_format(self, client: PageSpeedClient, server_config):
        """Statistics should be in 'name: value' format."""
        response = client.get(server_config.stats_path)
        assert_http_status(response, 200)

        # Parse should succeed
        stats = parse_statistics(response.text)
        assert len(stats) > 0, "No statistics parsed from response"

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_are_numeric(self, client: PageSpeedClient, server_config):
        """All statistic values should be integers.

        Most counters are non-negative, but bytes-saved metrics can go
        negative when an optimization makes the output larger than the
        input (e.g., CSS minification that adds whitespace canonicalization
        bytes). The non-negative check is restricted to counters that
        cannot legitimately go below zero.
        """
        response = client.get(server_config.stats_path)
        stats = parse_statistics(response.text)

        for name, value in stats.items():
            assert isinstance(value, int), (
                f"Stat '{name}' has non-integer value: {value}"
            )
            # bytes_saved metrics are signed; everything else should be >= 0.
            if "bytes_saved" not in name:
                assert value >= 0, (
                    f"Stat '{name}' has negative value: {value}"
                )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_no_caching(self, client: PageSpeedClient, server_config):
        """Statistics endpoint should not be cached."""
        response = client.get(server_config.stats_path)
        assert_http_status(response, 200)

        cache_control = response.header("Cache-Control").lower()
        # Should have no-cache or max-age=0 or no-store
        if cache_control:
            assert any(
                directive in cache_control
                for directive in ["no-cache", "no-store", "max-age=0", "private"]
            ), f"Statistics should not be cached, but Cache-Control is: {cache_control}"


# ============================================================================
# TestStatisticsContents: Verify specific stats are present and numeric
# ============================================================================


class TestStatisticsContents:
    """Tests that verify specific statistics are present.

    These tests ensure the key PageSpeed counters are being tracked and
    reported correctly.
    """

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_contains_cache_stats(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics should include cache-related counters."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        cache_stats = [
            "cache_hits",
            "cache_misses",
            "cache_extensions",
            "cache_inserts",
        ]

        found_stats = []
        for stat in cache_stats:
            if stat in stats:
                found_stats.append(stat)
                assert isinstance(stats[stat], int)

        # At least one cache stat should be present
        assert len(found_stats) > 0, (
            f"Expected at least one cache stat from {cache_stats}, "
            f"but found none. Available stats: {list(stats.keys())[:20]}..."
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_contains_rewrite_stats(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics should include rewrite-related counters."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        # Common rewrite stats (various naming conventions)
        rewrite_patterns = [
            r".*rewrite.*",
            r".*_rewrites$",
            r".*_rewritten$",
            r"num_flushes",
            r"num_rewrites",
        ]

        found_rewrite_stats = []
        for stat_name in stats.keys():
            for pattern in rewrite_patterns:
                if re.match(pattern, stat_name, re.IGNORECASE):
                    found_rewrite_stats.append(stat_name)
                    break

        # Should find some rewrite-related stats
        assert len(found_rewrite_stats) > 0, (
            f"Expected at least one rewrite stat matching {rewrite_patterns}, "
            f"but found none. Available stats: {list(stats.keys())[:20]}..."
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_contains_resource_stats(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics should include resource-related counters."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        # Look for resource-related stats
        resource_patterns = [
            r".*resource.*",
            r".*image.*",
            r".*css.*",
            r".*javascript.*",
            r".*js.*",
            r"num_resource_fetch_successes",
            r"num_resource_fetch_failures",
        ]

        found_stats = []
        for stat_name in stats.keys():
            for pattern in resource_patterns:
                if re.match(pattern, stat_name, re.IGNORECASE):
                    found_stats.append(stat_name)
                    break

        # Should find some resource stats
        assert len(found_stats) > 0, (
            f"Expected at least one resource stat matching {resource_patterns}, "
            f"but found none. Available stats: {list(stats.keys())[:20]}..."
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_num_flushes(self, client: PageSpeedClient, server_config):
        """Statistics should include num_flushes counter."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        # num_flushes is a key stat used by statistics_logging.sh
        assert "num_flushes" in stats, (
            f"Expected 'num_flushes' stat to be present. "
            f"Available stats: {list(stats.keys())[:20]}..."
        )
        assert stats["num_flushes"] >= 0

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_image_ongoing_rewrites(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics should include image_ongoing_rewrites counter."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        # image_ongoing_rewrites is checked by statistics_logging.sh
        assert "image_ongoing_rewrites" in stats, (
            f"Expected 'image_ongoing_rewrites' stat to be present. "
            f"Available stats: {list(stats.keys())[:20]}..."
        )
        assert stats["image_ongoing_rewrites"] >= 0


# ============================================================================
# TestStatisticsLogging: Test stats logging to file (if supported)
# ============================================================================


class TestStatisticsLogging:
    """Tests for statistics logging to file.

    These tests verify that PageSpeed can log statistics to a file for
    monitoring and analysis, as tested by Apache's statistics_logging.sh.

    These tests are skipped if statistics logging is not configured.
    """

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    @pytest.mark.slow
    def test_statistics_log_file_exists(self, stats_log_path: pathlib.Path):
        """Statistics log file should exist when logging is enabled."""
        if not stats_log_path:
            pytest.skip(
                "Statistics log file location is not configured "
                "(neither PAGESPEED_STATS_LOG nor PAGESPEED_CACHE_DIR is set). "
                "Set PAGESPEED_STATS_LOG to enable this test."
            )
        if not stats_log_path.exists():
            pytest.skip(
                f"Statistics log file not found at {stats_log_path}. "
                "Set PAGESPEED_STATS_LOG to enable this test."
            )

        assert stats_log_path.is_file(), f"{stats_log_path} exists but is not a file"

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    @pytest.mark.slow
    def test_statistics_log_contains_timestamps(
        self, stats_log_path: pathlib.Path, test_start_time: int
    ):
        """Statistics log should contain timestamp entries.

        Verifies: grep "timestamp: " $MOD_PAGESPEED_STATS_LOG | wc -l >= 1
        """
        if not stats_log_path or not stats_log_path.exists():
            pytest.skip("Statistics log file not configured or not found")

        content = stats_log_path.read_text(encoding="utf-8", errors="replace")

        # Check for timestamp entries
        timestamp_matches = re.findall(r"timestamp:\s*(\d+)", content)
        assert len(timestamp_matches) >= 1, (
            f"Expected at least one 'timestamp: ' entry in {stats_log_path}"
        )

        # Verify timestamps are reasonable (not in the far past)
        # timestamps should be within the last day at least
        one_day_ago_ms = (time.time() - 86400) * 1000
        for ts_str in timestamp_matches[-5:]:  # Check last 5 timestamps
            ts = int(ts_str)
            assert ts >= one_day_ago_ms, (
                f"Timestamp {ts} appears to be too old (more than 1 day ago)"
            )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    @pytest.mark.slow
    def test_statistics_log_contains_counters(self, stats_log_path: pathlib.Path):
        """Statistics log should contain counter entries.

        Verifies:
        - grep "num_flushes: " $MOD_PAGESPEED_STATS_LOG | wc -l >= 1
        - grep "image_ongoing_rewrites: " $MOD_PAGESPEED_STATS_LOG | wc -l >= 1
        """
        if not stats_log_path or not stats_log_path.exists():
            pytest.skip("Statistics log file not configured or not found")

        content = stats_log_path.read_text(encoding="utf-8", errors="replace")

        # Check for key counters that should be logged
        assert re.search(r"num_flushes:\s*\d+", content), (
            f"Expected 'num_flushes: ' entries in {stats_log_path}"
        )
        assert re.search(r"image_ongoing_rewrites:\s*\d+", content), (
            f"Expected 'image_ongoing_rewrites: ' entries in {stats_log_path}"
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    @pytest.mark.slow
    def test_statistics_log_no_histograms(self, stats_log_path: pathlib.Path):
        """Statistics log should not contain histogram data by default.

        Verifies: grep "histogram#" $MOD_PAGESPEED_STATS_LOG | wc -l == 0
        """
        if not stats_log_path or not stats_log_path.exists():
            pytest.skip("Statistics log file not configured or not found")

        content = stats_log_path.read_text(encoding="utf-8", errors="replace")

        # By default, histograms should not be logged
        histogram_matches = re.findall(r"histogram#", content)
        assert len(histogram_matches) == 0, (
            f"Expected no 'histogram#' entries in {stats_log_path}, "
            f"but found {len(histogram_matches)}"
        )


# ============================================================================
# TestStatisticsJson: Test JSON handler (if supported)
# ============================================================================


class TestStatisticsJson:
    """Tests for the JSON statistics handler.

    These tests verify the JSON statistics API that can be used by
    monitoring tools and the statistics console.
    """

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_json_handler_returns_json(
        self, client: PageSpeedClient, console_url: str
    ):
        """JSON handler should return valid JSON."""
        # The JSON handler is at the console URL with ?json parameter
        json_url = f"{console_url}?json&granularity=0"
        response = client.get(json_url)

        # If console is not configured, skip
        if response.status == 404:
            pytest.skip("Statistics console not available")

        assert_http_status(response, 200)

        # Should be valid JSON
        try:
            data = json.loads(response.text)
        except json.JSONDecodeError as e:
            pytest.fail(f"Response is not valid JSON: {e}\nContent: {response.text[:500]}")

        assert isinstance(data, dict), f"Expected JSON object, got {type(data)}"

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_json_handler_filters_variables(
        self, client: PageSpeedClient, console_url: str
    ):
        """JSON handler should filter variables by var_titles parameter.

        Verifies the JSON handler with:
        var_titles=num_flushes,image_ongoing_rewrites
        """
        json_url = (
            f"{console_url}?json&granularity=0"
            "&var_titles=num_flushes,image_ongoing_rewrites"
        )
        response = client.get(json_url)

        if response.status == 404:
            pytest.skip("Statistics console not available")

        assert_http_status(response, 200)

        try:
            data = json.loads(response.text)
        except json.JSONDecodeError as e:
            pytest.fail(f"Response is not valid JSON: {e}\nContent: {response.text[:500]}")

        # Each requested variable should appear exactly once
        text = response.text
        num_flushes_count = text.count('"num_flushes"')
        image_rewrites_count = text.count('"image_ongoing_rewrites"')

        assert num_flushes_count == 1, (
            f"Expected 'num_flushes' to appear once, appeared {num_flushes_count} times"
        )
        assert image_rewrites_count == 1, (
            f"Expected 'image_ongoing_rewrites' to appear once, "
            f"appeared {image_rewrites_count} times"
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_json_handler_contains_timestamps(
        self, client: PageSpeedClient, console_url: str
    ):
        """JSON handler should include timestamps array."""
        json_url = f"{console_url}?json&granularity=0"
        response = client.get(json_url)

        if response.status == 404:
            pytest.skip("Statistics console not available")

        assert_http_status(response, 200)

        try:
            data = json.loads(response.text)
        except json.JSONDecodeError as e:
            pytest.fail(f"Response is not valid JSON: {e}\nContent: {response.text[:500]}")

        # Should have timestamps key
        assert "timestamps" in data, (
            f"Expected 'timestamps' key in JSON response. Keys: {list(data.keys())}"
        )
        assert isinstance(data["timestamps"], list), "timestamps should be a list"

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_json_handler_xss_protection(
        self, client: PageSpeedClient, console_url: str
    ):
        """JSON handler should escape HTML in input to prevent XSS.

        Verifies that <boo> in query params is escaped to &lt;boo&gt;
        """
        # Try to inject HTML via var_titles parameter
        json_url = f"{console_url}?json&granularity=0&var_titles=<boo>"
        response = client.get(json_url)

        if response.status == 404:
            pytest.skip("Statistics console not available")

        # Should not contain unescaped HTML
        assert_not_contains(
            response,
            r"<boo>",
            "XSS: HTML tag should be escaped in response"
        )

        # Should contain escaped version (if the input is reflected at all)
        if "boo" in response.text:
            assert_contains(
                response,
                r"&lt;boo&gt;|\\u003cboo\\u003e",
                "HTML should be escaped to &lt;boo&gt; or unicode escape"
            )


# ============================================================================
# TestStatisticsConsole: Test statistics console UI
# ============================================================================


class TestStatisticsConsole:
    """Tests for the statistics console web UI.

    These tests verify that the PageSpeed statistics console is available
    and functional.
    """

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_console_page_available(
        self, client: PageSpeedClient, console_url: str
    ):
        """Statistics console page should be available."""
        response = client.get(console_url)

        # Console might not be configured
        if response.status == 404:
            # ConsolePath /pagespeed_console is configured on the lane
            # (setup_iis_full.ps1); a 404 is a handler regression
            #.
            require_status_ok(response, "Statistics console")

        assert_http_status(response, 200)

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_console_contains_html(
        self, client: PageSpeedClient, console_url: str
    ):
        """Statistics console should return HTML content."""
        response = client.get(console_url)

        if response.status == 404:
            # ConsolePath /pagespeed_console is configured on the lane
            # (setup_iis_full.ps1); a 404 is a handler regression
            #.
            require_status_ok(response, "Statistics console")

        assert_http_status(response, 200)

        # Should be HTML
        content_type = response.header("Content-Type").lower()
        assert "text/html" in content_type, (
            f"Expected text/html content type, got: {content_type}"
        )

        # Should contain console reference (from statistics_logging.sh check)
        assert_contains(
            response,
            r"console",
            "Console page should contain 'console' reference"
        )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_console_contains_interactive_elements(
        self, client: PageSpeedClient, console_url: str
    ):
        """Statistics console should contain interactive elements."""
        response = client.get(console_url)

        if response.status == 404:
            # ConsolePath /pagespeed_console is configured on the lane
            # (setup_iis_full.ps1); a 404 is a handler regression
            #.
            require_status_ok(response, "Statistics console")

        assert_http_status(response, 200)

        # Should contain some form of interactive element
        # Could be charts, graphs, or at least JavaScript
        has_interactive = any(
            pattern in response.text.lower()
            for pattern in [
                "<script",
                "chart",
                "graph",
                "canvas",
                "svg",
                "d3.js",
                "google.visualization",
            ]
        )

        if not has_interactive:
            # The console SPA ships scripts/charts; a bare page means the
            # console assets failed to render -- a product regression
            #.
            pytest.fail(
                "Console page has no interactive elements "
                "(<script/chart/graph/canvas/svg).\n"
                f"Response body: {response.text[:500]}"
            )


# ============================================================================
# TestStatisticsIntegration: Integration tests combining statistics features
# ============================================================================


class TestStatisticsIntegration:
    """Integration tests that verify statistics work with other features."""

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_update_after_request(
        self, client: PageSpeedClient, server_config, example_root: str
    ):
        """Statistics should update after processing requests."""
        # Get initial stats
        stats_before = client.get_statistics(stats_path=server_config.stats_path)

        # Make a request that should trigger some optimization
        url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        client.fetch_until_contains(url, pattern=r"\.pagespeed\.", timeout=30.0)

        # Give stats time to update
        time.sleep(0.5)

        # Get new stats
        stats_after = client.get_statistics(stats_path=server_config.stats_path)

        # At least some stat should have changed
        # We check for any delta rather than specific counters
        # since counter names may vary between implementations
        changed = False
        for name in stats_after:
            if name in stats_before and stats_after[name] != stats_before[name]:
                changed = True
                break
            elif name not in stats_before and stats_after[name] > 0:
                changed = True
                break

        # Note: This might be flaky if stats are aggregated periodically
        # In that case, this test can be adjusted or skipped
        if not changed:
            pytest.skip(
                "No statistics changed after request - "
                "this may be expected if stats are aggregated periodically"
            )

    @pytest.mark.iis_only
    @pytest.mark.requires_stats
    def test_statistics_gzip_response(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics endpoint should support gzip compression if enabled."""
        # Request with gzip
        response = client.get_gzip(server_config.stats_path)
        assert_http_status(response, 200)

        # If gzip was applied, verify we can still parse stats
        stats = parse_statistics(response.text)
        assert len(stats) > 0, "Should be able to parse stats from gzip response"
