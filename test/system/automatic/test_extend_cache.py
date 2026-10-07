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

"""Cache extension filter tests.

Ported from: pagespeed/automatic/system_tests/extend_cache.sh

These tests verify that the extend_cache filter works correctly.
"""

import re
from datetime import datetime, timezone

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    require_match,
)


class TestExtendCacheImages:
    """Tests for the extend_cache_images filter.

    Bash original:
        test_filter extend_cache_images rewrites an image tag.
        URL=$EXAMPLE_ROOT/extend_cache.html?PageSpeedFilters=extend_cache_images
        fetch_until $URL 'egrep -c src.*/Puzzle[.]jpg[.]pagespeed[.]ce[.].*[.]jpg' 1
    """

    def test_extend_cache_rewrites_images(
        self, client: PageSpeedClient, example_root: str
    ):
        """extend_cache_images should rewrite image URLs with cache-extended names."""
        url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"

        # Wait for image URLs to be rewritten
        response = client.fetch_until_contains(
            url,
            pattern=r'src=.*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg',
            timeout=30.0,
        )

        assert_http_status(response, 200)


class TestExtendCacheWithoutHash:
    """Tests for cache-extended URLs without hash.

    Bash original:
        start_test Attempt to fetch cache-extended image without hash should 404
        check_not run_wget_with_args $REWRITTEN_ROOT/images/Puzzle.jpg.pagespeed.ce..jpg
        check fgrep "404 Not Found" $WGET_OUTPUT
    """

    def test_cache_extended_without_hash_returns_404(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended URL without hash should return 404."""
        # URL with missing hash (empty between .ce. and .jpg)
        url = f"{example_root}/images/Puzzle.jpg.pagespeed.ce..jpg"
        response = client.get(url)
        assert_http_status(response, 404)


class TestIfModifiedSince:
    """Tests for If-Modified-Since handling.

    Bash original:
        start_test Cache-extended image should respond 304 to an If-Modified-Since.
        URL=$REWRITTEN_ROOT/images/Puzzle.jpg.pagespeed.ce.91_WewrLtP.jpg
        DATE=$(date -R)
        check_not run_wget_with_args --header "If-Modified-Since: $DATE" $URL
        check fgrep "304 Not Modified" $WGET_OUTPUT
    """

    def test_cache_extended_responds_304_to_if_modified_since(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended resources should return 304 for If-Modified-Since."""
        # First, get the page to find a cache-extended URL
        page_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'src="[^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg"', r.text
            )
            is not None,
            timeout=30.0,
        )

        # Extract the cache-extended URL
        match = require_match(
            r'src="([^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg)"',
            response,
            "cache-extended image URL",
        )

        image_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if image_url.startswith("http://") or image_url.startswith("https://"):
            from urllib.parse import urlparse
            image_url = urlparse(image_url).path
        elif not image_url.startswith("/"):
            image_url = f"{example_root}/{image_url}"

        # Request with If-Modified-Since in the future
        now = datetime.now(timezone.utc)
        date_header = now.strftime("%a, %d %b %Y %H:%M:%S GMT")

        response = client.get(
            image_url,
            headers={"If-Modified-Since": date_header},
        )

        assert_http_status(response, 304, "Should return 304 Not Modified")


class TestLastModifiedMatch:
    """Tests that Last-Modified date matches origin.

    Bash original:
        start_test Cache-extended last-modified date should match origin
    """

    def test_cache_extended_preserves_last_modified(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended resources should have same Last-Modified as origin."""
        # Get Last-Modified from origin
        origin_url = f"{example_root}/images/Puzzle.jpg?PageSpeed=off"
        origin_response = client.get(origin_url)
        assert_http_status(origin_response, 200)

        origin_last_modified = origin_response.header("Last-Modified")
        if not origin_last_modified:
            # Every lane's static file handling sends Last-Modified for
            # files on disk (Apache/nginx/IIS natively, the Envoy fixture
            # server explicitly), so a missing header is a fixture/server
            # defect, not a reason to pass.
            pytest.fail(
                "Origin served the fixture image without a Last-Modified "
                f"header. Response headers: {dict(origin_response.headers)!r}"
            )

        # Get a cache-extended version
        page_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'src="[^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg"', r.text
            )
            is not None,
            timeout=30.0,
        )

        # Extract and fetch the cache-extended URL
        match = require_match(
            r'src="([^"]*Puzzle\.jpg\.pagespeed\.ce\.[^"]+\.jpg)"',
            response,
            "cache-extended image URL",
        )

        image_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if image_url.startswith("http://") or image_url.startswith("https://"):
            from urllib.parse import urlparse
            image_url = urlparse(image_url).path
        elif not image_url.startswith("/"):
            image_url = f"{example_root}/{image_url}"

        extended_response = client.get(image_url)
        assert_http_status(extended_response, 200)

        extended_last_modified = extended_response.header("Last-Modified")

        assert origin_last_modified == extended_last_modified, \
            f"Last-Modified mismatch: origin={origin_last_modified}, extended={extended_last_modified}"


class TestLegacyUrls:
    """Tests for legacy format URLs.

    Bash original:
        start_test Legacy format URLs should still work.
        URL=$REWRITTEN_ROOT/images/ce.0123456789abcdef0123456789abcdef.Puzzle,j.jpg
    """

    def test_legacy_url_format_works(
        self, client: PageSpeedClient, example_root: str
    ):
        """Legacy format cache-extended URLs should still work."""
        # Legacy format: ce.HASH.NAME,EXTENSION.EXTENSION
        url = f"{example_root}/images/ce.0123456789abcdef0123456789abcdef.Puzzle,j.jpg"
        response = client.get(url)
        assert_http_status(response, 200)


_PDF_COUNT_PATTERN = re.compile(r"\.pagespeed\.")
_PDF_EXTENDED_LINK = re.compile(r'(?:href|src)="([^"]*\.pagespeed\.[^"]*\.pdf)"')
_PDF_ELEMENT = re.compile(r"<(?:a|embed)\b[^>]*pdf[^>]*>(?:[^<]*</a>)?", re.IGNORECASE)
_PDF_EVIDENCE_LIMIT = 2000


def _pdf_count_evidence(html: str) -> str:
    """Say which PDF references of extend_cache_pdfs.html were cache-extended.

    Goes into the timeout message of the count poll, so a lane where the
    count is not reached shows which reference stayed plain: the extended
    links found, the .pagespeed. total, and the PDF-referencing <a>/<embed>
    elements of the last page (bounded).
    """
    extended = _PDF_EXTENDED_LINK.findall(html)
    total = len(_PDF_COUNT_PATTERN.findall(html))
    elements = " | ".join(m.group(0) for m in _PDF_ELEMENT.finditer(html))
    if len(elements) > _PDF_EVIDENCE_LIMIT:
        elements = elements[:_PDF_EVIDENCE_LIMIT] + "...[truncated]"
    return (f".pagespeed. total={total} expected=3; "
            f"cache-extended PDF links={extended}; PDF elements: {elements}")


class TestPdfCountEvidence:
    """The count poll's failure message (no server needed)."""

    def test_names_extended_and_plain_links(self):
        html = (
            '<a href="example.pdf.pagespeed.ce.HASH.pdf">example.pdf</a>\n'
            '<embed src="example.pdf.pagespeed.ce.HASH.pdf" width="150">\n'
            '<a href="example.notpdf">example.notpdf</a>\n'
            '<a href="example.pdf?a=b">example.pdf?a=b</a>\n'
        )
        evidence = _pdf_count_evidence(html)
        assert ".pagespeed. total=2 expected=3" in evidence
        assert evidence.count("example.pdf.pagespeed.ce.HASH.pdf") == 4
        assert '<a href="example.pdf?a=b">example.pdf?a=b</a>' in evidence
        assert '<a href="example.notpdf">example.notpdf</a>' in evidence

    def test_bounds_the_element_excerpt(self):
        html = '<a href="x.pdf">x</a>' * 500
        evidence = _pdf_count_evidence(html)
        assert evidence.endswith("...[truncated]")
        assert len(evidence) < _PDF_EVIDENCE_LIMIT + 200


# The IIS lane runs this class too, to settle the count difference tracked in
# issue #1044: if the count is not reached there, the timeout message names
# the PDF reference that stayed plain.
class TestExtendCachePdfs:
    """Tests for PDF cache extension.

    Bash original::

        test_filter extend_cache_pdfs PDF cache extension
        fetch_until -save $URL 'fgrep -c .pagespeed.' 3
        check grep -q 'a href=".*pagespeed.*\\.pdf' $FETCH_FILE
    """

    def test_extend_cache_rewrites_pdfs(
        self, client: PageSpeedClient, example_root: str
    ):
        """extend_cache_pdfs should rewrite PDF URLs."""
        url = f"{example_root}/extend_cache_pdfs.html?PageSpeedFilters=extend_cache_pdfs"

        # Wait for PDF URLs to be rewritten. Occurrences are counted, as
        # fetch_until_count did; each reference sits on its own line, so this
        # equals the bash's fgrep -c line count.
        response = client.fetch_until(
            url,
            condition=lambda r: len(_PDF_COUNT_PATTERN.findall(r.text)) == 3,
            timeout=120.0,
            detail_fn=lambda r: _pdf_count_evidence(r.text),
        )

        assert_http_status(response, 200)

        # extend_cache.sh:62-65
        assert_contains(response, r'a href=".*pagespeed.*\.pdf')
        assert_contains(response, r'embed src=".*pagespeed.*\.pdf')
        assert_contains(response, r'<a href="example\.notpdf">')
        assert_contains(response, r'<a href=".*pagespeed.*\.pdf">example\.pdf\?a=b')

    def test_cache_extended_pdf_has_correct_mime_type(
        self, client: PageSpeedClient, example_root: str
    ):
        """Cache-extended PDFs should have application/pdf content type."""
        # First get the page to find a cache-extended PDF URL
        page_url = f"{example_root}/extend_cache_pdfs.html?PageSpeedFilters=extend_cache_pdfs"
        response = client.fetch_until_contains(
            page_url,
            pattern=r'pagespeed[^"]*\.pdf',
            timeout=120.0,
        )

        # Extract a cache-extended PDF URL
        match = require_match(
            r'href="([^"]*pagespeed[^"]*\.pdf)"', response, "cache-extended PDF URL"
        )

        pdf_url = match.group(1)
        if not pdf_url.startswith("http"):
            if pdf_url.startswith("/"):
                # Already absolute path
                pass
            else:
                pdf_url = f"{example_root}/{pdf_url}"

        pdf_response = client.get(pdf_url)
        assert_http_status(pdf_response, 200)

        content_type = pdf_response.header("Content-Type")
        assert "application/pdf" in content_type.lower(), \
            f"Expected application/pdf, got {content_type}"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
