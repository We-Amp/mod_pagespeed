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

"""HTTP header and response tests.

Ported from: pagespeed/system/system_tests/check_headers.sh
             pagespeed/system/system_tests/resource_404_count.sh

These tests verify correct HTTP header handling.
"""

import random
import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_stat_delta,
)


class TestPageSpeedHeader:
    """Tests for X-Page-Speed / X-Mod-PageSpeed header.

    Bash original:
        start_test Check for correct default pagespeed header format.
        OUT=$($WGET_DUMP $EXAMPLE_ROOT/combine_css.html)
        check_from "$OUT" egrep -q '^X-(Mod-Pagespeed|Page-Speed): ...'
    """

    def test_pagespeed_version_header_present(
        self, client: PageSpeedClient, example_root: str
    ):
        """Response should include X-Page-Speed or X-Mod-Pagespeed header."""
        response = client.get(f"{example_root}/combine_css.html")
        assert_http_status(response, 200)

        # Check for either Apache (X-Mod-Pagespeed) or Nginx (X-Page-Speed) header
        mod_pagespeed = response.header("X-Mod-Pagespeed")
        page_speed = response.header("X-Page-Speed")

        header_value = mod_pagespeed or page_speed
        assert header_value, \
            "Expected X-Mod-Pagespeed or X-Page-Speed header"

        # Header should contain version number (e.g., 1.1.0-beta.1 or 1.13.35.2-0)
        version_pattern = r"\d+\.\d+\.\d+[\w.\-]*"
        assert re.search(version_pattern, header_value), \
            f"Header should contain version number, got: {header_value}"


class TestDefaultFiltering:
    """Tests that PageSpeed is doing more than pass-through by default.

    Bash original:
        start_test pagespeed is defaulting to more than PassThrough
        fetch_until $TEST_ROOT/bot_test.html 'fgrep -c .pagespeed.' 2
    """

    def test_default_filtering_active(
        self, client: PageSpeedClient, test_root: str
    ):
        """Default configuration should apply some optimizations."""
        response = client.fetch_until_count(
            f"{test_root}/bot_test.html",
            pattern=r"\.pagespeed\.",
            expected_count=2,
            timeout=30.0,
        )
        assert_http_status(response, 200)


class TestIproEtag:
    """Tests for IPRO ETag handling.

    Bash original:
        start_test ipro resources have etag and not last-modified
        fetch_until -save "$URL" 'grep -c E[Tt]ag:.W/.PSA-aj.' 1
    """

    def test_ipro_resource_has_etag(
        self, client: PageSpeedClient, example_root: str
    ):
        """IPRO resources should have ETag header."""
        # Add random query param to avoid caching from previous tests
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 100000)}"

        # Fetch until IPRO generates an ETag with PSA-aj prefix
        response = client.fetch_until(
            url,
            condition=lambda r: "PSA-aj" in r.header("ETag", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        etag = response.header("ETag")
        assert etag, "IPRO resource should have ETag header"
        assert "PSA-aj" in etag, f"ETag should contain PSA-aj, got: {etag}"

    def test_ipro_resource_no_last_modified(
        self, client: PageSpeedClient, example_root: str
    ):
        """IPRO resources should not have Last-Modified header."""
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 100000)}"

        # Fetch until IPRO is done
        response = client.fetch_until(
            url,
            condition=lambda r: "PSA-aj" in r.header("ETag", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        last_modified = response.header("Last-Modified")
        assert not last_modified, \
            f"IPRO resource should not have Last-Modified, got: {last_modified}"

    def test_ipro_etag_returns_304(
        self, client: PageSpeedClient, example_root: str
    ):
        """If-None-Match with correct ETag should return 304."""
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 100000)}"

        # First fetch to get the ETag
        response = client.fetch_until(
            url,
            condition=lambda r: "PSA-aj" in r.header("ETag", ""),
            timeout=30.0,
        )

        etag = response.header("ETag")
        assert etag, "First request should return ETag"

        # Second request with If-None-Match should return 304
        response_304 = client.get(url, headers={"If-None-Match": etag})
        assert_http_status(response_304, 304)

        # 304 response should not have Content-Length
        content_length = response_304.header("Content-Length")
        # Note: Some servers may include Content-Length: 0, so just check body is empty
        assert len(response_304.body) == 0, "304 response should have empty body"

    def test_ipro_wrong_etag_returns_200(
        self, client: PageSpeedClient, example_root: str
    ):
        """If-None-Match with wrong ETag should return 200."""
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 100000)}"

        # First fetch to get the ETag
        response = client.fetch_until(
            url,
            condition=lambda r: "PSA-aj" in r.header("ETag", ""),
            timeout=30.0,
        )

        etag = response.header("ETag")
        # Create a different ETag by replacing PSA-aj with PSA-ic
        bad_etag = etag.replace("PSA-aj", "PSA-ic")

        # Request with wrong ETag should return 200
        response_200 = client.get(url, headers={"If-None-Match": bad_etag})
        assert_http_status(response_200, 200)

        # Should have Content-Length and actual content
        assert len(response_200.body) > 0, "200 response should have body"


class TestResource404:
    """Tests for 404 handling and statistics.

    Bash original:
        start_test 404s are served and properly recorded.
        NUM_404=$(scrape_stat resource_404_count)
        WGET_ERROR=$(check_not $WGET -O /dev/null $BAD_RESOURCE_URL 2>&1)
        check_from "$WGET_ERROR" fgrep -q "404 Not Found"
    """

    @pytest.mark.requires_stats
    def test_404_increments_stat(
        self, client: PageSpeedClient, stats_snapshot
    ):
        """404 responses should increment resource_404_count stat."""
        old_stats = stats_snapshot()
        old_404_count = old_stats.get("resource_404_count", 0)

        # Request a non-existent resource
        bad_url = "/mod_pagespeed/W.bad.pagespeed.cf.hash.css"
        response = client.get(bad_url)
        assert_http_status(response, 404)

        new_stats = stats_snapshot()
        new_404_count = new_stats.get("resource_404_count", 0)

        assert new_404_count == old_404_count + 1, \
            f"resource_404_count should increment by 1, was {old_404_count}, now {new_404_count}"

    @pytest.mark.requires_stats
    def test_200_does_not_increment_404_stat(
        self, client: PageSpeedClient, example_root: str, stats_snapshot
    ):
        """200 responses should not increment resource_404_count stat."""
        old_stats = stats_snapshot()
        old_404_count = old_stats.get("resource_404_count", 0)

        # Request a valid resource
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

        new_stats = stats_snapshot()
        new_404_count = new_stats.get("resource_404_count", 0)

        assert new_404_count == old_404_count, \
            f"resource_404_count should not change on 200, was {old_404_count}, now {new_404_count}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
