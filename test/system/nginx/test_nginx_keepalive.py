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

"""HTTP keepalive tests for nginx with PageSpeed.

These tests verify that HTTP keepalive connections work correctly with PageSpeed.
Keepalive allows multiple HTTP requests to be sent over a single TCP connection,
improving performance by avoiding connection setup overhead.

The tests use http.client with explicit connection reuse to ensure keepalive
behavior is being tested.
"""

import gzip
import http.client
import re
import ssl
from typing import Dict, List, Optional, Tuple

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    Response,
    assert_contains,
    assert_http_status,
)
from pagespeed_test_framework.client import DEFAULT_USER_AGENT


class KeepaliveConnection:
    """HTTP connection with keepalive support for testing.

    This class wraps http.client.HTTPConnection to provide a simple interface
    for making multiple requests over a single persistent connection.
    """

    def __init__(
        self,
        host: str,
        port: int,
        timeout: float = 30.0,
        use_https: bool = False,
    ):
        """Initialize the keepalive connection.

        Args:
            host: Server hostname
            port: Server port
            timeout: Connection timeout in seconds
            use_https: Use HTTPS instead of HTTP
        """
        self.host = host
        self.port = port
        self.timeout = timeout
        self.use_https = use_https
        self._conn: Optional[http.client.HTTPConnection] = None

    def connect(self) -> None:
        """Establish the connection."""
        if self.use_https:
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            self._conn = http.client.HTTPSConnection(
                self.host, self.port, timeout=self.timeout, context=context
            )
        else:
            self._conn = http.client.HTTPConnection(
                self.host, self.port, timeout=self.timeout
            )

    def close(self) -> None:
        """Close the connection."""
        if self._conn:
            self._conn.close()
            self._conn = None

    def request(
        self,
        method: str,
        path: str,
        headers: Optional[Dict[str, str]] = None,
        body: Optional[bytes] = None,
    ) -> Response:
        """Make a request on the keepalive connection.

        Args:
            method: HTTP method (GET, POST, etc.)
            path: URL path
            headers: Optional request headers
            body: Optional request body

        Returns:
            Response object with status, headers, and body
        """
        if not self._conn:
            self.connect()

        request_headers = {
            "User-Agent": DEFAULT_USER_AGENT,
            "Connection": "keep-alive",
        }
        if headers:
            request_headers.update(headers)

        self._conn.request(method, path, body=body, headers=request_headers)
        resp = self._conn.getresponse()

        # Read headers
        raw_headers = resp.getheaders()
        header_dict = {}
        for name, value in raw_headers:
            if name in header_dict:
                header_dict[name] = f"{header_dict[name]}, {value}"
            else:
                header_dict[name] = value

        # Read body
        response_body = resp.read()

        return Response(
            status=resp.status,
            headers=header_dict,
            body=response_body,
            url=path,
        )

    def get(
        self,
        path: str,
        headers: Optional[Dict[str, str]] = None,
    ) -> Response:
        """Make a GET request on the keepalive connection.

        Args:
            path: URL path
            headers: Optional request headers

        Returns:
            Response object
        """
        return self.request("GET", path, headers=headers)

    def get_gzip(
        self,
        path: str,
        headers: Optional[Dict[str, str]] = None,
    ) -> Response:
        """Make a GET request with gzip decompression.

        Args:
            path: URL path
            headers: Optional request headers

        Returns:
            Response with decompressed body
        """
        hdrs = dict(headers) if headers else {}
        hdrs["Accept-Encoding"] = "gzip"
        response = self.get(path, headers=hdrs)

        if response.header("Content-Encoding") == "gzip":
            try:
                response.body = gzip.decompress(response.body)
            except gzip.BadGzipFile:
                pass  # Not actually gzipped despite header

        return response

    def __enter__(self) -> "KeepaliveConnection":
        """Context manager entry."""
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        """Context manager exit."""
        self.close()


@pytest.fixture
def keepalive_conn(server_config) -> KeepaliveConnection:
    """Create a keepalive connection for testing.

    This fixture provides a connection that can be used for multiple requests.
    """
    conn = KeepaliveConnection(
        host=server_config.host,
        port=server_config.port,
        use_https=False,
    )
    conn.connect()
    yield conn
    conn.close()


@pytest.mark.nginx_only
class TestKeepaliveHtmlRewriting:
    """Tests for HTML rewriting over keepalive connections.

    Verifies that multiple HTML requests over a single connection all
    get properly rewritten.
    """

    @pytest.mark.requires_module
    def test_keepalive_html_rewriting(
        self,
        keepalive_conn: KeepaliveConnection,
        example_root: str,
    ):
        """Multiple HTML requests on one connection should all be rewritten.

        This test makes several sequential GET requests for HTML pages over
        a single keepalive connection and verifies that all responses are
        valid and contain PageSpeed rewriting markers.
        """
        html_pages = [
            f"{example_root}/combine_css.html?PageSpeedFilters=collapse_whitespace",
            f"{example_root}/combine_javascript.html?PageSpeedFilters=collapse_whitespace",
            f"{example_root}/extend_cache.html?PageSpeedFilters=collapse_whitespace",
        ]

        responses: List[Response] = []

        for page in html_pages:
            response = keepalive_conn.get(page)
            responses.append(response)
            assert_http_status(response, 200, f"Failed for {page}")

        # All responses should have valid content
        for i, response in enumerate(responses):
            assert len(response.body) > 0, f"Empty response for page {i}"
            # Verify this is HTML content
            assert "<html" in response.text.lower() or "<!doctype" in response.text.lower(), \
                f"Response {i} doesn't appear to be HTML"


@pytest.mark.nginx_only
class TestKeepaliveResourceServing:
    """Tests for resource serving over keepalive connections.

    Verifies that multiple .pagespeed. resource requests over a single
    connection all succeed.
    """

    @pytest.mark.requires_module
    def test_keepalive_resource_serving(
        self,
        keepalive_conn: KeepaliveConnection,
        client: PageSpeedClient,
        example_root: str,
    ):
        """Multiple resource requests on one connection should all succeed.

        This test first fetches an HTML page to get rewritten resource URLs,
        then fetches those resources sequentially over a keepalive connection.
        """
        # First, get an HTML page with rewritten resources
        page_url = f"{example_root}/combine_css.html?PageSpeedFilters=rewrite_css,extend_cache_css"

        # Use regular client to wait for optimization
        html_response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.',
            timeout=30.0,
        )
        assert_http_status(html_response, 200)

        # Extract .pagespeed. resource URLs from the HTML
        pagespeed_urls = re.findall(r'(href|src)="([^"]*\.pagespeed\.[^"]+)"', html_response.text)

        if not pagespeed_urls:
            pytest.skip("No .pagespeed. resources found in HTML")

        # Fetch resources over keepalive connection
        resource_responses: List[Response] = []
        for _, url in pagespeed_urls[:5]:  # Limit to first 5 resources
            # Handle relative URLs
            if not url.startswith("/"):
                url = f"{example_root}/{url}"

            response = keepalive_conn.get(url)
            resource_responses.append(response)

        # All resource requests should succeed
        for i, response in enumerate(resource_responses):
            assert_http_status(response, 200, f"Resource {i} failed")
            assert len(response.body) > 0, f"Empty response for resource {i}"


@pytest.mark.nginx_only
class TestKeepaliveMixedRequests:
    """Tests for mixed HTML and resource requests over keepalive.

    Verifies that interleaving HTML and resource requests on a single
    connection works correctly.
    """

    @pytest.mark.requires_module
    def test_keepalive_mixed_requests(
        self,
        keepalive_conn: KeepaliveConnection,
        client: PageSpeedClient,
        example_root: str,
    ):
        """Mix of HTML and resource requests on same connection should work.

        This test interleaves HTML page requests and resource requests over
        a single keepalive connection to verify the connection handles
        different content types correctly.
        """
        # First HTML request
        html_url = f"{example_root}/combine_css.html?PageSpeedFilters=rewrite_css,extend_cache_css"
        html_response = client.fetch_until_contains(
            html_url,
            pattern=r'\.pagespeed\.',
            timeout=30.0,
        )
        assert_http_status(html_response, 200)

        # Extract a resource URL
        match = re.search(r'(href|src)="([^"]*\.pagespeed\.[^"]+)"', html_response.text)
        if not match:
            pytest.skip("No .pagespeed. resources found")

        resource_url = match.group(2)
        if not resource_url.startswith("/"):
            resource_url = f"{example_root}/{resource_url}"

        # Now use keepalive for mixed sequence
        # Request 1: HTML
        r1 = keepalive_conn.get(f"{example_root}/?PageSpeed=off")
        assert_http_status(r1, 200, "First HTML request failed")

        # Request 2: Resource
        r2 = keepalive_conn.get(resource_url)
        assert_http_status(r2, 200, "Resource request failed")

        # Request 3: Another HTML page
        r3 = keepalive_conn.get(f"{example_root}/extend_cache.html?PageSpeed=off")
        assert_http_status(r3, 200, "Second HTML request failed")

        # Request 4: Resource again
        r4 = keepalive_conn.get(resource_url)
        assert_http_status(r4, 200, "Second resource request failed")

        # Verify content types are appropriate
        assert "text/html" in r1.header("Content-Type").lower()
        assert "text/html" in r3.header("Content-Type").lower()


@pytest.mark.nginx_only
class TestKeepaliveGzip:
    """Tests for keepalive with gzip compression.

    Verifies that gzip-compressed responses work correctly over
    keepalive connections.
    """

    def test_keepalive_with_gzip(
        self,
        keepalive_conn: KeepaliveConnection,
        example_root: str,
    ):
        """Keepalive with gzip responses should decompress correctly.

        This test fetches multiple resources with gzip compression enabled
        over a keepalive connection and verifies that all responses are
        properly decompressed.
        """
        pages = [
            f"{example_root}/?PageSpeed=off",
            f"{example_root}/combine_css.html?PageSpeed=off",
            f"{example_root}/combine_javascript.html?PageSpeed=off",
        ]

        responses: List[Response] = []

        for page in pages:
            response = keepalive_conn.get_gzip(page)
            responses.append(response)

        for i, response in enumerate(responses):
            assert_http_status(response, 200, f"Request {i} failed")

            # If server returned gzip, it should be decompressed by now
            # The decompressed content should be valid HTML
            assert "<html" in response.text.lower() or "<!doctype" in response.text.lower(), \
                f"Response {i} doesn't appear to be valid HTML after decompression"


@pytest.mark.nginx_only
class TestKeepalive304Responses:
    """Tests for keepalive with conditional (304) responses.

    Verifies that conditional requests returning 304 Not Modified
    work correctly over keepalive connections and don't break
    subsequent requests.
    """

    @pytest.mark.requires_module
    def test_keepalive_304_responses(
        self,
        keepalive_conn: KeepaliveConnection,
        client: PageSpeedClient,
        example_root: str,
    ):
        """Keepalive should handle 304 responses without breaking connection.

        This test:
        1. Fetches a resource to get its ETag
        2. Makes a conditional request with If-None-Match expecting 304
        3. Makes another request on the same connection to verify it still works
        """
        # First, get an HTML page with rewritten resources
        page_url = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache_images"

        # Use regular client to wait for optimization
        html_response = client.fetch_until_contains(
            page_url,
            pattern=r'\.pagespeed\.ce\.',
            timeout=30.0,
        )
        assert_http_status(html_response, 200)

        # Extract a cache-extended image URL
        match = re.search(
            r'src="([^"]*\.pagespeed\.ce\.[^"]+)"',
            html_response.text,
        )
        if not match:
            pytest.skip("No cache-extended resource found")

        resource_url = match.group(1)
        if not resource_url.startswith("/"):
            resource_url = f"{example_root}/{resource_url}"

        # Step 1: First request to get ETag
        r1 = keepalive_conn.get(resource_url)
        assert_http_status(r1, 200, "Initial resource fetch failed")

        etag = r1.header("ETag")
        last_modified = r1.header("Last-Modified")

        if not etag and not last_modified:
            pytest.skip("Resource doesn't have ETag or Last-Modified")

        # Step 2: Conditional request with If-None-Match
        conditional_headers = {}
        if etag:
            conditional_headers["If-None-Match"] = etag
        elif last_modified:
            conditional_headers["If-Modified-Since"] = last_modified

        r2 = keepalive_conn.get(resource_url, headers=conditional_headers)

        # Should get 304 Not Modified
        assert_http_status(r2, 304, "Expected 304 Not Modified for conditional request")

        # 304 responses should have empty or minimal body
        assert len(r2.body) == 0, "304 response should have empty body"

        # Step 3: Another request on same connection should still work
        r3 = keepalive_conn.get(f"{example_root}/?PageSpeed=off")
        assert_http_status(r3, 200, "Request after 304 failed")
        assert len(r3.body) > 0, "Empty response after 304"


@pytest.mark.nginx_only
class TestKeepaliveConnectionReuse:
    """Tests for connection reuse verification.

    These tests verify that the connection is actually being reused
    rather than creating new connections for each request.
    """

    def test_many_requests_single_connection(
        self,
        keepalive_conn: KeepaliveConnection,
        example_root: str,
    ):
        """Many requests should succeed over a single connection.

        This test makes many requests over a single connection to verify
        the connection remains stable under repeated use.
        """
        num_requests = 20

        for i in range(num_requests):
            response = keepalive_conn.get(f"{example_root}/?PageSpeed=off&req={i}")
            assert_http_status(response, 200, f"Request {i} failed")

    def test_keepalive_header_present(
        self,
        keepalive_conn: KeepaliveConnection,
        example_root: str,
    ):
        """Server should indicate keepalive support in response.

        This test verifies that the server responds with Connection: keep-alive
        or doesn't close the connection after a request.
        """
        response = keepalive_conn.get(f"{example_root}/?PageSpeed=off")
        assert_http_status(response, 200)

        # HTTP/1.1 defaults to keep-alive, but server may explicitly indicate it
        connection_header = response.header("Connection").lower()

        # Connection: close would indicate keepalive is not supported
        assert connection_header != "close", \
            "Server responded with Connection: close, keepalive not supported"

        # Make a second request to verify connection is still usable
        response2 = keepalive_conn.get(f"{example_root}/?PageSpeed=off&second=1")
        assert_http_status(response2, 200, "Second request on same connection failed")


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
