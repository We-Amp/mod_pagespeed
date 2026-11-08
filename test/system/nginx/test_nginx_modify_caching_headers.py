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

"""ModifyCachingHeaders off on nginx.

The option is about HTML: a page keeps the caching headers the operator
set.  A .pagespeed. resource is the module's own response and keeps the
module's caching headers either way: the long-lived ones when the hash in
the URL matches, and the short private ones on the fallback path (a hash
that does not match).  caching_headers_off/ and caching_headers_on/ hold
the same page and stylesheet; only the option differs.
"""

import http.client
import re
from typing import Optional

import pytest

from pagespeed_test_framework import PageSpeedClient, require_match

_CACHING_HEADERS = ("Cache-Control", "Expires", "ETag", "Last-Modified", "Date")


def _raw_get(client: PageSpeedClient, path: str, headers: Optional[dict] = None):
    """One-shot GET returning (status, raw header lines, body)."""
    conn = http.client.HTTPConnection(client.host, client.port, timeout=30.0)
    try:
        conn.putrequest("GET", path, skip_accept_encoding=True)
        for name, value in (headers or {}).items():
            conn.putheader(name, value)
        conn.endheaders()
        response = conn.getresponse()
        body = response.read()
        return response.status, response.getheaders(), body
    finally:
        conn.close()


def _header_lines(header_lines, name: str):
    return [value for key, value in header_lines if key.lower() == name.lower()]


def _cache_control_tokens(header_lines, ignore_max_age_value=False):
    """The Cache-Control directives, lowercased and sorted.

    On the fallback path max-age is the input's remaining lifetime capped
    at 300 s, so two requests a moment apart may differ in its value; with
    ignore_max_age_value only the directive's presence counts.
    """
    tokens = []
    for line in _header_lines(header_lines, "Cache-Control"):
        for token in line.split(","):
            token = token.strip().lower()
            if not token:
                continue
            if ignore_max_age_value and token.startswith("max-age="):
                token = "max-age"
            tokens.append(token)
    return sorted(tokens)


def _max_age(header_lines):
    for line in _header_lines(header_lines, "Cache-Control"):
        match = re.search(r"max-age=(\d+)", line.lower())
        if match:
            return int(match.group(1))
    return None


def _present(header_lines):
    return sorted(
        name for name in _CACHING_HEADERS if _header_lines(header_lines, name)
    )


def _rewritten_css_url(client: PageSpeedClient, test_root: str, setting: str):
    """The optimized stylesheet's URL, once it is served optimized."""
    directory = f"{test_root}/caching_headers_{setting}"
    response = client.fetch_until(
        f"{directory}/index.html?PageSpeedFilters=rewrite_css",
        condition=lambda r: re.search(r'href="[^"]*\.pagespeed\.cf\.[^"]*"', r.text)
        is not None,
        timeout=30.0,
    )
    css_url = require_match(
        r'href="([^"]*\.pagespeed\.cf\.[^"]*)"', response, "rewritten CSS URL"
    ).group(1)
    if not css_url.startswith("/"):
        css_url = f"{directory}/{css_url}"
    # Steady state: served from the module's cache, optimized.
    client.fetch_until(css_url, condition=lambda r: r.status == 200)
    return css_url


def _mismatched(css_url: str) -> str:
    """The same resource under a hash that never matches."""
    return re.sub(r"\.pagespeed\.cf\.[^.]+\.", ".pagespeed.cf.0000000000.", css_url)


@pytest.mark.nginx_only
class TestModifyCachingHeadersOff:
    """A .pagespeed. resource keeps the module's caching headers."""

    def test_hash_match_keeps_long_lived_caching_headers(
        self, client: PageSpeedClient, test_root: str
    ):
        on_status, on_lines, _ = _raw_get(
            client, _rewritten_css_url(client, test_root, "on")
        )
        off_status, off_lines, _ = _raw_get(
            client, _rewritten_css_url(client, test_root, "off")
        )
        assert on_status == 200 and off_status == 200, (on_status, off_status)
        assert any(
            "max-age=31536000" in value
            for value in _header_lines(off_lines, "Cache-Control")
        ), f"no long-lived Cache-Control with the option off: {off_lines}"
        assert _header_lines(off_lines, "Expires"), (
            f"no Expires with the option off: {off_lines}"
        )
        assert _header_lines(off_lines, "ETag"), (
            f"no ETag with the option off: {off_lines}"
        )
        assert _cache_control_tokens(off_lines) == _cache_control_tokens(on_lines), (
            f"off: {off_lines}\non: {on_lines}"
        )
        assert _present(off_lines) == _present(on_lines), (
            f"off: {off_lines}\non: {on_lines}"
        )

    def test_hash_mismatch_matches_the_option_on(
        self, client: PageSpeedClient, test_root: str
    ):
        on_status, on_lines, _ = _raw_get(
            client, _mismatched(_rewritten_css_url(client, test_root, "on"))
        )
        off_status, off_lines, _ = _raw_get(
            client, _mismatched(_rewritten_css_url(client, test_root, "off"))
        )
        assert on_status == 200 and off_status == 200, (on_status, off_status)
        assert _header_lines(off_lines, "Cache-Control"), (
            f"no Cache-Control on the fallback with the option off: {off_lines}"
        )
        assert _cache_control_tokens(
            off_lines, ignore_max_age_value=True
        ) == _cache_control_tokens(on_lines, ignore_max_age_value=True), (
            f"off: {off_lines}\non: {on_lines}"
        )
        max_age = _max_age(off_lines)
        assert max_age is not None and 0 <= max_age <= 300, (
            f"the fallback's max-age must be short: {off_lines}"
        )
        assert _present(off_lines) == _present(on_lines), (
            f"off: {off_lines}\non: {on_lines}"
        )

    def test_page_keeps_the_operators_caching_headers(
        self, client: PageSpeedClient, test_root: str
    ):
        page_url = f"{test_root}/caching_headers_off/index.html?PageSpeedFilters=rewrite_css"
        client.fetch_until(
            page_url, condition=lambda r: ".pagespeed.cf." in r.text, timeout=30.0
        )
        status, header_lines, _ = _raw_get(client, page_url)
        assert status == 200, f"expected 200, got {status}"
        assert _header_lines(header_lines, "Cache-Control") == [
            "private, max-age=3000"
        ], header_lines
        assert _header_lines(header_lines, "Last-Modified"), (
            f"no Last-Modified: {header_lines}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
