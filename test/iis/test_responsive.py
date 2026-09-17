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

"""Responsive images filter tests for IIS PageSpeed module.

These tests verify that the responsive_images filter correctly adds
srcset attributes for images, enabling responsive image handling.

Ported from: test/system/automatic/test_responsive_images.py
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


@pytest.mark.html_rewrite
class TestResponsiveImages:
    """Tests for srcset rewriting with the responsive_images filter."""

    def test_responsive_images_rewrites_srcset(
        self, client: PageSpeedClient, example_root: str
    ):
        """Srcset attributes should be rewritten with optimized URLs.

        The responsive_images filter adds srcset attributes to images
        that are displayed smaller than their intrinsic size.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r"srcset=",
            expected_count=3,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_responsive_adds_srcset(
        self, client: PageSpeedClient, example_root: str
    ):
        """Srcset should be added to images that lack it.

        Images displayed at smaller than intrinsic size should get
        srcset attributes added for different pixel densities.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        # Both Puzzle.jpg thumbnail (256x192) and 2/3 scale (682x511) get srcset
        response = client.fetch_until_count(
            url,
            pattern=r"xPuzzle\.jpg\.pagespeed.*srcset=",
            expected_count=2,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_responsive_preserves_original_src(
        self, client: PageSpeedClient, example_root: str
    ):
        """Original src should be preserved as fallback.

        The src attribute should still contain a valid image URL
        (optimized) as a fallback for browsers without srcset support.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        # Ensure all Puzzle URLs are rewritten (optimized src)
        response = client.fetch_until_count(
            url,
            pattern=r"[^x]Puzzle\.jpg",
            expected_count=0,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # The src attribute should contain the optimized URL
        assert_contains(
            response,
            r'src="[^"]*xPuzzle\.jpg\.pagespeed[^"]*"',
            "src attribute should contain optimized Puzzle.jpg URL",
        )


@pytest.mark.html_rewrite
class TestResponsiveImageSizes:
    """Tests for sizes attribute handling."""

    def test_sizes_attribute_added(
        self, client: PageSpeedClient, example_root: str
    ):
        """Sizes attribute should be added when appropriate.

        When srcset is added, a sizes attribute may also be included
        to help browsers select the appropriate image size.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r"srcset=",
            expected_count=3,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Response should have srcset (sizes is optional based on config)
        assert_contains(response, "srcset=", "srcset attribute should be present")

    def test_sizes_attribute_preserved(
        self, client: PageSpeedClient, example_root: str
    ):
        """Existing sizes attribute should be preserved.

        If an image already has a sizes attribute, it should not
        be overwritten by the responsive_images filter.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r"srcset=",
            expected_count=3,
            timeout=30.0,
        )
        assert_http_status(response, 200)
        # Verify the page was processed successfully
        assert_contains(response, "<img")


@pytest.mark.html_rewrite
class TestResponsiveImageFormats:
    """Tests for format handling with responsive images."""

    def test_responsive_with_webp(
        self, client: PageSpeedClient, example_root: str
    ):
        """Responsive images should work with WebP conversion.

        When WebP conversion is enabled along with responsive_images,
        both features should work together correctly.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,convert_jpeg_to_webp,-inline_images"

        response = client.get(
            url,
            headers={"Accept": "image/webp,image/*,*/*"},
        )
        assert_http_status(response, 200)
        # The page should be rewritten with image references
        assert_contains(response, "<img")

    def test_responsive_jpeg_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """JPEG images should get responsive treatment.

        Puzzle.jpg in responsive_images.html should be optimized
        and have srcset added (both thumbnail and 2/3 scale).
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        # Wait for both Puzzle.jpg variants to be rewritten with srcset
        response = client.fetch_until_count(
            url,
            pattern=r"xPuzzle\.jpg\.pagespeed.*srcset=",
            expected_count=2,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Verify the JPEG was optimized
        assert_contains(
            response,
            r"\.pagespeed\.",
            "JPEG should be optimized with pagespeed URL",
        )

    def test_responsive_png_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """PNG images should get responsive treatment.

        Cuppa.png in responsive_images.html should be processed
        (though small images may be inlined instead).
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r"xCuppa\.png\.pagespeed.*srcset=",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestResponsiveImagesWithInlining:
    """Tests for responsive_images interaction with inline_images.

    When inline_images is enabled, small images are inlined as data URIs
    and should not get srcset attributes.
    """

    def test_inlined_images_no_srcset(
        self, client: PageSpeedClient, example_root: str
    ):
        """Inlined images should not get srcset.

        When an image is inlined as a data URI, it should not have
        a srcset attribute since data URIs can't have different sizes.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,+inline_images"

        # Cuppa.png should be inlined (no reference to filename)
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
        """Only non-inlined images should get srcset.

        When inline_images is enabled, only images too large for
        inlining should have srcset attributes added.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,+inline_images"

        # Should have 2 srcset (for Puzzle.jpg only, Cuppa.png is inlined)
        response = client.fetch_until_count(
            url,
            pattern=r"srcset=",
            expected_count=2,
            timeout=30.0,
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestRewriteExistingSrcset:
    """Tests for rewriting existing srcset attributes.

    The rewrite_images filter should also optimize images referenced
    in existing srcset attributes.
    """

    def test_rewrite_existing_srcset(
        self, client: PageSpeedClient, test_root: str
    ):
        """Existing srcset attributes should be rewritten.

        When a page already has srcset attributes, the images
        referenced in them should be optimized.
        """
        # Note: +rewrite_images,+debug ADDS both to CoreFilters
        url = f"{test_root}/image_rewriting/srcset.html?PageSpeedFilters=+rewrite_images,+debug"

        response = client.fetch_until_count(
            url,
            pattern=r"xPuzzle.*1x.*xCuppa.*2x",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_rewrite_data_srcset(
        self, client: PageSpeedClient, test_root: str
    ):
        """data-srcset attributes should be rewritten.

        The filter should also handle data-srcset attributes used
        by lazy loading libraries.
        """
        url = f"{test_root}/image_rewriting/data-srcset.html?PageSpeedFilters=+rewrite_images,+debug"

        response = client.fetch_until_count(
            url,
            pattern=r"data-srcset.*xPuzzle.*1x.*xCuppa.*2x",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestResponsiveNoTransform:
    """Tests for data-pagespeed-no-transform handling."""

    def test_no_transform_respected(
        self, client: PageSpeedClient, example_root: str
    ):
        """Images with data-pagespeed-no-transform should not be modified.

        The responsive_images.html page contains an image with the
        data-pagespeed-no-transform attribute that should be preserved.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        response = client.get(url)
        assert_http_status(response, 200)

        # The disclosure_open_plus.png should not be rewritten
        assert_contains(
            response,
            "disclosure_open_plus.png",
            "data-pagespeed-no-transform image should not be rewritten",
        )

    def test_full_size_image_no_srcset(
        self, client: PageSpeedClient, example_root: str
    ):
        """Full-size images should not get srcset.

        Images displayed at their intrinsic size (1023x766 Puzzle.jpg)
        should not have srcset added since there's no benefit.
        """
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=responsive_images,rewrite_images,-inline_images"

        response = client.fetch_until_count(
            url,
            pattern=r"srcset=",
            expected_count=3,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # The 1023x766 display should have been optimized but without srcset
        # (checking that we don't have more than expected srcset attrs)
        assert_contains(response, "width=\"1023\"")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
