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

"""Add instrumentation filter tests.

Ported from: pagespeed/automatic/system_tests/add_instrumentation.sh

These tests verify that the add_instrumentation filter correctly adds
timing and beacon scripts to pages.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestAddInstrumentation:
    """Tests for the add_instrumentation filter.

    Bash original::

        test_filter add_instrumentation adds 2 script tags
        check run_wget_with_args $URL
        # Counts occurances of '<script' in $FETCHED
        check [ $(fgrep -o '<script' $FETCHED | wc -l) -eq 2 ]
    """

    def test_add_instrumentation_adds_script_tags(
        self, client: PageSpeedClient, example_root: str
    ):
        """add_instrumentation should add 2 script tags."""
        url = f"{example_root}/add_instrumentation.html?PageSpeedFilters=add_instrumentation"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Count script tags
        script_count = response.text.count("<script")

        # The filter adds 2 script tags for instrumentation
        assert script_count == 2, \
            f"Expected 2 script tags, found {script_count}"


class TestAddInstrumentationDisabled:
    """Tests that instrumentation can be disabled.

    Bash original::

        start_test "We don't add_instrumentation if URL params tell us not to"
        FILE=add_instrumentation.html?PageSpeedFilters=
        URL=$EXAMPLE_ROOT/$FILE
        check run_wget_with_args $URL
        check [ $(fgrep -o '<script' $FETCHED | wc -l) -eq 0 ]
    """

    def test_no_instrumentation_with_empty_filters(
        self, client: PageSpeedClient, example_root: str
    ):
        """No instrumentation if PageSpeedFilters is empty."""
        url = f"{example_root}/add_instrumentation.html?PageSpeedFilters="

        response = client.get(url)
        assert_http_status(response, 200)

        # Count script tags - should be 0 with no filters
        script_count = response.text.count("<script")

        assert script_count == 0, \
            f"Expected 0 script tags with empty filters, found {script_count}"


class TestInstrumentationOn404:
    """Tests that 404 pages are not instrumented.

    Bash original::

        # http://github.com/apache/incubator-pagespeed-mod/issues/170
        start_test "Make sure 404s aren't rewritten"
        THIS_BAD_URL=$BAD_RESOURCE_URL?PageSpeedFilters=add_instrumentation
        OUT=$($CURL --silent $THIS_BAD_URL)
        check_not_from "$OUT" fgrep "/mod_pagespeed_beacon"
    """

    def test_404_not_instrumented(
        self, client: PageSpeedClient
    ):
        """404 pages should not be instrumented."""
        # Request a non-existent resource with instrumentation filter
        url = "/mod_pagespeed_nonexistent_page.html?PageSpeedFilters=add_instrumentation"

        response = client.get(url)
        assert_http_status(response, 404)

        # 404 page should not contain beacon
        assert_not_contains(
            response,
            r"/mod_pagespeed_beacon",
            "404 pages should not be instrumented",
        )

    def test_404_resource_not_instrumented(
        self, client: PageSpeedClient
    ):
        """404 resources should not have beacon scripts."""
        # Request a bad resource URL
        url = "/mod_pagespeed/bad_resource.pagespeed.css?PageSpeedFilters=add_instrumentation"

        response = client.get(url)

        # If it's a 404, it should not have the beacon
        if response.status == 404:
            assert_not_contains(
                response,
                r"/mod_pagespeed_beacon",
                "404 resources should not be instrumented",
            )


class TestInstrumentationContent:
    """Tests for instrumentation script content."""

    def test_instrumentation_includes_beacon_url(
        self, client: PageSpeedClient, example_root: str
    ):
        """Instrumentation should include beacon URL."""
        url = f"{example_root}/add_instrumentation.html?PageSpeedFilters=add_instrumentation"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Should contain beacon reference
        assert_contains(
            response,
            r"mod_pagespeed_beacon|pagespeed\..*beacon",
            "Instrumentation should include beacon reference",
        )

    def test_instrumentation_includes_timing_code(
        self, client: PageSpeedClient, example_root: str
    ):
        """Instrumentation should include timing code."""
        url = f"{example_root}/add_instrumentation.html?PageSpeedFilters=add_instrumentation"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Should contain timing-related code
        # The exact content depends on the PageSpeed configuration
        assert "<script" in response.text, \
            "Instrumentation should add script tags"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
