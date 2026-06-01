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

"""Dedup inlined images filter tests.

Ported from: pagespeed/automatic/system_tests/dedup_inlined_images.sh

These tests verify that the dedup_inlined_images filter correctly deduplicates
multiple instances of the same inlined image.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestDedupInlinedImages:
    """Tests for the dedup_inlined_images filter.

    Bash original::

        test_filter dedup_inlined_images,inline_images
        fetch_until -save $URL 'fgrep -ocw inlineImg(' 4
        check grep -q "PageSpeed=noscript" $FETCH_FILE
    """

    def test_dedup_inlined_images_uses_inlineimg(
        self, client: PageSpeedClient, example_root: str
    ):
        """Deduplicated images should use inlineImg() function."""
        url = f"{example_root}/dedup_inlined_images.html?PageSpeedFilters=dedup_inlined_images,inline_images"

        # Should have 4 inlineImg( calls for deduplicated images
        response = client.fetch_until_count(
            url,
            pattern=r"\binlineImg\(",
            expected_count=4,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_dedup_inlined_images_has_noscript(
        self, client: PageSpeedClient, example_root: str
    ):
        """Dedup inlined images should have noscript fallback."""
        url = f"{example_root}/dedup_inlined_images.html?PageSpeedFilters=dedup_inlined_images,inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r"\binlineImg\(",
            expected_count=4,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            r"PageSpeed=noscript",
            "Should have noscript fallback",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
