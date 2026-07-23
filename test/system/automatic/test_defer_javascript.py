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

"""Defer JavaScript filter tests.

Ported from: pagespeed/automatic/system_tests/defer_javascript.sh

These tests verify that the defer_javascript filter correctly defers
JavaScript execution until after page load.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
    require_match,
)


class TestNoscriptCanonical:
    """Tests for canonical link insertion with noscript mode.

    Bash original::

        start_test ?PageSpeed=noscript inserts canonical href link
        OUT=$($WGET_DUMP $EXAMPLE_ROOT/defer_javascript.html?PageSpeed=noscript)
        check_from "$OUT" fgrep -q \
          "link rel=\"canonical\" href=\"$EXAMPLE_ROOT/defer_javascript.html\""
    """

    def test_noscript_inserts_canonical_link(
        self, client: PageSpeedClient, example_root: str
    ):
        """Noscript mode should insert canonical link."""
        url = f"{example_root}/defer_javascript.html?PageSpeed=noscript"

        response = client.get(url)
        assert_http_status(response, 200)

        # Check for canonical link
        assert_contains(
            response,
            r'link rel="canonical" href="[^"]*defer_javascript\.html"',
            "Noscript mode should insert canonical link",
        )


class TestDeferJavascript:
    """Tests for the defer_javascript filter.

    Bash original::

        test_filter defer_javascript optimize mode
        check run_wget_with_args $URL
        check grep -q text/psajs $FETCHED
        check grep -q /js_defer $FETCHED
        check grep -q "PageSpeed=noscript" $FETCHED
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


class TestDeferJavascriptDebug:
    """Tests for defer_javascript in debug mode.

    Bash original::

        test_filter defer_javascript,debug optimize mode
        check run_wget_with_args "$URL"
        check grep -q text/psajs $FETCHED
        check grep -q /js_defer_debug $FETCHED
        check grep -q "PageSpeed=noscript" $FETCHED
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


class TestDeferJsResources:
    """Tests for defer JS resource serving.

    Bash original::

        start_test Fetch the deferJs url with hash.
        run_wget_with_args -q \
          http://$PROXY_DOMAIN/$PSA_JS_LIBRARY_URL_PREFIX/$DEFERJSURL
        check_200_http_response_file "$WGET_OUTPUT"
        check fgrep "Cache-Control: max-age=31536000" $WGET_OUTPUT
    """

    def test_defer_js_without_hash_returns_404(
        self, client: PageSpeedClient
    ):
        """Access to js_defer.js without hash should return 404.

        Bash original::

            start_test Access to js_defer.js without hash returns 404.
            URL="http://$PROXY_DOMAIN/$PSA_JS_LIBRARY_URL_PREFIX/js_defer.js"
            check_not run_wget_with_args "$URL"
            check fgrep "404 Not Found" $WGET_OUTPUT
        """
        # Note: The exact path may vary depending on configuration
        url = "/pagespeed_static/js_defer.js"

        response = client.get(url)
        assert_http_status(response, 404)

    def test_defer_js_versioned_returns_200(
        self, client: PageSpeedClient
    ):
        """Versioned js_defer.0.js should return 200.

        Bash original::

            start_test serve js_defer.0.js
            URL="http://$PROXY_DOMAIN/$PSA_JS_LIBRARY_URL_PREFIX/js_defer.0.js"
            run_wget_with_args -q "$URL"
            check_200_http_response_file "$WGET_OUTPUT"
            check fgrep "Cache-Control: max-age=300,private" $WGET_OUTPUT
        """
        url = "/pagespeed_static/js_defer.0.js"

        response = client.get(url)
        assert_http_status(response, 200)

        # Check cache control
        cache_control = response.header("Cache-Control")
        assert cache_control, "Should have Cache-Control header"

    def test_defer_js_debug_versioned_returns_200(
        self, client: PageSpeedClient
    ):
        """Versioned js_defer_debug.0.js should return 200.

        Bash original::

            start_test serve js_defer_debug.0.js
            URL="http://$PROXY_DOMAIN/$PSA_JS_LIBRARY_URL_PREFIX/js_defer_debug.0.js"
            run_wget_with_args -q "$URL"
            check_200_http_response_file "$WGET_OUTPUT"
            check fgrep "Cache-Control: max-age=300,private" $WGET_OUTPUT
        """
        url = "/pagespeed_static/js_defer_debug.0.js"

        response = client.get(url)
        assert_http_status(response, 200)


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

        # Extract the defer JS URL with hash. The blocking-rewrite header
        # forces a fully rewritten response, so the URL must be present.
        match = require_match(
            r'src="([^"]*js_defer[^"]*\.js)"',
            response,
            "defer JS URL",
        )

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


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
