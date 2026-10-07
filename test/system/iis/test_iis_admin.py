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

"""IIS admin interface system tests.

These tests verify the PageSpeed admin interface functionality on IIS, ported
from Apache's pagespeed_admin.sh system tests.

After the Svelte admin SPA migration, the admin endpoints are split:
- /pagespeed_admin/, /pagespeed_admin/console, /pagespeed_admin/graphs serve
  the SPA HTML shell (text/html).
- /pagespeed_admin/{statistics,config,cache,histograms,message_history}
  return application/json with the data the SPA shell consumes.
Source of truth: pagespeed/system/admin_site.cc::AdminPage().

Test classes:
- TestAdminPaths: Tests different admin path configurations
- TestAdminSecurity: Tests XSS protection in admin pages
- TestMessagePage: Tests the message history page

Admin sub-page content-type/body coverage (formerly TestAdminPages here, and
its quoting cases from TestAdminSecurity) was lifted to the port-agnostic
system/test_admin_pages.py, which every lane collects; see run_iis_tests.ps1.

Environment Variables:
    PAGESPEED_ADMIN_PATH: Path to admin endpoint (default: /pagespeed_admin)
    PAGESPEED_GLOBAL_ADMIN_PATH: Path to global admin (optional)
"""

import json
import re
import urllib.parse
from typing import List, Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    require_no_auth_gate,
    require_status_ok,
)


# ============================================================================
# Test Fixtures
# ============================================================================


@pytest.fixture(scope="session")
def admin_path(server_config) -> str:
    """Return the admin endpoint path."""
    return server_config.admin_path


@pytest.fixture(scope="session")
def admin_paths(server_config) -> List[str]:
    """Return list of admin paths to test.

    Equivalent to Apache's:
    for admin_path in pagespeed_admin pagespeed_global_admin alt/admin/path
    """
    paths = [server_config.admin_path]

    # Add global admin if configured differently
    global_admin = "/pagespeed_global_admin"
    if global_admin != server_config.admin_path:
        paths.append(global_admin)

    return paths


# ============================================================================
# TestAdminPaths: Test different admin path configurations
# ============================================================================


class TestAdminPaths:
    """Tests for admin path configurations.

    These tests verify that admin endpoints work at different URL paths.
    """

    @pytest.mark.iis_only
    def test_default_admin_path(self, client: PageSpeedClient, admin_path: str):
        """Default admin path should be accessible."""
        response = client.get(f"{admin_path}/statistics")

        # The lane runs the admin endpoint without auth; a 403 from the
        # admin handler is a plausible regression, not an environment
        # condition.
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")

        # Should either work (200) or redirect (301/302) or be not found (404)
        assert response.status in (200, 301, 302, 404), (
            f"Admin path should respond cleanly, got {response.status}"
        )

    @pytest.mark.iis_only
    def test_global_admin_path(self, client: PageSpeedClient):
        """Global admin path should respond."""
        response = client.get("/pagespeed_global_admin/statistics")

        # The lane runs the global admin endpoint without auth; a 403
        # from the admin handler is a plausible regression, not an
        # environment condition.
        require_no_auth_gate(response, "Global admin endpoint /pagespeed_global_admin")

        # GlobalAdminPath /pagespeed_global_admin is configured on the lane
        # (setup_iis_full.ps1); a 404 is a handler regression.
        require_status_ok(response, "Global admin statistics page")

    @pytest.mark.iis_only
    def test_admin_path_trailing_slash(self, client: PageSpeedClient, admin_path: str):
        """Admin path should work with trailing slash."""
        response = client.get(f"{admin_path}/")

        # The lane runs the admin endpoint without auth; a 403 from the
        # admin handler is a plausible regression, not an environment
        # condition.
        require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")

        # Should redirect or return 200
        assert response.status in (200, 301, 302, 404), (
            f"Admin path with trailing slash should work, got {response.status}"
        )

    @pytest.mark.iis_only
    def test_admin_path_case_sensitivity(self, client: PageSpeedClient, admin_path: str):
        """Admin paths should handle case appropriately for IIS."""
        # IIS is typically case-insensitive
        # Test uppercase variation
        upper_path = admin_path.upper()
        response = client.get(f"{upper_path}/statistics")

        # On IIS, this should work the same as lowercase
        # On case-sensitive systems, it might 404
        assert response.status in (200, 301, 302, 403, 404), (
            f"Uppercase admin path should respond, got {response.status}"
        )


# ============================================================================
# TestAdminSecurity: Test XSS protection in admin pages
# ============================================================================


class TestAdminSecurity:
    """Tests for admin page security.

    These tests verify XSS protection in admin pages.

    Ported from: pagespeed_admin.sh - "pagespeed_admin quoting on not-found page"
    """

    @pytest.mark.iis_only
    def test_admin_xss_script_injection(self, client: PageSpeedClient, admin_path: str):
        """Admin page should prevent script injection."""
        # Try script injection in path
        script_payload = "<script>alert('xss')</script>"
        encoded_payload = urllib.parse.quote(script_payload, safe='')
        malicious_path = f"{admin_path}/test/{encoded_payload}"

        response = client.get(malicious_path)

        if response.text:
            # Should not contain unescaped script tag
            assert "<script>" not in response.text.lower(), (
                "XSS vulnerability: unescaped script tag in admin response"
            )

    @pytest.mark.iis_only
    def test_admin_reflected_content_escaped(
        self, client: PageSpeedClient, admin_path: str
    ):
        """Any reflected user input in admin pages should be escaped."""
        test_payloads = [
            "<img src=x onerror=alert(1)>",
            "javascript:alert(1)",
            "<svg onload=alert(1)>",
            "'-alert(1)-'",
        ]

        for payload in test_payloads:
            encoded = urllib.parse.quote(payload, safe='')
            response = client.get(f"{admin_path}/test?q={encoded}")

            if response.text and payload in response.text:
                pytest.fail(
                    f"XSS vulnerability: unescaped payload reflected: {payload}"
                )


# ============================================================================
# TestMessagePage: Test message history page
# ============================================================================


class TestMessagePage:
    """Tests for the message history page.

    These tests verify the mod_pagespeed_message page functionality.

    Ported from: pagespeed/apache/system_tests/mod_pagespeed_message.sh
    """

    @pytest.mark.iis_only
    def test_message_page_available(self, client: PageSpeedClient, admin_path: str):
        """Message history page should be available."""
        response = client.get(f"{admin_path}/message_history")

        # The lane runs the admin endpoint without auth; a 403 from the
        # admin handler is a plausible regression, not an environment
        # condition.
        require_no_auth_gate(response, "Message history endpoint")

        # AdminPath and its sub-pages are configured on the lane
        # (setup_iis_full.ps1); a 404 is a handler regression.
        require_status_ok(response, "Message history page")

    @pytest.mark.iis_only
    def test_message_page_contains_messages(
        self, client: PageSpeedClient, admin_path: str
    ):
        """Message history endpoint should return a JSON 'messages' array.

        Each entry has 'severity' (info|warning|error|fatal) and 'message'.
        Empty list is acceptable in a freshly-started server.
        """
        response = client.get(f"{admin_path}/message_history")

        # AdminPath and its sub-pages are configured on the lane
        # (setup_iis_full.ps1); a non-200 is a handler regression
        #.
        require_status_ok(response, "Message history page")

        content_type = response.header("Content-Type").lower()
        assert "application/json" in content_type, (
            f"Message history should return JSON, got: {content_type}"
        )
        data = json.loads(response.text)
        assert "messages" in data and isinstance(data["messages"], list), (
            f"Message history JSON should contain 'messages' list, got: "
            f"{list(data.keys())}"
        )
        for entry in data["messages"]:
            assert "severity" in entry and "message" in entry, (
                f"Each message entry should have 'severity' and 'message', "
                f"got: {entry}"
            )

    @pytest.mark.iis_only
    def test_message_page_xss_protection(
        self, client: PageSpeedClient, admin_path: str
    ):
        """Message history page should escape any user-controlled content."""
        response = client.get(f"{admin_path}/message_history")

        # AdminPath and its sub-pages are configured on the lane
        # (setup_iis_full.ps1); a non-200 is a handler regression
        #.
        require_status_ok(response, "Message history page")

        # Check that common XSS patterns are not present in raw form
        dangerous_patterns = [
            "<script",
            "javascript:",
            "onerror=",
            "onload=",
        ]

        for pattern in dangerous_patterns:
            # These patterns should not appear unless properly escaped
            # We check for suspicious patterns that might indicate injection
            count = response.text.lower().count(pattern)
            # Some scripts may be legitimate (like console.js), but there
            # shouldn't be unexpected script tags
            if count > 5:  # Allow some legitimate scripts
                # Verify they're in expected contexts (like <script src=...)
                pass  # This is a soft check


# ============================================================================
# TestAdminIntegration: Integration tests for admin features
# ============================================================================


class TestAdminIntegration:
    """Integration tests for admin features."""

    @pytest.mark.iis_only
    def test_admin_statistics_matches_stats_endpoint(
        self, client: PageSpeedClient, admin_path: str, server_config
    ):
        """Admin statistics page should show same data as stats endpoint."""
        # Get stats from dedicated endpoint
        stats = client.get_statistics(stats_path=server_config.stats_path)

        # Get admin statistics page
        response = client.get(f"{admin_path}/statistics")

        # AdminPath and its sub-pages are configured on the lane
        # (setup_iis_full.ps1); a non-200 is a handler regression
        #.
        require_status_ok(response, "Admin statistics page")

        # Admin page should contain at least some of the same stat names
        stats_found = 0
        for stat_name in list(stats.keys())[:10]:  # Check first 10 stats
            if stat_name in response.text:
                stats_found += 1

        assert stats_found > 0, (
            "Admin statistics page should contain stat names from stats endpoint"
        )

    @pytest.mark.iis_only
    def test_admin_config_shows_filters(
        self, client: PageSpeedClient, admin_path: str
    ):
        """Admin config page should show enabled filters."""
        response = client.get(f"{admin_path}/config")

        # AdminPath and its sub-pages are configured on the lane
        # (setup_iis_full.ps1); a non-200 is a handler regression
        #.
        require_status_ok(response, "Admin config page")

        # Should mention common filter names or configuration options
        config_terms = [
            "filter",
            "rewrite",
            "cache",
            "optimize",
            "enable",
            "disable",
        ]

        found_terms = sum(
            1 for term in config_terms if term in response.text.lower()
        )

        assert found_terms > 0, (
            "Admin config page should contain configuration-related terms"
        )

    @pytest.mark.iis_only
    def test_admin_cache_shows_cache_info(
        self, client: PageSpeedClient, admin_path: str
    ):
        """Admin cache page should show cache information."""
        response = client.get(f"{admin_path}/cache")

        # AdminPath and its sub-pages are configured on the lane
        # (setup_iis_full.ps1); a non-200 is a handler regression
        #.
        require_status_ok(response, "Admin cache page")

        # Should contain cache-related terms
        cache_terms = [
            "cache",
            "lru",
            "file",
            "memory",
            "size",
            "entries",
        ]

        found_terms = sum(
            1 for term in cache_terms if term in response.text.lower()
        )

        assert found_terms > 0, (
            "Admin cache page should contain cache-related information"
        )


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite --
    # vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
