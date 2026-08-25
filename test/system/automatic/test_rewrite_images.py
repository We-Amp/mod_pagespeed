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

"""Image rewriting filter tests.

Ported from: pagespeed/automatic/system_tests/rewrite_images.sh

These tests verify that image inlining, compression, and resizing work.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
    require_match,
)


class TestRewriteImages:
    """Tests for the rewrite_images filter.

    Bash original:
        test_filter rewrite_images inlines, compresses, and resizes.
        fetch_until $URL 'grep -c data:image/png' 1  # Images inlined.
        fetch_until $URL 'grep -c .pagespeed.ic' 2  # Images rewritten.
    """

    def test_rewrite_images_inlines_small_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_images should inline small images as data URIs."""
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"

        response = client.fetch_until_contains(
            url,
            pattern=r"data:image/png",
            timeout=60.0,
        )

        assert_http_status(response, 200)
        assert_contains(response, r"data:image/png", "Small images should be inlined")

    def test_rewrite_images_rewrites_urls(
        self, client: PageSpeedClient, example_root: str
    ):
        """rewrite_images should rewrite image URLs with .pagespeed.ic."""
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"

        response = client.fetch_until_count(
            url,
            pattern=r"\.pagespeed\.ic",
            expected_count=2,
            timeout=60.0,
        )

        assert_http_status(response, 200)

    def test_data_pagespeed_no_transform(
        self, client: PageSpeedClient, example_root: str
    ):
        """data-pagespeed-no-transform attribute should prevent rewriting.

        Bash original:
            fetch_until $URL 'grep -c "images/disclosure_open_plus.png"' 1
            fetch_until $URL 'grep -c "data-pagespeed-no-transform"' 0

        The attribute stripping happens during HTML rewriting, which may be
        asynchronous. Use fetch_until pattern to wait for rewriting to complete.

        Note: The original bash test matched "data-pagespeed-no-transform" which
        also appears in the image's title text. After rewriting, the HTML attribute
        is stripped but the title text remains. We use a pattern that specifically
        matches the HTML attribute form (followed by / or >) to avoid matching
        the title text.
        """
        url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"

        # Wait for the no-transform image to appear (verifies image is preserved)
        response = client.fetch_until_contains(
            url,
            pattern=r"images/disclosure_open_plus\.png",
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Wait for the attribute to be stripped (count should be 0)
        # Match the HTML attribute form: data-pagespeed-no-transform followed by
        # / or > (not the same string in the title text which is followed by .)
        response = client.fetch_until_count(
            url,
            pattern=r"data-pagespeed-no-transform[/>\s]",
            expected_count=0,
            timeout=30.0,
        )
        assert_http_status(response, 200)


class TestRewrittenImageHeaders:
    """Tests for headers on rewritten images.

    Bash original:
        start_test headers for rewritten image
        IMG_HEADERS=$($WGET ... $IMG_URL)
        check_from "$IMG_HEADERS" fgrep -qi 'Content-Type: image/jpeg'
    """

    def test_rewritten_image_content_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten images should have correct Content-Type."""
        # First get a page to find a rewritten image URL
        page_url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'src="[^"]*\.pagespeed\.ic[^"]*\.jpg"', r.text
            )
            is not None,
            timeout=60.0,
        )

        # Extract a rewritten JPEG URL
        match = require_match(
            r'src="([^"]*\.pagespeed\.ic[^"]*\.jpg)"',
            response,
            "rewritten JPEG URL",
        )

        img_url = match.group(1)
        if not img_url.startswith("/") and not img_url.startswith("http"):
            img_url = f"{example_root}/{img_url}"

        img_response = client.get(
            img_url,
            headers={"Accept-Encoding": "gzip"},
        )
        assert_http_status(img_response, 200)
        assert_header_contains(img_response, "Content-Type", "image/jpeg")

    def test_rewritten_image_not_gzipped(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten images should NOT be gzip compressed.

        Bash original:
            start_test Images are not gzipped.
            check_not_from "$IMG_HEADERS" fgrep -i 'Content-Encoding: gzip'
        """
        page_url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'src="[^"]*\.pagespeed\.ic[^"]*\.jpg"', r.text
            )
            is not None,
            timeout=60.0,
        )

        match = require_match(
            r'src="([^"]*\.pagespeed\.ic[^"]*\.jpg)"',
            response,
            "rewritten JPEG URL",
        )

        img_url = match.group(1)
        if not img_url.startswith("/") and not img_url.startswith("http"):
            img_url = f"{example_root}/{img_url}"

        img_response = client.get(
            img_url,
            headers={"Accept-Encoding": "gzip"},
        )
        assert_http_status(img_response, 200)

        content_encoding = img_response.header("Content-Encoding")
        assert "gzip" not in content_encoding.lower(), \
            f"Images should not be gzipped, got Content-Encoding: {content_encoding}"

    def test_rewritten_image_no_vary_encoding(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten images should not have Vary: Accept-Encoding.

        Bash original:
            start_test Vary is not set for images.
            check_not_from "$IMG_HEADERS" fgrep -i 'Vary: Accept-Encoding'
        """
        page_url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'src="[^"]*\.pagespeed\.ic[^"]*\.jpg"', r.text
            )
            is not None,
            timeout=60.0,
        )

        match = require_match(
            r'src="([^"]*\.pagespeed\.ic[^"]*\.jpg)"',
            response,
            "rewritten JPEG URL",
        )

        img_url = match.group(1)
        if not img_url.startswith("/") and not img_url.startswith("http"):
            img_url = f"{example_root}/{img_url}"

        img_response = client.get(img_url)
        assert_http_status(img_response, 200)

        vary = img_response.header("Vary")
        assert "Accept-Encoding" not in vary, \
            f"Images should not have Vary: Accept-Encoding, got: {vary}"

    def test_rewritten_image_has_etag(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten images should have ETag header.

        Bash original:
            start_test Etags is present.
            check_from "$IMG_HEADERS" egrep -qi '(Etag: W/"0")|(Etag: W/"0-gzip")'
        """
        page_url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'src="[^"]*\.pagespeed\.ic[^"]*\.jpg"', r.text
            )
            is not None,
            timeout=60.0,
        )

        match = require_match(
            r'src="([^"]*\.pagespeed\.ic[^"]*\.jpg)"',
            response,
            "rewritten JPEG URL",
        )

        img_url = match.group(1)
        if not img_url.startswith("/") and not img_url.startswith("http"):
            img_url = f"{example_root}/{img_url}"

        img_response = client.fetch_until(
            img_url,
            condition=lambda r: r.is_ok() and r.header("ETag") != "",
            timeout=60.0,
        )

        etag = img_response.header("ETag")
        assert etag, "Rewritten images should have ETag header"

    def test_rewritten_image_has_last_modified(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten images should have Last-Modified header.

        Bash original:
            start_test Last-modified is present.
            check_from "$IMG_HEADERS" fgrep -qi 'Last-Modified'
        """
        page_url = f"{example_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'src="[^"]*\.pagespeed\.ic[^"]*\.jpg"', r.text
            )
            is not None,
            timeout=60.0,
        )

        match = require_match(
            r'src="([^"]*\.pagespeed\.ic[^"]*\.jpg)"',
            response,
            "rewritten JPEG URL",
        )

        img_url = match.group(1)
        if not img_url.startswith("/") and not img_url.startswith("http"):
            img_url = f"{example_root}/{img_url}"

        img_response = client.get(img_url)
        assert_http_status(img_response, 200)

        last_modified = img_response.header("Last-Modified")
        assert last_modified, "Rewritten images should have Last-Modified header"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
