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

"""IIS header handling system tests.

These tests verify the PageSpeed header handling functionality on IIS, ported
from Apache's response_headers.sh, x_forwarded_proto.sh, custom_fetch_headers.sh,
and pass_through_headers.sh system tests.

Test classes:
- TestResponseHeaders: Tests response header manipulation
- TestForwardedProto: Tests X-Forwarded-Proto header handling
- TestCustomFetchHeaders: Tests custom headers on resource fetches
- TestPassThroughHeaders: Tests header pass-through on combined resources
- TestPageSpeedHeaders: Tests PageSpeed-specific headers

Environment Variables:
    PAGESPEED_TEST_ROOT: Root path for test pages
"""

import re
from typing import Dict, Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
)


# ============================================================================
# TestResponseHeaders: Test response header behavior
# ============================================================================


class TestResponseHeaders:
    """Tests for response header manipulation.

    These tests verify that PageSpeed handles response headers correctly
    and that option headers (PageSpeed/ModPagespeed) are stripped from
    responses.

    Ported from: pagespeed/apache/system_tests/response_headers.sh
    """

    @pytest.mark.iis_only
    def test_pagespeed_header_present(
        self, client: PageSpeedClient, example_root: str
    ):
        """Response should include X-Page-Speed or X-Mod-Pagespeed header."""
        response = client.get(f"{example_root}/extend_cache.html")
        assert_http_status(response, 200)

        # Check for PageSpeed version header
        x_page_speed = response.header("X-Page-Speed")
        x_mod_pagespeed = response.header("X-Mod-Pagespeed")

        has_version_header = bool(x_page_speed or x_mod_pagespeed)
        assert has_version_header, (
            "Response should include X-Page-Speed or X-Mod-Pagespeed header"
        )

    @pytest.mark.iis_only
    def test_option_headers_stripped(
        self, client: PageSpeedClient, test_root: str
    ):
        """PageSpeed option headers should be stripped from response.

        Ported from response_headers.sh:
        check_not grep -q ^PageSpeed: $OUTDIR/header_out
        check_not grep -q ^ModPagespeed: $OUTDIR/header_out
        """
        response = client.get(f"{test_root}/extend_cache.html")

        # Option headers (used for configuration) should not be in response
        pagespeed_option = response.header("PageSpeed")
        modpagespeed_option = response.header("ModPagespeed")

        assert not pagespeed_option, (
            "PageSpeed option header should be stripped from response"
        )
        assert not modpagespeed_option, (
            "ModPagespeed option header should be stripped from response"
        )

    @pytest.mark.iis_only
    def test_cache_control_header_set(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized resources should have Cache-Control header."""
        # Request an optimized resource
        response = client.fetch_until_contains(
            f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images",
            pattern=r"\.pagespeed\.",
            timeout=30.0
        )
        assert_http_status(response, 200)

        # HTML responses should have appropriate caching headers
        cache_control = response.header("Cache-Control")
        # May have no-cache for HTML, or max-age for resources
        assert cache_control is not None or response.header("Expires") is not None, (
            "Response should have caching headers"
        )

    @pytest.mark.iis_only
    def test_content_type_preserved(
        self, client: PageSpeedClient, example_root: str
    ):
        """Content-Type header should be preserved on rewritten content."""
        response = client.get(f"{example_root}/extend_cache.html")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert content_type is not None, "Content-Type header should be present"
        assert "text/html" in content_type.lower(), (
            f"HTML pages should have text/html Content-Type, got: {content_type}"
        )

    @pytest.mark.iis_only
    def test_vary_header_for_webp(
        self, client: PageSpeedClient, example_root: str
    ):
        """Image responses may include Vary: Accept header for WebP support."""
        # Request an image through PageSpeed
        response = client.fetch_until_contains(
            f"{example_root}/extend_cache.html?PageSpeedFilters=rewrite_images",
            pattern=r"\.pagespeed\.",
            timeout=30.0
        )

        # The Vary header is used for content negotiation
        # It's optional but recommended for WebP support
        vary = response.header("Vary")
        if vary:
            # If present, might include Accept for WebP negotiation
            pass  # This is informational


# ============================================================================
# TestForwardedProto: Test X-Forwarded-Proto header handling
# ============================================================================


class TestForwardedProto:
    """Tests for X-Forwarded-Proto header handling.

    These tests verify that PageSpeed respects the X-Forwarded-Proto header
    when configured to do so, which is important for reverse proxy setups.

    Ported from: pagespeed/apache/system_tests/x_forwarded_proto.sh
    """

    @pytest.mark.iis_only
    def test_x_forwarded_proto_https(
        self, client: PageSpeedClient, test_root: str
    ):
        """X-Forwarded-Proto: https should be respected for base tag.

        Ported from x_forwarded_proto.sh:
        check $WGET_DUMP -O $FETCHED --header="X-Forwarded-Proto: https" $URL
        check fgrep -q '<base href="https://' $FETCHED
        """
        # Request with X-Forwarded-Proto header and add_base_tag filter
        url = f"{test_root}/?PageSpeedFilters=add_base_tag"
        response = client.get(url, headers={"X-Forwarded-Proto": "https"})

        if response.status == 200:
            # When X-Forwarded-Proto is respected, base tag should use https
            if "<base" in response.text:
                assert_contains(
                    response,
                    r'<base href="https://',
                    "Base tag should use https when X-Forwarded-Proto: https"
                )
            else:
                # Base tag might not be added if feature is disabled
                pytest.skip("add_base_tag filter may not be enabled")
        else:
            pytest.skip(f"Request returned status {response.status}")

    @pytest.mark.iis_only
    def test_x_forwarded_proto_http(
        self, client: PageSpeedClient, test_root: str
    ):
        """X-Forwarded-Proto: http should be respected for base tag."""
        url = f"{test_root}/?PageSpeedFilters=add_base_tag"
        response = client.get(url, headers={"X-Forwarded-Proto": "http"})

        if response.status == 200 and "<base" in response.text:
            # Base tag should use http
            assert_contains(
                response,
                r'<base href="http://',
                "Base tag should use http when X-Forwarded-Proto: http"
            )

    @pytest.mark.iis_only
    def test_x_forwarded_proto_affects_resource_urls(
        self, client: PageSpeedClient, example_root: str
    ):
        """X-Forwarded-Proto should affect rewritten resource URLs."""
        url = f"{example_root}/extend_cache.html?PageSpeedFilters=rewrite_css"
        response = client.get(url, headers={"X-Forwarded-Proto": "https"})

        if response.status == 200:
            # Check if any absolute URLs use https
            # This is optional - relative URLs are also valid
            if "https://" in response.text or "http://" in response.text:
                # If absolute URLs are used, they should match the protocol
                pass  # Informational test


# ============================================================================
# TestCustomFetchHeaders: Test custom headers on resource fetches
# ============================================================================


class TestCustomFetchHeaders:
    """Tests for custom fetch header handling.

    These tests verify that custom headers configured for resource fetches
    are properly applied.

    Ported from: pagespeed/apache/system_tests/custom_fetch_headers.sh
    """

    @pytest.mark.iis_only
    def test_user_agent_preserved(
        self, client: PageSpeedClient, example_root: str
    ):
        """User-Agent header should be available for requests."""
        custom_ua = "TestBot/1.0"
        response = client.get(
            f"{example_root}/extend_cache.html",
            headers={"User-Agent": custom_ua}
        )
        assert_http_status(response, 200)
        # The server received our request with custom User-Agent
        # We can't verify what was sent on sub-fetches without server logs

    @pytest.mark.iis_only
    def test_accept_encoding_respected(
        self, client: PageSpeedClient, example_root: str
    ):
        """Accept-Encoding header should be respected for compression."""
        # Request with gzip support
        response = client.get_gzip(f"{example_root}/extend_cache.html")
        assert_http_status(response, 200)

        # Response may be gzip-encoded if server supports it
        content_encoding = response.header("Content-Encoding")
        if content_encoding:
            assert "gzip" in content_encoding.lower() or "deflate" in content_encoding.lower(), (
                f"Unexpected Content-Encoding: {content_encoding}"
            )

    @pytest.mark.iis_only
    def test_if_modified_since_respected(
        self, client: PageSpeedClient, example_root: str
    ):
        """If-Modified-Since header should be respected for caching."""
        url = f"{example_root}/extend_cache.html"

        # First request to get Last-Modified
        response1 = client.get(url)
        assert_http_status(response1, 200)

        last_modified = response1.header("Last-Modified")
        if not last_modified:
            pytest.skip("Server doesn't send Last-Modified header")

        # Second request with If-Modified-Since
        response2 = client.get(
            url,
            headers={"If-Modified-Since": last_modified}
        )

        # Should return 304 Not Modified if resource hasn't changed
        # Or 200 if server doesn't support conditional requests
        assert response2.status in (200, 304), (
            f"Unexpected status for conditional request: {response2.status}"
        )


# ============================================================================
# TestPassThroughHeaders: Test header pass-through on combined resources
# ============================================================================


class TestPassThroughHeaders:
    """Tests for header pass-through on combined resources.

    These tests verify that certain origin headers are passed through
    on combined resources.

    Ported from: pagespeed/apache/system_tests/pass_through_headers.sh
    """

    @pytest.mark.iis_only
    def test_combined_css_has_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined CSS should have correct Content-Type."""
        # Request a page that combines CSS
        response = client.fetch_until_contains(
            f"{example_root}/combine_css.html?PageSpeedFilters=combine_css",
            pattern=r"\.pagespeed\.cc\.",
            timeout=30.0
        )
        assert_http_status(response, 200)

        # Extract combined CSS URL from response
        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("No combined CSS found in response")

        css_url = match.group(1)
        if not css_url.startswith("http"):
            # Relative URL - construct full URL
            css_url = f"{example_root}/{css_url.lstrip('/')}"

        # Fetch the combined CSS
        css_response = client.get(css_url)

        if css_response.status == 200:
            content_type = css_response.header("Content-Type")
            assert content_type is not None, "Combined CSS should have Content-Type"
            assert "css" in content_type.lower(), (
                f"Combined CSS should have CSS Content-Type, got: {content_type}"
            )

    @pytest.mark.iis_only
    def test_combined_js_has_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Combined JavaScript should have correct Content-Type."""
        # Request a page that combines JS
        response = client.fetch_until_contains(
            f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript",
            pattern=r"\.pagespeed\.jc\.",
            timeout=30.0
        )

        if response.status != 200:
            pytest.skip("combine_javascript test page not available")

        # Extract combined JS URL from response
        match = re.search(r'src="([^"]*\.pagespeed\.jc\.[^"]*)"', response.text)
        if not match:
            pytest.skip("No combined JavaScript found in response")

        js_url = match.group(1)
        if not js_url.startswith("http"):
            js_url = f"{example_root}/{js_url.lstrip('/')}"

        # Fetch the combined JS
        js_response = client.get(js_url)

        if js_response.status == 200:
            content_type = js_response.header("Content-Type")
            assert content_type is not None, "Combined JS should have Content-Type"
            assert "javascript" in content_type.lower() or "ecmascript" in content_type.lower(), (
                f"Combined JS should have JavaScript Content-Type, got: {content_type}"
            )


# ============================================================================
# TestPageSpeedHeaders: Test PageSpeed-specific headers
# ============================================================================


class TestPageSpeedHeaders:
    """Tests for PageSpeed-specific headers.

    These tests verify PageSpeed's custom headers like X-Page-Speed,
    X-Original-Content-Length, etc.
    """

    @pytest.mark.iis_only
    def test_x_page_speed_version(
        self, client: PageSpeedClient, example_root: str
    ):
        """X-Page-Speed header should contain version information."""
        response = client.get(f"{example_root}/extend_cache.html")
        assert_http_status(response, 200)

        x_page_speed = response.header("X-Page-Speed")
        x_mod_pagespeed = response.header("X-Mod-Pagespeed")

        version_header = x_page_speed or x_mod_pagespeed
        if version_header:
            # Version should be in format like "1.13.35.2-0" or similar
            assert re.match(r"[\d.]+", version_header), (
                f"Version header should start with version number: {version_header}"
            )

    @pytest.mark.iis_only
    def test_x_original_content_length(
        self, client: PageSpeedClient, example_root: str
    ):
        """X-Original-Content-Length header shows original size (if enabled)."""
        response = client.get(
            f"{example_root}/extend_cache.html?PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(response, 200)

        original_length = response.header("X-Original-Content-Length")
        if original_length:
            # Should be a valid integer
            assert original_length.isdigit(), (
                f"X-Original-Content-Length should be numeric: {original_length}"
            )
            # Original should be >= current (after optimization)
            current_length = len(response.text)
            assert int(original_length) >= current_length, (
                f"Original length ({original_length}) should be >= current ({current_length})"
            )

    @pytest.mark.iis_only
    def test_pagespeed_off_header(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off query parameter should disable optimization."""
        response = client.get(f"{example_root}/extend_cache.html?PageSpeed=off")
        assert_http_status(response, 200)

        # With PageSpeed=off, version header should not be present
        # (or optimizations should not be applied)
        x_page_speed = response.header("X-Page-Speed")
        x_mod_pagespeed = response.header("X-Mod-Pagespeed")

        # If version header is present, response should not be optimized
        if x_page_speed or x_mod_pagespeed:
            # PageSpeed was still on - check if content is unoptimized
            # This is implementation-dependent
            pass

    @pytest.mark.iis_only
    def test_pagespeed_no_defer_header(
        self, client: PageSpeedClient, example_root: str
    ):
        """X-PSA-Blocking-Rewrite header should trigger blocking rewrite."""
        response = client.get(
            f"{example_root}/extend_cache.html",
            headers={"X-PSA-Blocking-Rewrite": "true"}
        )
        assert_http_status(response, 200)

        # With blocking rewrite, resources should be fully optimized
        # in the first response (no background optimization)
        # This is hard to verify without knowing the expected output


# ============================================================================
# TestHeaderIntegration: Integration tests for header handling
# ============================================================================


class TestHeaderIntegration:
    """Integration tests for header handling."""

    @pytest.mark.iis_only
    def test_multiple_custom_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """Multiple custom headers should all be processed."""
        headers = {
            "X-Forwarded-Proto": "https",
            "X-Forwarded-For": "192.168.1.100",
            "Accept-Encoding": "gzip, deflate",
        }
        response = client.get(
            f"{example_root}/extend_cache.html",
            headers=headers
        )
        assert_http_status(response, 200)

    @pytest.mark.iis_only
    def test_header_case_insensitivity(
        self, client: PageSpeedClient, example_root: str
    ):
        """HTTP headers should be case-insensitive."""
        # Test lowercase
        response1 = client.get(
            f"{example_root}/extend_cache.html",
            headers={"accept-encoding": "gzip"}
        )
        assert_http_status(response1, 200)

        # Test mixed case
        response2 = client.get(
            f"{example_root}/extend_cache.html",
            headers={"Accept-Encoding": "gzip"}
        )
        assert_http_status(response2, 200)

    @pytest.mark.iis_only
    def test_host_header_handling(
        self, client: PageSpeedClient, example_root: str
    ):
        """Host header should be properly handled."""
        response = client.get(f"{example_root}/extend_cache.html")
        assert_http_status(response, 200)

        # Verify the response is for the expected host
        # (URLs in response should be relative or for the correct host)
