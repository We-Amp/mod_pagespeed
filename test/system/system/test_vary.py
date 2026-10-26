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

"""Vary handling for rewritten and inlined resources.

Ported from: pagespeed/system/system_tests/no_respect_vary.sh and
pagespeed/system/system_tests/respect_vary.sh
"""

import re
import time

import pytest

from pagespeed_test_framework import PageSpeedClient, require_match, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestNoRespectVary:
    """Bash: Vary:User-Agent on resources is held by our cache."""

    def test_rewritten_css_varies_on_accept_encoding_only(
        self, client: PageSpeedClient, test_root: str
    ):
        page = client.fetch_until(
            f"{test_root}/vary/no_respect/index.html",
            condition=lambda r: count_matching_lines(r.text, re.escape(".pagespeed.cf.")) == 1,
            timeout=100.0,
        )
        href = require_match(r'stylesheet"[^>]*href="([^"]+)"', page, "rewritten CSS link").group(1)
        css_url = f"{test_root}/vary/no_respect/{href.rsplit('/', 1)[-1]}"
        css = client.get(css_url)
        require_status_ok(css, css_url)
        lines = [f"{name}: {value}" for name, value in css.raw_headers]
        assert any(line.startswith("Vary: Accept-Encoding") for line in lines), lines
        assert not any("User-Agent" in line for line in lines), lines
        assert "User-Agent" not in css.text, css.text[:200]


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestRespectVary:
    """Bash: respect vary user-agent (respect_vary.sh:15-24)."""

    PAGE = "/mod_pagespeed_test/vary/index.html?PageSpeedFilters=inline_css"

    def test_vary_user_agent_css_is_not_inlined(self, client: PageSpeedClient, vhost_client):
        # Control: without Vary: User-Agent the same stylesheet is inlined.
        client.fetch_until_contains(self.PAGE, r"<style>", timeout=100.0)
        vhost = vhost_client("respectvary.example.com")
        require_status_ok(vhost.get(self.PAGE), "respectvary first fetch")
        time.sleep(0.1)
        second = vhost.get(self.PAGE)
        require_status_ok(second, "respectvary second fetch")
        assert "<style>" not in second.text, (
            f"a Vary: User-Agent stylesheet was inlined: {second.text[:300]!r}"
        )


DESKTOP_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
)
SAFARI_UA = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/17.0 Safari/605.1.15"
)
PHONE_UA = (
    "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36"
)

# Rewritten HTML depends on Accept (image format), Save-Data (image quality)
# and User-Agent (device class, inlining) whenever image rewriting is on, and
# names them in Vary whatever the request carries. Sec-CH-DPR is read only by
# the optimizer worker at the image URL, never by the HTML rewriter.
HTML_VARY_TOKENS = ["accept", "save-data", "user-agent"]
CLIENTS = {
    "desktop-webp": {"User-Agent": DESKTOP_UA, "Accept": "text/html,image/webp,*/*"},
    "safari-no-webp": {"User-Agent": SAFARI_UA, "Accept": "text/html,*/*"},
    "phone": {"User-Agent": PHONE_UA, "Accept": "text/html,image/webp,*/*"},
    "save-data": {
        "User-Agent": DESKTOP_UA,
        "Accept": "text/html,image/webp,*/*",
        "Save-Data": "on",
    },
}


def vary_tokens(response):
    """Lower-cased Vary tokens across all Vary lines, in order, with repeats."""
    tokens = []
    for value in response.header_values("Vary"):
        tokens.extend(t.strip().lower() for t in value.split(",") if t.strip())
    return tokens


class TestHtmlVary:
    """Rewritten HTML names the request headers it depends on in Vary."""

    IMAGES_ON = "?PageSpeedFilters=rewrite_images"
    IMAGES_OFF = "?PageSpeedFilters=collapse_whitespace"

    @pytest.mark.parametrize("name", sorted(CLIENTS))
    def test_image_rewriting_names_the_three_headers_once(
        self, client: PageSpeedClient, example_root: str, name: str
    ):
        url = f"{example_root}/rewrite_images.html{self.IMAGES_ON}"
        response = client.get(url, headers=CLIENTS[name])
        require_status_ok(response, f"{name} {url}")
        tokens = vary_tokens(response)
        for wanted in HTML_VARY_TOKENS:
            assert tokens.count(wanted) == 1, (name, response.raw_headers)
        assert "sec-ch-dpr" not in tokens, (name, response.raw_headers)

    def test_head_carries_the_same_vary_as_get(self, client: PageSpeedClient, example_root: str):
        url = f"{example_root}/rewrite_images.html{self.IMAGES_ON}"
        headers = CLIENTS["desktop-webp"]
        get = client.get(url, headers=headers)
        head = client.head(url, headers=headers)
        require_status_ok(get, url)
        require_status_ok(head, url)
        assert sorted(t for t in vary_tokens(head) if t in HTML_VARY_TOKENS) == sorted(
            HTML_VARY_TOKENS
        ), head.raw_headers
        assert [t for t in vary_tokens(head) if t in HTML_VARY_TOKENS] == [
            t for t in vary_tokens(get) if t in HTML_VARY_TOKENS
        ]

    def test_images_off_names_none_of_them(self, client: PageSpeedClient, example_root: str):
        url = f"{example_root}/rewrite_images.html{self.IMAGES_OFF}"
        response = client.get(url, headers=CLIENTS["desktop-webp"])
        require_status_ok(response, url)
        tokens = vary_tokens(response)
        for unwanted in HTML_VARY_TOKENS:
            assert unwanted not in tokens, response.raw_headers


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
