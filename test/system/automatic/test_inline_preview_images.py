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

"""Inline preview images (delay_images) filter tests.

Ported from: pagespeed/automatic/system_tests/inline_preview_images.sh

These tests verify that the inline_preview_images filter correctly defers
image loading and shows low-quality placeholders.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


# iPhone user agent for mobile tests
IPHONE_USER_AGENT = (
    "Mozilla/5.0 (iPhone; CPU iPhone OS 10_0 like Mac OS X) "
    "AppleWebKit/602.1.38 (KHTML, like Gecko) Version/10.0 Mobile Safari/602.1"
)


class TestInlinePreviewImagesOptimize:
    r"""Tests for inline_preview_images in optimize mode.

    Bash original::

        test_filter inline_preview_images optimize mode
        FILE=delay_images.html?PageSpeedFilters=$FILTER_NAME
        URL=$EXAMPLE_ROOT/$FILE
        WGET_ARGS="${WGET_ARGS} --user-agent=iPhone"
        fetch_until $URL 'grep -c pagespeed.delayImagesInit' 1
        fetch_until $URL 'grep -c /\*' 0
    """

    def test_inline_preview_images_injects_init(
        self, client: PageSpeedClient, example_root: str
    ):
        """inline_preview_images should inject delayImagesInit."""
        url = f"{example_root}/delay_images.html?PageSpeedFilters=inline_preview_images"

        # Create mobile client
        mobile_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=IPHONE_USER_AGENT,
        )

        response = mobile_client.fetch_until_count(
            url,
            pattern=r"pagespeed\.delayImagesInit",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_inline_preview_images_minified(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimize mode should have minified JS (no block comments)."""
        url = f"{example_root}/delay_images.html?PageSpeedFilters=inline_preview_images"

        mobile_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=IPHONE_USER_AGENT,
        )

        response = mobile_client.fetch_until_count(
            url,
            pattern=r"/\*",
            expected_count=0,
            timeout=30.0,
        )
        assert_http_status(response, 200)


class TestInlinePreviewImagesDebug:
    """Tests for inline_preview_images in debug mode.

    Bash original::

        test_filter inline_preview_images,debug debug mode
        FILE=delay_images.html?PageSpeedFilters=$FILTER_NAME
        URL=$EXAMPLE_ROOT/$FILE
        WGET_ARGS="${WGET_ARGS} --user-agent=iPhone"
        fetch_until $URL 'grep -c pagespeed.delayImagesInit' 3
    """

    def test_inline_preview_images_debug_mode(
        self, client: PageSpeedClient, example_root: str
    ):
        """Debug mode should include multiple init references."""
        url = f"{example_root}/delay_images.html?PageSpeedFilters=inline_preview_images,debug"

        mobile_client = PageSpeedClient(
            host=client.host,
            port=client.port,
            user_agent=IPHONE_USER_AGENT,
        )

        # Note: The original bash test used "grep -c" which counts LINES (3 lines),
        # but Python's regex.findall counts total MATCHES (4 occurrences).
        # The pattern appears on 3 lines, but one line has it twice:
        #   Line 1: "pagespeed.delayImagesInit = function()"
        #   Line 2: "pagespeed.delayImagesInit = pagespeed.delayImagesInit"  (2 matches)
        #   Line 3: "pagespeed.delayImagesInit()"
        response = mobile_client.fetch_until_count(
            url,
            pattern=r"pagespeed\.delayImagesInit",
            expected_count=4,  # Total matches (bash grep -c counted 3 lines)
            timeout=30.0,
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
