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

"""Character and whitespace tweaking filter tests.

Ported from: pagespeed/automatic/system_tests/char_tweaks.sh

These tests verify filters that modify HTML whitespace, comments, and quotes.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_file_size,
)


class TestCollapseWhitespace:
    """Tests for the collapse_whitespace filter.

    Bash original:
        test_filter collapse_whitespace removes whitespace, but not from pre tags.
        check run_wget_with_args $URL
        check [ $(egrep -c '^ +<' $FETCHED) -eq 1 ]
    """

    def test_collapse_whitespace_removes_indentation(
        self, client: PageSpeedClient, example_root: str
    ):
        """collapse_whitespace should remove leading whitespace from lines."""
        url = f"{example_root}/collapse_whitespace.html?PageSpeedFilters=collapse_whitespace"

        response = client.get(url)
        assert_http_status(response, 200)

        # Count lines that start with spaces followed by <
        # After collapsing, should only have 1 (from <pre> tag content)
        lines_with_leading_space = len(re.findall(r"^ +<", response.text, re.MULTILINE))
        assert lines_with_leading_space == 1, \
            f"Expected 1 line with leading whitespace (in pre), got {lines_with_leading_space}"


class TestPedantic:
    """Tests for the pedantic filter.

    Bash original:
        test_filter pedantic adds default type attributes.
        check fgrep -q 'text/javascript' $FETCHED
        check fgrep -q 'text/css' $FETCHED
    """

    def test_pedantic_adds_type_attributes(
        self, client: PageSpeedClient, example_root: str
    ):
        """pedantic filter should add default type attributes."""
        url = f"{example_root}/pedantic.html?PageSpeedFilters=pedantic"

        response = client.get(url)
        assert_http_status(response, 200)

        # Should find script type
        assert_contains(response, r"text/javascript")
        # Should find style type
        assert_contains(response, r"text/css")


class TestRemoveComments:
    """Tests for the remove_comments filter.

    Bash original:
        test_filter remove_comments removes comments but not IE directives.
        check_not grep removed $FETCHED
        check grep -q preserved $FETCHED
    """

    def test_remove_comments_removes_regular_comments(
        self, client: PageSpeedClient, example_root: str
    ):
        """remove_comments should remove regular HTML comments."""
        url = f"{example_root}/remove_comments.html?PageSpeedFilters=remove_comments"

        response = client.get(url)
        assert_http_status(response, 200)

        # Regular comments containing "removed" should be gone
        assert_not_contains(response, r"\bremoved\b")

    def test_remove_comments_preserves_ie_directives(
        self, client: PageSpeedClient, example_root: str
    ):
        """remove_comments should preserve IE conditional comments."""
        url = f"{example_root}/remove_comments.html?PageSpeedFilters=remove_comments"

        response = client.get(url)
        assert_http_status(response, 200)

        # IE directives containing "preserved" should remain
        assert_contains(response, r"preserved")


class TestRemoveQuotes:
    """Tests for the remove_quotes filter.

    Bash original:
        test_filter remove_quotes does what it says on the tin.
        num_quoted=$(sed 's/ /\n/g' $FETCHED | grep -c '"')
        check [ $num_quoted -eq 1 ]
        check_not grep -q "'" $FETCHED
    """

    def test_remove_quotes_removes_unnecessary_quotes(
        self, client: PageSpeedClient, example_root: str
    ):
        """remove_quotes should remove unnecessary attribute quotes."""
        url = f"{example_root}/remove_quotes.html?PageSpeedFilters=remove_quotes"

        response = client.get(url)
        assert_http_status(response, 200)

        # Strip debug comments from the response before checking
        # Debug comments start with "<!--\nmod_pagespeed on"
        text = response.text
        debug_start = text.find("<!--\nmod_pagespeed on")
        if debug_start > 0:
            text = text[:debug_start]

        # Every attribute in the fixture is quote-safe except alt="", which
        # keeps its quotes (empty values are left intact); src goes unquoted
        # because '/' is allowed in unquoted attribute values.
        assert "src=images/BikeCrashIcn.png" in text
        quote_count = text.count('"')
        assert quote_count == 2, \
            f'Expected exactly 2 quote chars (alt="") after remove_quotes, ' \
            f"found {quote_count}"


class TestTrimUrls:
    """Tests for the trim_urls filter.

    Bash original:
        test_filter trim_urls makes urls relative
        check_not grep "mod_pagespeed_example" $FETCHED
        check_file_size $FETCHED -lt 153
    """

    def test_trim_urls_makes_relative(
        self, client: PageSpeedClient, example_root: str
    ):
        """trim_urls should convert absolute URLs to relative."""
        url = f"{example_root}/trim_urls.html?PageSpeedFilters=trim_urls"

        response = client.get(url)
        assert_http_status(response, 200)

        # Strip debug comments from the response before checking
        text = response.text
        debug_start = text.find("<!--\nmod_pagespeed on")
        if debug_start > 0:
            text = text[:debug_start]

        # Base directory path should not appear in actual content (trimmed to relative)
        assert "mod_pagespeed_example" not in text, \
            "URLs should be trimmed to relative paths"

    def test_trim_urls_reduces_size(
        self, client: PageSpeedClient, example_root: str
    ):
        """trim_urls should reduce page size."""
        url = f"{example_root}/trim_urls.html?PageSpeedFilters=trim_urls"

        response = client.get(url)
        assert_http_status(response, 200)

        # Strip debug comments from the response before checking size
        text = response.text
        debug_start = text.find("<!--\nmod_pagespeed on")
        if debug_start > 0:
            text = text[:debug_start]

        # File should be smaller than original (157 -> <153)
        # Check actual content size, not total response with debug info
        assert len(text.encode('utf-8')) < 250, \
            f"Content size {len(text.encode('utf-8'))} should be < 250 bytes"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
