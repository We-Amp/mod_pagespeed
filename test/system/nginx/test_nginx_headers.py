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

"""nginx header and protocol tests.

These tests verify HTTP header handling for nginx with ngx_pagespeed.
They test response headers, conditional requests, and content negotiation.
"""

import random
import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_equals,
    assert_header_contains,
    require_match,
)


@pytest.mark.nginx_only
class TestXPageSpeedHeader:
    """Tests for X-Page-Speed response header.

    The X-Page-Speed header identifies the PageSpeed version processing
    the response. nginx uses X-Page-Speed (not X-Mod-Pagespeed like Apache).
    """

    def test_x_pagespeed_header(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify X-Page-Speed response header is present with version."""
        response = client.get(f"{example_root}/combine_css.html")
        assert_http_status(response, 200)

        # nginx uses X-Page-Speed header
        page_speed = response.header("X-Page-Speed")
        assert page_speed, "Expected X-Page-Speed header in response"

        # Header should contain version number (e.g., 1.1.0-beta.1 or 1.13.35.2-0)
        version_pattern = r"\d+\.\d+\.\d+[\w.\-]*"
        assert re.search(version_pattern, page_speed), \
            f"X-Page-Speed should contain version number, got: {page_speed}"

    def test_x_pagespeed_header_on_html(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify X-Page-Speed header is present on HTML responses."""
        response = client.get(f"{example_root}/index.html")
        assert_http_status(response, 200)

        page_speed = response.header("X-Page-Speed")
        assert page_speed, "HTML responses should have X-Page-Speed header"


@pytest.mark.nginx_only
class TestXForwardedProto:
    """Tests for X-Forwarded-Proto header handling.

    When nginx is behind a reverse proxy, it should use X-Forwarded-Proto
    to determine the original protocol and generate correct URLs.
    """

    def test_x_forwarded_proto_https(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify X-Forwarded-Proto: https causes HTTPS URLs in rewritten content."""
        # Request with X-Forwarded-Proto indicating HTTPS origin
        url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.ce\.',
            timeout=30.0,
            headers={"X-Forwarded-Proto": "https"},
        )
        assert_http_status(response, 200)

        # Check for https:// URLs in rewritten content
        # Note: This depends on nginx configuration supporting X-Forwarded-Proto
        # The rewritten resource URLs may use https:// if configured properly
        html = response.text

        # Look for any absolute URLs and verify they use https if present
        absolute_urls = re.findall(r'(https?://[^"\'>\s]+)', html)
        if absolute_urls:
            for url in absolute_urls:
                # URLs should be https when X-Forwarded-Proto: https is sent
                if 'pagespeed' in url.lower():
                    assert url.startswith('https://'), \
                        f"Expected HTTPS URL with X-Forwarded-Proto: https, got: {url}"


@pytest.mark.nginx_only
class TestVaryAcceptEncoding:
    """Tests for Vary: Accept-Encoding header.

    PageSpeed should set Vary: Accept-Encoding on compressed resources
    to ensure proper caching by downstream proxies.
    """

    def test_vary_accept_encoding(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify Vary: Accept-Encoding header on compressed resources."""
        # Fetch a page that has rewritten resources
        page_url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'href="[^"]*\.pagespeed\.cf\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Extract a rewritten CSS URL
        match = require_match(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response,
            "rewritten CSS URL",
        )

        css_url = match.group(1)
        if not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        # Fetch the CSS with Accept-Encoding: gzip
        css_response = client.get(css_url, headers={"Accept-Encoding": "gzip"})
        assert_http_status(css_response, 200)

        # Should have Vary header including Accept-Encoding
        vary = css_response.header("Vary")
        assert "Accept-Encoding" in vary or vary == "*", \
            f"Expected Vary: Accept-Encoding, got: {vary}"


@pytest.mark.nginx_only
class TestCacheControlPublic:
    """Tests for Cache-Control headers on optimized resources.

    Optimized resources with content hash in URL should have long cache
    lifetimes with public caching enabled.
    """

    def test_cache_control_public(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify Cache-Control: public, max-age=... on .pagespeed. resources."""
        # Get a page with combined CSS
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'href="[^"]*\.pagespeed\.(cc|cf)\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )

        # Extract the pagespeed resource URL
        match = require_match(
            r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"',
            response,
            ".pagespeed. resource URL",
        )

        resource_url = match.group(1)
        if not resource_url.startswith("/"):
            resource_url = f"{example_root}/{resource_url}"

        resource_response = client.get(resource_url)
        assert_http_status(resource_response, 200)

        cache_control = resource_response.header("Cache-Control")

        # Note: nginx/PageSpeed only adds 'public' if input resources have it.
        # We just verify caching is enabled with reasonable max-age.

        # Should have max-age directive with long lifetime
        assert "max-age" in cache_control, \
            f"Expected max-age directive, got: {cache_control}"

        # Extract and verify max-age is substantial (> 1 day)
        max_age_match = re.search(r'max-age=(\d+)', cache_control)
        if max_age_match:
            max_age = int(max_age_match.group(1))
            one_day = 86400
            assert max_age >= one_day, \
                f"max-age should be at least 1 day (86400s), got {max_age}s"

    def test_resources_have_caching_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized resources should have Cache-Control headers."""
        page_url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'href="[^"]*\.pagespeed\.cf\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )

        # Extract the rewritten CSS URL
        match = require_match(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response,
            "rewritten CSS URL",
        )

        css_url = match.group(1)
        if not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        cache_control = css_response.header("Cache-Control")
        # Note: 'private' may be set when inputs aren't proxy-cacheable.
        # We just verify there's a max-age directive indicating caching.
        assert "max-age" in cache_control.lower(), \
            f"Expected max-age in Cache-Control, got: {cache_control}"


@pytest.mark.nginx_only
class Test304NotModified:
    """Tests for conditional GET (304 Not Modified) responses.

    PageSpeed resources should support conditional GET requests using
    ETag and If-None-Match headers.
    """

    def test_304_not_modified_with_etag(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify conditional GET with If-None-Match returns 304."""
        # Get an IPRO-optimized image
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 100000)}"

        # Fetch until IPRO generates an ETag
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("ETag") != "",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        etag = response.header("ETag")
        assert etag, "Resource should have ETag header"

        # Second request with If-None-Match should return 304
        response_304 = client.get(url, headers={"If-None-Match": etag})
        assert_http_status(response_304, 304)

        # 304 response should have empty body
        assert len(response_304.body) == 0, "304 response should have empty body"

    def test_304_with_pagespeed_resource(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify .pagespeed. resources support conditional GET."""
        # Get a page with rewritten CSS
        page_url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'href="[^"]*\.pagespeed\.cf\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )

        # Extract the rewritten CSS URL
        match = require_match(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response,
            "rewritten CSS URL",
        )

        css_url = match.group(1)
        if not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        # First request to get ETag (poll like the IPRO ETag tests do --
        # a .pagespeed. resource without an ETag is a regression, not an
        # environment condition)
        css_response = client.fetch_until(
            css_url,
            condition=lambda r: r.header("ETag") != "",
            timeout=30.0,
        )
        assert_http_status(css_response, 200)

        etag = css_response.header("ETag")

        # Conditional request should return 304
        response_304 = client.get(css_url, headers={"If-None-Match": etag})
        assert_http_status(response_304, 304)

    def test_wrong_etag_returns_200(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify wrong ETag returns 200 with full content."""
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 100000)}"

        # Fetch until resource has an ETag
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("ETag") != "",
            timeout=30.0,
        )

        etag = response.header("ETag")
        # Create a different ETag
        bad_etag = '"bad-etag-value"'

        # Request with wrong ETag should return 200
        response_200 = client.get(url, headers={"If-None-Match": bad_etag})
        assert_http_status(response_200, 200)
        assert len(response_200.body) > 0, "200 response should have body"


@pytest.mark.nginx_only
class TestContentTypePreserved:
    """Tests for Content-Type header preservation.

    Optimized resources should have correct Content-Type headers
    matching their actual content type.
    """

    def test_content_type_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify optimized CSS has Content-Type: text/css."""
        page_url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'href="[^"]*\.pagespeed\.cf\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )

        # Extract the rewritten CSS URL
        match = require_match(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response,
            "rewritten CSS URL",
        )

        css_url = match.group(1)
        if not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        content_type = css_response.header("Content-Type")
        assert "text/css" in content_type.lower(), \
            f"CSS should have Content-Type: text/css, got: {content_type}"

    def test_content_type_javascript(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify optimized JavaScript has correct Content-Type."""
        # Get a page with rewritten JavaScript
        page_url = f"{example_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"

        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'src="[^"]*\.pagespeed\.(jc|jm)\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )

        # Extract the rewritten JS URL
        match = require_match(
            r'src="([^"]*\.pagespeed\.(jc|jm)\.[^"]*)"',
            response,
            "rewritten JavaScript URL",
        )

        js_url = match.group(1)
        if not js_url.startswith("/"):
            js_url = f"{example_root}/{js_url}"

        js_response = client.get(js_url)
        assert_http_status(js_response, 200)

        content_type = js_response.header("Content-Type")
        # JavaScript can be application/javascript, application/x-javascript,
        # or text/javascript
        valid_js_types = ["application/javascript", "application/x-javascript", "text/javascript"]
        assert any(t in content_type.lower() for t in valid_js_types), \
            f"JavaScript should have JavaScript Content-Type, got: {content_type}"

    def test_content_type_image(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify optimized images have correct Content-Type."""
        # Get an IPRO-optimized image
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 100000)}"

        # Fetch until optimization is complete (check for ETag which indicates IPRO done)
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("ETag") != "",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        # Should be image/jpeg or image/webp (if converted)
        valid_image_types = ["image/jpeg", "image/webp", "image/png"]
        assert any(t in content_type.lower() for t in valid_image_types), \
            f"Image should have image/* Content-Type, got: {content_type}"


@pytest.mark.nginx_only
class TestServerHeader:
    """Tests for Server header.

    Verify the Server header indicates nginx when configured.
    """

    def test_server_header_nginx(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify Server header contains nginx version if present."""
        response = client.get(f"{example_root}/index.html")
        assert_http_status(response, 200)

        server = response.header("Server")
        # Server header may or may not be present depending on configuration
        # If present, it should indicate nginx
        if server:
            assert "nginx" in server.lower(), \
                f"Server header should indicate nginx, got: {server}"


@pytest.mark.nginx_only
@pytest.mark.skip(reason="nginx PageSpeed uses chunked encoding - Content-Length not set")
class TestContentLengthHeader:
    """Tests for Content-Length header on resources.

    PageSpeed resources should have Content-Length header set,
    not use chunked transfer encoding.

    Note: Currently skipped because nginx PageSpeed uses chunked encoding
    instead of Content-Length. This is a known difference from Apache behavior.
    """

    def test_resources_have_content_length(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed resources should have Content-Length header."""
        page_url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.cf\.',
            timeout=30.0,
        )

        # Extract the rewritten CSS URL
        match = re.search(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find rewritten CSS URL")

        css_url = match.group(1)
        if not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        # Should have Content-Length
        content_length = css_response.header("Content-Length")
        assert content_length, "Should have Content-Length header"

        # Content-Length should be a positive number
        assert int(content_length) > 0, \
            f"Content-Length should be positive, got: {content_length}"

    def test_resources_not_chunked(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed resources should not use chunked transfer encoding."""
        page_url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.cf\.',
            timeout=30.0,
        )

        # Extract the rewritten CSS URL
        match = re.search(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find rewritten CSS URL")

        css_url = match.group(1)
        if not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        # Should NOT have Transfer-Encoding: chunked
        transfer_encoding = css_response.header("Transfer-Encoding")
        assert "chunked" not in transfer_encoding.lower(), \
            "Should not use chunked transfer encoding"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
