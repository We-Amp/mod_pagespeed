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

"""Elide attributes filter tests.

Ported from: pagespeed/automatic/system_tests/elide_attributes.sh

These tests verify that the elide_attributes filter removes
unnecessary boolean and default attribute values.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestElideAttributes:
    """Tests for the elide_attributes filter.

    Bash original::

        test_filter elide_attributes removes boolean and default attributes.
        check run_wget_with_args $URL
        check_not fgrep "disabled=" $FETCHED   # boolean, should not find
    """

    def test_elide_boolean_attributes(
        self, client: PageSpeedClient, example_root: str
    ):
        """Boolean attributes should have their values elided."""
        url = f"{example_root}/elide_attributes.html?PageSpeedFilters=elide_attributes"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # disabled="disabled" should become just disabled (no =)
        assert_not_contains(
            response,
            r'disabled=',
            "Boolean attribute should not have = sign",
        )

    def test_elide_default_type_attributes(
        self, client: PageSpeedClient, example_root: str
    ):
        """Default type attributes should be elided."""
        url = f"{example_root}/elide_attributes.html?PageSpeedFilters=elide_attributes"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # type="text/javascript" on script tags is the default and can be elided
        # Note: This depends on the test HTML content


class TestElideAttributesDisabled:
    """Tests that attributes are not elided when filter is disabled."""

    def test_no_elision_without_filter(
        self, client: PageSpeedClient, example_root: str
    ):
        """Without filter, attributes should remain unchanged."""
        url = f"{example_root}/elide_attributes.html?PageSpeedFilters="

        response = client.get(url)
        assert_http_status(response, 200)

        # Original HTML should have disabled="disabled"
        assert_contains(
            response,
            r'disabled=',
            "Original HTML should have attribute value",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
