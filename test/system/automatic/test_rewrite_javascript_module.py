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

"""Module-script rewriting tests.

These tests verify that rewrite_javascript minifies <script type="module">
elements while preserving the module attribute and import specifiers, and
that modules are never combined or inlined.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)

BLOCKING_HEADERS = {"X-PSA-Blocking-Rewrite": "psatest"}

REWRITTEN_SRC_PATTERN = r"src=.*rewrite_javascript_module\.js\.pagespeed\.jm\."

# The rewrite only replaces the src attribute value, so the rewritten
# element must still carry type="module" ahead of the renamed src.
REWRITTEN_MODULE_ELEMENT_PATTERN = (
    r'<script type=["\']?module["\']?[^>]*\.pagespeed\.jm\.'
)


class TestRewriteJavascriptModule:
    """Tests for rewrite_javascript applied to module scripts."""

    def _fetch_rewritten_page(self, client: PageSpeedClient, example_root: str):
        url = (
            f"{example_root}/rewrite_javascript_module.html"
            "?PageSpeedFilters=rewrite_javascript"
        )
        return client.fetch_until_count(
            url,
            pattern=REWRITTEN_SRC_PATTERN,
            expected_count=1,
            timeout=30.0,
            headers=BLOCKING_HEADERS,
        )

    def test_module_src_rewritten(
        self, client: PageSpeedClient, example_root: str
    ):
        """The external module src is renamed and stays a module."""
        response = self._fetch_rewritten_page(client, example_root)
        assert_http_status(response, 200)
        assert_contains(
            response,
            REWRITTEN_MODULE_ELEMENT_PATTERN,
            "Rewritten module element should keep type=module",
        )

    def test_module_resource_minified_and_import_preserved(
        self, client: PageSpeedClient, example_root: str
    ):
        """The rewritten module body loses comments but keeps its import."""
        response = self._fetch_rewritten_page(client, example_root)

        match = re.search(
            r'src="([^"]*rewrite_javascript_module\.js\.pagespeed\.jm\.[^"]*)"',
            response.text,
        )
        assert match, "Could not find rewritten module JS URL"

        js_url = match.group(1)
        if js_url.startswith("http://") or js_url.startswith("https://"):
            from urllib.parse import urlparse
            js_url = urlparse(js_url).path
        elif not js_url.startswith("/"):
            js_url = f"{example_root}/{js_url}"

        # The rewritten resource stays in the source directory so the
        # relative import keeps resolving identically.
        assert "/mod_pagespeed_example/" in js_url, (
            f"Rewritten module URL left the source directory: {js_url}"
        )

        js_response = client.get(js_url)
        assert_http_status(js_response, 200)
        assert_not_contains(js_response, r"\bremoved\b")
        # The minifier never rewrites import specifiers.
        assert_contains(
            js_response,
            r"'\./rewrite_javascript_module_util\.js'",
            "Relative import specifier should survive minification intact",
        )

    def test_inline_module_minified(
        self, client: PageSpeedClient, example_root: str
    ):
        """The inline module is minified in place and stays a module."""
        url = (
            f"{example_root}/rewrite_javascript_module.html"
            "?PageSpeedFilters=rewrite_javascript"
        )
        response = client.fetch_until_count(
            url,
            pattern=r"This comment will be removed",
            expected_count=0,
            timeout=30.0,
            headers=BLOCKING_HEADERS,
        )
        assert_http_status(response, 200)
        assert_contains(
            response,
            r'<script type=["\']?module["\']?>',
            "Inline module should keep type=module",
        )
        assert_contains(
            response,
            r"'\./rewrite_javascript_module_util\.js'",
            "Inline module import specifier should survive minification",
        )

    def test_module_not_combined_or_inlined(
        self, client: PageSpeedClient, example_root: str
    ):
        """Modules survive combine_javascript and inline_javascript."""
        url = (
            f"{example_root}/rewrite_javascript_module.html"
            "?PageSpeedFilters=combine_javascript,inline_javascript"
            ",rewrite_javascript"
        )
        response = client.fetch_until_count(
            url,
            pattern=REWRITTEN_SRC_PATTERN,
            expected_count=1,
            timeout=30.0,
            headers=BLOCKING_HEADERS,
        )
        assert_http_status(response, 200)
        # Not combined: no eval-trigger rewrite of any module element.
        assert_not_contains(response, r"eval\(mod_pagespeed_")
        assert_not_contains(response, r"\.pagespeed\.jc\.")
        # Not inlined: the external module element keeps its src.
        assert_contains(
            response,
            REWRITTEN_MODULE_ELEMENT_PATTERN,
            "External module element should keep type=module and src",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
