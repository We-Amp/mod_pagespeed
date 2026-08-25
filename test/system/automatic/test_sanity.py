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

"""Initial sanity check tests.

Ported from: pagespeed/automatic/system_tests/initial_sanity_checks.sh

These tests verify basic server functionality before running more complex tests.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    assert_header_contains,
)


class TestDirectoryMapping:
    """Test that directory is mapped to index.html.

    Bash original:
        start_test directory is mapped to index.html.
        check $WGET -q $EXAMPLE_ROOT/?PageSpeed=off -O $OUTDIR/mod_pagespeed_example
        check $WGET -q $EXAMPLE_ROOT/index.html?PageSpeed=off -O $OUTDIR/index.html
        check diff $OUTDIR/index.html $OUTDIR/mod_pagespeed_example
    """

    def test_directory_maps_to_index(self, client: PageSpeedClient, example_root: str):
        """Fetching / should return the same content as /index.html."""
        # Fetch with PageSpeed=off to ensure consistent (non-rewritten) content
        root_response = client.get(f"{example_root}/?PageSpeed=off")
        index_response = client.get(f"{example_root}/index.html?PageSpeed=off")

        assert_http_status(root_response, 200)
        assert_http_status(index_response, 200)

        # Both should return the same content
        assert root_response.text == index_response.text, \
            "Directory request should return same content as index.html"


class TestCompression:
    """Test that gzip compression is enabled.

    Bash original:
        start_test compression is enabled for HTML.
        OUT=$($WGET -O /dev/null -q -S --header='Accept-Encoding: gzip' $EXAMPLE_ROOT/ 2>&1)
        check_from "$OUT" fgrep -qi 'Content-Encoding: gzip'
    """

    @pytest.mark.not_iis  # IIS gzip requires explicit configuration
    def test_gzip_compression_enabled(self, client: PageSpeedClient, example_root: str, server_config):
        """Server should return gzip-compressed responses when requested."""
        response = client.get(
            f"{example_root}/",
            headers={"Accept-Encoding": "gzip"},
        )

        assert_http_status(response, 200)
        # Check that Content-Encoding header indicates gzip
        content_encoding = response.header("Content-Encoding")
        assert "gzip" in content_encoding.lower(), \
            f"Expected gzip compression, got Content-Encoding: {content_encoding}"


class TestWhitespaceHandling:
    """Test that whitespace-only HTML is handled correctly.

    Bash original:
        start_test We behave sanely on whitespace served as HTML
        OUT=$($WGET_DUMP $TEST_ROOT/whitespace.html)
        check_200_http_response "$OUT"
    """

    def test_whitespace_html_returns_200(self, client: PageSpeedClient, test_root: str):
        """Whitespace-only HTML should return 200 OK."""
        response = client.get(f"{test_root}/whitespace.html")
        assert_http_status(response, 200)


class TestPageSpeedOff:
    """Test that PageSpeed=off disables optimization."""

    def test_pagespeed_off_disables_rewriting(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off query param should disable all optimization."""
        response = client.get(f"{example_root}/combine_css.html?PageSpeed=off")

        assert_http_status(response, 200)
        # Should not see any pagespeed rewriting markers
        assert ".pagespeed." not in response.text, \
            "PageSpeed=off should disable rewriting"


class TestBasicConnectivity:
    """Basic connectivity tests."""

    def test_server_responds(self, client: PageSpeedClient, example_root: str):
        """Verify the server is responding to requests."""
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

    def test_404_for_missing_page(self, client: PageSpeedClient, example_root: str):
        """Non-existent pages should return 404."""
        response = client.get(f"{example_root}/this_page_does_not_exist_12345.html")
        assert_http_status(response, 404)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
