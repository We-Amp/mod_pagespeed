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

"""nginx filter-order tests.

With the module in its proper chain position (the rewriting filter after
SSI, charset, sub, addition and the headers filter, and after the copy
filter; the etag filter after compression):

- there is exactly ONE Vary line on an identity 200 of a rewritten
  .pagespeed. resource and of an in-place optimized css (the module states
  Vary itself, and its etag filter clears gzip_vary so nginx's core header
  filter does not add a second line -- that only works when the etag filter
  runs after gzip);
- an expires directive cannot touch the module's own caching headers:
  rewritten HTML keeps max-age=0, no-cache and gains no Expires, and a
  .pagespeed. resource keeps its one-year cache extension;
- add_header applies exactly once to module-served responses;
- the charset directive's decision survives on rewritten HTML;
- an <img> inside an SSI-included fragment is optimized.

Several assertions are on RAW header lines (a parsed dict joins duplicate
lines with ", " and would hide a duplicate), so this file fetches with its
own one-shot http.client connections where that matters.
"""

import http.client
import re
from typing import Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    require_match,
)


def _raw_get(client: PageSpeedClient, path: str, headers: Optional[dict] = None):
    """One-shot GET on a fresh connection, returning RAW header lines.

    Sends no Accept-Encoding at all unless the caller passes one: these
    tests exercise the identity leg, and http.client would otherwise add
    "Accept-Encoding: identity" itself. Returns (status, header_lines,
    body) with header_lines a list of (name, value) pairs in wire order --
    duplicates preserved.
    """
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
    """All raw values for one header name (case-insensitive)."""
    return [value for key, value in header_lines if key.lower() == name.lower()]


@pytest.mark.nginx_only
class TestSingleVaryLine:
    """Exactly one Vary header line on identity 200s."""

    def test_single_vary_on_pagespeed_resource(
        self, client: PageSpeedClient, test_root: str
    ):
        """A rewritten .pagespeed. css answers identity with ONE Vary line."""
        # The css must come from a location where gzip is ON: the duplicate
        # Vary this test guards against is nginx's gzip filter setting
        # gzip_vary, and the harness's generic .pagespeed. location disables
        # gzip, which would hide the duplicate. The ^~ filter_order location
        # preempts that regex location, so gzip stays on.
        page_url = f"{test_root}/filter_order/index.html?PageSpeedFilters=rewrite_css"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'href="[^"]*\.pagespeed\.cf\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )
        match = require_match(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response,
            "rewritten CSS URL",
        )
        css_url = match.group(1)
        if not css_url.startswith("/"):
            css_url = f"{test_root}/filter_order/{css_url}"

        # Steady state: the optimized resource with its ETag.
        client.fetch_until(css_url, condition=lambda r: r.header("ETag") != "")

        status, header_lines, _ = _raw_get(client, css_url)
        assert status == 200, f"expected 200, got {status}"
        vary_lines = _header_lines(header_lines, "Vary")
        assert len(vary_lines) == 1, (
            f"expected exactly ONE Vary line on the identity 200, "
            f"got {len(vary_lines)}: {vary_lines}"
        )

    def test_single_vary_on_in_place_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """An in-place optimized css answers identity with ONE Vary line."""
        css_url = f"{example_root}/styles/yellow.css"

        # Steady state: the module has optimized the resource in place and
        # states its own Vary.
        client.fetch_until(css_url, condition=lambda r: r.header("Vary") != "")

        status, header_lines, _ = _raw_get(client, css_url)
        assert status == 200, f"expected 200, got {status}"
        vary_lines = _header_lines(header_lines, "Vary")
        assert len(vary_lines) == 1, (
            f"expected exactly ONE Vary line on the identity 200, "
            f"got {len(vary_lines)}: {vary_lines}"
        )

    def test_encoding_token_once_when_vary_lists_more(
        self, client: PageSpeedClient, test_root: str
    ):
        """A Vary listing the encoding token among others states it once."""
        page_url = f"{test_root}/filter_order_vary/index.html"
        status, header_lines, _ = _raw_get(
            client, page_url, headers={"Accept-Encoding": "gzip"}
        )
        assert status == 200, f"expected 200, got {status}"
        assert _header_lines(header_lines, "Content-Encoding") == ["gzip"], (
            f"the response must be compressed for this test to mean anything: "
            f"{header_lines}"
        )
        tokens = [
            token.strip().lower()
            for line in _header_lines(header_lines, "Vary")
            for token in line.split(",")
        ]
        assert tokens.count("accept-encoding") == 1, (
            f"expected the encoding token exactly once, got Vary lines "
            f"{_header_lines(header_lines, 'Vary')}"
        )
        assert "user-agent" in tokens, (
            f"the location's own Vary must survive: {tokens}"
        )


@pytest.mark.nginx_only
class TestExpiresAndAddHeader:
    """An expires directive must not touch the module's caching headers."""

    def test_rewritten_html_keeps_module_caching_headers(
        self, client: PageSpeedClient, test_root: str
    ):
        """Rewritten HTML under `expires 1h` keeps max-age=0, no-cache."""
        # rewrite_css alone (no inline_css) so the stylesheet link becomes a
        # .pagespeed. URL: the page is then demonstrably module-processed.
        page_url = f"{test_root}/filter_order/index.html?PageSpeedFilters=rewrite_css"

        client.fetch_until(
            page_url,
            condition=lambda r: ".pagespeed.cf." in r.text,
            timeout=30.0,
        )

        status, header_lines, _ = _raw_get(client, page_url)
        assert status == 200, f"expected 200, got {status}"
        cache_control = _header_lines(header_lines, "Cache-Control")
        assert cache_control == ["max-age=0, no-cache"], (
            f"rewritten HTML must keep the module's caching headers under "
            f"expires, got Cache-Control: {cache_control}"
        )
        assert _header_lines(header_lines, "Expires") == [], (
            "rewritten HTML must not gain an Expires header"
        )
        # This holds because the HTML response INHERITS the header from its
        # fetched input (the module's own loopback fetch, which passed the
        # headers filter), not because the headers filter runs on the
        # module-generated response -- it never does.
        x_test_order = _header_lines(header_lines, "X-Test-Order")
        assert x_test_order == ["yes"], (
            f"add_header must apply exactly once, got: {x_test_order}"
        )

    def test_pagespeed_resource_keeps_one_year_extension(
        self, client: PageSpeedClient, test_root: str
    ):
        """A .pagespeed. resource under `expires 1h` keeps max-age=31536000."""
        page_url = f"{test_root}/filter_order/index.html?PageSpeedFilters=rewrite_css"
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'href="[^"]*\.pagespeed\.cf\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )
        match = require_match(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response,
            "rewritten CSS URL",
        )
        css_url = match.group(1)
        if not css_url.startswith("/"):
            css_url = f"{test_root}/filter_order/{css_url}"

        client.fetch_until(css_url, condition=lambda r: r.header("ETag") != "")

        status, header_lines, _ = _raw_get(client, css_url)
        assert status == 200, f"expected 200, got {status}"
        cache_control = _header_lines(header_lines, "Cache-Control")
        assert any(
            "max-age=31536000" in value for value in cache_control
        ), (
            f"the resource must keep its one-year cache extension under "
            f"expires, got Cache-Control: {cache_control}"
        )
        # Same inheritance as the HTML case: the header comes from the
        # fetched input's response (merged into the rewritten resource by
        # the optimization library), not from the headers filter running
        # downstream of this response.
        x_test_order = _header_lines(header_lines, "X-Test-Order")
        assert x_test_order == ["yes"], (
            f"add_header must apply exactly once, got: {x_test_order}"
        )


@pytest.mark.nginx_only
class TestCharsetSurvives:
    """The charset directive's decision survives on rewritten HTML."""

    def test_rewritten_html_keeps_charset(
        self, client: PageSpeedClient, test_root: str
    ):
        page_url = f"{test_root}/filter_order_charset/index.html?PageSpeedFilters=rewrite_css"
        response = client.fetch_until(
            page_url,
            condition=lambda r: ".pagespeed.cf." in r.text,
            timeout=30.0,
        )
        assert_http_status(response, 200)
        content_type = response.header("Content-Type")
        assert "charset=utf-8" in content_type, (
            f"rewritten HTML must keep the charset directive's decision, "
            f"got Content-Type: {content_type!r}"
        )


@pytest.mark.nginx_only
class TestLoadFromFileHeaders:
    """add_header on a .pagespeed. resource under LoadFromFile.

    Module-generated responses enter the chain at the module's own position
    and never pass nginx's headers filter, so an operator header reaches
    them only by inheritance from the input's response. A resource loaded
    from file has no fetched input: the header is ABSENT unless the
    operator supplies it with AddResourceHeader.
    """

    def _rewritten_css_url(self, client: PageSpeedClient, page_url: str) -> str:
        response = client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'href="[^"]*\.pagespeed\.cf\.[^"]*"', r.text
            )
            is not None,
            timeout=30.0,
        )
        match = require_match(
            r'href="([^"]*\.pagespeed\.cf\.[^"]*)"',
            response,
            "rewritten CSS URL",
        )
        return match.group(1)

    def test_operator_header_absent_on_loadfromfile_resource(
        self, client: PageSpeedClient, test_root: str
    ):
        page_url = f"{test_root}/filter_order_lff/index.html?PageSpeedFilters=rewrite_css"
        css_url = self._rewritten_css_url(client, page_url)
        client.fetch_until(css_url, condition=lambda r: r.header("ETag") != "")

        status, header_lines, _ = _raw_get(client, css_url)
        assert status == 200, f"expected 200, got {status}"
        x_test_order = _header_lines(header_lines, "X-Test-Order")
        assert x_test_order == [], (
            f"no fetched input, nothing to inherit: X-Test-Order must be "
            f"ABSENT on the LoadFromFile resource, got: {x_test_order}"
        )

    def test_addresourceheader_supplies_the_header_once(
        self, client: PageSpeedClient, test_root: str
    ):
        page_url = f"{test_root}/filter_order_lff_arh/index.html?PageSpeedFilters=rewrite_css"
        css_url = self._rewritten_css_url(client, page_url)
        client.fetch_until(css_url, condition=lambda r: r.header("ETag") != "")

        status, header_lines, _ = _raw_get(client, css_url)
        assert status == 200, f"expected 200, got {status}"
        x_test_order = _header_lines(header_lines, "X-Test-Order")
        assert x_test_order == ["yes"], (
            f"AddResourceHeader must supply the header exactly once, "
            f"got: {x_test_order}"
        )


@pytest.mark.nginx_only
class TestProxiedHeadThenGet:
    """A HEAD on a proxy_pass location must not poison the connection."""

    def test_head_then_get_on_one_connection(
        self, client: PageSpeedClient, test_root: str
    ):
        url = f"{test_root}/proxied/styles/yellow.css"
        conn = http.client.HTTPConnection(client.host, client.port, timeout=30.0)
        try:
            conn.request("HEAD", url)
            head = conn.getresponse()
            head_body = head.read()
            assert head.status == 200, f"HEAD: expected 200, got {head.status}"
            assert head_body == b"", "a HEAD answer must have no body"

            # The GET on the SAME connection succeeds only if the HEAD's
            # final chunk was forwarded through the module's body filter
            # rather than dropped there.
            conn.request("GET", url)
            get = conn.getresponse()
            get_body = get.read()
            assert get.status == 200, (
                f"GET after HEAD on one connection: expected 200, "
                f"got {get.status}"
            )
            assert len(get_body) > 0
        finally:
            conn.close()


@pytest.mark.nginx_only
class TestInPlaceIfNoneMatch:
    """If-None-Match on an in-place optimized resource answers 304."""

    def test_in_place_css_304(self, client: PageSpeedClient, example_root: str):
        css_url = f"{example_root}/styles/yellow.css"
        response = client.fetch_until(
            css_url, condition=lambda r: r.header("ETag") != "", timeout=30.0
        )
        etag = response.header("ETag")
        response_304 = client.get(css_url, headers={"If-None-Match": etag})
        assert_http_status(response_304, 304)
        assert len(response_304.body) == 0


@pytest.mark.nginx_only
class TestSsiFragmentOptimized:
    """An <img> inside an SSI-included fragment is optimized."""

    def test_img_in_included_fragment_is_rewritten(
        self, client: PageSpeedClient, test_root: str
    ):
        page_url = f"{test_root}/filter_order_ssi/page.shtml"

        # SSI itself works first of all: the fragment's marker text is in
        # the assembled page.
        response = client.get(page_url)
        assert_http_status(response, 200)
        assert_contains(response, "ssi-fragment-included")

        # And the module sees the ASSEMBLED page: the fragment's <img> is
        # rewritten to a .pagespeed. URL. (With the module ahead of SSI it
        # only ever sees the include directive, so this never happens.)
        client.fetch_until(
            page_url,
            condition=lambda r: re.search(
                r'Puzzle\.jpg\.pagespeed\.', r.text
            )
            is not None,
            timeout=30.0,
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
