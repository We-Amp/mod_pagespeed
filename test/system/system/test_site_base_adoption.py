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

"""A page that names another site in its <base> does not put that site's
resources in its own site's cache entries.

The lane serves base-a.example.test from the first listener (document root
a) and base-b.example.test from the second (document root b), each with its
own site identity. base-a can load base-b's names from its own root, and its
base-page.html names base-b in a <base> element, so the stylesheet it links
is base-b's name with base-a's file behind it. The two roots hold a style.css
with a rule only that root has (.site-a-only, .site-b-only).
"""

import os
import re

import pytest

from pagespeed_test_framework.client import PageSpeedClient

pytestmark = [pytest.mark.not_iis, pytest.mark.not_envoy]

SITE_A = "base-a.example.test"
SITE_B = "base-b.example.test"


def _client(port_name: str) -> PageSpeedClient:
    value = os.environ.get(port_name, "")
    if not value:
        pytest.skip("the lane runs no site listeners (PAGESPEED_SITE_PORT_A/B)")
    return PageSpeedClient("127.0.0.1", int(value))


def _host(name: str):
    return {"Host": name}


class TestBaseNamingAnotherSite:
    def test_the_named_site_is_not_answered_from_the_pages_entry(self):
        a, b = _client("PAGESPEED_SITE_PORT_A"), _client("PAGESPEED_SITE_PORT_B")

        # The page on site A is optimized with its stylesheet under site B's
        # name, which site A loads from its own root.
        page = a.fetch_until(
            "/base-page.html",
            condition=lambda r: r.status == 200 and ".pagespeed.cf." in r.text,
            headers=_host(SITE_A),
        )
        # The link is relative to the <base>, that is, to site B's name.
        match = re.search(
            r'href="(?:http://%s)?/?([^"/][^"]*\.pagespeed\.cf\.[^"]*)"'
            % re.escape(SITE_B),
            page.text,
        )
        assert match, page.text

        # Site B's resource, fetched from site B, is site B's own.
        plain = b.get("/style.css", headers=_host(SITE_B))
        assert plain.status == 200, plain.raw_headers
        assert "site-a-only" not in plain.text, plain.text
        assert "site-b-only" in plain.text, plain.text

        optimized = b.fetch_until(
            "/style.css",
            condition=lambda r: r.status == 200 and "{color:red}" in r.text,
            headers=_host(SITE_B),
        )
        assert "site-a-only" not in optimized.text, optimized.text
        assert ".site-b-only{color:red}" in optimized.text, optimized.text

        # The optimized name the page carries is not answered from site A's
        # entry by site B: it rebuilds from its own input or declines.
        output = b.get("/" + match.group(1), headers=_host(SITE_B))
        assert output.status in (200, 404), output.raw_headers
        assert "site-a-only" not in output.text, output.text
        if output.status == 200:
            assert ".site-b-only{color:red}" in output.text, output.text


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
