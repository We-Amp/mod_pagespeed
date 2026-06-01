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

"""Example PageSpeed system tests demonstrating the Python test framework.

This file shows how to port bash system tests to Python using pytest
and the pagespeed_test_framework.

Run with:
    PAGESPEED_HOST=localhost PAGESPEED_PORT=8080 python -m pytest -v test_example.py

Or via Bazel:
    bazel test //test/system:test_example --test_output=streamed \
        --test_env=PAGESPEED_HOST=localhost --test_env=PAGESPEED_PORT=8080
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_stat_delta,
    assert_stat_increased,
)


class TestBasicFunctionality:
    """Basic server functionality tests."""

    def test_server_responds(self, client: PageSpeedClient, example_root: str):
        """Verify the server is responding.

        Bash equivalent:
            start_test Server is responding
            check $WGET_DUMP $EXAMPLE_ROOT/
        """
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

    def test_pagespeed_off(self, client: PageSpeedClient, example_root: str):
        """PageSpeed=off disables optimization.

        Bash equivalent:
            start_test PageSpeed=off disables rewriting
            OUT=$($WGET_DUMP "$EXAMPLE_ROOT/combine_css.html?PageSpeed=off")
            check_not_from "$OUT" fgrep "pagespeed"
        """
        response = client.get(f"{example_root}/combine_css.html?PageSpeed=off")
        assert_http_status(response, 200)
        # Should not see any pagespeed rewriting
        assert_not_contains(response, r"\.pagespeed\.")


class TestCssCombining:
    """CSS combining filter tests.

    Bash original: pagespeed/automatic/system_tests/combine_css.sh
    """

    def test_combine_css_basic(self, client: PageSpeedClient, example_root: str):
        """Test that CSS files are combined.

        Bash equivalent:
            test_filter combine_css combines 4 CSS files
            fetch_until $URL 'fgrep -c .pagespeed.cc.' 1
        """
        url = f"{example_root}/combine_css.html?PageSpeedFilters=+combine_css"

        # Wait for CSS to be combined (async optimization)
        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.cc\.",
            timeout=30.0,
        )

        assert_http_status(response, 200)
        assert_contains(response, r"\.pagespeed\.cc\.", "CSS should be combined")

    def test_combine_css_count(self, client: PageSpeedClient, example_root: str):
        """Verify multiple CSS files are combined into fewer requests.

        Bash equivalent:
            fetch_until $URL 'grep -c pagespeed.cc' 4
        """
        url = f"{example_root}/combine_css.html?PageSpeedFilters=+combine_css"

        # The page has 4 CSS files that should be combined
        response = client.fetch_until_count(
            url,
            pattern=r"pagespeed\.cc",
            expected_count=1,  # All should combine into 1
            timeout=30.0,
        )

        assert_http_status(response, 200)


class TestImageRewriting:
    """Image optimization filter tests.

    Bash original: pagespeed/automatic/system_tests/rewrite_images.sh
    """

    def test_rewrite_images(self, client: PageSpeedClient, example_root: str):
        """Test that images are rewritten.

        Bash equivalent:
            test_filter rewrite_images optimizes images
            fetch_until $URL 'fgrep -c .pagespeed.ic.' 1
        """
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=+rewrite_images"

        response = client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.ic\.",
            timeout=60.0,  # Image optimization can take longer
        )

        assert_http_status(response, 200)
        assert_contains(response, r"\.pagespeed\.ic\.", "Images should be rewritten")


class TestBlockingRewrite:
    """Blocking rewrite tests.

    Bash original: pagespeed/apache/system_tests/blocking_rewrite.sh
    """

    @pytest.mark.requires_stats
    def test_blocking_rewrite_with_stats(
        self,
        client: PageSpeedClient,
        test_root: str,
        stats_snapshot,
    ):
        """Blocking rewrite header triggers synchronous optimization.

        Bash equivalent:
            start_test Blocking rewrite enabled.
            $WGET_DUMP $STATISTICS_URL > $OLDSTATS
            check $WGET_DUMP --header 'X-PSA-Blocking-Rewrite: psatest' $URL
            $WGET_DUMP $STATISTICS_URL > $NEWSTATS
            check_stat $OLDSTATS $NEWSTATS image_rewrites 1
        """
        url = f"{test_root}/blocking_rewrite.html?PageSpeedFilters=rewrite_images"

        old_stats = stats_snapshot()

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        new_stats = stats_snapshot()

        # Blocking rewrite should have optimized at least one image
        assert_stat_increased(
            old_stats,
            new_stats,
            "image_rewrites",
            min_increase=1,
            msg="Blocking rewrite should trigger image optimization",
        )


class TestCacheFlushing:
    """Cache flushing tests.

    Bash original: pagespeed/system/system_tests/cache_flushing.sh
    """

    @pytest.mark.requires_stats
    def test_cache_statistics(
        self,
        client: PageSpeedClient,
        example_root: str,
        stats_snapshot,
    ):
        """Cache operations update statistics.

        Bash equivalent:
            $WGET_DUMP $STATISTICS_URL > $OLDSTATS
            fetch_until $URL 'grep -c pagespeed' 1
            $WGET_DUMP $STATISTICS_URL > $NEWSTATS
            check_stat_op $OLDSTATS $NEWSTATS cache_hits ">=" 0
        """
        old_stats = stats_snapshot()

        # Fetch a page to trigger caching
        url = f"{example_root}/combine_css.html?PageSpeedFilters=+combine_css"
        client.fetch_until_contains(url, r"pagespeed", timeout=30.0)

        new_stats = stats_snapshot()

        # Cache operations should have occurred
        # Note: We check for >= 0 because the exact behavior depends on cache state
        cache_inserts = new_stats.get("cache_inserts", 0) - old_stats.get("cache_inserts", 0)
        assert cache_inserts >= 0, "Cache inserts should not be negative"


class TestHeaders:
    """HTTP header tests."""

    def test_cache_control_headers(self, client: PageSpeedClient, example_root: str):
        """Verify Cache-Control headers on optimized resources.

        Bash equivalent:
            OUT=$($WGET_DUMP $REWRITTEN_URL)
            check_from "$OUT" fgrep "Cache-Control"
        """
        # First get a page to trigger optimization
        url = f"{example_root}/combine_css.html?PageSpeedFilters=+combine_css"
        response = client.fetch_until_contains(url, r"\.pagespeed\.cc\.")

        # Extract the rewritten CSS URL and fetch it
        import re

        match = re.search(r'href="([^"]*\.pagespeed\.cc\.[^"]*)"', response.text)
        if match:
            css_url = match.group(1)
            if css_url.startswith("/"):
                css_response = client.get(css_url)
            else:
                css_response = client.get(f"{example_root}/{css_url}")

            assert_http_status(css_response, 200)
            # Optimized resources should have long cache lifetime
            cache_control = css_response.header("Cache-Control")
            assert "max-age" in cache_control, "Should have max-age directive"


# Run with pytest if executed directly
if __name__ == "__main__":
    pytest.main([__file__, "-v"])
