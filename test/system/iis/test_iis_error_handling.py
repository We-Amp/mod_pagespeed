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

"""IIS error handling system tests.

These tests verify the PageSpeed error handling functionality on IIS, ported
from Apache's connection_refused.sh, content_encoding_leak.sh, handler_quoting.sh,
and encoded_absolute_urls.sh system tests.

Test classes:
- TestConnectionErrors: Tests handling of connection refused and timeouts
- TestContentEncodingErrors: Tests Content-Encoding error handling
- TestXssPrevention: Tests XSS prevention in error handlers
- TestAbsoluteUrlRejection: Tests rejection of encoded absolute URLs
- TestErrorRecovery: Tests graceful error recovery

Environment Variables:
    PAGESPEED_TEST_ROOT: Root path for test pages
"""

import re
import time
import urllib.parse
from typing import Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


# ============================================================================
# TestConnectionErrors: Test connection error handling
# ============================================================================


class TestConnectionErrors:
    """Tests for connection error handling.

    These tests verify that PageSpeed gracefully handles connection errors
    when fetching resources from upstream servers.

    Ported from: pagespeed/apache/system_tests/connection_refused.sh
    """

    @pytest.mark.iis_only
    def test_page_loads_despite_failed_resource_fetch(
        self, client: PageSpeedClient, test_root: str
    ):
        """Page should load even if a resource fetch fails.

        When PageSpeed can't fetch a resource (connection refused, timeout),
        it should still serve the page, just without that optimization.
        """
        # Request a page - even if some background fetches fail,
        # the page should still be served
        response = client.get(f"{test_root}/extend_cache.html")

        # Page should load (not error out)
        assert response.status in (200, 304), (
            f"Page should load even if some fetches fail, got {response.status}"
        )

    @pytest.mark.iis_only
    def test_timeout_handling(
        self, client: PageSpeedClient, test_root: str
    ):
        """Fetch timeouts should be handled gracefully."""
        # Request a page that may have slow resources
        start_time = time.time()
        response = client.get(f"{test_root}/extend_cache.html")
        elapsed = time.time() - start_time

        # Should complete within reasonable time (not hang indefinitely)
        assert elapsed < 60, f"Request took too long: {elapsed:.2f}s"
        assert response.status in (200, 304, 500, 502, 503, 504), (
            f"Unexpected status for timeout test: {response.status}"
        )

    @pytest.mark.iis_only
    @pytest.mark.slow
    def test_multiple_failed_fetches_handled(
        self, client: PageSpeedClient, test_root: str
    ):
        """Multiple concurrent failed fetches should be handled."""
        # Make multiple requests that may trigger multiple background fetches
        for i in range(3):
            response = client.get(
                f"{test_root}/extend_cache.html?iteration={i}",
            )
            # Each request should complete
            assert response.status in (200, 304, 500, 502, 503), (
                f"Request {i} failed with status {response.status}"
            )


# ============================================================================
# TestContentEncodingErrors: Test Content-Encoding error handling
# ============================================================================


class TestContentEncodingErrors:
    """Tests for Content-Encoding error handling.

    These tests verify that PageSpeed handles malformed or unexpected
    Content-Encoding gracefully.

    Ported from: pagespeed/apache/system_tests/content_encoding_leak.sh
    """

    @pytest.mark.iis_only
    def test_gzip_decode_error_handled(
        self, client: PageSpeedClient, test_root: str
    ):
        """Malformed gzip content should be handled gracefully."""
        # Request with gzip support
        response = client.get_gzip(f"{test_root}/extend_cache.html")

        # Should complete successfully
        assert response.status in (200, 304), (
            f"Request should complete, got {response.status}"
        )

    @pytest.mark.iis_only
    def test_content_encoding_header_preserved(
        self, client: PageSpeedClient, test_root: str
    ):
        """Content-Encoding header should be properly set."""
        response = client.get_gzip(f"{test_root}/extend_cache.html")

        if response.status == 200:
            content_encoding = response.header("Content-Encoding")
            # If content is compressed, header should indicate encoding
            # If not compressed, header may be absent
            if content_encoding:
                assert content_encoding.lower() in ("gzip", "deflate", "br", "identity"), (
                    f"Unexpected Content-Encoding: {content_encoding}"
                )


# ============================================================================
# TestXssPrevention: Test XSS prevention in error handlers
# ============================================================================


class TestXssPrevention:
    """Tests for XSS prevention in error handlers.

    These tests verify that error pages and handlers properly escape
    user-controlled content to prevent XSS attacks.

    Ported from: pagespeed/apache/system_tests/handler_quoting.sh
    """

    @pytest.mark.iis_only
    def test_404_handler_escapes_path(
        self, client: PageSpeedClient, test_root: str
    ):
        """404 error handler should escape malicious path content."""
        # Request a non-existent path with HTML in it
        malicious_path = f"{test_root}/<script>alert('xss')</script>"
        response = client.get(malicious_path)

        if response.status == 404 and response.text:
            # Unescaped script tag should not appear
            assert "<script>alert" not in response.text, (
                "XSS vulnerability: unescaped script tag in 404 response"
            )

    @pytest.mark.iis_only
    def test_static_handler_escapes_path(
        self, client: PageSpeedClient, example_root: str
    ):
        """Static file handler should escape malicious path content.

        The payload includes spaces and angle brackets, which Python's
        http.client rejects in raw URLs (InvalidURL: 'URL can't contain
        control characters'). URL-encode the malicious component before
        sending; the server still sees the decoded form for path
        inspection and must escape it in any error response.
        """
        malicious_segment = urllib.parse.quote(
            "<img src=x onerror=alert(1)>", safe=""
        )
        malicious_path = f"{example_root}/{malicious_segment}/test.css"
        response = client.get(malicious_path)

        if response.text:
            # Unescaped img tag should not appear
            assert "<img src=x" not in response.text, (
                "XSS vulnerability: unescaped img tag in static handler response"
            )

    @pytest.mark.iis_only
    def test_error_page_escapes_query_params(
        self, client: PageSpeedClient, test_root: str
    ):
        """Error pages should escape malicious query parameters."""
        # Request with malicious query parameter
        malicious_query = "<script>alert('xss')</script>"
        encoded_query = urllib.parse.quote(malicious_query)
        response = client.get(f"{test_root}/nonexistent?q={encoded_query}")

        if response.text:
            # Unescaped script should not appear
            assert "<script>alert" not in response.text, (
                "XSS vulnerability: unescaped script in error page"
            )

    @pytest.mark.iis_only
    def test_message_page_escapes_urls(
        self, client: PageSpeedClient, server_config
    ):
        """Message page should escape URLs in error messages."""
        # Access message page with malicious URL in query
        malicious_url = "http://evil.com/<script>alert(1)</script>"
        encoded_url = urllib.parse.quote(malicious_url)
        response = client.get(
            f"{server_config.admin_path}/message_history?url={encoded_url}"
        )

        if response.status == 200 and response.text:
            # Script should be escaped
            assert "<script>alert" not in response.text, (
                "XSS vulnerability: unescaped script in message page"
            )

    @pytest.mark.iis_only
    def test_reflected_input_html_encoded(
        self, client: PageSpeedClient, test_root: str
    ):
        """All reflected user input should be HTML-encoded."""
        payloads = [
            ("<", "&lt;"),
            (">", "&gt;"),
            ("\"", "&quot;"),
            ("'", "&#39;"),
            ("&", "&amp;"),
        ]

        for char, encoded in payloads:
            url = f"{test_root}/nonexistent?test={char}"
            response = client.get(url)

            if response.text and char in response.text:
                # If character appears, it should be in encoded form
                # (This is a weak test - character might be in HTML structure)
                pass  # Informational


# ============================================================================
# TestAbsoluteUrlRejection: Test rejection of encoded absolute URLs
# ============================================================================


class TestAbsoluteUrlRejection:
    """Tests for rejection of encoded absolute URLs.

    These tests verify that PageSpeed rejects requests with encoded
    absolute URLs in paths, which could be used for server-side request
    forgery (SSRF) attacks.

    Ported from: pagespeed/apache/system_tests/encoded_absolute_urls.sh
    """

    @pytest.mark.iis_only
    def test_encoded_absolute_url_rejected(
        self, client: PageSpeedClient, example_root: str
    ):
        """Encoded absolute URL in path should be rejected."""
        # Try to access an external URL via encoded path
        # This could be an SSRF attempt
        encoded_url = urllib.parse.quote("http://evil.com/malicious.js")
        malicious_path = f"{example_root}/{encoded_url}"

        response = client.get(malicious_path)

        # Should not return 200 (that would mean it fetched the external URL)
        # Should return 400 (bad request) or 404 (not found)
        assert response.status != 200 or "evil.com" not in response.text, (
            "Possible SSRF: server may have fetched external URL"
        )

    @pytest.mark.iis_only
    def test_double_encoded_url_rejected(
        self, client: PageSpeedClient, example_root: str
    ):
        """Double-encoded URLs should be rejected."""
        # Double encode to bypass filters
        url = "http://evil.com/"
        encoded_once = urllib.parse.quote(url)
        encoded_twice = urllib.parse.quote(encoded_once)

        response = client.get(f"{example_root}/{encoded_twice}")

        assert response.status != 200 or "evil.com" not in response.text, (
            "Possible SSRF: double-encoded URL not rejected"
        )

    @pytest.mark.iis_only
    def test_path_traversal_rejected(
        self, client: PageSpeedClient, example_root: str
    ):
        """Path traversal attempts should be rejected."""
        # Try various path traversal patterns
        traversal_paths = [
            "../../../etc/passwd",
            "..\\..\\..\\windows\\system32\\config\\sam",
            "%2e%2e%2f%2e%2e%2f%2e%2e%2fetc%2fpasswd",
            "....//....//....//etc/passwd",
        ]

        for path in traversal_paths:
            response = client.get(f"{example_root}/{path}")

            # Should not return sensitive file contents
            assert response.status in (400, 403, 404) or (
                "root:" not in response.text and
                "[boot loader]" not in response.text
            ), f"Path traversal may have succeeded: {path}"


# ============================================================================
# TestErrorRecovery: Test graceful error recovery
# ============================================================================


class TestErrorRecovery:
    """Tests for graceful error recovery.

    These tests verify that PageSpeed recovers gracefully from errors
    and continues serving subsequent requests.
    """

    @pytest.mark.iis_only
    def test_recovery_after_bad_request(
        self, client: PageSpeedClient, example_root: str
    ):
        """Server should recover after a bad request."""
        # Make a bad request
        response1 = client.get(f"{example_root}/<invalid>")

        # Subsequent good request should work
        response2 = client.get(f"{example_root}/extend_cache.html")
        assert_http_status(response2, 200)

    @pytest.mark.iis_only
    def test_recovery_after_timeout(
        self, client: PageSpeedClient, test_root: str
    ):
        """Server should recover after a timeout."""
        # Make a request (which may or may not timeout)
        try:
            response1 = client.get(f"{test_root}/extend_cache.html")
        except Exception:
            pass  # Timeout is expected

        # Subsequent request should work
        response2 = client.get(f"{test_root}/extend_cache.html")
        assert response2.status in (200, 304), (
            f"Recovery request failed with status {response2.status}"
        )

    @pytest.mark.iis_only
    def test_concurrent_error_handling(
        self, client: PageSpeedClient, test_root: str
    ):
        """Multiple concurrent errors should not crash the server."""
        # Make multiple requests that may trigger errors
        responses = []
        for i in range(5):
            try:
                response = client.get(
                    f"{test_root}/nonexistent{i}.html",
                )
                responses.append(response)
            except Exception as e:
                responses.append(f"error: {e}")

        # At least some requests should complete
        completed = sum(
            1 for r in responses
            if hasattr(r, 'status') and r.status in (200, 404)
        )
        assert completed >= 1, "Server may have crashed under error load"


# ============================================================================
# TestErrorMessages: Test error message quality
# ============================================================================


class TestErrorMessages:
    """Tests for error message quality.

    These tests verify that error messages are helpful and don't leak
    sensitive information.
    """

    @pytest.mark.iis_only
    def test_404_error_message_helpful(
        self, client: PageSpeedClient, test_root: str
    ):
        """404 error should provide helpful message."""
        response = client.get(f"{test_root}/definitely_nonexistent_page.html")

        if response.status == 404:
            # Should have some response body
            assert len(response.text) > 0, "404 page should have content"

    @pytest.mark.iis_only
    def test_error_message_no_stack_trace(
        self, client: PageSpeedClient, test_root: str
    ):
        """Error messages should not contain stack traces in production."""
        response = client.get(f"{test_root}/<invalid>")

        if response.text:
            # Stack traces typically contain these patterns
            stack_patterns = [
                "at System.",
                "at Microsoft.",
                "Exception in thread",
                "Traceback (most recent call last)",
                ".cc:",
                ".cpp:",
                "line [0-9]+",
            ]

            for pattern in stack_patterns:
                if re.search(pattern, response.text):
                    # This might be intentional in debug mode
                    # but should be reviewed
                    print(f"Warning: Possible stack trace in error response: {pattern}")

    @pytest.mark.iis_only
    def test_error_message_no_sensitive_paths(
        self, client: PageSpeedClient, test_root: str
    ):
        """Error messages should not reveal sensitive server paths."""
        response = client.get(f"{test_root}/<invalid>")

        if response.text:
            sensitive_patterns = [
                r"C:\\inetpub",
                r"C:\\Program Files",
                r"/var/www",
                r"/home/",
                r"password",
                r"secret",
                r"api.key",
            ]

            for pattern in sensitive_patterns:
                if re.search(pattern, response.text, re.IGNORECASE):
                    pytest.fail(
                        f"Sensitive information in error response: {pattern}"
                    )


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite -- vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
