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
    unknown content type heuristically. The origin also sets a cookie on
    every response: the proxied copy must not carry it to browsers under
    the proxying site's own domain.
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

    def test_proxied_non_pagespeed_content_strips_origin_cookies(
        self, client: PageSpeedClient
    ):
        """The proxied passthrough must drop the origin's Set-Cookie.

        This URL is not a .pagespeed. encoding, so ProxyFetch itself does
        not sanitize it (no cross-domain decode); the strip has to happen
        where the fetched headers are handed to Apache.
        """
        response = client.get(FONT_URL)
        require_status_ok(response, FONT_URL)
        assert FONT_TEXT in response.text, response.text[:200]
        assert response.header_values("Set-Cookie") == [], (
            f"proxied passthrough leaks origin cookies: "
            f"{response.header_values('Set-Cookie')}"
        )
        # The origin really does set the cookie, or the strip above proves
        # nothing.
        origin = client.get(
            "/do_not_modify/not_really_a_font.woff",
            headers={"Host": "selfsigned.modpagespeed.com"},
        )
        require_status_ok(origin, "origin not_really_a_font.woff")
        assert any(
            v.startswith("test-cookie") for v in origin.header_values("Set-Cookie")
        ), f"origin cookie missing: {origin.header_values('Set-Cookie')}"


PROXY_AUTH_URL = "/mod_pagespeed_example/combine_css.css"


@pytest.mark.apache_only
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestProxyAuth:
    """ModPagespeedProxyAuth in TestProxy mode (module issue #325).

    The secondary-port vhosts proxyauth{,-redirect}.example.com run with
    ModPagespeedTestProxy on and a ProxyAuth cookie. The auth check runs
    before any fetch, so the rejection cases need no reachable origin:
    no cookie must get a real 403 (not 200 with an empty body), and the
    redirect form a 307 with its Location.
    """

    def test_missing_cookie_is_rejected_with_403(self, vhost_client):
        response = vhost_client("proxyauth.example.com").get(PROXY_AUTH_URL)
        assert response.status == 403, (
            f"cookieless proxy request: HTTP {response.status}: "
            f"{response.text[:200]}"
        )

    def test_unrelated_cookie_is_rejected_with_403(self, vhost_client):
        response = vhost_client("proxyauth.example.com").get(
            PROXY_AUTH_URL, headers={"Cookie": "some-other-cookie=1"}
        )
        assert response.status == 403, (
            f"proxy request with an unrelated cookie: HTTP {response.status}: "
            f"{response.text[:200]}"
        )

    def test_redirect_form_rejects_with_307(self, vhost_client):
        response = vhost_client("proxyauth-redirect.example.com").get(
            PROXY_AUTH_URL
        )
        assert response.status == 307, (
            f"cookieless proxy request on the redirect form: "
            f"HTTP {response.status}: {response.text[:200]}"
        )
        assert (
            response.header("Location")
            == "http://proxyauth-redirect.example.com/login"
        ), f"Location: {response.header('Location')!r}"


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
