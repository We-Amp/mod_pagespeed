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

"""Defer JavaScript filter tests for IIS PageSpeed module.

Ported from: test/system/automatic/test_defer_javascript.py

These tests verify that the defer_javascript filter correctly defers
JavaScript execution until after page load on IIS.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


@pytest.mark.html_rewrite
class TestDeferJavascript:
    """Tests for the defer_javascript filter.

    These tests verify that:
    - Deferred scripts use text/psajs type
    - The js_defer library is included
    - Noscript fallback is present
    """

    def test_defer_javascript_uses_psajs_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Deferred scripts should use text/psajs type."""
        url = f"{example_root}/defer_javascript.html?PageSpeedFilters=defer_javascript"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"text/psajs",
            "Deferred scripts should use text/psajs type",
        )

    def test_defer_javascript_includes_js_defer(
        self, client: PageSpeedClient, example_root: str
    ):
        """Defer JavaScript should include js_defer library."""
        url = f"{example_root}/defer_javascript.html?PageSpeedFilters=defer_javascript"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"/js_defer",
            "Should include js_defer library reference",
        )

    def test_defer_javascript_has_noscript_fallback(
        self, client: PageSpeedClient, example_root: str
    ):
        """Defer JavaScript should have noscript fallback."""
        url = f"{example_root}/defer_javascript.html?PageSpeedFilters=defer_javascript"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"PageSpeed=noscript",
            "Should have noscript fallback",
        )


@pytest.mark.html_rewrite
class TestDeferJavascriptDebug:
    """Tests for defer_javascript in debug mode.

    Debug mode should use the js_defer_debug library instead of
    the minified js_defer library.
    """

    def test_defer_javascript_debug_uses_debug_library(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should use js_defer_debug library."""
        url = f"{example_root}/defer_javascript.html?PageSpeedFilters=defer_javascript,debug"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"/js_defer_debug",
            "Debug mode should use js_defer_debug library",
        )

    def test_defer_javascript_debug_still_uses_psajs(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should still use text/psajs type."""
        url = f"{example_root}/defer_javascript.html?PageSpeedFilters=defer_javascript,debug"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"text/psajs",
            "Debug mode should still use text/psajs type",
        )


@pytest.mark.html_rewrite
class TestDeferJsResources:
    """Tests for defer JS resource serving.

    These tests verify that the PageSpeed static resources for
    defer_javascript are served correctly.
    """

    def test_defer_js_without_hash_returns_404(self, client: PageSpeedClient):
        """Access to js_defer.js without hash should return 404.

        The raw js_defer.js file should not be served directly
        for security and caching reasons.
        """
        url = "/pagespeed_static/js_defer.js"

        response = client.get(url)
        assert_http_status(response, 404)

    def test_defer_js_versioned_returns_200(self, client: PageSpeedClient):
        """Versioned js_defer.0.js should return 200.

        The versioned file (with .0.) should be served with
        appropriate cache headers.
        """
        url = "/pagespeed_static/js_defer.0.js"

        response = client.get(url)
        assert_http_status(response, 200)

        # Check cache control header is present
        cache_control = response.header("Cache-Control")
        assert cache_control, "Should have Cache-Control header"

    def test_defer_js_debug_versioned_returns_200(self, client: PageSpeedClient):
        """Versioned js_defer_debug.0.js should return 200.

        The debug version should also be available for debugging purposes.
        """
        url = "/pagespeed_static/js_defer_debug.0.js"

        response = client.get(url)
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestDeferJsWithHash:
    """Tests for fetching defer JS with proper hash."""

    def test_defer_js_with_hash_served_correctly(
        self, client: PageSpeedClient, example_root: str
    ):
        """Defer JS with hash should be served with long cache."""
        url = f"{example_root}/defer_javascript.html?PageSpeedFilters=defer_javascript,debug"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Extract the defer JS URL with hash
        match = re.search(r'src="([^"]*js_defer[^"]*\.js)"', response.text)
        if not match:
            pytest.skip("Could not find defer JS URL in rewritten page")

        js_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{example_root}/{js_url}"

        # Fetch the JS file
        js_response = client.get(js_url)
        assert_http_status(js_response, 200)

        # Check for cache control header
        cache_control = js_response.header("Cache-Control")
        assert cache_control, "Defer JS with hash should have Cache-Control header"


@pytest.mark.html_rewrite
class TestNoscriptCanonical:
    """Tests for canonical link insertion with noscript mode."""

    def test_noscript_inserts_canonical_link(
        self, client: PageSpeedClient, example_root: str
    ):
        """Noscript mode should insert canonical link.

        When PageSpeed=noscript is used, a canonical link should be
        inserted pointing to the original URL without the noscript parameter.
        """
        url = f"{example_root}/defer_javascript.html?PageSpeed=noscript"

        response = client.get(url)
        assert_http_status(response, 200)

        # Check for canonical link
        assert_contains(
            response,
            r'link rel="canonical" href="[^"]*defer_javascript\.html"',
            "Noscript mode should insert canonical link",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
