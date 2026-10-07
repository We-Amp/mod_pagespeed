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

"""Custom headers survive rewriting exactly once.

Ported from: pagespeed/system/system_tests/more_custom_headers.sh

The header comes from a <Location /mod_pagespeed_test> append, where the
bash config appended it server-wide.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok


def _lines(response):
    return [f"{name}: {value}" for name, value in response.raw_headers]


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestMoreCustomHeaders:
    """Bash: Custom headers remain on HTML / on resources."""

    def test_html_keeps_custom_header_once_and_is_not_cacheable(
        self, client: PageSpeedClient, test_root: str
    ):
        response = client.get(f"{test_root}/rewrite_compressed_js.html")
        require_status_ok(response, "rewrite_compressed_js.html")
        lines = _lines(response)
        assert any(l.startswith("X-Extra-Header: 1") for l in lines), lines
        assert not any("X-Extra-Header: 1, 1" in l for l in lines), lines
        assert any("Cache-Control: max-age=0, no-cache" in l for l in lines), lines

    def test_resource_keeps_custom_header_once_and_is_cached_a_year(
        self, client: PageSpeedClient, test_root: str
    ):
        url = f"{test_root}/compressed/hello_js.custom_ext.pagespeed.ce.HdziXmtLIV.txt"
        response = client.get(url)
        require_status_ok(response, url)
        lines = _lines(response)
        assert not any("X-Extra-Header: 1, 1" in l for l in lines), lines
        assert sum(1 for l in lines if l.startswith("X-Extra-Header: 1")) == 1, lines
        assert any("Cache-Control: max-age=31536000" in l for l in lines), lines


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
