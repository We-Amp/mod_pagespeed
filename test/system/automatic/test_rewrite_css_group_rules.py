#!/usr/bin/env python3
# Copyright 2026 We-Amp B.V.
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

"""CSS conditional group rule rewriting tests.

These tests verify that rewrite_css minifies the contents of @supports,
@layer, and @container blocks and that images referenced inside them are
optimized, while the group conditions themselves are preserved verbatim.
"""

import re
from urllib.parse import urlparse

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)

BLOCKING_HEADERS = {"X-PSA-Blocking-Rewrite": "psatest"}

FILTERS = "rewrite_css,rewrite_images,extend_cache"

REWRITTEN_CSS_LINK_PATTERN = r"rewrite_css_group_rules\.css\.pagespeed\.cf\."

# Cache extension and recompression rename differently (.ce. vs .ic.), so
# only pin the .pagespeed. marker after the original file name.
REWRITTEN_INLINE_IMAGE_PATTERN = r"Cuppa\.png\.pagespeed\."
REWRITTEN_CSS_IMAGE_PATTERN = r"BikeCrashIcn\.png\.pagespeed\."


class TestRewriteCssGroupRules:
    """Tests for rewrite_css applied to CSS conditional group rules."""

    def _page_url(self, example_root: str) -> str:
        return (
            f"{example_root}/rewrite_css_group_rules.html"
            f"?PageSpeedFilters={FILTERS}"
        )

    def test_inline_group_rule_minified_and_image_rewritten(
        self, client: PageSpeedClient, example_root: str
    ):
        """The inline style's @supports body is minified, its image URL
        rewritten, and the condition text kept verbatim."""
        response = client.fetch_until_count(
            self._page_url(example_root),
            pattern=REWRITTEN_INLINE_IMAGE_PATTERN,
            expected_count=1,
            timeout=60.0,
            headers=BLOCKING_HEADERS,
        )
        assert_http_status(response, 200)
        # Minified group serialization: verbatim condition, no whitespace
        # before the brace, minified body.
        assert_contains(
            response,
            r"@supports \(display: grid\)\{",
            "Group condition should be preserved and its block minified",
        )
        assert_not_contains(response, r"This comment will be removed")

    def test_group_rule_stylesheet_rewritten(
        self, client: PageSpeedClient, example_root: str
    ):
        """The external group-rule stylesheet gets a rewritten URL."""
        response = client.fetch_until_count(
            self._page_url(example_root),
            pattern=REWRITTEN_CSS_LINK_PATTERN,
            expected_count=1,
            timeout=60.0,
            headers=BLOCKING_HEADERS,
        )
        assert_http_status(response, 200)

    def test_group_rule_stylesheet_contents_optimized(
        self, client: PageSpeedClient, example_root: str
    ):
        """The rewritten stylesheet keeps its group structure, loses the
        comment, and carries an optimized image URL inside @layer."""
        response = client.fetch_until_count(
            self._page_url(example_root),
            pattern=REWRITTEN_CSS_LINK_PATTERN,
            expected_count=1,
            timeout=60.0,
            headers=BLOCKING_HEADERS,
        )

        match = re.search(
            r'href="([^"]*rewrite_css_group_rules\.css\.pagespeed\.cf\.[^"]*)"',
            response.text,
        )
        assert match, "Could not find rewritten group-rule CSS URL"

        css_url = match.group(1)
        if css_url.startswith("http://") or css_url.startswith("https://"):
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        # The nested image rewrite completes asynchronously, so poll the
        # stylesheet itself until the image URL inside @layer is rewritten.
        css_response = client.fetch_until_count(
            css_url,
            pattern=REWRITTEN_CSS_IMAGE_PATTERN,
            expected_count=1,
            timeout=60.0,
            headers=BLOCKING_HEADERS,
        )
        assert_http_status(css_response, 200)
        # Group structure is intact and minified; conditions are verbatim,
        # including the MQ4 range media query.
        assert_contains(
            css_response,
            r"@layer components\{",
            "@layer block should survive rewriting minified",
        )
        assert_contains(
            css_response,
            r"@supports \(display: grid\)\{",
            "@supports block should survive rewriting minified",
        )
        assert_contains(
            css_response,
            r"@media \(width >= 200px\)\{",
            "MQ4 range media query should be preserved verbatim",
        )
        assert_not_contains(css_response, r"This comment will be removed")


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
