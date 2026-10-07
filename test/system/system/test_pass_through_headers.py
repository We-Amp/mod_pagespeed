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

"""Origin headers survive when Cache-Control is set early.

Ported from: pagespeed/apache/system_tests/pass_through_headers.sh
"""

import re
from urllib.parse import urljoin, urlsplit

import pytest

from pagespeed_test_framework import require_match, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

HOST = "issue809.example.com"


def _has_issue809(response) -> bool:
    return "Issue809Value" in response.header_values("Issue809")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestPassThroughHeaders:
    """Bash: Pass through headers ... on HTML / on combined resources."""

    def test_early_cache_control_keeps_origin_headers_on_html(self, vhost_client):
        response = vhost_client(HOST).get("/mod_pagespeed_example/index.html")
        require_status_ok(response, "issue809 index.html")
        assert _has_issue809(response), response.raw_headers

    def test_combined_css_keeps_common_origin_headers(self, vhost_client):
        vhost = vhost_client(HOST)
        page_url = "/mod_pagespeed_example/combine_css.html"
        page = vhost.fetch_until(
            page_url,
            condition=lambda r: count_matching_lines(r.text, re.escape("css.pagespeed.cc.")) == 1,
            timeout=100.0,
        )
        href = require_match(
            r'stylesheet"[^>]*href="([^"]*css\.pagespeed\.cc\.[^"]*)"', page, "combined CSS link"
        ).group(1)
        parts = urlsplit(urljoin(f"http://{HOST}{page_url}", href))
        css = vhost.get(parts.path + (f"?{parts.query}" if parts.query else ""))
        require_status_ok(css, href)
        assert _has_issue809(css), css.raw_headers


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
