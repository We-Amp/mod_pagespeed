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

"""Responsive images filter tests.

Ported from: pagespeed/automatic/system_tests/responsive_images.sh

These tests verify that the responsive_images filter correctly adds
srcset attributes for images.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestResponsiveImages:
    """Tests for the responsive_images filter.

    Bash original::

        test_filter responsive_images,rewrite_images,-inline_images adds srcset for \
          Puzzle.jpg and Cuppa.png
        fetch_until $URL 'grep -c srcset=' 3
    """

    def test_responsive_images_adds_srcset(
        self, client: PageSpeedClient, example_root: str
    ):
        """responsive_images should add srcset attributes."""
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r"srcset=",
            expected_count=3,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_responsive_images_rewrites_puzzle(
        self, client: PageSpeedClient, example_root: str
    ):
        """Puzzle.jpg should be rewritten with srcset.

        Bash original::

            # Make sure all Puzzle URLs are rewritten.
            fetch_until -save $URL 'grep -c [^x]Puzzle.jpg' 0
            check egrep -q 'xPuzzle.jpg.pagespeed.+srcset='
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        # Fetch until all Puzzle URLs are rewritten (no non-rewritten ones)
        response = client.fetch_until_count(
            url,
            pattern=r"[^x]Puzzle\.jpg",
            expected_count=0,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Check for rewritten Puzzle with srcset
        assert_contains(
            response,
            r'xPuzzle\.jpg\.pagespeed.*srcset=',
            "Puzzle.jpg should have srcset",
        )

    def test_responsive_images_rewrites_cuppa(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cuppa.png should be rewritten with srcset."""
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        # Fetch until all Cuppa URLs are rewritten
        response = client.fetch_until_count(
            url,
            pattern=r"[^x]Cuppa\.png",
            expected_count=0,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Check for rewritten Cuppa with srcset
        assert_contains(
            response,
            r'xCuppa\.png\.pagespeed.*srcset=',
            "Cuppa.png should have srcset",
        )


class TestResponsiveImagesWithInlining:
    """Tests for responsive_images with inline_images enabled.

    Bash original::

        test_filter responsive_images,rewrite_images,+inline_images adds srcset for \
          Puzzle.jpg, but not Cuppa.png
        # Cuppa.png will be inlined, so we should not get a srcset for it.
        fetch_until $URL 'grep -c Cuppa.png' 0  # Make sure Cuppa.png is inlined.
        fetch_until $URL 'grep -c srcset=' 2    # And only two srcsets (for Puzzle.jpg).
    """

    def test_inlined_images_no_srcset(
        self, client: PageSpeedClient, example_root: str
    ):
        """Inlined images should not get srcset."""
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,+inline_images"

        # Cuppa.png should be inlined (no reference to file)
        response = client.fetch_until_count(
            url,
            pattern=r"Cuppa\.png",
            expected_count=0,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_only_non_inlined_images_get_srcset(
        self, client: PageSpeedClient, example_root: str
    ):
        """Only non-inlined images should get srcset (Puzzle.jpg only)."""
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,+inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r"srcset=",
            expected_count=2,
            timeout=30.0,
        )
        assert_http_status(response, 200)


class TestRewriteSrcset:
    """Tests for rewriting existing srcset attributes.

    Bash original::

        start_test rewrite_images can rewrite srcset itself
        URL=$TEST_ROOT/image_rewriting/srcset.html?PageSpeedFilters=+rewrite_images,+debug
        fetch_until -save $URL 'grep -c xPuzzle.*1x.*xCuppa.*2x' 1
    """

    def test_rewrite_existing_srcset(
        self, client: PageSpeedClient, test_root: str
    ):
        """Existing srcset attributes should be rewritten."""
        url = f"{test_root}/image_rewriting/srcset.html?PageSpeedFilters=+rewrite_images,+debug"

        response = client.fetch_until_count(
            url,
            pattern=r"xPuzzle.*1x.*xCuppa.*2x",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)


class TestRewriteDataSrcset:
    """Tests for rewriting data-srcset attributes.

    Bash original::

        start_test rewrite_images_datasrcset can rewrite data-srcset itself
        URL=$TEST_ROOT/image_rewriting/data-srcset.html?PageSpeedFilters=+rewrite_images,+debug
        fetch_until -save $URL 'grep -c srcset.*xPuzzle.*1x.*xCuppa.*2x' 2
    """

    def test_rewrite_data_srcset(
        self, client: PageSpeedClient, test_root: str
    ):
        """data-srcset attributes should be rewritten."""
        url = f"{test_root}/image_rewriting/data-srcset.html?PageSpeedFilters=+rewrite_images,+debug"

        response = client.fetch_until_count(
            url,
            pattern=r"srcset.*xPuzzle.*1x.*xCuppa.*2x",
            expected_count=2,
            timeout=30.0,
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
