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

"""Initial sanity check tests.

Ported from: pagespeed/automatic/system_tests/initial_sanity_checks.sh

These tests verify basic server functionality before running more complex tests.
"""

import collections
import os

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    assert_header_contains,
    require_status_ok,
)


class TestDirectoryMapping:
    """Test that directory is mapped to index.html.

    Bash original:
        start_test directory is mapped to index.html.
        check $WGET -q $EXAMPLE_ROOT/?PageSpeed=off -O $OUTDIR/mod_pagespeed_example
        check $WGET -q $EXAMPLE_ROOT/index.html?PageSpeed=off -O $OUTDIR/index.html
        check diff $OUTDIR/index.html $OUTDIR/mod_pagespeed_example
    """

    def test_directory_maps_to_index(self, client: PageSpeedClient, example_root: str):
        """Fetching / should return the same content as /index.html."""
        # Fetch with PageSpeed=off to ensure consistent (non-rewritten) content
        root_response = client.get(f"{example_root}/?PageSpeed=off")
        index_response = client.get(f"{example_root}/index.html?PageSpeed=off")

        assert_http_status(root_response, 200)
        assert_http_status(index_response, 200)

        # Both should return the same content
        assert root_response.text == index_response.text, \
            "Directory request should return same content as index.html"


class TestCompression:
    """Test that gzip compression is enabled.

    Bash original:
        start_test compression is enabled for HTML.
        OUT=$($WGET -O /dev/null -q -S --header='Accept-Encoding: gzip' $EXAMPLE_ROOT/ 2>&1)
        check_from "$OUT" fgrep -qi 'Content-Encoding: gzip'
    """

    @pytest.mark.not_iis  # IIS gzip requires explicit configuration
    def test_gzip_compression_enabled(self, client: PageSpeedClient, example_root: str, server_config):
        """Server should return gzip-compressed responses when requested."""
        response = client.get(
            f"{example_root}/",
            headers={"Accept-Encoding": "gzip"},
        )

        assert_http_status(response, 200)
        # Check that Content-Encoding header indicates gzip
        content_encoding = response.header("Content-Encoding")
        assert "gzip" in content_encoding.lower(), \
            f"Expected gzip compression, got Content-Encoding: {content_encoding}"


class TestWhitespaceHandling:
    """Test that whitespace-only HTML is handled correctly.

    Bash original:
        start_test We behave sanely on whitespace served as HTML
        OUT=$($WGET_DUMP $TEST_ROOT/whitespace.html)
        check_200_http_response "$OUT"
    """

    def test_whitespace_html_returns_200(self, client: PageSpeedClient, test_root: str):
        """Whitespace-only HTML should return 200 OK."""
        response = client.get(f"{test_root}/whitespace.html")
        assert_http_status(response, 200)


class TestPageSpeedOff:
    """Test that PageSpeed=off disables optimization."""

    def test_pagespeed_off_disables_rewriting(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off query param should disable all optimization."""
        response = client.get(f"{example_root}/combine_css.html?PageSpeed=off")

        assert_http_status(response, 200)
        # Should not see any pagespeed rewriting markers
        assert ".pagespeed." not in response.text, \
            "PageSpeed=off should disable rewriting"

    def test_pagespeed_off_omits_the_version_header(
        self, client: PageSpeedClient, example_root: str
    ):
        """Bash original (initial_header_check.sh:51-57):

            OUT=$($WGET_DUMP $EXAMPLE_ROOT/combine_css.html?PageSpeed=on)
            check_from "$OUT" egrep -q 'X-Mod-Pagespeed|X-Page-Speed'
            OUT=$($WGET_DUMP $EXAMPLE_ROOT/combine_css.html?PageSpeed=off)
            check_not_from "$OUT" egrep 'X-Mod-Pagespeed|X-Page-Speed'
        """
        def version_headers(response):
            return (response.header_values("X-Mod-Pagespeed")
                    + response.header_values("X-Page-Speed"))

        on = client.get(f"{example_root}/combine_css.html?PageSpeed=on")
        require_status_ok(on, "combine_css.html?PageSpeed=on")
        assert version_headers(on), (
            f"PageSpeed=on: no X-Mod-Pagespeed/X-Page-Speed header: {on.raw_headers}"
        )
        off = client.get(f"{example_root}/combine_css.html?PageSpeed=off")
        require_status_ok(off, "combine_css.html?PageSpeed=off")
        assert not version_headers(off), (
            f"PageSpeed=off still sends the version header: {version_headers(off)}"
        )


class TestInitialHeaders:
    """The headers of a rewritten HTML page.

    Bash original (initial_header_check.sh:15-49): exactly one
    X-Mod-Pagespeed/X-Page-Speed header, no repeated header line, no ETag,
    Vary: Accept-Encoding, no Last-Modified, Cache-Control: max-age=0,
    no-cache, no X-Frame-Options.
    """

    def test_html_carries_the_expected_headers(
        self, client: PageSpeedClient, example_root: str
    ):
        # The module adds Vary: Accept-Encoding on no port; the server's
        # compressor does. Apache and nginx name it on every response of a
        # compressible type, IIS only on a response it actually compressed,
        # so on IIS the request has to ask for gzip to see it.
        on_iis = os.environ.get("PAGESPEED_SERVER_TYPE") == "iis"
        request_headers = {"Accept-Encoding": "gzip"} if on_iis else None
        response = client.get(f"{example_root}/combine_css.html", headers=request_headers)
        require_status_ok(response, "combine_css.html")
        headers = response.raw_headers
        lines = [f"{name}: {value}" for name, value in headers]
        version = [l for l in lines if l.split(":", 1)[0].lower()
                   in ("x-mod-pagespeed", "x-page-speed")]
        assert len(version) == 1, f"expected one version header, got {version}"
        repeated = [l for l, n in collections.Counter(lines).items() if n > 1]
        assert not repeated, f"repeated header lines: {repeated}"
        assert not response.header_values("ETag"), f"ETag present: {lines}"
        assert any(l.lower().startswith("vary:") and "accept-encoding" in l.lower()
                   for l in lines), f"no Vary: Accept-Encoding: {lines}"
        assert not response.header_values("Last-Modified"), f"Last-Modified present: {lines}"
        if on_iis:
            # The IIS port answers rewritten HTML with a bare "no-cache"; the
            # other ports send "max-age=0, no-cache". Both forbid reuse
            # without revalidation.
            cache_control = [l.split(":", 1)[1].strip().lower() for l in lines
                             if l.lower().startswith("cache-control:")]
            assert cache_control == ["no-cache"], f"Cache-Control on IIS: {lines}"
        else:
            assert any("cache-control: max-age=0, no-cache" in l.lower() for l in lines), (
                f"no 'Cache-Control: max-age=0, no-cache': {lines}"
            )
        assert not response.header_values("X-Frame-Options"), f"X-Frame-Options present: {lines}"


class TestBasicConnectivity:
    """Basic connectivity tests."""

    def test_server_responds(self, client: PageSpeedClient, example_root: str):
        """Verify the server is responding to requests."""
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

    def test_404_for_missing_page(self, client: PageSpeedClient, example_root: str):
        """Non-existent pages should return 404."""
        response = client.get(f"{example_root}/this_page_does_not_exist_12345.html")
        assert_http_status(response, 404)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
