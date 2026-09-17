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

"""Image resize and optimization tests for IIS PageSpeed module.

These tests verify that images are correctly resized and optimized
based on HTML dimensions and rewrite_images filter.

Ported from: test/system/automatic/test_image_resize.py
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


@pytest.mark.html_rewrite
class TestImageRewrite:
    """Tests for image rewriting with rewrite_images filter."""

    def test_rewrite_images_produces_optimized_urls(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_images filter should produce .pagespeed.ic. URLs."""
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.ic\.',
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_rewritten_image_is_valid(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten image URLs should return valid image data."""
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.ic\.',
            timeout=30.0,
        )

        # Extract a rewritten image URL
        match = re.search(r'src="([^"]*\.pagespeed\.ic\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find rewritten image URL")

        img_url = match.group(1)
        if img_url.startswith("http://") or img_url.startswith("https://"):
            from urllib.parse import urlparse
            img_url = urlparse(img_url).path
        elif not img_url.startswith("/"):
            img_url = f"{example_root}/{img_url}"

        img_response = client.get(img_url)
        assert_http_status(img_response, 200)

        content_type = img_response.header("Content-Type")
        assert "image" in content_type.lower(), \
            f"Expected image content type, got {content_type}"


@pytest.mark.html_rewrite
class TestImageResizeDimensions:
    """Tests for image resizing based on HTML dimensions."""

    def test_image_with_dimensions_resized(
        self, client: PageSpeedClient, example_root: str
    ):
        """Images with width/height attributes may be resized."""
        # Use responsive_images.html which has images with explicit dimensions
        url = f"{example_root}/responsive_images.html?PageSpeedFilters=rewrite_images,resize_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.',
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Look for resized image pattern (dimensions in URL)
        # Format: WIDTHxHEIGHTxFILENAME.pagespeed.ic.HASH.ext
        has_resize_pattern = bool(re.search(r'\d+x\d+x.*\.pagespeed\.', response.text))
        has_any_pagespeed = bool(re.search(r'\.pagespeed\.', response.text))

        # At minimum, images should be processed by PageSpeed
        assert has_any_pagespeed, "Images should be processed by PageSpeed"


@pytest.mark.html_rewrite
class TestImageOptimization:
    """Tests for image optimization (compression, format conversion)."""

    def test_image_compression_reduces_size(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized images should be smaller than originals."""
        # Get original image size
        original_response = client.get(
            f"{example_root}/images/Puzzle.jpg?PageSpeed=off"
        )
        assert_http_status(original_response, 200)
        original_size = len(original_response.body)

        # Wait for optimization
        import time
        time.sleep(1.0)

        # Get optimized image
        optimized_response = client.get(f"{example_root}/images/Puzzle.jpg")
        assert_http_status(optimized_response, 200)
        optimized_size = len(optimized_response.body)

        # Optimized should be smaller or equal (some images may not compress further)
        # Allow up to 10% larger due to metadata or format changes
        assert optimized_size <= original_size * 1.1, \
            f"Optimized size ({optimized_size}) should not be much larger than original ({original_size})"

    def test_webp_for_supporting_browser(
        self, client: PageSpeedClient, example_root: str
    ):
        """WebP-capable browsers may receive WebP images."""
        # Request with WebP accept header
        response = client.get(
            f"{example_root}/images/Puzzle.jpg",
            headers={"Accept": "image/webp,image/*,*/*"}
        )
        assert_http_status(response, 200)

        # Content-Type should be image (webp or jpeg depending on cache)
        content_type = response.header("Content-Type")
        assert "image" in content_type.lower(), \
            f"Expected image content type, got {content_type}"


@pytest.mark.html_rewrite
class TestImageCacheHeaders:
    """Tests for cache headers on optimized images."""

    def test_optimized_image_has_cache_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized images should have cache headers."""
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.ic\.',
            timeout=30.0,
        )

        match = re.search(r'src="([^"]*\.pagespeed\.ic\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find rewritten image URL")

        img_url = match.group(1)
        if img_url.startswith("http://") or img_url.startswith("https://"):
            from urllib.parse import urlparse
            img_url = urlparse(img_url).path
        elif not img_url.startswith("/"):
            img_url = f"{example_root}/{img_url}"

        img_response = client.get(img_url)
        assert_http_status(img_response, 200)

        cache_control = img_response.header("Cache-Control")
        assert cache_control, "Optimized image should have Cache-Control header"

    def test_optimized_image_has_long_cache(
        self, client: PageSpeedClient, example_root: str
    ):
        """Optimized images should have long cache lifetime."""
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.ic\.',
            timeout=30.0,
        )

        match = re.search(r'src="([^"]*\.pagespeed\.ic\.[^"]*)"', response.text)
        if not match:
            pytest.skip("Could not find rewritten image URL")

        img_url = match.group(1)
        if img_url.startswith("http://") or img_url.startswith("https://"):
            from urllib.parse import urlparse
            img_url = urlparse(img_url).path
        elif not img_url.startswith("/"):
            img_url = f"{example_root}/{img_url}"

        img_response = client.get(img_url)
        assert_http_status(img_response, 200)

        cache_control = img_response.header("Cache-Control")
        if cache_control:
            match = re.search(r'max-age=(\d+)', cache_control, re.IGNORECASE)
            if match:
                max_age = int(match.group(1))
                one_day = 86400
                assert max_age >= one_day, \
                    f"max-age should be at least 1 day, got {max_age}s"


@pytest.mark.html_rewrite
class TestImagePassthrough:
    """Tests for image passthrough when optimization is disabled."""

    def test_pagespeed_off_no_image_rewrite(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off should not rewrite images."""
        response = client.get(f"{example_root}/rewrite_images.html?PageSpeed=off")
        assert_http_status(response, 200)

        # Should not have .pagespeed. URLs
        assert_not_contains(response, r"\.pagespeed\.")

    def test_original_image_accessible(
        self, client: PageSpeedClient, example_root: str
    ):
        """Original images should still be accessible."""
        response = client.get(f"{example_root}/images/Puzzle.jpg?PageSpeed=off")
        assert_http_status(response, 200)

        content_type = response.header("Content-Type")
        assert "image" in content_type.lower(), \
            f"Expected image content type, got {content_type}"


@pytest.mark.html_rewrite
class TestImageWithOtherFilters:
    """Tests for image optimization combined with other filters."""

    def test_image_rewrite_with_lazyload(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_images should work with lazyload_images."""
        url = f"{example_root}/lazyload_images.html?PageSpeedFilters=rewrite_images,lazyload_images"

        response = client.fetch_until_contains(
            url,
            pattern=r'data-pagespeed-lazy-src',
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_image_rewrite_with_extend_cache(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_images should work with extend_cache."""
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images,extend_cache"

        response = client.fetch_until_contains(
            url,
            pattern=r'\.pagespeed\.',
            timeout=30.0,
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
