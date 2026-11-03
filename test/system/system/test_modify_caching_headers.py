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

"""Caching-header handling for rewritten HTML.

ModifyCachingHeaders off keeps the origin's headers; caching headers that
arrive in err_headers_out are replaced like any other, and a private or
no-store on any Cache-Control line stays on the module's.

Ported from: pagespeed/system/system_tests/modify_caching_headers.sh
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok


def _lines(response):
    return [f"{name}: {value}" for name, value in response.raw_headers]


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestModifyCachingHeaders:
    """Bash: ModifyCachingHeaders (modify_caching_headers.sh:15-26)."""

    def test_modify_caching_headers_off_keeps_origin_cache_control(
        self, client: PageSpeedClient, test_root: str
    ):
        response = client.get(f"{test_root}/retain_cache_control/index.html")
        require_status_ok(response, "retain_cache_control/index.html")
        lines = _lines(response)
        assert any("Cache-Control: private, max-age=3000" in l for l in lines), lines
        assert response.header_values("Last-Modified"), f"no Last-Modified: {lines}"

    def test_downstream_caching_keeps_cache_control_and_drops_last_modified(
        self, client: PageSpeedClient, test_root: str
    ):
        response = client.get(
            f"{test_root}/retain_cache_control_with_downstream_caching/index.html"
        )
        require_status_ok(response, "retain_cache_control_with_downstream_caching")
        lines = _lines(response)
        assert not response.header_values("Last-Modified"), f"Last-Modified present: {lines}"
        assert any("Cache-Control: private, max-age=3000" in l for l in lines), lines


@pytest.mark.apache_only
class TestAlwaysSetCachingHeaders:
    """Caching headers delivered in err_headers_out.

    "Header always set" writes to err_headers_out, the same table httpd's
    script-header parser fills for php-fpm and CGI origins.  The rewritten
    page must carry a single no-cache Cache-Control and no Expires, not the
    origin's values next to the module's.
    """

    def test_rewritten_html_carries_a_single_no_cache_cache_control(
        self, client: PageSpeedClient, test_root: str
    ):
        response = client.get(f"{test_root}/always_set_cache_control/index.html")
        require_status_ok(response, "always_set_cache_control/index.html")
        lines = _lines(response)
        assert response.header_values("Cache-Control") == ["max-age=0, no-cache"], (
            f"expected a single no-cache Cache-Control: {lines}"
        )
        assert not response.header_values("Expires"), f"Expires present: {lines}"


def _cache_control_tokens(value: str):
    """The directives of one Cache-Control line, lowercased, as a set."""
    return {token.strip().lower() for token in value.split(",") if token.strip()}


@pytest.mark.apache_only
class TestCacheControlLimitsKept:
    """Rewritten HTML is never more storable than the operator said.

    The module writes ONE Cache-Control line on rewritten HTML.  A private or
    no-store on any of the page's Cache-Control lines, in headers_out or
    err_headers_out ("Header always"), stays on it; other values do not.
    """

    @pytest.mark.parametrize(
        "name, expected",
        [
            ("private", {"max-age=0", "no-cache", "private"}),
            ("no_store", {"max-age=0", "no-cache", "no-store"}),
            ("both", {"max-age=0", "no-cache", "private", "no-store"}),
            ("lines", {"max-age=0", "no-cache", "private", "no-store"}),
            ("public", {"max-age=0", "no-cache"}),
        ],
    )
    def test_one_line_keeps_private_and_no_store(
        self, client: PageSpeedClient, test_root: str, name: str, expected: set
    ):
        response = client.get(f"{test_root}/cache_control_limits/{name}.html")
        require_status_ok(response, f"cache_control_limits/{name}.html")
        cache_control = response.header_values("Cache-Control")
        assert len(cache_control) == 1, (
            f"rewritten HTML must carry ONE Cache-Control line, got: "
            f"{_lines(response)}"
        )
        assert _cache_control_tokens(cache_control[0]) == expected, (
            f"expected the directives {sorted(expected)}, got: {cache_control}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
