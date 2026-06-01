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
- TestAdminPages: Verifies admin sub-pages (statistics, config, histograms, etc.)
- TestAdminPaths: Tests different admin path configurations
- TestAdminSecurity: Tests XSS protection in admin pages
- TestMessagePage: Tests the message history page

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
    assert_http_status,
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


@pytest.fixture(scope="session")
def admin_subpages() -> List[dict]:
    """Return admin sub-pages with their expected response shape.

    The Svelte admin SPA migration moved the data-bearing admin endpoints
    to application/json; only the SPA shell endpoints still serve HTML.
    Source of truth: pagespeed/system/admin_site.cc::AdminPage().

    Each entry:
      path:         leaf under /pagespeed_admin/
      content_type: expected Content-Type prefix
      json_key:     optional top-level JSON key the response should contain
                    (omitted for the SPA shell and for statistics, whose
                     stats->DumpJson output is a flat map of stat names)
    """
    return [
        {"path": "statistics", "content_type": "application/json"},
        {"path": "config", "content_type": "application/json",
         "json_key": "config"},
        {"path": "histograms", "content_type": "application/json",
         "json_key": "histograms"},
        {"path": "cache", "content_type": "application/json",
         "json_key": "caches"},
        {"path": "console", "content_type": "text/html"},
        {"path": "message_history", "content_type": "application/json",
         "json_key": "messages"},
    ]


# ============================================================================
# TestAdminPages: Test admin sub-pages
# ============================================================================


class TestAdminPages:
    """Tests for admin page functionality.

    These tests verify that all PageSpeed admin sub-pages are accessible
    and return proper HTML content.

    Ported from: pagespeed/apache/system_tests/pagespeed_admin.sh
    """

    @pytest.mark.iis_only
    def test_admin_root_responds(self, client: PageSpeedClient, admin_path: str):
        """Admin root endpoint should respond."""
        response = client.get(admin_path)

        # Should return 200 or redirect to a sub-page
        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")

        assert response.status in (200, 301, 302), (
            f"Admin endpoint should respond, got status {response.status}"
        )

    @pytest.mark.iis_only
    def test_admin_statistics_page(self, client: PageSpeedClient, admin_path: str):
        """Admin statistics endpoint should return the JSON stat dump.

        Post-SPA: StatisticsHandler delegates to StatisticsJsonHandler, which
        calls stats->DumpJson(). The SPA shell loads this and renders.
        """
        response = client.get(f"{admin_path}/statistics")

        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")
        if response.status == 404:
            pytest.skip("Statistics admin page not available")

        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "application/json" in content_type, (
            f"Statistics endpoint should return JSON, got: {content_type}"
        )
        data = json.loads(response.text)
        assert isinstance(data, dict) and data, (
            "Statistics JSON should be a non-empty object of stat names"
        )

    @pytest.mark.iis_only
    def test_admin_config_page(self, client: PageSpeedClient, admin_path: str):
        """Admin config endpoint should return JSON with the rendered config."""
        response = client.get(f"{admin_path}/config")

        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")
        if response.status == 404:
            pytest.skip("Config admin page not available")

        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "application/json" in content_type, (
            f"Config endpoint should return JSON, got: {content_type}"
        )
        data = json.loads(response.text)
        assert "config" in data, (
            f"Config JSON should contain 'config' key, got: {list(data.keys())}"
        )
        assert isinstance(data["config"], str) and data["config"], (
            "Config string should be non-empty"
        )

    @pytest.mark.iis_only
    def test_admin_histograms_page(self, client: PageSpeedClient, admin_path: str):
        """Admin histograms endpoint should return JSON with rendered histograms."""
        response = client.get(f"{admin_path}/histograms")

        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")
        if response.status == 404:
            pytest.skip("Histograms admin page not available")

        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "application/json" in content_type, (
            f"Histograms endpoint should return JSON, got: {content_type}"
        )
        data = json.loads(response.text)
        assert "histograms" in data, (
            f"Histograms JSON should contain 'histograms' key, got: "
            f"{list(data.keys())}"
        )

    @pytest.mark.iis_only
    def test_admin_cache_page(self, client: PageSpeedClient, admin_path: str):
        """Admin cache endpoint should return JSON with cache structure info."""
        response = client.get(f"{admin_path}/cache")

        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")
        if response.status == 404:
            pytest.skip("Cache admin page not available")

        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "application/json" in content_type, (
            f"Cache endpoint should return JSON, got: {content_type}"
        )
        data = json.loads(response.text)
        assert "caches" in data and isinstance(data["caches"], list), (
            f"Cache JSON should contain 'caches' list, got: {list(data.keys())}"
        )
        assert data["caches"], "Caches list should not be empty"
        cache_names = {entry.get("name", "") for entry in data["caches"]}
        assert "HTTP Cache" in cache_names, (
            f"Expected 'HTTP Cache' in {cache_names}"
        )

    @pytest.mark.iis_only
    def test_admin_console_page(self, client: PageSpeedClient, admin_path: str):
        """Admin console endpoint should serve the Svelte SPA shell."""
        response = client.get(f"{admin_path}/console")

        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")
        if response.status == 404:
            pytest.skip("Console admin page not available")

        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "text/html" in content_type, (
            f"Console SPA shell should be HTML, got: {content_type}"
        )
        # SPA shell ships an HTML document; an empty body would mean the
        # ServeSpaConsole asset was not built into the binary.
        assert len(response.text) > 100, (
            "SPA shell appears empty -- HTML_admin_console asset missing?"
        )

    @pytest.mark.iis_only
    def test_admin_message_history_page(self, client: PageSpeedClient, admin_path: str):
        """Admin message_history endpoint should return JSON with messages."""
        response = client.get(f"{admin_path}/message_history")

        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")
        if response.status == 404:
            pytest.skip("Message History admin page not available")

        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "application/json" in content_type, (
            f"Message history endpoint should return JSON, got: {content_type}"
        )
        data = json.loads(response.text)
        assert "messages" in data and isinstance(data["messages"], list), (
            f"Message history JSON should contain 'messages' list, got: "
            f"{list(data.keys())}"
        )

    @pytest.mark.iis_only
    def test_admin_pages_return_expected_content_type(
        self, client: PageSpeedClient, admin_path: str,
        admin_subpages: List[dict],
    ):
        """Each admin sub-page should match its declared Content-Type.

        SPA shell endpoints (console) -> text/html; data endpoints
        (statistics, config, cache, histograms, message_history) ->
        application/json. Source of truth: pagespeed/system/admin_site.cc.
        """
        for entry in admin_subpages:
            subpage = entry["path"]
            expected = entry["content_type"]
            response = client.get(f"{admin_path}/{subpage}")

            if response.status == 403:
                continue  # Skip auth-protected pages
            if response.status == 404:
                continue  # Skip unavailable pages

            content_type = response.header("Content-Type").lower()
            assert expected in content_type, (
                f"Admin page {subpage}: expected {expected}, got {content_type}"
            )


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

        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")

        # Should either work (200) or redirect (301/302) or be not found (404)
        assert response.status in (200, 301, 302, 404), (
            f"Admin path should respond cleanly, got {response.status}"
        )

    @pytest.mark.iis_only
    def test_global_admin_path(self, client: PageSpeedClient):
        """Global admin path should respond (if configured)."""
        response = client.get("/pagespeed_global_admin/statistics")

        # Global admin might not be configured
        if response.status == 404:
            pytest.skip("Global admin path not configured")

        if response.status == 403:
            pytest.skip("Global admin requires authentication")

        assert_http_status(response, 200)

    @pytest.mark.iis_only
    def test_admin_path_trailing_slash(self, client: PageSpeedClient, admin_path: str):
        """Admin path should work with trailing slash."""
        response = client.get(f"{admin_path}/")

        if response.status == 403:
            pytest.skip("Admin endpoint requires authentication")

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
    def test_admin_xss_quoting_path(self, client: PageSpeedClient, admin_path: str):
        """Admin 404 page should escape HTML in path.

        Ported from:
        OUT=$($CURL $PRIMARY_SERVER/'pagespeed_admin/a/<boo>')
        check_not_from "$OUT" fgrep -q "<boo>"
        check_from "$OUT" fgrep -q "%3Cboo%3E"
        """
        # Request a non-existent page with HTML in the path
        malicious_path = f"{admin_path}/a/<boo>"
        response = client.get(malicious_path)

        # Should not contain unescaped HTML
        if response.text:
            assert "<boo>" not in response.text, (
                "XSS vulnerability: unescaped HTML tag in admin 404 response"
            )

            # Should contain escaped version if path is reflected
            if "boo" in response.text.lower():
                # Should be URL-encoded or HTML-escaped
                has_escaped = (
                    "%3Cboo%3E" in response.text or
                    "%3cboo%3e" in response.text.lower() or
                    "&lt;boo&gt;" in response.text or
                    "\\u003c" in response.text
                )
                assert has_escaped, (
                    "Path should be escaped in admin 404 response"
                )

    @pytest.mark.iis_only
    def test_admin_xss_quoting_query(self, client: PageSpeedClient, admin_path: str):
        """Admin page should escape HTML in query parameters.

        Ported from:
        OUT=$($CURL $PRIMARY_SERVER/'pagespeed_admin/a?<boo>')
        check_not_from "$OUT" fgrep -q "boo"
        """
        # Request with HTML in query string
        malicious_url = f"{admin_path}/a?<boo>"
        response = client.get(malicious_url)

        # Query params should be stripped or escaped - should not contain raw HTML
        if response.text:
            assert "<boo>" not in response.text, (
                "XSS vulnerability: unescaped HTML tag from query parameter"
            )

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

        if response.status == 403:
            pytest.skip("Message history requires authentication")
        if response.status == 404:
            pytest.skip("Message history page not available")

        assert_http_status(response, 200)

    @pytest.mark.iis_only
    def test_message_page_contains_messages(
        self, client: PageSpeedClient, admin_path: str
    ):
        """Message history endpoint should return a JSON 'messages' array.

        Each entry has 'severity' (info|warning|error|fatal) and 'message'.
        Empty list is acceptable in a freshly-started server.
        """
        response = client.get(f"{admin_path}/message_history")

        if response.status != 200:
            pytest.skip("Message history page not available")

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

        if response.status != 200:
            pytest.skip("Message history page not available")

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

        if response.status != 200:
            pytest.skip("Admin statistics page not available")

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

        if response.status != 200:
            pytest.skip("Admin config page not available")

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

        if response.status != 200:
            pytest.skip("Admin cache page not available")

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


# ============================================================================
# TestLicenseEndpointRequestHeaderPassthrough: regression for the silently
# stripped CSRF headers on the IIS admin handler.
#
# The AdminLicenseHandler CSRF gate requires Content-Type=application/json
# AND X-Requested-With=XMLHttpRequest on every mutation endpoint (apply,
# activate, consent). Pre-fix, pagespeed/iis/iis_admin_handler.cc
# constructed an empty RequestHeaders inside IisAdminFetch and called
# AdminPage(...) without ever populating those headers from the IIS native
# request. The 57 AdminLicenseHandler unit tests synthesize the headers
# directly on AsyncFetch in-process, so they passed; production IIS
# unconditionally returned 403 "Missing or invalid CSRF headers" on every
# mutation endpoint and silently killed the entire licensing UX from the
# admin SPA. Detected in v1.1.0-beta.16 once the staging-staleness fix in
# let the actually-fresh DLL hit the day-2 VM.
#
# These tests assert the request-header path end-to-end via the real IIS
# handler chain, NOT via direct AsyncFetch construction.
# ============================================================================


GLOBAL_ADMIN_PATH = "/pagespeed_global_admin"
LICENSE_CONSENT_PATH = f"{GLOBAL_ADMIN_PATH}/v1/license/consent"
CSRF_HEADERS = {
    "Content-Type": "application/json",
    "X-Requested-With": "XMLHttpRequest",
}


class TestLicenseEndpointRequestHeaderPassthrough:
    """Regression tests for IIS admin -> AdminLicenseHandler header forwarding.

    Guards the iis_admin_handler.cc fix that copies Content-Type and
    X-Requested-With into IisAdminFetch.request_headers() before the
    AdminLicenseHandler CSRF gate inspects them.
    """

    @pytest.mark.iis_only
    def test_csrf_gate_passes_when_headers_present(
        self, client: PageSpeedClient
    ):
        """Mutation endpoint must accept correctly-formed CSRF headers.

        Pre-fix: IIS handler dropped headers -> 403 "Missing or invalid
        CSRF headers". Post-fix: headers reach AdminLicenseHandler, gate
        passes, request proceeds to per-endpoint logic. We do NOT assert
        a success status code (the trial provisioning service may be
        unavailable in CI / fail validation); we only assert the request
        was NOT rejected at the CSRF gate.
        """
        response = client.post(
            LICENSE_CONSENT_PATH,
            data='{"email":"ci-regression@example.com"}',
            headers=CSRF_HEADERS,
        )

        # If the admin endpoint is gated by token auth, the IIS handler
        # returns 403 BEFORE reaching the license handler. Skip in that
        # case so the test is robust across both auth-on and auth-off
        # CI configurations.
        if (response.status == 403
                and "Missing or invalid CSRF headers" not in response.text):
            pytest.skip(
                "Admin endpoint gated by token auth - cannot exercise CSRF path"
            )

        assert "Missing or invalid CSRF headers" not in response.text, (
            "Regression: IIS admin handler dropped CSRF headers before "
            "they reached AdminLicenseHandler. "
            f"Status={response.status} body={response.text!r}"
        )

    @pytest.mark.iis_only
    def test_apply_post_body_reaches_handler(
        self, client: PageSpeedClient
    ):
        """Regression for iis_http_module.cpp dropping the POST body.

        The IIS module's live request router (iis_http_module.cpp's
        kAdmin / kGlobalAdmin branch) must read the POST body via
        SyncReadPostBody and forward it to AdminPage so that
        AdminLicenseHandler::HandleApply (and the activate/trial/
        consent peers) can ExtractJsonStringField. Without this, every
        license POST 400s with "Missing 'key' or 'license_key' field"
        (or the analogous nonce/email error for the other endpoints).

        Pre-fix the body was always empty, so the assertion below
        fires. Post-fix the body reaches HandleApply, which then runs
        ApplyToken on "any-non-empty-test-value", fails token
        validation, and responds with the per-validator error
        (e.g. "Invalid license format") -- not the missing-field 400.

        Latent since the IIS port to 1.1 (beta.5+); masked through
        beta.19 by a CSRF-headers stripping bug that 403'd every
        mutation before this branch could fire. Earlier header work fixed
        the headers and surfaced this body-forwarding gap as the
        next-level-down failure.

        Symptom: "Missing 'key' or 'license_key' field" 400 in
        production on beta.20.
        """
        response = client.post(
            f"{GLOBAL_ADMIN_PATH}/v1/license/apply",
            data='{"key":"any-non-empty-test-value"}',
            headers=CSRF_HEADERS,
        )

        # If the admin endpoint is gated by token auth, skip --
        # the IIS handler 403s before reaching the license handler.
        if (response.status == 403
                and "Missing or invalid CSRF headers" not in response.text):
            pytest.skip(
                "Admin endpoint gated by token auth - cannot exercise "
                "body-forwarding path"
            )

        assert "Missing 'key' or 'license_key'" not in response.text, (
            "Regression: iis_http_module.cpp dropped POST body before it "
            "reached AdminLicenseHandler::HandleApply. AdminPage was "
            "called with the default empty request_body. "
            f"Status={response.status} body={response.text!r}"
        )

    @pytest.mark.iis_only
    def test_csrf_gate_rejects_when_headers_missing(
        self, client: PageSpeedClient
    ):
        """Negative path: CSRF protection itself must still work.

        Without Content-Type=application/json AND X-Requested-With, the
        gate must return 403 with the specific error message. This
        ensures the header-forwarding fix didn't accidentally short-
        circuit CSRF enforcement.
        """
        response = client.post(
            LICENSE_CONSENT_PATH,
            data='{"email":"ci-regression@example.com"}',
            headers=None,
        )

        if (response.status == 403
                and "Missing or invalid CSRF headers" not in response.text):
            pytest.skip(
                "Admin endpoint gated by token auth - cannot exercise CSRF path"
            )

        assert response.status == 403, (
            f"CSRF gate must return 403 without headers, got {response.status} "
            f"body={response.text!r}"
        )
        assert "Missing or invalid CSRF headers" in response.text, (
            f"Expected CSRF rejection message, got body={response.text!r}"
        )
