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

"""IIS HTTPS system tests.

Verify that the PageSpeed module serves and rewrites correctly when IIS
terminates TLS. The Full IIS harness (setup_iis_full.ps1) adds an HTTPS
binding on port 8443 with a self-signed localhost certificate (trusted in
LocalMachine\\Root so the module's WinHTTP loopback fetcher validates it), and
run_iis_tests.ps1 exports PAGESPEED_HTTPS_HOST / PAGESPEED_HTTPS_PORT only
under -UseFullIIS. These tests therefore self-skip when HTTPS is not
configured (e.g. the IIS Express path), and the iis_only marker skips them on
non-IIS servers.

Coverage: the HTTPS *listener* (HTML served over TLS carries the PageSpeed
header, static assets are served, the admin endpoint is reachable) and
*resource rewriting* over TLS -- the module fetches sub-resources over the
loopback HTTPS connection, enabled by the WINHTTP_FLAG_SECURE fix.
"""

import os

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


# Skip the whole module unless HTTPS is configured (set by run_iis_tests.ps1
# only under -UseFullIIS). Mirrors automatic/test_https.py's module-level gate.
pytestmark = pytest.mark.skipif(
    not os.environ.get("PAGESPEED_HTTPS_HOST"),
    reason="HTTPS tests require PAGESPEED_HTTPS_HOST (Full IIS only)",
)


class TestIisHttps:
    """HTTPS coverage specific to the IIS platform."""

    @pytest.mark.iis_only
    def test_https_html_served_with_pagespeed_header(
        self, https_client: PageSpeedClient, example_root: str
    ):
        """An HTML page served over HTTPS carries the PageSpeed version header."""
        response = https_client.get(f"{example_root}/combine_css.html")
        assert_http_status(response, 200)

        mod_pagespeed = response.header("X-Mod-Pagespeed")
        page_speed = response.header("X-Page-Speed")
        assert mod_pagespeed or page_speed, \
            "Expected X-Mod-Pagespeed or X-Page-Speed header over HTTPS"

    @pytest.mark.iis_only
    def test_https_static_css_served(
        self, https_client: PageSpeedClient, example_root: str
    ):
        """A static stylesheet is served over HTTPS (binding + cert are healthy)."""
        response = https_client.get(f"{example_root}/styles/blue.css")
        assert_http_status(response, 200)

    @pytest.mark.iis_only
    def test_https_admin_endpoint_reachable(
        self, https_client: PageSpeedClient
    ):
        """The PageSpeed admin endpoint is reachable over HTTPS from localhost."""
        admin_path = os.environ.get("PAGESPEED_ADMIN_PATH", "/pagespeed_admin")
        response = https_client.get(f"{admin_path}/")
        assert_http_status(response, 200)

    @pytest.mark.iis_only
    def test_https_resource_rewriting(
        self, https_client: PageSpeedClient, example_root: str
    ):
        """CSS combination produces a rewritten resource URL over HTTPS.

        Exercises the module's WinHTTP loopback sub-resource fetch over TLS
        (LoopbackRouteFetcher -> 127.0.0.1:8443, validated against the
        LocalMachine\\Root-trusted cert). Regression guard for the
        WINHTTP_FLAG_SECURE fix: before it, the fetch went out as
        cleartext and the resource was never combined.
        """
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"
        response = https_client.fetch_until_contains(
            url,
            pattern=r"\.pagespeed\.cc\.",
            timeout=60.0,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)
        assert_contains(
            response,
            r"\.pagespeed\.cc\.",
            "Combined CSS resource URL should be present over HTTPS",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
