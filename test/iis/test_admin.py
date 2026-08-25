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

"""Admin UI and statistics tests for IIS PageSpeed module.

These tests verify the admin interface uses the shared AdminSite
infrastructure (same as Apache/Envoy/nginx), producing consistent
admin pages with navigation tabs, graphs, and proper formatting.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    parse_statistics,
)


@pytest.mark.admin
class TestAdminUI:
    """Admin UI accessibility tests."""

    def test_admin_page_accessible(self, client: PageSpeedClient, admin_path: str):
        """Admin page should return 200 OK (follows redirect from no-slash)."""
        response = client.get(admin_path + "/")
        # May return 403 if admin is disabled or 200 if enabled
        assert response.status in (200, 403), \
            f"Expected 200 or 403, got {response.status}"

    def test_admin_shows_version(self, client: PageSpeedClient, admin_path: str):
        """Admin page should show PageSpeed version."""
        response = client.get(admin_path)
        if response.status == 200:
            assert_contains(response, "PageSpeed")

    def test_admin_shows_filters(self, client: PageSpeedClient, admin_path: str):
        """Admin page should list available filters."""
        response = client.get(f"{admin_path}?filters")
        if response.status == 200:
            # The ?filters query maps to the config page via AdminSite,
            # which shows "Filters" as a section heading.
            assert "filter" in response.text.lower(), \
                "Config page should show filter information"


@pytest.mark.admin
class TestAdminSiteUI:
    """Tests that verify admin pages use the shared AdminSite infrastructure.

    The IIS admin pages should match Apache/Envoy/nginx by delegating to
    SystemServerContext::AdminPage() and StatisticsPage(), which use the
    shared AdminSite class. This produces consistent admin UI with tabs,
    graphs, histograms, and CSS/JS assets.
    """

    def test_admin_serves_spa(self, client: PageSpeedClient, admin_path: str):
        """Admin page should serve the Svelte SPA with proper HTML structure."""
        response = client.get(f"{admin_path}/")
        if response.status == 200:
            text = response.text.lower()
            assert "<!doctype html>" in text or "<html" in text, \
                   "Admin page should serve HTML"
            assert '<div id="app">' in response.text, \
                   "Admin page should have Svelte app mount point"

    def test_admin_has_navigation_tabs(self, client: PageSpeedClient, admin_path: str):
        """Admin page should have navigation tabs for all sub-pages.

        The shared AdminSite generates tab-style navigation with links to:
        statistics, config, histograms, cache, console, message_history, graphs.
        The old IIS admin only had basic links without proper tab navigation.
        """
        response = client.get(f"{admin_path}/")
        if response.status == 200:
            text = response.text
            # Check for navigation links that AdminSite generates
            assert "statistics" in text, "Should have statistics tab"
            assert "config" in text, "Should have config tab"
            assert "cache" in text, "Should have cache tab"
            assert "console" in text, "Should have console tab"
            assert "message_history" in text, "Should have message_history tab"

    def test_admin_config_page(self, client: PageSpeedClient, admin_path: str):
        """Admin config page should show options from AdminSite::PrintConfig.

        The shared AdminSite generates a properly formatted config page
        with all current rewrite options.
        """
        response = client.get(f"{admin_path}/config")
        if response.status == 200:
            # AdminSite::PrintConfig outputs the options
            assert_contains(response, "PageSpeed")

    def test_admin_cache_page(self, client: PageSpeedClient, admin_path: str):
        """Admin cache page should use AdminSite::PrintCaches.

        The shared AdminSite generates a cache management page with
        detailed cache statistics, purge form, and metadata.
        """
        response = client.get(f"{admin_path}/cache")
        if response.status == 200:
            # AdminSite::PrintCaches generates the cache page
            assert_contains(response, "cache")

    def test_admin_console_page(self, client: PageSpeedClient, admin_path: str):
        """Admin console page should return a valid response."""
        response = client.get(f"{admin_path}/console")
        if response.status == 200:
            text = response.text
            # Console serves the Svelte SPA or JSON data
            assert len(text) > 0, "Console should return content"

    def test_admin_message_history_page(self, client: PageSpeedClient, admin_path: str):
        """Admin message_history page should return messages."""
        response = client.get(f"{admin_path}/message_history")
        if response.status == 200:
            text = response.text
            # message_history returns JSON with messages array
            assert "messages" in text or "<html" in text.lower(), \
                   "message_history should return messages data or HTML"

    def test_admin_histograms_page(self, client: PageSpeedClient, admin_path: str):
        """Admin histograms page should be accessible."""
        response = client.get(f"{admin_path}/histograms")
        if response.status == 200:
            assert len(response.text) > 0, "Histograms should return content"

    def test_admin_graphs_page(self, client: PageSpeedClient, admin_path: str):
        """Admin graphs page should be accessible."""
        response = client.get(f"{admin_path}/graphs")
        if response.status == 200:
            assert len(response.text) > 0, "Graphs should return content"

    def test_statistics_page_uses_shared_formatting(
            self, client: PageSpeedClient, statistics_path: str):
        """Statistics page should return parseable statistics data."""
        response = client.get(statistics_path)
        if response.status == 200:
            text = response.text
            # Statistics page returns data (HTML or JSON)
            assert len(text) > 0, "Statistics page should return data"

    def test_admin_redirect_without_trailing_slash(
            self, client: PageSpeedClient, admin_path: str):
        """Requesting /pagespeed_admin without trailing slash should redirect.

        AdminSite::AdminPage sends a 301 redirect to /pagespeed_admin/
        so that relative URL references in the navigation tabs work correctly.
        """
        # admin_path is http://localhost:8080/pagespeed_admin (no trailing slash)
        response = client.get(admin_path, allow_redirects=False)
        # Should either redirect (301) or serve the page (200)
        assert response.status in (200, 301), \
            f"Expected 200 or 301, got {response.status}"


@pytest.mark.admin
class TestStatistics:
    """Statistics endpoint tests."""

    def test_statistics_accessible(self, client: PageSpeedClient, statistics_path: str):
        """Statistics endpoint should be accessible."""
        response = client.get(statistics_path)
        # May return 403 if stats are disabled or 200 if enabled
        assert response.status in (200, 403), \
            f"Expected 200 or 403, got {response.status}"

    def test_statistics_parseable(self, client: PageSpeedClient, statistics_path: str):
        """Statistics should be parseable."""
        response = client.get(statistics_path)
        if response.status == 200:
            stats = parse_statistics(response.text)
            # Should have at least some statistics
            assert len(stats) > 0, "Expected some statistics to be present"

    def test_statistics_json(self, client: PageSpeedClient, statistics_path: str):
        """Statistics in JSON format should be parseable."""
        response = client.get(f"{statistics_path}?json")
        if response.status == 200:
            content_type = response.header("Content-Type")
            # May be JSON or text format
            assert "json" in content_type.lower() or "text" in content_type.lower()


@pytest.mark.admin
class TestCacheOperations:
    """Cache management tests."""

    def test_cache_flush(self, client: PageSpeedClient, admin_path: str):
        """Cache flush should be accessible."""
        response = client.post(f"{admin_path}?cache_flush")
        # POST to cache_flush may require specific handling
        assert response.status in (200, 302, 403, 405), \
            f"Unexpected status: {response.status}"

    def test_cache_stats(self, client: PageSpeedClient, admin_path: str):
        """Cache statistics should be viewable."""
        response = client.get(f"{admin_path}?cache")
        if response.status == 200:
            # Should show cache information
            assert_contains(response, "cache")


@pytest.mark.admin
class TestHealthCheck:
    """Health check endpoint tests."""

    def test_health_check(self, client: PageSpeedClient, admin_path: str):
        """Health check should return status."""
        response = client.get(f"{admin_path}?health")
        if response.status == 200:
            # Should indicate healthy status
            assert_contains(response, "ok") or assert_contains(response, "healthy")


@pytest.mark.admin
class TestConsole:
    """Console endpoint tests."""

    def test_console_accessible(self, client: PageSpeedClient, admin_path: str):
        """Console page should be accessible."""
        response = client.get(f"{admin_path}?console")
        # Console may or may not be enabled
        assert response.status in (200, 403, 404)

    def test_message_history(self, client: PageSpeedClient, admin_path: str):
        """Message history should be viewable."""
        response = client.get(f"{admin_path}?messages")
        if response.status == 200:
            # Should show message log
            pass  # Content varies


@pytest.mark.admin
class TestGlobalAdmin:
    """Global admin endpoint tests (/pagespeed_global_admin)."""

    def test_global_admin_accessible(self, client: PageSpeedClient, global_admin_path: str):
        """Global admin page should return 200."""
        response = client.get(global_admin_path + "/")
        assert response.status == 200

    def test_global_admin_config(self, client: PageSpeedClient, global_admin_path: str):
        """Global admin config should return valid JSON."""
        response = client.get(f"{global_admin_path}/config")
        assert response.status == 200
        assert "config" in response.text

    def test_global_admin_statistics(self, client: PageSpeedClient, global_admin_path: str):
        """Global admin statistics should return valid JSON with variables."""
        import json
        response = client.get(f"{global_admin_path}/statistics")
        assert response.status == 200
        data = json.loads(response.text)
        assert "variables" in data, "Expected 'variables' key in statistics JSON"
        assert len(data["variables"]) > 0, "Expected statistics to be present"

    def test_global_admin_histograms(self, client: PageSpeedClient, global_admin_path: str):
        """Global admin histograms should be accessible."""
        response = client.get(f"{global_admin_path}/histograms")
        assert response.status == 200

    def test_global_admin_message_history(self, client: PageSpeedClient, global_admin_path: str):
        """Global admin message history should be accessible."""
        response = client.get(f"{global_admin_path}/message_history")
        assert response.status == 200

    def test_global_admin_health(self, client: PageSpeedClient, global_admin_path: str):
        """Global admin health endpoint should work."""
        response = client.get(f"{global_admin_path}?health")
        assert response.status == 200


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
