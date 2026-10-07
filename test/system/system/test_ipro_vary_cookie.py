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

"""A resource that varies on Cookie or Cookie2 is never optimized in place.

Ported from: pagespeed/system/system_tests/ipro_vary_cookie.sh
"""

import re
import time

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.stats import settled_stats

ORIGINAL = "    background: MediumPurple;"


def _expect_no_rewrite(client, stats_snapshot, url, headers=None):
    """ipro_expect_no_rewrite: fetch until ipro_not_rewritable changes."""
    start = settled_stats(stats_snapshot, ["ipro_not_rewritable"]).get(
        "ipro_not_rewritable", 0
    )
    deadline = time.monotonic() + 30.0
    while True:
        response = client.get(url, headers=headers)
        if stats_snapshot().get("ipro_not_rewritable", 0) != start:
            return response
        if time.monotonic() > deadline:
            pytest.fail(f"{url}: ipro_not_rewritable never moved from {start}")
        time.sleep(0.1)


@pytest.mark.requires_stats
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestIproVaryCookie:
    """Bash: ipro with vary:cookie / vary:cookie2, with and without the cookie."""

    @pytest.mark.parametrize(
        "path,request_headers,vary_regex",
        [
            pytest.param("ipro/cookie/vary_cookie.css", None,
                         r"(Accept-Encoding,)?Cookie", id="cookie-unset"),
            pytest.param("ipro/cookie/vary_cookie.css", {"Cookie": "cookie-data"},
                         r"(Accept-Encoding,)?Cookie", id="cookie-set"),
            pytest.param("ipro/cookie2/vary_cookie2.css", None,
                         r"(Accept-Encoding,)?Cookie2", id="cookie2-unset"),
            pytest.param("ipro/cookie2/vary_cookie2.css", {"Cookie2": "cookie2-data"},
                         r"(Accept-Encoding,)?Cookie2", id="cookie2-set"),
        ],
    )
    def test_vary_cookie_resource_is_never_optimized(
        self, client: PageSpeedClient, test_root: str, stats_snapshot,
        path, request_headers, vary_regex,
    ):
        url = f"{test_root}/{path}"
        response = _expect_no_rewrite(client, stats_snapshot, url, request_headers)
        require_status_ok(response, url)
        assert ORIGINAL in response.text, f"{url}: optimized body {response.text[:200]!r}"
        vary_lines = [f"Vary: {v}" for v in response.header_values("Vary")]
        assert any(re.search("Vary: " + vary_regex, line) for line in vary_lines), (
            f"{url}: Vary headers {vary_lines}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
