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


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
