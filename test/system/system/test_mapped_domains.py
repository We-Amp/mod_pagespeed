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

"""Domain mapping between a site, its origin and a CDN.

Ported from: pagespeed/system/system_tests/mapped_domain_relative_css.sh and
shared_cdn_host_header.sh
"""

import re

import pytest

from pagespeed_test_framework import require_match, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

DIR = "mod_pagespeed_test/map_css_embedded"
MAPPED_PREFIX = f"{DIR}/A.styles.css.pagespeed.cf"


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestMappedDomains:
    """Bash: Relative images embedded in a CSS file served from a mapped domain /
    shared CDN short-circuit back to origin via host-header override."""

    def test_css_from_a_mapped_domain_keeps_its_hash(self, vhost_client):
        www = vhost_client("www.example.com")
        page_path = f"/{DIR}/issue494.html"
        www.fetch_until(
            page_path,
            condition=lambda r: count_matching_lines(r.text, "cdn.example.com/" + MAPPED_PREFIX) == 1,
            timeout=100.0,
        )
        page = www.get(page_path)
        require_status_ok(page, page_path)
        mapped_css = require_match(re.escape(MAPPED_PREFIX) + r"..*?\.css", page,
                                   "mapped CSS path").group(0)
        css = vhost_client("origin.example.com").get(f"/{mapped_css}")
        require_status_ok(css, mapped_css)
        assert "max-age=31536000" in css.header("Cache-Control"), css.raw_headers

    def test_shared_cdn_maps_back_with_a_host_header(self, vhost_client):
        vhost_client("customhostheader.example.com").fetch_until(
            "/map_origin_host_header.html",
            condition=lambda r: count_matching_lines(r.text, r"data:image/png;base64") == 1,
            timeout=100.0,
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
