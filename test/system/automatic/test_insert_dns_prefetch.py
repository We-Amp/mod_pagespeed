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

"""Insert DNS prefetch filter tests.

Ported from: pagespeed/automatic/system_tests/insert_dns_prefetch.sh

These tests verify that the insert_dns_prefetch filter correctly adds
DNS prefetch hints for external domains.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestInsertDnsPrefetch:
    """Tests for the insert_dns_prefetch filter.

    Bash original::

        # Test DNS prefetching. DNS prefetching is dependent on user agent, but is
        # enabled for Wget UAs, allowing this test to work with our default wget params.
        test_filter insert_dns_prefetch
        fetch_until $URL 'fgrep -ci //www.gstatic.com' 2
        fetch_until $URL 'fgrep -ci //ajax.googleapis.com' 2
    """

    def test_insert_dns_prefetch_for_gstatic(
        self, client: PageSpeedClient, example_root: str
    ):
        """DNS prefetch should be added for gstatic.com domain."""
        url = f"{example_root}/insert_dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"

        response = client.fetch_until_count(
            url,
            pattern=r"//www\.gstatic\.com",
            expected_count=2,
            timeout=30.0,
            case_insensitive=True,
        )
        assert_http_status(response, 200)

        # Check for DNS prefetch link
        assert_contains(
            response,
            r"//www\.gstatic\.com",
            "DNS prefetch should be added for gstatic.com",
        )

    def test_insert_dns_prefetch_for_googleapis(
        self, client: PageSpeedClient, example_root: str
    ):
        """DNS prefetch should be added for googleapis.com domain."""
        url = f"{example_root}/insert_dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"

        response = client.fetch_until_count(
            url,
            pattern=r"//ajax\.googleapis\.com",
            expected_count=2,
            timeout=30.0,
            case_insensitive=True,
        )
        assert_http_status(response, 200)

        # Check for DNS prefetch link
        assert_contains(
            response,
            r"//ajax\.googleapis\.com",
            "DNS prefetch should be added for googleapis.com",
        )

    def test_insert_dns_prefetch_uses_link_tag(
        self, client: PageSpeedClient, example_root: str
    ):
        """Connection warm-up hints should use rel=preconnect link tags.

        The example page has exactly two external domains, both of which fit
        within the filter's preconnect budget, so both inserted hints use
        rel=preconnect (domains beyond the budget would get rel=dns-prefetch).
        """
        url = f"{example_root}/insert_dns_prefetch.html?PageSpeedFilters=insert_dns_prefetch"

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        # Should have preconnect rel attribute
        assert_contains(
            response,
            r'rel=["\']?preconnect',
            "Should use rel=preconnect",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
