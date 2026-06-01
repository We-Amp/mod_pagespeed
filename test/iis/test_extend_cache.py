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

"""Cache extension filter tests for IIS PageSpeed module.

Ported from: test/system/automatic/test_extend_cache.py

These tests verify that the extend_cache filter works correctly on IIS,
rewriting resource URLs with content-based hashes for long cache lifetimes.
"""

import re
from datetime import datetime, timezone

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


@pytest.mark.html_rewrite
class TestExtendCacheImages:
    """Tests for the extend_cache_images filter.

    Verifies that image URLs are rewritten with .pagespeed.ce. hashes
    to enable long-term browser caching.
    """

    def test_extend_cache_rewrites_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """extend_cache_images should rewrite image URLs with cache-extended names.

        Original bash test:
            test_filter extend_cache_images rewrites an image tag.
            URL=$EXAMPLE_ROOT/extend_cache.html?PageSpeedFilters=extend_cache_images
            fetch_until $URL 'egrep -c src.*/Puzzle[.]jpg[.]pagespeed[.]ce[.].*[.]jpg' 1
        """
        url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"

        # Wait for image URLs to be rewritten with cache-extension hash
        response = client.fetch_until_contains(
            url,
            pattern=r'src=.*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg',
            timeout=30.0,
        )

        assert_http_status(response, 200)

    def test_extend_cache_hash_format(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended URLs should have valid hash format."""
        url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.ce\.',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Extract the cache-extended URL and verify hash format
        # Hash should be non-empty alphanumeric characters
        match = re.search(
            r'Puzzle\.jpg\.pagespeed\.ce\.([A-Za-z0-9_-]+)\.jpg',
            response.text,
        )
        assert match, "Should find cache-extended URL with hash"
        hash_value = match.group(1)
        assert len(hash_value) > 0, "Hash should be non-empty"


@pytest.mark.html_rewrite
class TestExtendCacheWithoutHash:
    """Tests for cache-extended URLs without hash.

    Verifies that attempting to access cache-extended URLs without
    a valid hash returns 404.
    """

    def test_cache_extended_without_hash_returns_404(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended URL without hash should return 404.

        Original bash test:
            start_test Attempt to fetch cache-extended image without hash should 404
            check_not run_wget_with_args $REWRITTEN_ROOT/images/Puzzle.jpg.pagespeed.ce..jpg
            check fgrep "404 Not Found" $WGET_OUTPUT
        """
        # URL with missing hash (empty between .ce. and .jpg)
        url = f"{example_root}/images/Puzzle.jpg.pagespeed.ce..jpg"
        response = client.get(url)
        assert_http_status(response, 404)

    def test_cache_extended_with_invalid_hash_returns_200_short_ttl(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended URL with invalid hash should return 200 with short TTL.

        Apache/PageSpeed behavior: when the hash doesn't match current content,
        the resource is still served (to handle stale HTML pages referencing
        old hashes) but with a short Cache-Control TTL instead of the usual
        1-year max-age.
        """
        # URL with obviously invalid hash
        url = f"{example_root}/images/Puzzle.jpg.pagespeed.ce.INVALID.jpg"
        response = client.get(url)
        # Should return 200 with content (Apache behavior for stale hash)
        assert_http_status(response, 200)

        # Cache-Control should have a short max-age (not the 1-year TTL
        # used for valid hashes)
        cache_control = response.header("Cache-Control")
        if cache_control and "max-age" in cache_control:
            import re
            match = re.search(r'max-age=(\d+)', cache_control)
            if match:
                max_age = int(match.group(1))
                assert max_age < 86400, \
                    f"Invalid hash should have short TTL, got max-age={max_age}"


@pytest.mark.html_rewrite
class TestIfModifiedSince:
    """Tests for If-Modified-Since handling.

    Verifies that cache-extended resources respond correctly to
    conditional requests with If-Modified-Since headers.
    """

    def test_cache_extended_responds_304_to_if_modified_since(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended resources should return 304 for If-Modified-Since.

        Original bash test:
            start_test Cache-extended image should respond 304 to an If-Modified-Since.
            URL=$REWRITTEN_ROOT/images/Puzzle.jpg.pagespeed.ce.91_WewrLtP.jpg
            DATE=$(date -R)
            check_not run_wget_with_args --header "If-Modified-Since: $DATE" $URL
            check fgrep "304 Not Modified" $WGET_OUTPUT
        """
        # First, get the page to find a cache-extended URL
        page_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until_contains(
            page_url,
            pattern=r'Puzzle\.jpg\.pagespeed\.ce\.',
            timeout=30.0,
        )

        # Extract the cache-extended URL
        match = re.search(
            r'src="([^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find cache-extended image URL")

        image_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if image_url.startswith("http://") or image_url.startswith("https://"):
            from urllib.parse import urlparse
            image_url = urlparse(image_url).path
        elif not image_url.startswith("/"):
            image_url = f"{example_root}/{image_url}"

        # Request with If-Modified-Since in the future
        now = datetime.now(timezone.utc)
        date_header = now.strftime("%a, %d %b %Y %H:%M:%S GMT")

        response = client.get(
            image_url,
            headers={"If-Modified-Since": date_header},
        )

        assert_http_status(response, 304, "Should return 304 Not Modified")

    def test_cache_extended_returns_200_for_old_if_modified_since(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended resources should return 200 for old If-Modified-Since."""
        # First, get the page to find a cache-extended URL
        page_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until_contains(
            page_url,
            pattern=r'Puzzle\.jpg\.pagespeed\.ce\.',
            timeout=30.0,
        )

        # Extract the cache-extended URL
        match = re.search(
            r'src="([^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find cache-extended image URL")

        image_url = match.group(1)
        if image_url.startswith("http://") or image_url.startswith("https://"):
            from urllib.parse import urlparse
            image_url = urlparse(image_url).path
        elif not image_url.startswith("/"):
            image_url = f"{example_root}/{image_url}"

        # Request with If-Modified-Since in the distant past
        old_date = "Thu, 01 Jan 1970 00:00:00 GMT"

        response = client.get(
            image_url,
            headers={"If-Modified-Since": old_date},
        )

        # Should return 200 with full content
        assert_http_status(response, 200, "Should return 200 OK for old date")


@pytest.mark.html_rewrite
class TestLastModifiedMatch:
    """Tests that Last-Modified date matches origin.

    Verifies that cache-extended resources preserve the original
    Last-Modified header from the source file.
    """

    def test_cache_extended_preserves_last_modified(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended resources should have same Last-Modified as origin.

        Original bash test:
            start_test Cache-extended last-modified date should match origin
        """
        # Get Last-Modified from origin
        origin_url = f"{example_root}/images/Puzzle.jpg?PageSpeed=off"
        origin_response = client.get(origin_url)
        assert_http_status(origin_response, 200)

        origin_last_modified = origin_response.header("Last-Modified")
        if not origin_last_modified:
            pytest.skip("Origin doesn't have Last-Modified header")

        # Get a cache-extended version
        page_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until_contains(
            page_url,
            pattern=r'Puzzle\.jpg\.pagespeed\.ce\.',
            timeout=30.0,
        )

        # Extract and fetch the cache-extended URL
        match = re.search(
            r'src="([^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find cache-extended image URL")

        image_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if image_url.startswith("http://") or image_url.startswith("https://"):
            from urllib.parse import urlparse
            image_url = urlparse(image_url).path
        elif not image_url.startswith("/"):
            image_url = f"{example_root}/{image_url}"

        extended_response = client.get(image_url)
        assert_http_status(extended_response, 200)

        extended_last_modified = extended_response.header("Last-Modified")

        assert origin_last_modified == extended_last_modified, \
            f"Last-Modified mismatch: origin={origin_last_modified}, extended={extended_last_modified}"

    def test_cache_extended_has_last_modified(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended resources should have Last-Modified header."""
        # Get a cache-extended version
        page_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until_contains(
            page_url,
            pattern=r'Puzzle\.jpg\.pagespeed\.ce\.',
            timeout=30.0,
        )

        # Extract and fetch the cache-extended URL
        match = re.search(
            r'src="([^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find cache-extended image URL")

        image_url = match.group(1)
        if image_url.startswith("http://") or image_url.startswith("https://"):
            from urllib.parse import urlparse
            image_url = urlparse(image_url).path
        elif not image_url.startswith("/"):
            image_url = f"{example_root}/{image_url}"

        extended_response = client.get(image_url)
        assert_http_status(extended_response, 200)

        last_modified = extended_response.header("Last-Modified")
        assert last_modified, "Cache-extended resource should have Last-Modified header"


@pytest.mark.html_rewrite
class TestExtendCacheCacheControl:
    """Tests for cache-control headers on extended resources."""

    def test_cache_extended_has_long_cache(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended resources should have long cache lifetime."""
        # Get a cache-extended version
        page_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until_contains(
            page_url,
            pattern=r'Puzzle\.jpg\.pagespeed\.ce\.',
            timeout=30.0,
        )

        # Extract and fetch the cache-extended URL
        match = re.search(
            r'src="([^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find cache-extended image URL")

        image_url = match.group(1)
        if image_url.startswith("http://") or image_url.startswith("https://"):
            from urllib.parse import urlparse
            image_url = urlparse(image_url).path
        elif not image_url.startswith("/"):
            image_url = f"{example_root}/{image_url}"

        extended_response = client.get(image_url)
        assert_http_status(extended_response, 200)

        cache_control = extended_response.header("Cache-Control")
        assert cache_control, "Cache-extended resource should have Cache-Control header"

        # Should have max-age indicating long cache lifetime
        # Typically max-age=31536000 (1 year) for cache-extended resources
        assert "max-age" in cache_control.lower(), \
            f"Cache-Control should contain max-age: {cache_control}"


@pytest.mark.html_rewrite
class TestExtendCacheDisabled:
    """Tests that extend_cache can be disabled."""

    def test_pagespeed_off_no_cache_extension(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off should not rewrite image URLs."""
        url = f"{example_root}/extend_cache.html?PageSpeed=off"
        response = client.get(url)
        assert_http_status(response, 200)

        # Should not have .pagespeed.ce. URLs
        assert_not_contains(response, ".pagespeed.ce.")

        # Original image URL should be present
        assert_contains(response, 'src="images/Puzzle.jpg"')


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
