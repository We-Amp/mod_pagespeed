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

"""CSS sprite images filter tests.

Ported from: pagespeed/automatic/system_tests/css_sprite_images.sh

These tests verify that the sprite_images filter correctly combines
multiple images into CSS sprites.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestCssSpriteImagesInline:
    """Tests for sprite_images with inline_css.

    Bash original::

        start_test inline_css,rewrite_css,sprite_images sprites images in CSS.
        FILE=sprite_images.html?PageSpeedFilters=inline_css,rewrite_css,sprite_images
        URL=$EXAMPLE_ROOT/$FILE
        fetch_until $URL \
          'grep -c Cuppa.png.*BikeCrashIcn.png.*IronChef2.gif.*.pagespeed.is.*.png' 1
    """

    def test_sprite_images_combines_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """Multiple images should be combined into a sprite."""
        url = f"{example_root}/sprite_images.html?PageSpeedFilters=inline_css,rewrite_css,sprite_images"

        # The sprite should contain multiple images combined.
        #
        # The filters here are a chain, not a set: the pattern matches only
        # once the CSS has been rewritten with sprite .pagespeed.is URLs AND
        # that CSS has been inlined into the HTML, so this page's convergence
        # stacks rewrites where most tests wait for one. On a busy runner
        # that stacks past a 60s window, so keep the framework's full
        # default: the bash original ran every fetch_until with TIMEOUT=100
        # (system_test_helpers.sh) and 60.0 was a Python-port tightening
        # this test never needed to make. The wait itself stays a
        # conditional poll (fetch_until_contains, 0.5s interval); the
        # timeout only bounds how long the poll may run. #786.
        response = client.fetch_until_contains(
            url,
            pattern=r"Cuppa\.png.*BikeCrashIcn\.png.*IronChef2\.gif.*\.pagespeed\.is\..*\.png",
            timeout=100.0,
        )
        assert_http_status(response, 200)


class TestCssSpriteImagesExternal:
    """Tests for sprite_images with external CSS.

    Bash original::

        start_test rewrite_css,sprite_images sprites images in CSS.
        FILE=sprite_images.html?PageSpeedFilters=rewrite_css,sprite_images
        URL=$EXAMPLE_ROOT/$FILE
        fetch_until -save -recursive $URL 'grep -c css.pagespeed.cf' 1
        CSS=$(grep stylesheet "$WGET_DIR/$(basename $URL)" | cut -d\" -f 6)
        check [ $(grep -c "ic.pagespeed.is" "$SPRITE_CSS_OUT") -gt 0 ]
    """

    def test_external_css_sprite_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """External CSS should contain sprite references.

        The original bash test uses ``fetch_until -save -recursive`` which
        re-downloads the HTML **and** linked resources on every retry.  This
        is important because the ``.pagespeed.cf.<hash>`` URL contains a
        content hash — once sprite optimization completes the hash changes,
        so we must re-extract the CSS URL from the HTML on each attempt.
        """
        url = f"{example_root}/sprite_images.html?PageSpeedFilters=rewrite_css,sprite_images"

        def html_has_sprited_css(response):
            """Check if the HTML links to a CSS file that contains sprites."""
            match = re.search(
                r'href="([^"]*\.pagespeed\.cf\.[^"]*\.css)"', response.text
            )
            if not match:
                return False
            css_url = match.group(1)
            if css_url.startswith(("http://", "https://")):
                from urllib.parse import urlparse
                css_url = urlparse(css_url).path
            elif not css_url.startswith("/"):
                css_url = f"{example_root}/{css_url}"
            css_response = client.get(css_url)
            return (
                css_response.status == 200
                and re.search(r"ic\.pagespeed\.is", css_response.text) is not None
            )

        response = client.fetch_until(url, html_has_sprited_css, timeout=120.0)
        assert_http_status(response, 200)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
