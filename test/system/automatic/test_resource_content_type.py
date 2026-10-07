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

"""Resource content type and nosniff tests.

Ported from: pagespeed/automatic/system_tests/resource_content_type_html.sh

These tests verify that resources are served with correct content types
and X-Content-Type-Options: nosniff header.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    require_status_ok,
)


def _verify_nosniff(client: PageSpeedClient, rewritten_root: str, leaf: str, *content_types):
    """verify_nosniff LEAF CONTENT_TYPE... (resource_content_type_html.sh:15-45)."""
    response = client.get(f"{rewritten_root}/{leaf}")
    require_status_ok(response, leaf)
    content_type = response.header("Content-Type")
    assert any(content_type.startswith(t) for t in content_types), (
        f"{leaf}: Content-Type {content_type!r} is none of {content_types}"
    )
    assert response.header("X-Content-Type-Options").startswith("nosniff"), (
        f"{leaf}: X-Content-Type-Options {response.header('X-Content-Type-Options')!r}"
    )


class TestResourceContentTypeNosniff:
    """Tests for correct content type and nosniff headers.

    Bash original::

        start_test js minification css
        verify_nosniff styles/big.css.pagespeed.jm.0.foo \
          text/css application/javascript
    """

    def test_css_has_nosniff(self, client: PageSpeedClient, rewritten_root: str):
        """js minification css: a .jm. URL for a CSS file keeps a CSS or JS type."""
        _verify_nosniff(
            client, rewritten_root, "styles/big.css.pagespeed.jm.0.foo",
            "text/css", "application/javascript",
        )

    def test_js_has_nosniff(self, client: PageSpeedClient, rewritten_root: str):
        """js minification js.

        text/javascript is accepted next to the bash's two types: it is the
        registered JavaScript type (RFC 9239), and the IIS port serves
        JavaScript under it.
        """
        _verify_nosniff(
            client, rewritten_root, "rewrite_javascript.js.pagespeed.jm.0.foo",
            "application/javascript", "application/x-javascript", "text/javascript",
        )

    def test_png_has_nosniff(self, client: PageSpeedClient, rewritten_root: str):
        """js minification png: a .jm. URL for a PNG keeps image/png."""
        _verify_nosniff(
            client, rewritten_root, "images/Cuppa.png.pagespeed.jm.0.foo", "image/png"
        )

    @pytest.mark.parametrize(
        "leaf,content_types",
        [
            pytest.param("styles/big.css.pagespeed.is.0.foo", ("text/css",),
                         id="image-spriting-css"),
            pytest.param("styles/xbig.css.pagespeed.ic.0.foo", ("text/css",),
                         id="image-compression-css"),
            pytest.param("styles/big.css.pagespeed.ce.0.foo", ("text/css",),
                         id="cache-extension-css"),
            pytest.param("images/IronChef2.gif.pagespeed.jm.0.foo", ("image/gif",),
                         id="js-minification-gif"),
            pytest.param("images/Puzzle.jpg.pagespeed.jm.0.foo", ("image/jpeg",),
                         id="js-minification-jpg"),
            pytest.param("images/gray_saved_as_rgb.webp.pagespeed.jm.0.foo",
                         ("image/webp",), id="js-minification-webp"),
            pytest.param("example.pdf.pagespeed.jm.0.foo", ("application/pdf",),
                         id="js-minification-pdf"),
        ],
    )
    def test_nosniff_for_other_rewriter_codes(
        self, client: PageSpeedClient, rewritten_root: str, leaf, content_types
    ):
        """verify_nosniff for the other rewriter codes (resource_content_type_html.sh:62-96).

        A .pagespeed. URL whose rewriter code does not match the resource type
        is still served with the resource's own type and nosniff.
        """
        _verify_nosniff(client, rewritten_root, leaf, *content_types)


class TestResourceContentTypeErrors:
    """Tests for error responses on HTML and SVG resources.

    Bash original::

        # test that we 404 html
        start_test js minification html
        verify_error index.html.pagespeed.jm.0.foo
    """

    def test_html_returns_error(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """HTML files should return 404 or 500 when accessed as resources."""
        url = f"{rewritten_root}/index.html.pagespeed.jm.0.foo"

        response = client.get(url)

        # Should be an error (404 or 500)
        assert response.status in [404, 500], \
            f"HTML resource should return 404 or 500, got: {response.status}"

    def test_svg_returns_error(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """SVG files should return 404 or 500 when accessed as resources."""
        url = f"{rewritten_root}/images/schedule_event.svg.pagespeed.jm.0.foo"

        response = client.get(url)

        # Should be an error (404 or 500)
        assert response.status in [404, 500], \
            f"SVG resource should return 404 or 500, got: {response.status}"

    @pytest.mark.parametrize(
        "leaf",
        [
            pytest.param("index.html.pagespeed.is.0.foo", id="image-spriting-html"),
            pytest.param("xindex.html.pagespeed.ic.0.foo", id="image-compression-html"),
            pytest.param("index.html.pagespeed.ce.0.foo", id="cache-extension-html"),
        ],
    )
    def test_html_via_other_rewriter_codes_is_an_error(
        self, client: PageSpeedClient, rewritten_root: str, leaf
    ):
        """verify_error (resource_content_type_html.sh:47-57, 104-114): 404 or 500."""
        response = client.get(f"{rewritten_root}/{leaf}")
        assert response.status in (404, 500), (
            f"{leaf}: HTML must never be served as a rewritten resource, "
            f"got HTTP {response.status}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
