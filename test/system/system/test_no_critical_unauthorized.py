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

"""Critical CSS selectors never come from unauthorized resources unless allowed.

Ported from: pagespeed/system/system_tests/no_critical_unauthorized_resources.sh
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
)
from pagespeed_test_framework.client import Response

_PAGE = (
    "/unauthorized/prioritize_critical_css.html"
    "?PageSpeedFilters=prioritize_critical_css,debug"
)
_BEACON_INIT = "pagespeed.criticalCssBeaconInit"
_IMPORT_FAILURE = (
    "<!--Flattening failed: Cannot import http://www.google.com/css/maia.css "
    "as it is on an unauthorized domain-->"
)
_NOT_AUTHORIZED = (
    "<!--The preceding resource was not rewritten because its domain "
    "(www.modpagespeed.com) is not authorized-->"
)
_REWRITTEN_CSS = re.compile(r"with_unauthorized_imports\.css\.pagespeed\.cf")
_FULLY_REWRITTEN = re.compile(
    _REWRITTEN_CSS.pattern + r".*gsc-completion-selected.*gsc-completion-selected")


def _lines_containing(text: str, needle: str) -> int:
    """Bash: fgrep -c <needle> (counts lines)."""
    return sum(1 for line in text.splitlines() if needle in line)


class TestNoCriticalUnauthorizedResources:

    def test_no_critical_selectors_from_unauthorized_resources(
        self, client: PageSpeedClient, test_root: str
    ):
        """start_test no critical selectors chosen from unauthorized resources"""
        # Wait for the beacon AND for CssFilter's rewrite of the CSS link: that
        # rewrite emits the flattening-failure comment; the beacon comes from
        # the summarizer, a separate rewrite with its own cache entry and lock.
        # On a cold cache the first request's CssFilter rewrite misses the
        # rewrite deadline and keeps running detached, holding its creation
        # lock. A request arriving before it finishes loses that lock (or has
        # the rewrite load-shed) and renders nothing for the link: no .cf. URL,
        # no debug comment, not even a deadline comment. Its summarizer can
        # still finish, so the page carries the beacon without the flattening
        # comment until the detached rewrite lands. Waiting for the beacon
        # alone let the test assert on that transient page.
        response = client.fetch_until(
            f"{test_root}{_PAGE}",
            lambda r: (_lines_containing(r.text, _BEACON_INIT) == 3
                       and _REWRITTEN_CSS.search(r.text) is not None),
            detail_fn=lambda r: (
                f"lines with {_BEACON_INIT}: "
                f"{_lines_containing(r.text, _BEACON_INIT)} expected=3; "
                f"rewritten .cf. CSS present: "
                f"{_REWRITTEN_CSS.search(r.text) is not None}"),
        )
        text = response.text
        # Except for the occurrence in html, gsc-completion-selected must not
        # occur anywhere else, i.e. in the selector list.
        assert _lines_containing(text, "gsc-completion-selected") == 1, text[-2000:]
        # a) no selectors from the unauthorized @import
        assert_not_contains(text, "maia-display")
        # b) no selectors from the authorized @import (it won't be flattened)
        assert_not_contains(text, "interesting_color")
        # c) selectors that don't depend on flattening appear
        assert _lines_containing(text, "non_flattened_selector") == 1, text[-2000:]
        assert text.count(_IMPORT_FAILURE) == 1, text[-2000:]
        assert text.count(_NOT_AUTHORIZED) == 1, text[-2000:]


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "external_origin")
class TestUnauthorizedResourcesAllowedOnAllowingVhost:

    def test_inline_unauthorized_resources_allows_unauthorized_selectors(
        self, vhost_client
    ):
        """start_test inline_unauthorized_resources allows unauthorized css selectors"""

        def fully_rewritten(r: Response) -> bool:
            return _FULLY_REWRITTEN.search(r.text.replace("\n", " ")) is not None

        response = vhost_client("unauthorizedresources.example.com").fetch_until(
            f"/mod_pagespeed_test{_PAGE}",
            fully_rewritten,
            detail_fn=lambda r: "rewritten .cf. CSS and a second gsc-completion-selected not both present",
        )
        text = response.text
        # This page had beaconing javascript on it.
        assert _lines_containing(text, _BEACON_INIT) == 3, text[-2000:]
        assert_not_contains(text, "maia-display")
        assert_not_contains(text, "interesting_color")
        assert _lines_containing(text, "non_flattened_selector") == 1, text[-2000:]
        assert_contains(text, re.escape(_IMPORT_FAILURE))


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
