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

"""Hint preload subresources filter tests.

Ported from: pagespeed/automatic/system_tests/hint_preload_subresources.sh

These tests verify that the hint_preload_subresources filter correctly adds
Link headers with preload hints for critical resources.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


@pytest.mark.not_iis  # Link:rel=preload headers sent after IIS collects headers (architectural)
class TestHintPreloadSubresources:
    """Tests for the hint_preload_subresources filter.

    Bash original::

        test_filter hint_preload_subresources works, and finds indirects
        # Expect 6 resources to be hinted
        fetch_until -save $URL 'grep -c ^Link:' 6 --save-headers
    """

    def test_preload_hints_count(
        self, client: PageSpeedClient, example_root: str
    ):
        """hint_preload_subresources should add 6 Link headers.

        Bash original::

            fetch_until -save $URL 'grep -c ^Link:' 6 --save-headers
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Count Link headers - the example page hints its stylesheet, the
        # three @imported stylesheets, one @font-face woff2 font, and one
        # script: 6 preloads.
        response = client.fetch_until(
            url,
            condition=lambda r: r.header("Link", "").count("rel=preload") >= 6,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        link_count = link_header.count("rel=preload")
        assert link_count >= 6, f"Expected 6 Link preload headers, got {link_count}"

    def test_preload_css_main(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should hint preload for main CSS file.

        Bash original::

            check_from "$OUT" fgrep \
              'Link: </mod_pagespeed_example/styles/all_using_imports.css>; rel=preload; as=style; nopush'
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Use fetch_until to wait for Link headers (not blocking rewrite)
        response = client.fetch_until(
            url,
            condition=lambda r: "all_using_imports.css" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert re.search(r"all_using_imports\.css.*rel=preload.*as=style", link_header), \
            f"Should hint preload main CSS, got Link: {link_header}"

    def test_preload_css_imports(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should hint preload for imported CSS files.

        Bash original::

            check_from "$OUT" fgrep \
              'Link: </mod_pagespeed_example/styles/yellow.css>; rel=preload; as=style; nopush'
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Use fetch_until to wait for Link headers
        response = client.fetch_until(
            url,
            condition=lambda r: "yellow.css" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert re.search(r"yellow\.css.*rel=preload.*as=style", link_header), \
            f"Should hint preload yellow.css, got Link: {link_header}"

    def test_preload_javascript(
        self, client: PageSpeedClient, example_root: str
    ):
        """Should hint preload for JavaScript files.

        Bash original::

            check_from "$OUT" fgrep \
              'Link: </mod_pagespeed_example/inline_javascript.js>; rel=preload; as=script; nopush'
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Use fetch_until to wait for Link headers
        response = client.fetch_until(
            url,
            condition=lambda r: "inline_javascript.js" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert re.search(r"inline_javascript\.js.*rel=preload.*as=script", link_header), \
            f"Should hint preload JavaScript, got Link: {link_header}"

    def test_preload_font(
        self, client: PageSpeedClient, example_root: str
    ):
        """woff2 fonts from @font-face rules should preload with as=font.

        Font preloads must carry the crossorigin attribute: fonts are always
        fetched in anonymous CORS mode, so a preload without it would not
        match the later fetch and the font would be downloaded twice.
        """
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Use fetch_until to wait for the font Link header
        response = client.fetch_until(
            url,
            condition=lambda r: "example.woff2" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert re.search(
            r"example\.woff2[^,]*rel=preload[^,]*as=font[^,]*crossorigin",
            link_header,
        ), f"Should hint preload woff2 font with as=font and crossorigin, got Link: {link_header}"

    def test_preload_includes_nopush(
        self, client: PageSpeedClient, example_root: str
    ):
        """Preload hints should include nopush directive."""
        url = f"{example_root}/hint_preload_subresources.html?PageSpeedFilters=hint_preload_subresources"

        # Use fetch_until to wait for Link headers
        response = client.fetch_until(
            url,
            condition=lambda r: "nopush" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert re.search(r"rel=preload.*nopush", link_header), \
            f"Preload hints should include nopush, got Link: {link_header}"


@pytest.mark.not_iis  # Link:rel=preload headers sent after IIS collects headers (architectural)
class TestHintPreloadModuleScripts:
    """<script type=module> subresources are hinted with rel=modulepreload.

    Asserted against rewrite_javascript_module.html rather than
    hint_preload_subresources.html on purpose: the bash original of the
    class above pins that page's Link-header count at exactly 6, so adding a
    module to it would break the bash lane.
    """

    MODULE_URL_SUFFIX = (
        "rewrite_javascript_module.html"
        "?PageSpeedFilters=hint_preload_subresources,rewrite_javascript"
    )

    def test_module_hinted_with_modulepreload(
        self, client: PageSpeedClient, example_root: str
    ):
        """The external module gets rel=modulepreload, not rel=preload.

        A rel=preload; as=script hint occupies a different preload-cache slot
        than the module map fetch, so it would make the browser fetch the
        module twice.
        """
        url = f"{example_root}/{self.MODULE_URL_SUFFIX}"

        response = client.fetch_until(
            url,
            condition=lambda r: "modulepreload" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        link_header = response.header("Link", "")
        assert re.search(
            r"rewrite_javascript_module[^,]*\.js>; rel=modulepreload; nopush",
            link_header,
        ), f"Should hint the module with rel=modulepreload, got Link: {link_header}"

    def test_modulepreload_has_no_as_or_crossorigin(
        self, client: PageSpeedClient, example_root: str
    ):
        """modulepreload carries neither an as= nor a crossorigin param.

        Its destination already defaults to script, and its default
        credentials mode already matches a <script type=module> without a
        crossorigin attribute, so both params would be redundant bytes.
        """
        url = f"{example_root}/{self.MODULE_URL_SUFFIX}"

        response = client.fetch_until(
            url,
            condition=lambda r: "modulepreload" in r.header("Link", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Scope the assertions to the module's own link-value: a page-global
        # check would silently start passing (or failing) on whatever else
        # the page happens to hint.
        link_header = response.header("Link", "")
        module_values = [
            value for value in link_header.split(",")
            if "modulepreload" in value
        ]
        assert len(module_values) == 1, \
            f"Expected exactly one modulepreload hint, got Link: {link_header}"
        module_value = module_values[0]
        assert "as=" not in module_value, \
            f"modulepreload must not carry an as= param, got: {module_value}"
        assert "crossorigin" not in module_value, \
            f"modulepreload must not carry crossorigin, got: {module_value}"
        assert_contains(module_value, "nopush")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
