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

"""Envoy PageSpeed filter sanity tests.

These tests verify that the PageSpeed filter is correctly loaded in Envoy
and can handle basic request/response flows.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_contains,
)


@pytest.mark.envoy_only
class TestEnvoyFilterLoading:
    """Tests for Envoy PageSpeed filter loading and initialization."""

    def test_filter_responds_to_requests(
        self, client: PageSpeedClient, example_root: str
    ):
        """Verify the PageSpeed filter responds to requests."""
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

    def test_pagespeed_header_present(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed filter should add X-Page-Speed header."""
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

        # Envoy PageSpeed filter uses X-Page-Speed header
        ps_header = response.header("X-Page-Speed")
        assert ps_header, "X-Page-Speed header not found in response"

    def test_backend_connectivity(
        self, client: PageSpeedClient, example_root: str
    ):
        """Envoy should successfully proxy requests to backend."""
        # Fetch with PageSpeed=off to bypass optimization
        response = client.get(f"{example_root}/index.html?PageSpeed=off")
        assert_http_status(response, 200)

        # Should get HTML content
        assert_contains(response, r"<html", "Response should contain HTML")


@pytest.mark.envoy_only
class TestEnvoyBasicFunctionality:
    """Basic functionality tests for Envoy PageSpeed filter."""

    @pytest.mark.skip(reason="Python http.server returns directory listing, not index.html")
    def test_directory_maps_to_index(
        self, client: PageSpeedClient, example_root: str
    ):
        """Fetching / should return the same content as /index.html."""
        # Fetch with PageSpeed=off for consistent comparison
        root_response = client.get(f"{example_root}/?PageSpeed=off")
        index_response = client.get(f"{example_root}/index.html?PageSpeed=off")

        assert_http_status(root_response, 200)
        assert_http_status(index_response, 200)

        # Both should return the same content
        assert root_response.text == index_response.text, \
            "Directory request should return same content as index.html"

    def test_404_for_missing_page(
        self, client: PageSpeedClient, example_root: str
    ):
        """Non-existent pages should return 404."""
        response = client.get(
            f"{example_root}/this_page_does_not_exist_12345.html"
        )
        assert_http_status(response, 404)

    def test_pagespeed_off_disables_rewriting(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off query param should disable all optimization."""
        response = client.get(f"{example_root}/combine_css.html?PageSpeed=off")

        assert_http_status(response, 200)
        # Should not see any pagespeed rewriting markers
        assert_not_contains(
            response, r"\.pagespeed\.",
            "PageSpeed=off should disable rewriting"
        )

    def test_whitespace_html_returns_200(
        self, client: PageSpeedClient, test_root: str
    ):
        """Whitespace-only HTML should return 200 OK."""
        response = client.get(f"{test_root}/whitespace.html")
        assert_http_status(response, 200)


@pytest.mark.envoy_only
class TestEnvoyIPRO:
    """Tests for In-Place Resource Optimization (IPRO) via Envoy.

    IPRO allows resources to be optimized at their original URLs
    without changing the HTML that references them.
    """

    def test_ipro_image_optimization(
        self, client: PageSpeedClient, test_root: str
    ):
        """IPRO should optimize images at their original URL."""
        import random

        # Use random query param to avoid cached versions
        url = f"{test_root}/ipro/test_image_dont_reuse.png?r={random.randint(1, 100000)}"
        threshold_size = 13000

        # Fetch until the image is compressed (IPRO optimization complete)
        response = client.fetch_until(
            url,
            condition=lambda r: len(r.body) < threshold_size,
            timeout=60.0,
        )
        assert_http_status(response, 200)

        # Verify optimization occurred
        assert len(response.body) < threshold_size, \
            f"IPRO should compress image below {threshold_size}, got {len(response.body)}"

    def test_ipro_preserves_original_url(
        self, client: PageSpeedClient, test_root: str
    ):
        """IPRO should serve optimized content at the original URL."""
        import random

        url = f"{test_root}/ipro/test_image_dont_reuse.png?r={random.randint(1, 100000)}"

        # Request the image - URL should remain unchanged
        response = client.get(url)
        assert_http_status(response, 200)

        # The URL in the response should match our request
        assert response.url == url


@pytest.mark.envoy_only
class TestEnvoyStatistics:
    """Tests for Envoy PageSpeed statistics endpoints."""

    @pytest.mark.requires_stats
    def test_statistics_endpoint_responds(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics endpoint should respond with stats."""
        response = client.get(f"{server_config.stats_path}?PageSpeed=off")
        assert_http_status(response, 200)

        # Should contain some statistics: either the plain-text
        # 'name: value' format or the JSON '"name": value' format.
        assert_contains(response, r"\"?\w+\"?:\s*\d+")

    @pytest.mark.requires_stats
    def test_statistics_parsing(
        self, client: PageSpeedClient, server_config
    ):
        """Statistics should be parseable."""
        stats = client.get_statistics(stats_path=server_config.stats_path)

        # Should have some common statistics
        assert len(stats) > 0, "Should have some statistics"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
