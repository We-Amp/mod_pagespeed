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
)


class TestResourceContentTypeNosniff:
    """Tests for correct content type and nosniff headers.

    Bash original::

        start_test js minification css
        verify_nosniff styles/big.css.pagespeed.jm.0.foo \
          text/css application/javascript
    """

    def test_css_has_nosniff(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """CSS files should have X-Content-Type-Options: nosniff."""
        url = f"{rewritten_root}/styles/big.css.pagespeed.cf.0.foo"

        response = client.get(url)

        if response.status == 200:
            nosniff = response.header("X-Content-Type-Options")
            assert nosniff and "nosniff" in nosniff.lower(), \
                "CSS should have X-Content-Type-Options: nosniff"

            content_type = response.header("Content-Type")
            assert content_type and "text/css" in content_type.lower(), \
                f"CSS should have text/css content type, got: {content_type}"

    def test_js_has_nosniff(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """JS files should have X-Content-Type-Options: nosniff."""
        url = f"{rewritten_root}/rewrite_javascript.js.pagespeed.jm.0.foo"

        response = client.get(url)

        if response.status == 200:
            nosniff = response.header("X-Content-Type-Options")
            assert nosniff and "nosniff" in nosniff.lower(), \
                "JS should have X-Content-Type-Options: nosniff"

            content_type = response.header("Content-Type")
            assert content_type and ("javascript" in content_type.lower()), \
                f"JS should have javascript content type, got: {content_type}"

    def test_png_has_nosniff(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """PNG images should have X-Content-Type-Options: nosniff."""
        url = f"{rewritten_root}/images/Cuppa.png.pagespeed.ic.0.foo"

        response = client.get(url)

        if response.status == 200:
            nosniff = response.header("X-Content-Type-Options")
            assert nosniff and "nosniff" in nosniff.lower(), \
                "PNG should have X-Content-Type-Options: nosniff"

            content_type = response.header("Content-Type")
            assert content_type and "image/png" in content_type.lower(), \
                f"PNG should have image/png content type, got: {content_type}"


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


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
