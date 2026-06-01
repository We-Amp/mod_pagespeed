#!/usr/bin/env python3
# Copyright 2024 Google LLC
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

"""Canonicalize JavaScript libraries filter tests.

Ported from: pagespeed/automatic/system_tests/canonicalize_javascript_libraries.sh

These tests verify that the canonicalize_javascript_libraries filter correctly
identifies and rewrites known library URLs to canonical versions.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestCanonicalizeJavascriptLibraries:
    """Tests for the canonicalize_javascript_libraries filter.

    Bash original::

        test_filter canonicalize_javascript_libraries finds library urls
        fetch_until $URL 'fgrep -c http://www.modpagespeed.com/rewrite_javascript.js' 1
    """

    def test_canonicalize_finds_library_urls(
        self, client: PageSpeedClient, example_root: str
    ):
        """canonicalize_javascript_libraries should find and rewrite library URLs."""
        url = f"{example_root}/canonicalize_javascript_libraries.html?PageSpeedFilters=canonicalize_javascript_libraries"

        # Debug: log first response and module debug file
        import re, os, glob
        # Check all debug logs in the cache dir
        for log_path in glob.glob(r"C:\PageSpeed\cache\*.log"):
            with open(log_path) as f:
                print(f"\n[DEBUG {os.path.basename(log_path)}]\n{f.read()}")
        debug_log = r"C:\PageSpeed\cache\debug.log"
        if os.path.exists(debug_log):
            with open(debug_log) as f:
                print(f"\n[DEBUG pagespeed_debug.log]\n{f.read()}")
        else:
            print(f"\n[DEBUG] {debug_log} not found")
        first = client.get(url)
        # Read again after first request (triggers lazy context creation)
        if os.path.exists(debug_log):
            with open(debug_log) as f:
                print(f"[DEBUG pagespeed_debug.log after request]\n{f.read()}")
        print(f"[DEBUG canonicalize] status={first.status} "
              f"x-pagespeed={first.header('X-PageSpeed')} "
              f"body_len={len(first.text)}")
        scripts = re.findall(r'<script[^>]*src=[^>]*>', first.text)
        print(f"[DEBUG canonicalize] script tags: {scripts}")

        response = client.fetch_until_count(
            url,
            pattern=r"http://www\.modpagespeed\.com/rewrite_javascript\.js",
            expected_count=1,
            timeout=30.0,
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
