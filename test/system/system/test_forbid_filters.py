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

"""ForbidFilters: forbidden filters stay off for queries, headers and URLs.

Ported from: pagespeed/system/system_tests/forbid_filters.sh

forbidden.example.com: CoreFilters, forbids remove_quotes, remove_comments,
collapse_whitespace, rewrite_css, resize_images; inline_css disabled.
"""

import re

import pytest

from pagespeed_test_framework import (
    assert_contains,
    assert_http_status,
    require_status_ok,
)

HOST = "forbidden.example.com"
_PAGE = "/mod_pagespeed_test/forbidden.html"
_ENABLE_FORBIDDEN = "+remove_quotes,+remove_comments,+collapse_whitespace"
_STYLES = "/mod_pagespeed_example/styles"
_IMAGES = "/mod_pagespeed_example/images"


def _assert_forbidden_filters_did_not_run(client, path, headers=None):
    """Bash: test_forbid_filters"""
    text = require_status_ok(client.get(path, headers=headers), path).text
    assert_contains(text, '<link rel="stylesheet')
    assert_contains(text, "<!--")
    assert_contains(text, "    <li>")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestForbidFilters:

    def test_baseline(self, vhost_client):
        """start_test ForbidFilters baseline check."""
        _assert_forbidden_filters_did_not_run(vhost_client(HOST), _PAGE)

    def test_query_parameters(self, vhost_client):
        """start_test ForbidFilters query parameters check."""
        _assert_forbidden_filters_did_not_run(
            vhost_client(HOST), f"{_PAGE}?PageSpeedFilters={_ENABLE_FORBIDDEN}")

    def test_request_headers(self, vhost_client):
        """start_test "ForbidFilters request headers check." """
        _assert_forbidden_filters_did_not_run(
            vhost_client(HOST), _PAGE, {"PageSpeedFilters": _ENABLE_FORBIDDEN})

    def test_direct_resource_rewriting_disallowed(self, vhost_client):
        """start_test ForbidFilters disallows direct resource rewriting."""
        client = vhost_client(HOST)
        # .ce. is allowed
        assert_http_status(
            client.get(f"{_STYLES}/all_styles.css.pagespeed.ce.n7OstQtwiS.css"), 200)
        # .cf. is forbidden
        assert_http_status(
            client.get(f"{_STYLES}/A.all_styles.css.pagespeed.cf.UH8L-zY4b4.css"), 404)
        # The image will be optimized but NOT resized to the much smaller size,
        # so it will be >200k (optimized) rather than <20k (resized).
        resized = client.get(
            f"{_IMAGES}/256x192xPuzzle.jpg.pagespeed.ic.8AB3ykr7Of.jpg",
            headers={"X-PSA-Blocking-Rewrite": "psatest"})
        length = resized.content_length
        assert length is not None, "resized image response has no Content-Length"
        assert length > 200000, f"image was resized: Content-Length {length}"
        cache_control = resized.cache_control
        assert re.search(r"\bmax-age=300\b", cache_control), cache_control
        assert re.search(r"\bprivate\b", cache_control), cache_control


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
