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

"""nginx PageSpeed module sanity tests.

These tests verify basic PageSpeed functionality on nginx. They serve as
initial sanity checks to ensure the module is properly loaded and working.

Test classes:
- TestNginxHtmlRewriting: Verifies HTML pages are rewritten
- TestNginxResources: Verifies optimized resources are served correctly
- TestNginxStatistics: Verifies statistics endpoint functionality
- TestNginxAdmin: Verifies admin endpoint functionality
- TestNginxCacheHeaders: Verifies cache headers on optimized resources
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
    require_no_auth_gate,
    require_status_ok,
)


# ============================================================================
# TestNginxHtmlRewriting: HTML rewriting tests
# ============================================================================


class TestNginxHtmlRewriting:
    """Tests for HTML rewriting functionality.

    These tests verify that the PageSpeed module correctly rewrites HTML
    pages to include optimized resource URLs.
    """

    @pytest.mark.nginx_only
    def test_html_rewriting(self, client: PageSpeedClient, example_root: str):
        """Verify HTML pages are rewritten with .pagespeed. URLs.

        The module should rewrite HTML pages to reference optimized resources
        with .pagespeed. patterns in their URLs (e.g., .pagespeed.ce. for
        cache-extended resources).
        """
        # Use extend_cache filter which produces predictable .pagespeed. URLs
        url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"

        # Poll until rewriting completes (async optimization)
        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.",
            timeout=60.0,
        )
        assert_http_status(response, 200)
        assert_contains(
            response,
            r"\.pagespeed\.",
            "HTML should contain rewritten .pagespeed. URLs"
        )

    @pytest.mark.nginx_only
    def test_html_rewriting_with_combine_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify CSS combining produces rewritten URLs."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        # Poll until CSS combining completes
        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.cc\.",
            timeout=60.0,
        )
        assert_http_status(response, 200)
        assert_contains(
            response,
            r"\.pagespeed\.cc\.",
            "HTML should contain combined CSS .pagespeed.cc. URLs"
        )

    @pytest.mark.nginx_only
    def test_pagespeed_off_disables_rewriting(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off query param should disable all optimization."""
        response = client.get(f"{example_root}/combine_css.html?PageSpeed=off")

        assert_http_status(response, 200)
        assert_not_contains(
            response,
            r"\.pagespeed\.",
            "PageSpeed=off should disable rewriting"
        )


# ============================================================================
# TestNginxResources: Optimized resource serving tests
# ============================================================================


class TestNginxResources:
    """Tests for serving optimized PageSpeed resources.

    These tests verify that .pagespeed. URLs return valid optimized content
    with correct headers.
    """

    @pytest.mark.nginx_only
    def test_pagespeed_resource_served(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify optimized resources are served with correct content-type.

        First fetch an HTML page to generate optimized resource URLs,
        then fetch one of those resources directly.
        """
        # First, get HTML with rewritten URLs
        html_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"
        html_response = client.fetch_until_contains(
            html_url,
            pattern=r"\.pagespeed\.cc\.",
            timeout=60.0,
        )
        assert_http_status(html_response, 200)

        # Extract a .pagespeed. URL from the HTML
        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', html_response.text)
        if not match:
            # Try single quotes
            match = re.search(r"href='([^']*\.pagespeed\.cc\.[^']*)'", html_response.text)

        assert match, "Should find a .pagespeed.cc. CSS URL in HTML"

        # Fetch the optimized resource
        resource_url = match.group(1)
        # Handle relative URLs
        if resource_url.startswith('/'):
            resource_response = client.get(resource_url)
        else:
            resource_response = client.get(f"{example_root}/{resource_url}")

        assert_http_status(resource_response, 200)

        # Check content-type is CSS
        content_type = resource_response.header("Content-Type").lower()
        assert "text/css" in content_type, \
            f"Expected CSS content-type, got: {content_type}"

    @pytest.mark.nginx_only
    def test_pagespeed_js_resource_served(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify optimized JavaScript resources are served correctly."""
        # Get HTML with rewritten JS URLs
        html_url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"
        html_response = client.fetch_until_contains(
            html_url,
            pattern=r"\.pagespeed\.jc\.",
            timeout=60.0,
        )
        assert_http_status(html_response, 200)

        # Extract a .pagespeed.jc. URL from the HTML
        match = re.search(r'src="([^"]*\.pagespeed\.jc\.[^"]*)"', html_response.text)
        if not match:
            match = re.search(r"src='([^']*\.pagespeed\.jc\.[^']*)'", html_response.text)

        assert match, "Should find a .pagespeed.jc. JS URL in HTML"

        # Fetch the optimized resource
        resource_url = match.group(1)
        if resource_url.startswith('/'):
            resource_response = client.get(resource_url)
        else:
            resource_response = client.get(f"{example_root}/{resource_url}")

        assert_http_status(resource_response, 200)

        # Check content-type is JavaScript
        content_type = resource_response.header("Content-Type").lower()
        assert "javascript" in content_type or "application/x-javascript" in content_type, \
            f"Expected JavaScript content-type, got: {content_type}"


# ============================================================================
# TestNginxStatistics: Statistics endpoint tests
# ============================================================================


class TestNginxStatistics:
    """Tests for the statistics endpoint.

    These tests verify that /pagespeed_statistics returns valid statistics
    data that can be used for monitoring PageSpeed performance.
    """

    @pytest.mark.nginx_only
    @pytest.mark.requires_stats
    def test_statistics_endpoint(self, client: PageSpeedClient, server_config):
        """Verify /pagespeed_statistics works and contains expected stats.

        The statistics endpoint should return a parseable response containing
        statistic names and their values.

        Note: Unlike Apache, nginx admin endpoints don't work with ?PageSpeed=off
        because the module declines to handle any requests when disabled.
        """
        response = client.get(server_config.stats_path)
        assert_http_status(response, 200)

        # Should contain parseable statistics (handles both JSON and text formats)
        stats = client.get_statistics(
            stats_path=server_config.stats_path, disable_pagespeed=False
        )
        assert len(stats) > 0, "Statistics endpoint should return parseable stats"

    @pytest.mark.nginx_only
    @pytest.mark.requires_stats
    def test_statistics_contains_common_stats(
        self, client: PageSpeedClient, server_config
    ):
        """Verify statistics contain common PageSpeed stat names."""
        # nginx admin endpoints don't work with ?PageSpeed=off, so disable it
        stats = client.get_statistics(
            stats_path=server_config.stats_path, disable_pagespeed=False
        )

        # Should have at least some statistics
        assert len(stats) > 0, "Should have some statistics"

        # Check for some common stat names that should be present
        common_stats = [
            "num_rewrites_executed",
            "num_rewrites_dropped",
            "cache_hits",
            "cache_misses",
        ]

        found_stats = [s for s in common_stats if s in stats]
        assert len(found_stats) > 0, \
            f"Expected some common stats, got: {list(stats.keys())[:10]}"

    @pytest.mark.nginx_only
    @pytest.mark.requires_stats
    def test_statistics_parseable(self, client: PageSpeedClient, server_config):
        """Verify statistics can be parsed programmatically."""
        # nginx admin endpoints don't work with ?PageSpeed=off, so disable it
        stats = client.get_statistics(
            stats_path=server_config.stats_path, disable_pagespeed=False
        )

        # All values should be integers
        for name, value in stats.items():
            assert isinstance(value, int), \
                f"Stat {name} should be an integer, got: {type(value)}"


# ============================================================================
# TestNginxAdmin: Admin endpoint tests
# ============================================================================


class TestNginxAdmin:
    """Tests for the admin endpoint.

    These tests verify that /pagespeed_admin returns valid HTML content
    with administrative information about the PageSpeed module.
    """

    @pytest.mark.nginx_only
    def test_admin_endpoint(self, client: PageSpeedClient, server_config):
        """Verify /pagespeed_admin works and returns HTML.

        The admin endpoint should return an HTML page with administrative
        controls and status information.
        """
        response = client.get(server_config.admin_path)

        # The lane runs the admin endpoint without auth; a 403 from
        # the admin handler is a plausible regression, not an
        # environment condition.
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")

        assert response.status in (200, 301, 302), \
            f"Admin endpoint should respond, got status {response.status}"

        if response.status == 200:
            # Check content-type is HTML
            content_type = response.header("Content-Type").lower()
            assert "text/html" in content_type, \
                f"Admin should return HTML, got: {content_type}"

    @pytest.mark.nginx_only
    def test_admin_statistics_subpage(
        self, client: PageSpeedClient, server_config
    ):
        """Verify admin statistics sub-page works."""
        response = client.get(f"{server_config.admin_path}/statistics")

        # The lane runs the admin endpoint without auth; a 403 from
        # the admin handler is a plausible regression, not an
        # environment condition.
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")

        # The nginx lane configures AdminPath /pagespeed_admin, so a missing
        # statistics sub-page is a handler regression, not an environment
        # condition.
        require_status_ok(response, "Admin statistics page")
        assert_contains(
            response,
            r"Statistics|statistics",
            "Admin statistics page should mention 'Statistics'"
        )

    @pytest.mark.nginx_only
    def test_admin_config_subpage(self, client: PageSpeedClient, server_config):
        """Verify admin config sub-page works."""
        response = client.get(f"{server_config.admin_path}/config")

        # The lane runs the admin endpoint without auth; a 403 from
        # the admin handler is a plausible regression, not an
        # environment condition.
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")

        # The nginx lane configures AdminPath /pagespeed_admin, so a missing
        # config sub-page is a handler regression, not an environment
        # condition.
        require_status_ok(response, "Admin config page")
        assert_contains(
            response,
            r"Configuration|Config|config",
            "Admin config page should mention configuration"
        )

    @pytest.mark.nginx_only
    def test_admin_cache_subpage(self, client: PageSpeedClient, server_config):
        """Verify admin cache sub-page works."""
        response = client.get(f"{server_config.admin_path}/cache")

        # The lane runs the admin endpoint without auth; a 403 from
        # the admin handler is a plausible regression, not an
        # environment condition.
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")

        # The nginx lane configures AdminPath /pagespeed_admin, so a missing
        # cache sub-page is a handler regression, not an environment
        # condition.
        require_status_ok(response, "Admin cache page")
        assert_contains(
            response,
            r"Cache|cache",
            "Admin cache page should mention cache"
        )


# ============================================================================
# TestNginxCacheHeaders: Cache header tests
# ============================================================================


class TestNginxCacheHeaders:
    """Tests for cache headers on optimized resources.

    These tests verify that optimized .pagespeed. resources have appropriate
    cache headers set for long-term caching.
    """

    @pytest.mark.nginx_only
    def test_cache_headers_on_optimized_resource(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify Cache-Control header has long max-age on optimized resources.

        Optimized resources with content hashes in their URLs should be
        cached for a long time (typically 1 year = 31536000 seconds).
        """
        # First, get HTML with rewritten URLs
        html_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"
        html_response = client.fetch_until_contains(
            html_url,
            pattern=r"\.pagespeed\.cc\.",
            timeout=60.0,
        )
        assert_http_status(html_response, 200)

        # Extract a .pagespeed. URL
        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', html_response.text)
        if not match:
            match = re.search(r"href='([^']*\.pagespeed\.cc\.[^']*)'", html_response.text)

        assert match, "Should find a .pagespeed.cc. CSS URL in HTML"

        # Fetch the optimized resource
        resource_url = match.group(1)
        if resource_url.startswith('/'):
            resource_response = client.get(resource_url)
        else:
            resource_response = client.get(f"{example_root}/{resource_url}")

        assert_http_status(resource_response, 200)

        # Check Cache-Control header
        cache_control = resource_response.header("Cache-Control")
        assert cache_control, "Optimized resource should have Cache-Control header"

        # Should have a long max-age (at least 1 year is common)
        # Look for max-age=NNNNNNNN pattern
        max_age_match = re.search(r"max-age=(\d+)", cache_control)
        assert max_age_match, \
            f"Cache-Control should include max-age, got: {cache_control}"

        max_age = int(max_age_match.group(1))
        # PageSpeed typically sets max-age to 1 year (31536000 seconds)
        # We'll accept anything over 1 week (604800) as "long-term"
        assert max_age >= 604800, \
            f"Expected long max-age (>= 1 week), got: {max_age}"

    @pytest.mark.nginx_only
    def test_cache_headers_present(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify optimized resources have Cache-Control headers."""
        # First, get HTML with rewritten URLs
        html_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        html_response = client.fetch_until_contains(
            html_url,
            pattern=r"\.pagespeed\.ce\.",
            timeout=60.0,
        )
        assert_http_status(html_response, 200)

        # Extract a .pagespeed. image URL
        match = re.search(r'src="([^"]*\.pagespeed\.ce\.[^"]*)"', html_response.text)
        if not match:
            match = re.search(r"src='([^']*\.pagespeed\.ce\.[^']*)'", html_response.text)

        assert match, "Should find a .pagespeed.ce. image URL in HTML"

        # Fetch the optimized resource
        resource_url = match.group(1)
        if resource_url.startswith('/'):
            resource_response = client.get(resource_url)
        else:
            resource_response = client.get(f"{example_root}/{resource_url}")

        assert_http_status(resource_response, 200)

        # Check Cache-Control header - note 'public' is only added when
        # input resources explicitly have 'public' (per PageSpeed design)
        cache_control = resource_response.header("Cache-Control")
        assert cache_control, "Optimized resource should have Cache-Control header"
        assert "max-age" in cache_control.lower(), \
            f"Cache-Control should include 'max-age', got: {cache_control}"


# ============================================================================
# TestNginxBasicConnectivity: Basic connectivity tests
# ============================================================================


class TestNginxBasicConnectivity:
    """Basic connectivity tests for nginx PageSpeed module.

    These tests verify fundamental server behavior before running more
    complex optimization tests.
    """

    @pytest.mark.nginx_only
    def test_server_responds(self, client: PageSpeedClient, example_root: str):
        """Verify the server is responding to requests."""
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

    @pytest.mark.nginx_only
    def test_404_for_missing_page(self, client: PageSpeedClient, example_root: str):
        """Non-existent pages should return 404."""
        response = client.get(f"{example_root}/this_page_does_not_exist_12345.html")
        assert_http_status(response, 404)

    @pytest.mark.nginx_only
    def test_html_content_type(self, client: PageSpeedClient, example_root: str):
        """HTML pages should return text/html content type."""
        response = client.get(f"{example_root}/index.html?PageSpeed=off")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type").lower()
        assert "text/html" in content_type, \
            f"Expected text/html, got: {content_type}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
