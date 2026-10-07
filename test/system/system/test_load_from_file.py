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

"""LoadFromFile mappings and their Allow/Disallow rules.

Ported from: pagespeed/system/system_tests/load_from_file.sh

The web_dir/ and file_dir/ copies of each stylesheet differ, so the
inlined text shows whether a resource was read from disk or fetched.
"""

import re

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines


def _poll(client, url, pattern):
    return client.fetch_until(
        url,
        condition=lambda r: count_matching_lines(r.text, pattern) == 1,
        timeout=100.0,
        detail_fn=lambda r: f"lines matching {pattern!r}: {count_matching_lines(r.text, pattern)}",
    )


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestLoadFromFile:
    """Bash: LoadFromFile, LoadFromFileMatch, nostore on a subdirectory."""

    def test_load_from_file_rules(self, client: PageSpeedClient, test_root: str):
        url = f"{test_root}/load_from_file/index.html?PageSpeedFilters=inline_css"
        _poll(client, url, r"blue")
        _poll(client, url, re.escape("web.httponly.example.css"))
        _poll(client, url, re.escape("web.example.ssp.css"))
        _poll(client, url, re.escape("file.exception.ssp.css"))

    def test_load_from_file_match(self, client: PageSpeedClient, test_root: str):
        _poll(client, f"{test_root}/load_from_file_match/index.html?PageSpeedFilters=inline_css",
              r"blue")

    def test_nostore_on_a_subdirectory_is_retained(self, client: PageSpeedClient, test_root: str):
        response = client.get(f"{test_root}/nostore/nostore.html")
        require_status_ok(response, "nostore/nostore.html")
        lines = [f"{name}: {value}" for name, value in response.raw_headers]
        assert any("Cache-Control: max-age=0, no-cache, no-store" in l for l in lines), lines


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
