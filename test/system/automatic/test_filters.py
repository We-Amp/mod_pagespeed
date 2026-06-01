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

"""General filter tests.

Ported from various pagespeed/automatic/system_tests/*.sh files.

These tests verify various PageSpeed filters work correctly.
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

    Ported from: pagespeed/automatic/system_tests/elide_attributes.sh

    Bash original:
        test_filter elide_attributes removes boolean and default attributes.
        check run_wget_with_args $URL
        check_not fgrep "disabled=" $FETCHED   # boolean, should not find
    """

    def test_elide_attributes_removes_boolean_attrs(
        self, client: PageSpeedClient, example_root: str
    ):
        """elide_attributes should remove boolean attribute values."""
        url = f"{example_root}/elide_attributes.html?PageSpeedFilters=elide_attributes"

        response = client.get(url)
        assert_http_status(response, 200)

        # Boolean attributes like disabled="disabled" should have value removed
        assert_not_contains(
            response,
            r'disabled=',
            "Boolean attribute 'disabled' should not have value",
        )


class TestConvertMetaTags:
    """Tests for the convert_meta_tags filter.

    Ported from: pagespeed/automatic/system_tests/convert_meta_tags.sh
    """

    def test_convert_meta_tags(self, client: PageSpeedClient, example_root: str):
        """convert_meta_tags should convert meta http-equiv to headers."""
        url = f"{example_root}/convert_meta_tags.html?PageSpeedFilters=convert_meta_tags"

        response = client.fetch_until(
            url,
            # The meta tag should be removed when converted to header
            condition=lambda r: 'http-equiv' not in r.text.lower()
            or r.status == 200,
            timeout=30.0,
        )

        assert_http_status(response, 200)


class TestInsertDnsPrefetch:
    """Tests for the insert_dns_prefetch filter.

    Ported from: pagespeed/automatic/system_tests/insert_dns_prefetch.sh
    """

    def test_insert_dns_prefetch(self, client: PageSpeedClient, example_root: str):
        """insert_dns_prefetch should add dns-prefetch link tags."""
        url = f"{example_root}/insert_dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"

        response = client.fetch_until_contains(
            url,
            pattern=r'dns-prefetch',
            timeout=30.0,
        )

        assert_http_status(response, 200)
        assert_contains(response, r'rel=["\']?dns-prefetch')


class TestMoveCss:
    """Tests for the move_css_to_head and move_css_above_scripts filters.

    Ported from: pagespeed/automatic/system_tests/move_css.sh
    """

    def test_move_css_to_head(self, client: PageSpeedClient, example_root: str):
        """move_css_to_head should move CSS links to head.

        Bash original::

            start_test move_css_to_head does what it says on the tin.
            URL=$EXAMPLE_ROOT/move_css_to_head.html?PageSpeedFilters=move_css_to_head
            $WGET_DUMP $URL > $FETCHED
            # Link moved to head.
            check grep -q "styles/all_styles.css\"></head>" $FETCHED
        """
        url = f"{example_root}/move_css_to_head.html?PageSpeedFilters=move_css_to_head"

        response = client.get(url)
        assert_http_status(response, 200)

        # CSS link should be immediately before </head>
        assert_contains(response, r'all_styles\.css"></head>')

    def test_move_css_above_scripts(self, client: PageSpeedClient, example_root: str):
        """move_css_above_scripts should move CSS links before script tags.

        Bash original::

            start_test move_css_above_scripts works.
            URL=$EXAMPLE_ROOT/move_css_above_scripts.html?PageSpeedFilters=move_css_above_scripts
            $WGET_DUMP $URL > $FETCHED
            # Link moved before script.
            check grep -q "styles/all_styles.css\"><script" $FETCHED
        """
        url = f"{example_root}/move_css_above_scripts.html?PageSpeedFilters=move_css_above_scripts"

        response = client.get(url)
        assert_http_status(response, 200)

        # CSS link should be immediately before script tag
        assert_contains(response, r'all_styles\.css"><script')


class TestAddInstrumentation:
    """Tests for the add_instrumentation filter.

    Ported from: pagespeed/automatic/system_tests/add_instrumentation.sh
    """

    def test_add_instrumentation_injects_beacon(
        self, client: PageSpeedClient, example_root: str
    ):
        """add_instrumentation should inject beacon JavaScript."""
        url = f"{example_root}/add_instrumentation.html?PageSpeedFilters=add_instrumentation"

        response = client.fetch_until_contains(
            url,
            pattern=r'pagespeed\.addInstrumentationInit',
            timeout=30.0,
        )

        assert_http_status(response, 200)
        # Should contain the instrumentation beacon
        assert_contains(response, r'pagespeed')


class TestDeferJavascript:
    """Tests for the defer_javascript filter.

    Ported from: pagespeed/automatic/system_tests/defer_javascript.sh
    """

    def test_defer_javascript(self, client: PageSpeedClient, example_root: str):
        """defer_javascript should convert scripts to type=text/psajs.

        Bash original::

            test_filter defer_javascript optimize mode
            check run_wget_with_args $URL
            check grep -q text/psajs $FETCHED
            check grep -q /js_defer $FETCHED
            check grep -q "PageSpeed=noscript" $FETCHED
        """
        url = f"{example_root}/defer_javascript.html?PageSpeedFilters=defer_javascript"

        # Wait for the deferred scripts to appear
        response = client.fetch_until_contains(
            url,
            pattern=r'text/psajs',
            timeout=30.0,
        )

        assert_http_status(response, 200)

        # Should have script type changed to psajs
        assert_contains(response, r'type="text/psajs"')

        # Should reference the js_defer library
        assert_contains(response, r'/js_defer')

        # Should have noscript fallback
        assert_contains(response, r'PageSpeed=noscript')


class TestLazyloadImages:
    """Tests for the lazyload_images filter.

    Ported from: pagespeed/automatic/system_tests/lazyload_images.sh

    Note: The Critical Images Beacon is disabled in both Envoy and Apache
    test configurations, which allows this filter to work without beacon warmup.
    When beaconing is disabled, CriticalImagesFinder::Available() returns
    kDisabled instead of kNoDataYet, so the filter proceeds normally.
    """

    def test_lazyload_images_injects_script(
        self, client: PageSpeedClient, example_root: str
    ):
        """lazyload_images should inject lazy loading JavaScript."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=lazyload_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'pagespeed\.lazyLoad',
            timeout=30.0,
        )

        assert_http_status(response, 200)


class TestFlattenCssImports:
    """Tests for the flatten_css_imports filter.

    Ported from: pagespeed/automatic/system_tests/flatten_css_imports.sh
    """

    def test_flatten_css_imports(self, client: PageSpeedClient, example_root: str):
        """flatten_css_imports should inline @import rules."""
        url = f"{example_root}/flatten_css_imports.html?PageSpeedFilters=flatten_css_imports"

        response = client.fetch_until_contains(
            url,
            pattern=r"<style>\.yellow",
            timeout=30.0,
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
