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

"""Move CSS filter tests.

Ported from: pagespeed/automatic/system_tests/move_css.sh

These tests verify that CSS is moved above scripts or to head as expected.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestMoveCssAboveScripts:
    """Tests for the move_css_above_scripts filter.

    Bash original::

        start_test move_css_above_scripts works.
        URL=$EXAMPLE_ROOT/move_css_above_scripts.html?PageSpeedFilters=move_css_above_scripts
        $WGET_DUMP $URL > $FETCHED
        # Link moved before script.
        check grep -q "styles/all_styles.css\"><script" $FETCHED
    """

    def test_move_css_above_scripts_enabled(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS links should be moved before scripts."""
        url = f"{example_root}/move_css_above_scripts.html?PageSpeedFilters=move_css_above_scripts"

        response = client.get(url)
        assert_http_status(response, 200)

        # CSS link should appear before script
        assert_contains(
            response,
            r'styles/all_styles\.css"><script',
            "CSS link should be moved before script",
        )

    def test_move_css_above_scripts_disabled(
        self, client: PageSpeedClient, example_root: str
    ):
        """Without filter, CSS links should not be moved.

        Bash original::

            start_test move_css_above_scripts off.
            URL=$EXAMPLE_ROOT/move_css_above_scripts.html?PageSpeedFilters=
            $WGET_DUMP $URL > $FETCHED
            # Link not moved before script.
            check_not grep "styles/all_styles.css\"><script" $FETCHED
        """
        url = f"{example_root}/move_css_above_scripts.html?PageSpeedFilters="

        response = client.get(url)
        assert_http_status(response, 200)

        # CSS link should NOT appear immediately before script
        assert_not_contains(
            response,
            r'styles/all_styles\.css"><script',
            "CSS link should not be moved without filter",
        )


class TestMoveCssToHead:
    """Tests for the move_css_to_head filter.

    Bash original::

        start_test move_css_to_head does what it says on the tin.
        URL=$EXAMPLE_ROOT/move_css_to_head.html?PageSpeedFilters=move_css_to_head
        $WGET_DUMP $URL > $FETCHED
        # Link moved to head.
        check grep -q "styles/all_styles.css\"></head>" $FETCHED
    """

    def test_move_css_to_head_enabled(
        self, client: PageSpeedClient, example_root: str
    ):
        """CSS links should be moved to head."""
        url = f"{example_root}/move_css_to_head.html?PageSpeedFilters=move_css_to_head"

        response = client.get(url)
        assert_http_status(response, 200)

        # CSS link should appear at end of head
        assert_contains(
            response,
            r'styles/all_styles\.css"></head>',
            "CSS link should be moved to head",
        )

    def test_move_css_to_head_disabled(
        self, client: PageSpeedClient, example_root: str
    ):
        """Without filter, CSS links should not be moved to head.

        Bash original::

            start_test move_css_to_head off.
            URL=$EXAMPLE_ROOT/move_css_to_head.html?PageSpeedFilters=
            $WGET_DUMP $URL > $FETCHED
            # Link not moved to head.
            check_not grep "styles/all_styles.css\"></head>" $FETCHED
        """
        url = f"{example_root}/move_css_to_head.html?PageSpeedFilters="

        response = client.get(url)
        assert_http_status(response, 200)

        # CSS link should NOT appear at end of head
        assert_not_contains(
            response,
            r'styles/all_styles\.css"></head>',
            "CSS link should not be moved to head without filter",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
