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

"""Local storage cache filter tests.

Ported from: pagespeed/automatic/system_tests/local_storage_cache.sh

These tests verify that the local_storage_cache filter correctly caches
resources in browser local storage.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    require_match,
)


class TestLocalStorageCacheOptimizeMode:
    r"""Tests for local_storage_cache in optimize mode.

    Bash original::

        test_filter local_storage_cache,inline_css,inline_images optimize mode
        WGET_ARGS="${WGET_ARGS} --header=X-PSA-Blocking-Rewrite:psatest"
        check run_wget_with_args "$URL"
        check grep -q "pagespeed.localStorageCacheInit()" $FETCHED
        check [ $(grep -c ' data-pagespeed-lsc-url=' $FETCHED) = 2 ]
        check grep -q "yellow {background-color: yellow" $FETCHED
        check grep -q "<img src=\"data:image/png;base64" $FETCHED
        check grep -q "<img .* alt=\"A cup of joe\"" $FETCHED
        check_not grep -q "/\*" $FETCHED
        check grep -q "PageSpeed=noscript" $FETCHED
    """

    def test_local_storage_cache_injects_init(
        self, client: PageSpeedClient, example_root: str
    ):
        """local_storage_cache should inject init function."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images"

        response = client.fetch_until_contains(
            url,
            pattern=r"pagespeed\.localStorageCacheInit\(\)",
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_local_storage_cache_adds_lsc_attributes(
        self, client: PageSpeedClient, example_root: str
    ):
        """local_storage_cache should add data-pagespeed-lsc-url attributes."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r'data-pagespeed-lsc-url=',
            expected_count=2,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_local_storage_cache_inlines_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS should be inlined with local_storage_cache."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images"

        response = client.fetch_until_contains(
            url,
            pattern=r"yellow.*background-color:.*yellow",
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_local_storage_cache_inlines_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """Images should be inlined as data URIs."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'<img src="data:image/png;base64',
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_local_storage_cache_preserves_alt(
        self, client: PageSpeedClient, example_root: str
    ):
        """Image alt attributes should be preserved."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'<img.*alt="A cup of joe"',
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_local_storage_cache_has_noscript(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should have noscript fallback."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images"

        response = client.fetch_until_contains(
            url,
            pattern=r"PageSpeed=noscript",
            timeout=30.0,
        )
        assert_http_status(response, 200)


class TestLocalStorageCacheDebugMode:
    r"""Tests for local_storage_cache in debug mode.

    Bash original::

        test_filter local_storage_cache,inline_css,inline_images,debug debug mode
        WGET_ARGS="${WGET_ARGS} --header=X-PSA-Blocking-Rewrite:psatest"
        check run_wget_with_args "$URL"
        check grep -q "pagespeed.localStorageCacheInit()" $FETCHED
        check_not grep -q "/\*" $FETCHED
        check_not grep -q "goog.require" $FETCHED
        check grep -q "PageSpeed=noscript" $FETCHED
    """

    def test_debug_mode_has_init_function(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should still have init function."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images,debug"

        response = client.fetch_until_contains(
            url,
            pattern=r"pagespeed\.localStorageCacheInit\(\)",
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_debug_mode_no_goog_require(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should not have unresolved goog.require."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images,debug"

        # Wait for rewriting to complete by checking for positive indicator
        response = client.fetch_until_contains(
            url,
            pattern=r"pagespeed\.localStorageCacheInit\(\)",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_not_contains(
            response,
            r"goog\.require",
            "Debug mode should not have unresolved goog.require",
        )


class TestLocalStorageCacheCookie:
    """Tests for local_storage_cache with cookie indicating cached resources.

    Bash original::

        # Checks that local_storage_cache doesn't send the inlined data for a resource
        # whose hash is in the magic cookie.
        HASHES=$(grep "data-pagespeed-lsc-hash=" $FETCHED |...)
        COOKIE="Cookie: _GPSLSC=$HASHES"
        # Fetch with the cookie set.
        test_filter local_storage_cache,inline_css,inline_images cookies set
        check run_wget_with_args --save-headers --no-cookies --header "$COOKIE" $URL
        # Check that this run did NOT inline the data.
        check_not fgrep "yellow {background-color: yellow" $FETCHED
        check_not grep "src=.data:image/png;base64," $FETCHED
    """

    def test_with_cache_cookie_skips_inlining(
        self, client: PageSpeedClient, example_root: str
    ):
        """With cache cookie, inlined data should not be sent."""
        url = f"{example_root}/local_storage_cache.html?PageSpeedFilters=local_storage_cache,inline_css,inline_images"

        # First request: wait for rewriting to complete and get the hashes
        response1 = client.fetch_until_contains(
            url,
            pattern=r'data-pagespeed-lsc-hash="[^"]*"',
            timeout=30.0,
        )
        assert_http_status(response1, 200)

        # Extract hashes from data-pagespeed-lsc-hash attributes
        require_match(
            r'data-pagespeed-lsc-hash="[^"]*"',
            response1,
            "data-pagespeed-lsc-hash attributes",
        )
        hashes = re.findall(r'data-pagespeed-lsc-hash="([^"]*)"', response1.text)

        # Create the cookie with hashes
        cookie_value = "!".join(hashes)

        # Second request with cookie set - wait for rewriting to complete
        # then check that inlining is skipped
        response2 = client.fetch_until_contains(
            url,
            pattern=r"pagespeed\.localStorageCacheInit\(\)",
            timeout=30.0,
            headers={
                "Cookie": f"_GPSLSC={cookie_value}",
            },
        )
        assert_http_status(response2, 200)

        # CSS should NOT be inlined
        assert_not_contains(
            response2,
            r"yellow \{background-color: yellow",
            "CSS should not be inlined when cookie indicates cached",
        )

        # Images should NOT be inlined as data URIs
        assert_not_contains(
            response2,
            r'src="data:image/png;base64,',
            "Images should not be inlined when cookie indicates cached",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
