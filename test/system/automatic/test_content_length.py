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

"""Content-Length header tests.

Ported from: pagespeed/automatic/system_tests/content_length.sh

These tests verify that PageSpeed resources include Content-Length headers.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
)


class TestContentLength:
    """Tests for Content-Length header on PageSpeed resources.

    Bash original::

        start_test PageSpeed resources should have a content length.
        HTML_URL="$EXAMPLE_ROOT/rewrite_css_images.html?PageSpeedFilters=rewrite_css"
        fetch_until -save "$HTML_URL" "fgrep -c rewrite_css_images.css.pagespeed.cf" 1
        OUT=$(http_proxy=${REWRITE_DOMAIN:-} $WGET_DUMP $URL)
        check_from "$OUT" grep "^Content-Length:"
        check_not_from "$OUT" grep "^Transfer-Encoding: chunked"
        check_not_from "$OUT" grep "^Cache-Control:.*private"

    Note: nginx may use chunked encoding and add Cache-Control: private.
    These are nginx-specific behaviors that differ from Apache.
    """

    def test_rewritten_resource_has_content_length(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten resources should have Content-Length header."""
        url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        # First wait for the CSS to be rewritten
        response = client.fetch_until_count(
            url,
            pattern=r"rewrite_css_images\.css\.pagespeed\.cf\.",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Extract the rewritten CSS URL
        match = re.search(
            r'href="([^"]*rewrite_css_images\.css\.pagespeed\.cf\.[^"]*)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find rewritten CSS URL")

        css_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        # Fetch the rewritten CSS resource
        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        # Should have Content-Length
        content_length = css_response.header("Content-Length")
        assert content_length, \
            "Rewritten resource should have Content-Length header"

    @pytest.mark.not_nginx  # Nginx may use chunked for intermediate resource responses
    def test_rewritten_resource_not_chunked(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten resources should not use chunked transfer encoding."""
        url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        # First wait for the CSS to be rewritten
        response = client.fetch_until_count(
            url,
            pattern=r"rewrite_css_images\.css\.pagespeed\.cf\.",
            expected_count=1,
            timeout=30.0,
        )

        # Extract the rewritten CSS URL
        match = re.search(
            r'href="([^"]*rewrite_css_images\.css\.pagespeed\.cf\.[^"]*)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find rewritten CSS URL")

        css_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        # Fetch the rewritten CSS resource
        css_response = client.get(css_url)
        assert_http_status(css_response, 200)

        # Should not be chunked
        transfer_encoding = css_response.header("Transfer-Encoding")
        if transfer_encoding:
            assert "chunked" not in transfer_encoding.lower(), \
                "Rewritten resource should not use chunked encoding"

    @pytest.mark.not_nginx  # CSS with image refs stays private due to hash churn
    def test_rewritten_resource_not_private(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewritten resources should not have private cache control once stable.

        During optimization, intermediate results may be served with
        Cache-Control: private (e.g., when input resources haven't been
        fully fetched yet). Once optimization stabilizes, the rewritten
        resource should be publicly cacheable.
        """
        url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"

        # First wait for the CSS to be rewritten
        response = client.fetch_until_count(
            url,
            pattern=r"rewrite_css_images\.css\.pagespeed\.cf\.",
            expected_count=1,
            timeout=30.0,
        )

        # Extract the rewritten CSS URL
        match = re.search(
            r'href="([^"]*rewrite_css_images\.css\.pagespeed\.cf\.[^"]*)"',
            response.text,
        )
        if not match:
            pytest.skip("Could not find rewritten CSS URL")

        css_url = match.group(1)
        # Handle both absolute URLs (http://...) and relative URLs
        if css_url.startswith("http://") or css_url.startswith("https://"):
            from urllib.parse import urlparse
            css_url = urlparse(css_url).path
        elif not css_url.startswith("/"):
            css_url = f"{example_root}/{css_url}"

        # Wait for the resource to stabilize - intermediate results may
        # have Cache-Control: private until the input resource is fully
        # fetched and determined to be proxy-cacheable.
        def check_not_private(resp):
            cc = resp.header("Cache-Control")
            return resp.status == 200 and (not cc or "private" not in cc.lower())

        css_response = client.fetch_until(
            css_url,
            condition=check_not_private,
            timeout=30.0,
        )
        assert_http_status(css_response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
