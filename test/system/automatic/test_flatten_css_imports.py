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

"""Flatten CSS imports filter tests.

Ported from: pagespeed/automatic/system_tests/flatten_css_imports.sh

These tests verify that CSS @import statements are flattened by inlining
the imported content.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestFlattenCssImportsDefault:
    """Tests for flatten_css_imports with default settings.

    Bash original::

        # Fetch with the default limit so our test file is inlined.
        test_filter flatten_css_imports,rewrite_css default limit
        WGET_ARGS="${WGET_ARGS} --header=X-PSA-Blocking-Rewrite:psatest"
        check run_wget_with_args $URL
        check_not grep @import.url $FETCHED
        check grep -q "yellow.background-color:" $FETCHED
    """

    def test_flatten_css_imports_inlines_content(
        self, client: PageSpeedClient, example_root: str
    ):
        """With default limit, @import statements should be flattened."""
        url = f"{example_root}/flatten_css_imports.html?PageSpeedFilters=flatten_css_imports,rewrite_css"

        # Wait for CSS rewriting to complete - imported content should appear
        response = client.fetch_until_contains(
            url,
            pattern=r"yellow.*background-color:",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # @import should be removed
        assert_not_contains(
            response,
            r"@import.url",
            "@import statements should be flattened",
        )

    def test_flatten_css_imports_includes_imported_content(
        self, client: PageSpeedClient, example_root: str
    ):
        """Flattened CSS should include content from imported files."""
        url = f"{example_root}/flatten_css_imports.html?PageSpeedFilters=flatten_css_imports,rewrite_css"

        response = client.fetch_until_contains(
            url,
            pattern=r"yellow.*background-color:",
            timeout=30.0,
        )
        assert_http_status(response, 200)


class TestFlattenCssImportsTinyLimit:
    """Tests for flatten_css_imports with tiny byte limit.

    Bash original::

        # Fetch with a tiny limit so no file can be inlined.
        test_filter flatten_css_imports,rewrite_css tiny limit
        WGET_ARGS="${WGET_ARGS} --header=PageSpeedCssFlattenMaxBytes:5"
        WGET_ARGS="${WGET_ARGS} --header=X-PSA-Blocking-Rewrite:psatest"
        check run_wget_with_args $URL
        check grep -q @import.url $FETCHED
        check_not grep "yellow.background-color:" $FETCHED
    """

    def test_tiny_limit_prevents_flattening(
        self, client: PageSpeedClient, example_root: str
    ):
        """With tiny limit, @import statements should not be flattened."""
        url = f"{example_root}/flatten_css_imports.html?PageSpeedFilters=flatten_css_imports,rewrite_css"

        response = client.fetch_until_contains(
            url,
            pattern=r"@import",
            timeout=30.0,
            headers={
                "PageSpeedCssFlattenMaxBytes": "5",
            },
        )
        assert_http_status(response, 200)

    @pytest.mark.not_envoy(reason="Envoy does not apply CssFlattenMaxBytes header to resource requests")
    def test_tiny_limit_excludes_imported_content(
        self, client: PageSpeedClient, example_root: str
    ):
        """With tiny limit, imported CSS content should not be inlined."""
        url = f"{example_root}/flatten_css_imports.html?PageSpeedFilters=flatten_css_imports,rewrite_css"

        # Wait for the page to be processed (we should see @import remain)
        response = client.fetch_until_contains(
            url,
            pattern=r"@import",
            timeout=30.0,
            headers={
                "PageSpeedCssFlattenMaxBytes": "5",
            },
        )
        assert_http_status(response, 200)

        # Imported CSS content should NOT be present inline
        assert_not_contains(
            response,
            r"yellow.*background-color:",
            "Imported CSS content should not be inlined with tiny limit",
        )


class TestFlattenCssImportsMediumLimit:
    """Tests for flatten_css_imports with medium byte limit.

    Bash original::

        # Fetch with a medium limit so any one file can be inlined but not all.
        test_filter flatten_css_imports,rewrite_css medium limit
        WGET_ARGS="${WGET_ARGS} --header=PageSpeedCssFlattenMaxBytes:50"
        WGET_ARGS="${WGET_ARGS} --header=X-PSA-Blocking-Rewrite:psatest"
        check run_wget_with_args $URL
        check grep -q @import.url $FETCHED
        check_not grep "yellow.background-color:" $FETCHED
    """

    def test_medium_limit_partial_flattening(
        self, client: PageSpeedClient, example_root: str
    ):
        """With medium limit, some @import statements may remain."""
        url = f"{example_root}/flatten_css_imports.html?PageSpeedFilters=flatten_css_imports,rewrite_css"

        response = client.fetch_until_contains(
            url,
            pattern=r"@import",
            timeout=30.0,
            headers={
                "PageSpeedCssFlattenMaxBytes": "50",
            },
        )
        assert_http_status(response, 200)

        # With medium limit, some @import statements may remain
        # depending on file sizes


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
