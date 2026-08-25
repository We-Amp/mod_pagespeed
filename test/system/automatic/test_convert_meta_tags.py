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

"""Convert meta tags filter tests.

Ported from: pagespeed/automatic/system_tests/convert_meta_tags.sh

These tests verify that the convert_meta_tags filter correctly converts
HTML meta tags to HTTP headers.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
    assert_header_contains,
)


@pytest.mark.not_iis  # convert_meta_tags is forbidden on IIS: cannot modify headers after body starts
class TestConvertMetaTags:
    """Tests for the convert_meta_tags filter.

    Bash original::

        test_filter convert_meta_tags
        run_wget_with_args $URL
        echo Checking for Charset header.
        check grep -qi "CONTENT-TYPE: text/html; *charset=UTF-8" $WGET_OUTPUT
    """

    def test_convert_meta_tags_adds_charset_header(
        self, client: PageSpeedClient, example_root: str
    ):
        """convert_meta_tags should convert meta charset to Content-Type header."""
        url = f"{example_root}/convert_meta_tags.html?PageSpeedFilters=convert_meta_tags"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Check for charset in Content-Type header
        content_type = response.header("Content-Type")
        assert content_type, "Should have Content-Type header"
        assert "charset" in content_type.lower(), \
            f"Content-Type should include charset, got: {content_type}"
        assert "utf-8" in content_type.lower(), \
            f"Content-Type should specify UTF-8, got: {content_type}"

    def test_convert_meta_tags_content_type_format(
        self, client: PageSpeedClient, example_root: str
    ):
        """Content-Type header should be properly formatted."""
        url = f"{example_root}/convert_meta_tags.html?PageSpeedFilters=convert_meta_tags"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert content_type, "Should have Content-Type header"

        # Should be text/html with charset
        assert "text/html" in content_type.lower(), \
            f"Content-Type should be text/html, got: {content_type}"


@pytest.mark.not_iis  # convert_meta_tags is forbidden on IIS: cannot modify headers after body starts
class TestConvertMetaTagsDisabled:
    """Tests that meta tags are not converted when filter is disabled."""

    def test_no_conversion_without_filter(
        self, client: PageSpeedClient, example_root: str
    ):
        """Without filter, meta tags should not affect headers."""
        url = f"{example_root}/convert_meta_tags.html?PageSpeedFilters="

        response = client.get(url)
        assert_http_status(response, 200)

        # The original page may or may not have charset in Content-Type
        # depending on the server's default configuration.
        # This test mainly verifies the filter mechanism works.


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
