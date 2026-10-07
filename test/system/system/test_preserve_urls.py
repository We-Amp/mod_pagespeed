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

"""PreserveURLs: no resource URL is rewritten where it is on.

Ported from: pagespeed/system/system_tests/preserve_urls.sh

mod_pagespeed_test/preserveurls/ runs CoreFilters; preserveurls/on/ sets
Js/Image/CssPreserveURLs On.

The bash fetched these pages through the secondary host, whose cache is its
own. The port does the same: the page references the shared example-site
resources, and on the primary host the metadata other tests leave behind for
rewrite_javascript.js can keep it from being inlined here.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_not_contains,
    require_status_ok,
)

_BLOCKING = {"X-PSA-Blocking-Rewrite": "psatest"}


def _lines_matching(text: str, pattern: str) -> int:
    """Bash: grep -c / egrep -c (counts lines)."""
    return sum(1 for line in text.splitlines() if re.search(pattern, line))


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestPreserveUrls:

    def test_preserve_urls_on_prevents_rewriting(
        self, secondary_client: PageSpeedClient, test_root: str
    ):
        """start_test PreserveURLs on prevents URL rewriting"""
        path = f"{test_root}/preserveurls/on/preserveurls.html"
        response = require_status_ok(secondary_client.get(path, headers=_BLOCKING), path)
        assert_not_contains(response, r"\.pagespeed\.")

    def test_preserve_urls_off_rewrites(self, secondary_client: PageSpeedClient, test_root: str):
        """start_test PreserveURLs off causes URL rewriting"""
        path = f"{test_root}/preserveurls/off/preserveurls.html"
        for pattern in (
            r"big.css.pagespeed.",                  # style.css was inlined
            r'document\.write\("External',          # introspection.js was inlined
            r"BikeCrashIcn\.png\.pagespeed\.",      # the image was optimized
        ):
            secondary_client.fetch_until(
                path,
                lambda r, p=pattern: _lines_matching(r.text, p) == 1,
                detail_fn=lambda r, p=pattern: (
                    f"lines matching {p!r}: {_lines_matching(r.text, p)} expected=1"),
            )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
