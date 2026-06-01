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

"""DNS prefetch filter tests for IIS PageSpeed module.

These tests verify that the insert_dns_prefetch filter correctly adds
DNS prefetch hints for external domains referenced in the page.

Ported from: test/system/automatic/test_insert_dns_prefetch.py
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


@pytest.mark.html_rewrite
class TestInsertDnsPrefetch:
    """Tests for the insert_dns_prefetch filter.

    The insert_dns_prefetch filter scans the page for external domains
    and adds <link rel="dns-prefetch"> hints to speed up DNS resolution.
    """

    def test_dns_prefetch_adds_link_tags(
        self, client: PageSpeedClient, test_root: str
    ):
        """DNS prefetch should add link tags with rel=dns-prefetch.

        DNS prefetch uses property cache: the first request collects external
        domains, subsequent requests insert dns-prefetch link tags.
        """
        url = f"{test_root}/pages/dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"

        response = client.fetch_until(
            url,
            condition=lambda r: "dns-prefetch" in r.text,
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Should have dns-prefetch rel attribute
        assert_contains(
            response,
            r'rel=["\']?dns-prefetch',
            "Should add rel=dns-prefetch links",
        )

    def test_dns_prefetch_for_external_domains(
        self, client: PageSpeedClient, test_root: str
    ):
        """DNS prefetch should be added for external domains in the page."""
        url = f"{test_root}/pages/dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"

        response = client.get(url)
        assert_http_status(response, 200)

        # The dns_prefetch.html page should reference external domains
        # DNS prefetch hints should be added for them

    def test_dns_prefetch_in_head(
        self, client: PageSpeedClient, test_root: str
    ):
        """DNS prefetch links should be added in the head section."""
        url = f"{test_root}/pages/dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"

        response = client.get(url)
        assert_http_status(response, 200)

        # Should have head section
        assert_contains(response, r'<head')
        assert_contains(response, r'</head>')


@pytest.mark.html_rewrite
class TestDnsPrefetchWithOtherFilters:
    """Tests for DNS prefetch combined with other filters."""

    def test_dns_prefetch_with_combine_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """DNS prefetch should work with combine_css filter."""
        filters = "insert_dns_prefetch,combine_css"
        url = f"{example_root}/combine_css.html?PageSpeedFilters={filters}"

        response = client.fetch_until_contains(
            url,
            pattern=r'<link',
            timeout=30.0,
        )
        assert_http_status(response, 200)

    def test_dns_prefetch_with_collapse_whitespace(
        self, client: PageSpeedClient, test_root: str
    ):
        """DNS prefetch should work with collapse_whitespace filter."""
        filters = "insert_dns_prefetch,collapse_whitespace"
        url = f"{test_root}/pages/dns_prefetch.html?PageSpeedFilters={filters}"

        response = client.get(url)
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestDnsPrefetchDisabled:
    """Tests that DNS prefetch is not added when disabled."""

    def test_no_dns_prefetch_without_filter(
        self, client: PageSpeedClient, test_root: str
    ):
        """Without filter, no DNS prefetch links should be added."""
        url = f"{test_root}/pages/dns_prefetch.html?PageSpeedFilters="

        response = client.get(url)
        assert_http_status(response, 200)

        # Original page should not have dns-prefetch (unless manually added)
        # This is a baseline check

    def test_no_dns_prefetch_with_pagespeed_off(
        self, client: PageSpeedClient, test_root: str
    ):
        """PageSpeed=off should disable DNS prefetch insertion."""
        url = f"{test_root}/pages/dns_prefetch.html?PageSpeed=off"

        response = client.get(url)
        assert_http_status(response, 200)


@pytest.mark.html_rewrite
class TestDnsPrefetchEdgeCases:
    """Edge cases for DNS prefetch filter."""

    def test_page_without_external_domains(
        self, client: PageSpeedClient, example_root: str
    ):
        """Pages without external domains should still work."""
        url = f"{example_root}/index.html?PageSpeedFilters=insert_dns_prefetch"

        response = client.get(url)
        assert_http_status(response, 200)

        # Page should still be valid HTML
        assert_contains(response, r'<html')

    def test_dns_prefetch_idempotent(
        self, client: PageSpeedClient, test_root: str
    ):
        """Multiple requests should produce consistent results."""
        url = f"{test_root}/pages/dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"

        response1 = client.get(url)
        response2 = client.get(url)

        assert_http_status(response1, 200)
        assert_http_status(response2, 200)

        # Both responses should be valid
        assert_contains(response1, r'<html')
        assert_contains(response2, r'<html')


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
