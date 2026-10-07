#!/usr/bin/env python3
# Copyright (c) 2026 We-Amp B.V.
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

"""Proxying through MapProxyDomain.

Ported from: pagespeed/apache/system_tests/proxying.sh
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_stat_delta,
    require_status_ok,
)
from pagespeed_test_framework.stats import settled_stats

FONT_URL = "/modpagespeed_http/not_really_a_font.woff"
FONT_TEXT = "This is not really font data"


@pytest.mark.requires_stats
@pytest.mark.requires_fixture("external_origin")
class TestProxyNonPagespeedContent:
    """Bash: Issue 609 -- proxying non-.pagespeed content, and caching it
    locally (proxying.sh:15-31).

    The stand-in origin serves the font with an explicit Cache-Control
    lifetime, as the real origin did, because the module does not cache an
    unknown content type heuristically; the origin's test cookie stays.
    """

    def test_proxied_non_pagespeed_content_is_cached_locally(
        self, client: PageSpeedClient, stats_snapshot
    ):
        first = client.get(FONT_URL)
        require_status_ok(first, FONT_URL)
        assert FONT_TEXT in first.text, first.text[:200]
        before = settled_stats(stats_snapshot, ["cache_hits", "cache_misses"])
        second = client.get(FONT_URL)
        require_status_ok(second, FONT_URL)
        assert FONT_TEXT in second.text, second.text[:200]
        after = stats_snapshot()
        assert_stat_delta(before, after, "cache_hits", 1)
        assert_stat_delta(before, after, "cache_misses", 0)


@pytest.mark.apache_only  # the refusal is the Apache fetcher's (bash: apache/)
@pytest.mark.requires_fixture("flush_origin")
class TestProxyContentTypeRequired:
    """Bash: Do not proxy content without a Content-Type header / But do
    proxy content if the Content-Type header is present (proxying.sh:33-54).
    Both origins are routes of the local flushing origin, where the bash
    used a remote test host on two ports."""

    def test_content_without_content_type_is_not_proxied(self, client: PageSpeedClient):
        response = client.get("/content_type_absent/")
        assert response.status == 403, f"HTTP {response.status}: {response.text[:200]}"
        assert "Missing Content-Type required for proxied resource" in response.text, (
            response.text[:300]
        )
        assert "This file should not be proxied" not in response.text

    def test_content_with_content_type_is_proxied(self, client: PageSpeedClient):
        response = client.get("/content_type_present/")
        require_status_ok(response, "/content_type_present/")
        assert "This file should be proxied" in response.text, response.text[:200]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
