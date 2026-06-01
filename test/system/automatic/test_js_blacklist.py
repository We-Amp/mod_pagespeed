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

"""JavaScript blacklist tests.

Ported from: pagespeed/automatic/system_tests/js_blacklist.sh

These tests verify that blacklisted JavaScript files are not rewritten.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestJsBlacklist:
    """Tests for JavaScript blacklist functionality.

    Bash original::

        start_test Filters do not rewrite blacklisted JavaScript files.
        URL=$TEST_ROOT/blacklist/blacklist.html?PageSpeedFilters=extend_cache,rewrite_javascript,trim_urls
        fetch_until -save $URL 'grep -c .js.pagespeed.' 4
    """

    def test_blacklist_allows_normal_js(
        self, client: PageSpeedClient, test_root: str
    ):
        """Normal JS files should be rewritten."""
        url = f"{test_root}/blacklist/blacklist.html?PageSpeedFilters=extend_cache,rewrite_javascript,trim_urls"

        response = client.fetch_until_count(
            url,
            pattern=r"\.js\.pagespeed\.",
            expected_count=4,
            timeout=120.0,
        )
        assert_http_status(response, 200)

        # Normal JS should be rewritten
        assert_contains(
            response,
            r'<script src=".*normal\.js\.pagespeed\..*\.js">',
            "Normal JS should be rewritten",
        )

    def test_blacklist_blocks_tinymce(
        self, client: PageSpeedClient, test_root: str
    ):
        """TinyMCE JS files should not be rewritten.

        Bash original::

            check grep -q "<script src=\"js_tinyMCE.js\"></script>" $FETCHED
            check grep -q "<script src=\"tiny_mce.js\"></script>" $FETCHED
            check grep -q "<script src=\"tinymce.js\"></script>" $FETCHED
        """
        url = f"{test_root}/blacklist/blacklist.html?PageSpeedFilters=extend_cache,rewrite_javascript,trim_urls"

        response = client.fetch_until_count(
            url,
            pattern=r"\.js\.pagespeed\.",
            expected_count=4,
            timeout=120.0,
        )
        assert_http_status(response, 200)

        # TinyMCE variants should not be rewritten
        assert_contains(
            response,
            r'<script src="js_tinyMCE\.js"></script>',
            "TinyMCE should not be rewritten",
        )
        assert_contains(
            response,
            r'<script src="tiny_mce\.js"></script>',
            "tiny_mce should not be rewritten",
        )
        assert_contains(
            response,
            r'<script src="tinymce\.js"></script>',
            "tinymce should not be rewritten",
        )

    def test_blacklist_blocks_scriptaculous(
        self, client: PageSpeedClient, test_root: str
    ):
        """Scriptaculous JS should not be rewritten."""
        url = f"{test_root}/blacklist/blacklist.html?PageSpeedFilters=extend_cache,rewrite_javascript,trim_urls"

        response = client.fetch_until_count(
            url,
            pattern=r"\.js\.pagespeed\.",
            expected_count=4,
            timeout=120.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'<script src="scriptaculous\.js\?load=effects,builder"></script>',
            "Scriptaculous should not be rewritten",
        )

    def test_blacklist_blocks_ckeditor(
        self, client: PageSpeedClient, test_root: str
    ):
        """CKEditor JS should not be rewritten."""
        url = f"{test_root}/blacklist/blacklist.html?PageSpeedFilters=extend_cache,rewrite_javascript,trim_urls"

        response = client.fetch_until_count(
            url,
            pattern=r"\.js\.pagespeed\.",
            expected_count=4,
            timeout=120.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'<script src=".*ckeditor\.js">',
            "CKEditor should not be rewritten",
        )

    def test_blacklist_allows_jquery(
        self, client: PageSpeedClient, test_root: str
    ):
        """jQuery JS should be rewritten (not blacklisted by default)."""
        url = f"{test_root}/blacklist/blacklist.html?PageSpeedFilters=extend_cache,rewrite_javascript,trim_urls"

        response = client.fetch_until_count(
            url,
            pattern=r"\.js\.pagespeed\.",
            expected_count=4,
            timeout=120.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r'<script src=".*jquery.*\.js\.pagespeed\..*\.js">',
            "jQuery should be rewritten",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
