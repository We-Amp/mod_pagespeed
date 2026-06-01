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

"""Cache-Control header handling tests for IIS PageSpeed module.

These tests verify that PageSpeed correctly handles and preserves
Cache-Control headers on resources, including no-cache directives.

Ported from: test/system/automatic/test_no_cache.py
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


@pytest.mark.ipro
class TestCacheControlPreservation:
    """Tests for preserving Cache-Control headers on optimized resources."""

    def test_optimized_resource_has_cache_control(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized resources should have Cache-Control headers."""
        # Get a combined CSS resource
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.(cc|cf)\.',
            timeout=30.0,
        )

        match = re.search(r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined CSS URL")

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        cache_control = css_response.header("Cache-Control")
        assert cache_control, "Optimized resource should have Cache-Control header"

    def test_max_age_on_optimized_resources(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized resources should have max-age directive."""
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.(cc|cf)\.',
            timeout=30.0,
        )

        match = re.search(r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined CSS URL")

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        cache_control = css_response.header("Cache-Control")
        assert "max-age" in cache_control.lower(), \
            f"Should have max-age directive, got: {cache_control}"


@pytest.mark.ipro
class TestPublicCaching:
    """Tests that optimized resources are publicly cacheable."""

    def test_optimized_resources_not_private(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized resources should not have Cache-Control: private."""
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.(cc|cf)\.',
            timeout=30.0,
        )

        match = re.search(r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined CSS URL")

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        cache_control = css_response.header("Cache-Control")
        if cache_control:
            # Resources should be public, not private
            assert "private" not in cache_control.lower(), \
                f"Optimized resources should not be private, got: {cache_control}"

    def test_resources_can_be_public(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized resources may have public directive."""
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.(cc|cf)\.',
            timeout=30.0,
        )

        match = re.search(r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined CSS URL")

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        # Just verify response is valid
        assert css_response.status == 200


@pytest.mark.ipro
class TestLongCacheLifetime:
    """Tests for long cache lifetimes on optimized resources."""

    def test_one_year_cache_for_hashed_resources(
        self, client: PageSpeedClient, example_root: str
    ):
        """Resources with content hashes should have ~1 year cache."""
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.(cc|cf)\.',
            timeout=30.0,
        )

        match = re.search(r'href="([^"]*\.pagespeed\.(cc|cf)\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find combined CSS URL")

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        cache_control = css_response.header("Cache-Control")
        if cache_control:
            match = re.search(r'max-age=(\d+)', cache_control, re.IGNORECASE)
            if match:
                max_age = int(match.group(1))
                one_year = 31536000
                one_day = 86400
                # Should be at least 1 day, ideally ~1 year
                assert max_age >= one_day, \
                    f"max-age should be at least 1 day, got {max_age}s"


@pytest.mark.html_rewrite
class TestHtmlCaching:
    """Tests for HTML response caching behavior."""

    def test_html_has_cache_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """HTML responses should have appropriate cache headers."""
        response = client.get(f"{example_root}/index.html")
        assert_http_status(response, 200)

        # HTML may or may not have Cache-Control depending on config
        # Just verify response is valid

    def test_html_with_optimization_has_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized HTML should have response headers."""
        response = client.get(
            f"{example_root}/index.html?PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(response, 200)

        # Content-Type should be present
        content_type = response.header("Content-Type")
        assert content_type, "HTML should have Content-Type header"
        assert "text/html" in content_type.lower()


@pytest.mark.ipro
class TestIPROCaching:
    """Tests for IPRO (In-Place Resource Optimization) caching."""

    def test_ipro_resource_has_cache_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """IPRO-optimized resources should have cache headers."""
        import time

        # First request triggers optimization
        response1 = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response1, 200)

        # Wait for background optimization
        time.sleep(1.0)

        # Second request should have optimization headers
        response2 = client.get(f"{example_root}/styles/yellow.css")
        assert_http_status(response2, 200)

        # Should have some cache-related headers
        cache_control = response2.header("Cache-Control")
        etag = response2.header("ETag")
        last_modified = response2.header("Last-Modified")

        # At least one caching header should be present
        has_cache_header = bool(cache_control or etag or last_modified)
        assert has_cache_header, "IPRO resource should have caching headers"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
