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

"""PageSpeed option headers set by the server configure the response.

Ported from: pagespeed/apache/system_tests/response_headers.sh

response_headers.html is answered by an Apache handler that sets
PageSpeed / PageSpeedFilters in headers_out and err_headers_out according
to the query; the module applies them (err_headers_out after headers_out,
a disabled filter winning over an enabled one) and strips the option
headers from every response, whether or not it ends up rewriting the
response -- the bash's "^PageSpeed:" greps ran on indented wget -S output
and could not observe this either way. The page's own <Location> also
enables add_instrumentation, which is what the override/combine cases'
"<script" expectation actually detects.

Deviation from the bash: the "off" and Disallow early-return paths used to
leave the option headers themselves (e.g. "PageSpeed: off") on the wire;
this asserts the fix for #1058, which strips them on those paths too.

TestServerSetOptionHeaders covers the same contract where the lane sets
the option headers with its own server configuration (the
option_response_headers lane fixture; nginx add_header), #1080.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok


@pytest.mark.apache_only  # mod_pagespeed_response_options_handler is an Apache handler (bash: apache/)
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestResponseHeaderOptions:
    """Bash: Request Headers affect MPS options."""

    @pytest.mark.parametrize(
        "query,pagespeed_on,comments_removed",
        [
            pytest.param("headers_out", False, False, id="headers-out-off"),
            pytest.param("headers_errout", False, False, id="err-headers-out-on"),
            pytest.param("headers_override", True, False, id="override"),
            pytest.param("headers_combine", True, True, id="combine"),
        ],
    )
    def test_option_headers_are_applied_and_stripped(
        self, client: PageSpeedClient, test_root: str, query, pagespeed_on, comments_removed
    ):
        response = client.get(f"{test_root}/response_headers.html?{query}")
        require_status_ok(response, query)
        names = [name.lower() for name, _ in response.raw_headers]
        # The module strips its option headers from every response -- the
        # "off" and Disallow paths included (#1058) -- regardless of whether
        # it goes on to rewrite the response.
        assert "pagespeed" not in names and "modpagespeed" not in names, response.raw_headers
        assert bool(response.header_values("X-Mod-Pagespeed")) == pagespeed_on, response.raw_headers
        assert ("<script" in response.text) == pagespeed_on, response.text[:400]
        assert ("<!--" not in response.text) == comments_removed, response.text[:400]


_OPTION_HEADERS = ("pagespeed", "modpagespeed", "pagespeedfilters")
_SPACED = "Option      response      headers."
_COLLAPSED = "Option response headers."


@pytest.mark.requires_fixture("option_response_headers")
class TestServerSetOptionHeaders:
    """Option headers added by the server (add_header on nginx) configure
    the response and are stripped from it, on the "off" path that leaves the
    response alone as well as on the path that rewrites it (#1080)."""

    @pytest.mark.parametrize(
        "page,rewritten",
        [
            pytest.param("off.html", False, id="pagespeed-off"),
            pytest.param("modpagespeed_off.html", False, id="modpagespeed-off"),
            pytest.param("on.html", True, id="pagespeedfilters-on"),
        ],
    )
    def test_option_headers_are_applied_and_stripped(
        self, client: PageSpeedClient, test_root: str, page, rewritten
    ):
        response = client.get(f"{test_root}/option_headers/{page}")
        require_status_ok(response, page)
        names = [name.lower() for name, _ in response.raw_headers]
        leaked = [name for name in names if name in _OPTION_HEADERS]
        assert not leaked, response.raw_headers
        if rewritten:
            # PageSpeedFilters: +collapse_whitespace took effect.
            assert _COLLAPSED in response.text, response.text[:400]
            assert _SPACED not in response.text, response.text[:400]
        else:
            # "off" left the response exactly as served.
            assert _SPACED in response.text, response.text[:400]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
