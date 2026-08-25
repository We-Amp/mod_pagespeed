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

"""Make show_ads async filter tests.

Ported from: pagespeed/automatic/system_tests/make_show_ads_async.sh

These tests verify that the make_show_ads_async filter correctly converts
synchronous AdSense ads to asynchronous format.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestMakeShowAdsAsync:
    """Tests for the make_show_ads_async filter.

    Bash original::

        test_filter make_show_ads_async works
        OUT=$($WGET_DUMP $URL)
        check_from     "$OUT" grep -q 'data-ad'
        check_not_from "$OUT" grep -q 'google_ad'
        check_from     "$OUT" grep -q 'adsbygoogle.js'
        check_not_from "$OUT" grep -q 'show_ads.js'
        check_from     "$OUT" fgrep -q "<script>(adsbygoogle = window.adsbygoogle || []).push({})</script>"
    """

    def test_converts_to_data_ad_attributes(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should convert to data-ad attributes."""
        url = f"{example_root}/make_show_ads_async.html?PageSpeedFilters=make_show_ads_async"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"data-ad",
            "Should convert to data-ad attributes",
        )

    def test_removes_google_ad_attributes(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should remove google_ad attributes."""
        url = f"{example_root}/make_show_ads_async.html?PageSpeedFilters=make_show_ads_async"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_not_contains(
            response,
            r"google_ad",
            "Should remove google_ad attributes",
        )

    def test_uses_adsbygoogle_script(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should use adsbygoogle.js instead of show_ads.js."""
        url = f"{example_root}/make_show_ads_async.html?PageSpeedFilters=make_show_ads_async"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"adsbygoogle\.js",
            "Should use adsbygoogle.js",
        )

    def test_removes_show_ads_script(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should remove show_ads.js reference."""
        url = f"{example_root}/make_show_ads_async.html?PageSpeedFilters=make_show_ads_async"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_not_contains(
            response,
            r"show_ads\.js",
            "Should remove show_ads.js",
        )

    def test_adds_push_script(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should add adsbygoogle push script."""
        url = f"{example_root}/make_show_ads_async.html?PageSpeedFilters=make_show_ads_async"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"<script>\(adsbygoogle = window\.adsbygoogle \|\| \[\]\)\.push\(\{\}\)</script>",
            "Should add adsbygoogle push script",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
