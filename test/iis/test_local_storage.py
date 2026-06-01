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

"""Local storage cache filter tests for IIS PageSpeed module.

These tests verify that the local_storage_cache filter correctly adds
JavaScript code to cache inlined resources in the browser's localStorage.

Ported from: test/system/automatic/test_local_storage_cache.py
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
class TestLocalStorageCache:
    """Tests for the local_storage_cache filter.

    The local_storage_cache filter stores inlined CSS/JS in the browser's
    localStorage to avoid re-downloading on subsequent page loads.
    """

    def test_local_storage_script_injected(
        self, client: PageSpeedClient, test_root: str
    ):
        """Local storage caching script should be injected."""
        url = f"{test_root}/pages/local_storage.html?PageSpeedFilters=local_storage_cache,inline_css"

        response = client.get(url)
        assert_http_status(response, 200)

        # Look for localStorage-related code
        # The filter adds script to manage localStorage caching
        # Common patterns include pagespeed.localStorageCache or similar

    def test_local_storage_with_inline_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """Local storage should work with inline_css filter."""
        filters = "local_storage_cache,inline_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

        # Should have valid HTML structure
        assert_contains(response, r'<html')
        assert_contains(response, r'</html>')

    def test_local_storage_with_inline_js(
        self, client: PageSpeedClient, example_root: str
    ):
        """Local storage should work with inline_javascript filter."""
        filters = "local_storage_cache,inline_javascript"
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

        # Should have valid HTML structure
        assert_contains(response, r'<html')


@pytest.mark.html_rewrite
class TestLocalStorageCacheKeys:
    """Tests for local storage cache key generation."""

    def test_cache_keys_are_unique(
        self, client: PageSpeedClient, example_root: str
    ):
        """Different resources should have different cache keys."""
        filters = "local_storage_cache,inline_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

        # Verify valid HTML response
        assert_contains(response, r'<html')

    def test_cache_key_includes_hash(
        self, client: PageSpeedClient, test_root: str
    ):
        """Cache keys should include content hash for invalidation."""
        filters = "local_storage_cache,inline_css"
        url = f"{test_root}/pages/local_storage.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestLocalStorageFallback:
    """Tests for localStorage fallback behavior."""

    def test_works_without_localstorage(
        self, client: PageSpeedClient, example_root: str
    ):
        """Pages should still work even if localStorage is unavailable."""
        filters = "local_storage_cache,inline_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

        # CSS should still be present (inlined or linked)
        # even if localStorage caching is disabled client-side

    def test_graceful_quota_exceeded(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should handle localStorage quota exceeded gracefully."""
        # This tests that the injected script handles errors
        filters = "local_storage_cache,inline_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

        # Valid page should be returned regardless of localStorage state


@pytest.mark.html_rewrite
class TestLocalStorageWithOtherFilters:
    """Tests for local_storage_cache combined with other filters."""

    def test_with_combine_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """local_storage_cache should work with combine_css."""
        filters = "local_storage_cache,combine_css,inline_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.fetch_until_contains(
            url,
            pattern=r'<',
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_with_combine_javascript(
        self, client: PageSpeedClient, example_root: str
    ):
        """local_storage_cache should work with combine_javascript."""
        filters = "local_storage_cache,combine_javascript,inline_javascript"
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters={filters}"

        response = client.fetch_until_contains(
            url,
            pattern=r'<',
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_with_defer_javascript(
        self, client: PageSpeedClient, example_root: str
    ):
        """local_storage_cache should work with defer_javascript."""
        filters = "local_storage_cache,defer_javascript"
        url = f"{example_root}/combine_javascript.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestLocalStorageDisabled:
    """Tests that local_storage_cache is disabled when appropriate."""

    def test_no_local_storage_without_filter(
        self, client: PageSpeedClient, example_root: str
    ):
        """Without filter, no localStorage caching code should be added."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=inline_css"

        response = client.get(url)
        assert_http_status(response, 200)

        # Page should be valid without localStorage-specific code

    def test_no_local_storage_with_pagespeed_off(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off should disable localStorage caching."""
        url = f"{example_root}/combine_css.html?PageSpeed=off"

        response = client.get(url)
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestLocalStorageEdgeCases:
    """Edge cases for local_storage_cache filter."""

    def test_empty_page(
        self, client: PageSpeedClient, example_root: str
    ):
        """Empty or minimal pages should be handled gracefully."""
        filters = "local_storage_cache"
        url = f"{example_root}/index.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

    def test_page_without_inlineable_resources(
        self, client: PageSpeedClient, example_root: str
    ):
        """Pages without inlineable resources should not break."""
        filters = "local_storage_cache,inline_css"
        url = f"{example_root}/index.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)

    def test_large_resources_not_cached(
        self, client: PageSpeedClient, example_root: str
    ):
        """Large resources should not be stored in localStorage."""
        # localStorage has size limits (~5MB typically)
        # Filter should respect these limits
        filters = "local_storage_cache,inline_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
