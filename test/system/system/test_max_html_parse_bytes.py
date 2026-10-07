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

"""MaxHtmlParseBytes: over the limit, pages are handed back unparsed.

Ported from: pagespeed/system/system_tests/large_html_files.sh and
pagespeed/apache/system_tests/max_html_parse_bytes.sh
"""

import time

import pytest

from pagespeed_test_framework import PageSpeedClient, require_match, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

REDIRECT = r'window.location=".*&PageSpeed=off'


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestMaxHtmlParseBytesSet:
    """Bash: large_html_files.sh:15-35 (MaxHtmlParseBytes 5000 on the directory)."""

    def test_large_page_redirects_then_is_left_alone(
        self, client: PageSpeedClient, test_root: str
    ):
        url = f"{test_root}/max_html_parse_size/large_file.html?value={int(time.time())}"
        first = client.get(url, headers={"PageSpeedFilters": "rewrite_images"})
        require_status_ok(first, url)
        require_match(REDIRECT, first, "the PageSpeed=off redirect script")
        later = client.fetch_until(
            url,
            condition=lambda r: count_matching_lines(r.text, REDIRECT) == 0,
            timeout=100.0,
        )
        assert "pagespeed.ic" not in later.text, "the unparsed page was rewritten"


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestMaxHtmlParseBytesUnset:
    """Bash: When ModPagespeedMaxHtmlParseBytes is not set, we do not insert a redirect."""

    def test_no_redirect_when_the_limit_is_unset(self, vhost_client):
        response = vhost_client("secondary.example.com").get(
            "/mod_pagespeed_test/large_file.html?PageSpeedFilters="
        )
        require_status_ok(response, "secondary large_file.html")
        assert "window.location=" not in response.text, "a redirect script was inserted"
        assert "Lorem ipsum dolor sit amet" in response.text, response.text[:200]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
