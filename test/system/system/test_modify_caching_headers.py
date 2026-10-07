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

"""ModifyCachingHeaders off keeps the origin's caching headers.

Ported from: pagespeed/system/system_tests/modify_caching_headers.sh
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok


def _lines(response):
    return [f"{name}: {value}" for name, value in response.raw_headers]


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestModifyCachingHeaders:
    """Bash: ModifyCachingHeaders (modify_caching_headers.sh:15-26)."""

    def test_modify_caching_headers_off_keeps_origin_cache_control(
        self, client: PageSpeedClient, test_root: str
    ):
        response = client.get(f"{test_root}/retain_cache_control/index.html")
        require_status_ok(response, "retain_cache_control/index.html")
        lines = _lines(response)
        assert any("Cache-Control: private, max-age=3000" in l for l in lines), lines
        assert response.header_values("Last-Modified"), f"no Last-Modified: {lines}"

    def test_downstream_caching_keeps_cache_control_and_drops_last_modified(
        self, client: PageSpeedClient, test_root: str
    ):
        response = client.get(
            f"{test_root}/retain_cache_control_with_downstream_caching/index.html"
        )
        require_status_ok(response, "retain_cache_control_with_downstream_caching")
        lines = _lines(response)
        assert not response.header_values("Last-Modified"), f"Last-Modified present: {lines}"
        assert any("Cache-Control: private, max-age=3000" in l for l in lines), lines


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
