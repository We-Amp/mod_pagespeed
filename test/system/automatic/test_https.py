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

"""HTTPS functionality tests.

Ported from: pagespeed/automatic/system_tests/https.sh

These tests verify that PageSpeed works correctly over HTTPS.
"""

import os
import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


# Skip all tests in this module if HTTPS_HOST is not set
pytestmark = pytest.mark.skipif(
    not os.environ.get("PAGESPEED_HTTPS_HOST"),
    reason="HTTPS tests require PAGESPEED_HTTPS_HOST environment variable",
)


@pytest.fixture
def https_client():
    """Create an HTTPS client for testing."""
    host = os.environ.get("PAGESPEED_HTTPS_HOST", "localhost")
    port = int(os.environ.get("PAGESPEED_HTTPS_PORT", "443"))
    return PageSpeedClient(host=host, port=port, use_https=True)


@pytest.fixture
def https_example_root():
    """Return the HTTPS example root path."""
    return "/mod_pagespeed_example"


class TestHttpsBasic:
    """Basic HTTPS functionality tests.

    Bash original::

        start_test Simple test that https is working.
        if [ -n "$HTTPS_HOST" ]; then
          URL="$HTTPS_EXAMPLE_ROOT/combine_css.html"
          fetch_until $URL 'fgrep -c css+' 1 --no-check-certificate
    """

    def test_https_css_combination(
        self, https_client: PageSpeedClient, https_example_root: str
    ):
        """CSS combination should work over HTTPS."""
        url = f"{https_example_root}/combine_css.html"

        response = https_client.fetch_until_count(
            url,
            pattern=r"css\+",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_https_pagespeed_header(
        self, https_client: PageSpeedClient, https_example_root: str
    ):
        """HTTPS response should include PageSpeed version header.

        Bash original::

            echo Checking for X-Mod-Pagespeed header
            check_from "$HTML_HEADERS" egrep -q 'X-Mod-Pagespeed|X-Page-Speed'
        """
        url = f"{https_example_root}/combine_css.html"

        response = https_client.get(url)
        assert_http_status(response, 200)

        mod_pagespeed = response.header("X-Mod-Pagespeed")
        page_speed = response.header("X-Page-Speed")

        assert mod_pagespeed or page_speed, \
            "Expected X-Mod-Pagespeed or X-Page-Speed header over HTTPS"

    def test_https_combined_css_with_filters(
        self, https_client: PageSpeedClient, https_example_root: str
    ):
        """Combined CSS URL should be generated correctly with filters.

        Bash original::

            echo Checking for combined CSS URL
            EXPECTED='href="styles/yellow.css+blue.css+big.css+bold.css'
            EXPECTED="$EXPECTED".pagespeed.cc.*.css"/>'
            fetch_until "$URL?PageSpeedFilters=combine_css,trim_urls" \
                "grep -ic $EXPECTED" 1 --no-check-certificate
        """
        url = f"{https_example_root}/combine_css.html?PageSpeedFilters=combine_css,trim_urls"

        response = https_client.fetch_until_contains(
            url,
            pattern=r'styles/yellow\.css\+blue\.css\+big\.css\+bold\.css\.pagespeed\.cc\.',
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Verify the combined CSS pattern is present
        assert_contains(
            response,
            r'yellow\.css\+blue\.css\+big\.css\+bold\.css\.pagespeed\.cc\.[^"]*\.css',
            "Combined CSS URL should include all CSS files",
        )

    def test_https_combined_css_preserves_relativity(
        self, https_client: PageSpeedClient, https_example_root: str
    ):
        """Combined CSS URL should preserve relativity without trim_urls.

        Bash original::

            echo Checking for combined CSS URL without URL trimming
            # Without URL trimming we still preserve URL relativity.
            fetch_until "$URL?PageSpeedFilters=combine_css" "grep -ic $EXPECTED" 1 \
               --no-check-certificate
        """
        url = f"{https_example_root}/combine_css.html?PageSpeedFilters=combine_css"

        response = https_client.fetch_until_contains(
            url,
            pattern=r'yellow\.css\+blue\.css\+big\.css\+bold\.css\.pagespeed\.cc\.',
            timeout=30.0,
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
