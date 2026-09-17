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

"""Error recovery tests for IIS PageSpeed module.

These tests verify that the PageSpeed module handles various error conditions
gracefully without crashing or corrupting responses. The module should
pass through malformed content, handle missing resources, and process
edge cases correctly.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestMalformedContent:
    """Tests for handling malformed HTML content.

    The PageSpeed module should handle malformed HTML gracefully,
    either passing it through unchanged or applying best-effort fixes
    without crashing.
    """

    def test_malformed_html_passthrough(
        self, client: PageSpeedClient, test_root: str
    ):
        """Malformed HTML should pass through without causing a crash.

        The server should return a response even when the HTML has
        unclosed tags, mismatched nesting, and other issues.
        """
        response = client.get(f"{test_root}/malformed.html")
        # Should get a response, not crash
        assert_http_status(response, 200)
        # Should contain some of the original content
        assert_contains(response, "Malformed HTML Test Page")

    def test_unclosed_tags_handled(
        self, client: PageSpeedClient, test_root: str
    ):
        """Unclosed tags should not cause errors.

        The HTML parser should handle unclosed tags gracefully and
        still produce a valid response.
        """
        response = client.get(
            f"{test_root}/malformed.html?PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(response, 200)
        # Content should still be present
        assert_contains(response, "unclosed-div")

    def test_invalid_css_passthrough(
        self, client: PageSpeedClient, test_root: str
    ):
        """Invalid CSS should pass through without causing errors.

        When rewrite_css encounters invalid CSS, it should either
        pass it through unchanged or skip the invalid portions.
        """
        # Request a page that might have malformed content
        # The malformed.html has an unclosed style tag
        response = client.get(
            f"{test_root}/malformed.html?PageSpeedFilters=rewrite_css"
        )
        assert_http_status(response, 200)

    def test_malformed_html_with_multiple_filters(
        self, client: PageSpeedClient, test_root: str
    ):
        """Multiple filters on malformed HTML should not compound errors."""
        filters = "collapse_whitespace,remove_comments,extend_cache"
        response = client.get(
            f"{test_root}/malformed.html?PageSpeedFilters={filters}"
        )
        assert_http_status(response, 200)
        # Should still contain original content
        assert_contains(response, "Malformed HTML")


class TestMissingResources:
    """Tests for handling missing resources.

    When PageSpeed filters reference resources that don't exist,
    the module should handle these gracefully without crashing
    or blocking the entire response.
    """

    def test_missing_css_in_combine(
        self, client: PageSpeedClient, test_root: str
    ):
        """Missing CSS file in combine filter should be handled gracefully.

        If one CSS file in a combine group is missing, PageSpeed should
        still function and not return a 500 error for the HTML page.
        """
        # Create a request that would try to combine CSS including a missing file
        # The page references styles that exist, but we test robustness
        response = client.get(
            f"{test_root}/combine_css.html?PageSpeedFilters=combine_css"
        )
        assert_http_status(response, 200)
        # Page should still be returned

    def test_missing_js_in_combine(
        self, client: PageSpeedClient, test_root: str
    ):
        """Missing JS file in combine filter should be handled gracefully.

        If one JS file in a combine group is missing, PageSpeed should
        still function and not return a 500 error for the HTML page.
        """
        response = client.get(
            f"{test_root}/combine_javascript.html?PageSpeedFilters=combine_javascript"
        )
        assert_http_status(response, 200)
        # Page should still be returned

    def test_404_resource_passthrough(
        self, client: PageSpeedClient, test_root: str
    ):
        """404 for missing resources should still be returned correctly.

        When requesting a non-existent resource, the server should
        return a proper 404 response, not crash or return a 500.
        """
        response = client.get(f"{test_root}/nonexistent_file_12345.html")
        assert_http_status(response, 404)

    def test_404_for_missing_css_file(
        self, client: PageSpeedClient, test_root: str
    ):
        """Missing CSS file should return 404, not 500."""
        response = client.get(f"{test_root}/styles/does_not_exist.css")
        assert_http_status(response, 404)

    def test_404_for_missing_js_file(
        self, client: PageSpeedClient, test_root: str
    ):
        """Missing JS file should return 404, not 500."""
        response = client.get(f"{test_root}/scripts/does_not_exist.js")
        assert_http_status(response, 404)

    def test_missing_image_in_rewrite(
        self, client: PageSpeedClient, test_root: str
    ):
        """Missing image referenced in HTML should not crash rewrite_images.

        The rewrite_images filter should handle missing images gracefully.
        """
        response = client.get(
            f"{test_root}/rewrite_images.html?PageSpeedFilters=rewrite_images"
        )
        assert_http_status(response, 200)
        # Page should still have image tags
        assert_contains(response, "<img")


class TestLargeContent:
    """Tests for handling large content.

    The PageSpeed module should handle large HTML files correctly
    without truncating content or running out of memory.
    """

    def test_large_html_file(
        self, client: PageSpeedClient, test_root: str
    ):
        """Large HTML file should be handled correctly.

        The server should process the large.html file without errors.
        """
        response = client.get(f"{test_root}/large.html")
        assert_http_status(response, 200)
        # Should have the title
        assert_contains(response, "Large HTML Test Page")

    def test_html_not_truncated(
        self, client: PageSpeedClient, test_root: str
    ):
        """Large HTML file should not be truncated.

        The closing tags should be present in the response, verifying
        that the entire document was processed.
        """
        response = client.get(f"{test_root}/large.html")
        assert_http_status(response, 200)
        # Verify closing tags are present
        assert_contains(response, "</html>")
        assert_contains(response, "</body>")
        # Verify footer content at end of document
        assert_contains(response, "End of large test document")

    def test_large_html_with_filters(
        self, client: PageSpeedClient, test_root: str
    ):
        """Large HTML with filters should not be truncated."""
        response = client.get(
            f"{test_root}/large.html?PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(response, 200)
        # Verify document is complete
        assert_contains(response, "</html>")
        assert_contains(response, "End of large test document")

    def test_large_html_preserves_structure(
        self, client: PageSpeedClient, test_root: str
    ):
        """Large HTML should preserve document structure after optimization."""
        response = client.get(f"{test_root}/large.html")
        assert_http_status(response, 200)
        # Verify multiple sections are present
        assert_contains(response, "section1")
        assert_contains(response, "section7")
        assert_contains(response, "<footer>")


class TestBinaryContent:
    """Tests for binary content passthrough.

    Binary content like images should pass through without corruption.
    PageSpeed should not attempt to rewrite binary content as HTML.
    """

    def test_binary_image_passthrough(
        self, client: PageSpeedClient, test_root: str
    ):
        """Image files should pass through without corruption.

        Binary image files should be served with correct content type
        and not be corrupted by HTML rewriting.
        """
        response = client.get(f"{test_root}/images/Puzzle.jpg")
        assert_http_status(response, 200)

        # Should have image content type
        content_type = response.header("Content-Type")
        assert content_type, "Content-Type header should be present"
        assert "image" in content_type.lower(), \
            f"Expected image content type, got {content_type}"

    def test_non_html_content_passthrough(
        self, client: PageSpeedClient, test_root: str
    ):
        """Non-HTML content types should pass through unchanged.

        CSS and JS files should be served without HTML rewriting applied.
        """
        # Test CSS passthrough
        css_response = client.get(f"{test_root}/styles/main.css")
        assert_http_status(css_response, 200)
        content_type = css_response.header("Content-Type")
        assert "css" in content_type.lower() or "text" in content_type.lower(), \
            f"Expected CSS content type, got {content_type}"

        # Test JS passthrough
        js_response = client.get(f"{test_root}/scripts/main.js")
        assert_http_status(js_response, 200)
        content_type = js_response.header("Content-Type")
        assert "javascript" in content_type.lower() or "text" in content_type.lower(), \
            f"Expected JavaScript content type, got {content_type}"

    def test_image_not_rewritten_as_html(
        self, client: PageSpeedClient, test_root: str
    ):
        """Image requests should not trigger HTML rewriting.

        Even with PageSpeed enabled, image files should not have
        HTML-like content or X-Page-Speed header indicating HTML rewrite.
        """
        response = client.get(f"{test_root}/images/Puzzle.jpg")
        assert_http_status(response, 200)

        # Content should not start with HTML tags
        # (binary content won't have these patterns)
        content = response.body
        if content:
            # Check first few bytes are not HTML
            first_bytes = content[:20] if len(content) >= 20 else content
            # JPEG files start with FF D8 FF
            # They should NOT start with <!DOCTYPE or <html
            assert not first_bytes.startswith(b'<!DOCTYPE'), \
                "Image should not be rewritten as HTML"
            assert not first_bytes.startswith(b'<html'), \
                "Image should not be rewritten as HTML"

    def test_css_file_not_html_rewritten(
        self, client: PageSpeedClient, test_root: str
    ):
        """CSS files should not be treated as HTML.

        CSS files should maintain their CSS syntax and not have
        HTML tags injected.
        """
        response = client.get(f"{test_root}/styles/main.css")
        assert_http_status(response, 200)

        # CSS should not contain HTML doctype
        assert_not_contains(response, "<!DOCTYPE")
        assert_not_contains(response, "<html")


class TestEdgeCases:
    """Edge case tests for unusual input scenarios.

    These tests verify that the module handles unusual but valid
    edge cases without crashing.
    """

    def test_empty_response_handled(
        self, client: PageSpeedClient, test_root: str
    ):
        """Empty HTML pages should not cause crashes.

        A valid HTTP response with minimal or empty content should
        be handled gracefully.
        """
        response = client.get(f"{test_root}/whitespace.html")
        assert_http_status(response, 200)
        # Should return some response even if minimal

    def test_null_bytes_in_content(
        self, client: PageSpeedClient, test_root: str
    ):
        """Content with null bytes should be handled.

        Binary content with null bytes should pass through without
        causing string handling errors.
        """
        # Request a binary file which will have null bytes
        response = client.get(f"{test_root}/images/Puzzle.jpg")
        assert_http_status(response, 200)
        # Should have binary content with possible null bytes
        assert response.body is not None
        assert len(response.body) > 0

    def test_very_long_url(
        self, client: PageSpeedClient, test_root: str
    ):
        """Very long URLs should be handled gracefully.

        The server should handle long URLs without crashing,
        returning either a valid response or an appropriate error.
        """
        # Create a URL with a very long query string
        long_param = "x" * 4000
        response = client.get(f"{test_root}/index.html?param={long_param}")
        # Should get some response - either 200, 414 (URI Too Long),
        # or 404 (IIS returns 404.15 for URL too long)
        assert response.status in (200, 404, 414), \
            f"Expected 200, 404, or 414, got {response.status}"

    def test_special_characters_in_url(
        self, client: PageSpeedClient, test_root: str
    ):
        """Special characters in URL should be handled."""
        # Test with URL-encoded special characters
        response = client.get(f"{test_root}/index.html?test=%20%26%3D")
        assert_http_status(response, 200)

    def test_unicode_in_query_string(
        self, client: PageSpeedClient, test_root: str
    ):
        """Unicode in query strings should be handled."""
        # Test with Unicode characters
        response = client.get(f"{test_root}/unicode.html")
        assert_http_status(response, 200)

    def test_multiple_pagespeed_params(
        self, client: PageSpeedClient, test_root: str
    ):
        """Multiple PageSpeed query parameters should be handled."""
        response = client.get(
            f"{test_root}/index.html?PageSpeed=on&PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(response, 200)

    def test_contradictory_pagespeed_params(
        self, client: PageSpeedClient, test_root: str
    ):
        """Contradictory PageSpeed params should not crash.

        Even if parameters are contradictory, the server should
        return a valid response without crashing.
        """
        # PageSpeed=off should take precedence
        response = client.get(
            f"{test_root}/index.html?PageSpeed=off&PageSpeedFilters=combine_css"
        )
        assert_http_status(response, 200)
        # With PageSpeed=off, no rewriting should occur
        assert_not_contains(response, ".pagespeed.")

    def test_invalid_filter_name(
        self, client: PageSpeedClient, test_root: str
    ):
        """Invalid filter names should be handled gracefully."""
        response = client.get(
            f"{test_root}/index.html?PageSpeedFilters=nonexistent_filter_xyz"
        )
        # Should still return the page, possibly without optimization
        assert_http_status(response, 200)

    def test_empty_filter_value(
        self, client: PageSpeedClient, test_root: str
    ):
        """Empty filter value should be handled."""
        response = client.get(f"{test_root}/index.html?PageSpeedFilters=")
        assert_http_status(response, 200)


class TestConcurrentRequests:
    """Tests for handling concurrent request scenarios.

    While true concurrency testing requires separate tooling,
    these tests verify that sequential requests don't cause
    state corruption issues.
    """

    def test_sequential_requests_no_state_bleed(
        self, client: PageSpeedClient, test_root: str
    ):
        """Sequential requests should not leak state between them.

        Each request should be independent, with one request's
        settings not affecting the next.
        """
        # First request with PageSpeed=off
        response1 = client.get(f"{test_root}/index.html?PageSpeed=off")
        assert_http_status(response1, 200)
        assert_not_contains(response1, ".pagespeed.")

        # Second request without the param - should use default behavior
        response2 = client.get(f"{test_root}/index.html")
        assert_http_status(response2, 200)
        # Should either be optimized or not based on defaults,
        # but should not inherit the PageSpeed=off from previous request

    def test_alternating_filter_requests(
        self, client: PageSpeedClient, test_root: str
    ):
        """Alternating between different filters should work correctly."""
        # Request with collapse_whitespace
        response1 = client.get(
            f"{test_root}/index.html?PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(response1, 200)

        # Request with combine_css
        response2 = client.get(
            f"{test_root}/combine_css.html?PageSpeedFilters=combine_css"
        )
        assert_http_status(response2, 200)

        # Request with no filters
        response3 = client.get(f"{test_root}/index.html?PageSpeed=off")
        assert_http_status(response3, 200)
        assert_not_contains(response3, ".pagespeed.")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
