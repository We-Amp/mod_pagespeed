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

"""ForbidAllDisabledFilters: disabled filters stay forbidden in subdirectories.

Ported from: pagespeed/apache/system_tests/forbid_all_disabled.sh

forbid_all_disabled/.htaccess turns ForbidAllDisabledFilters back off and
enables remove_quotes, remove_comments and collapse_whitespace (disabling
inline_css) for that directory; the <Directory> block for
forbid_all_disabled/disabled forbids all disabled filters (making those
three un-re-enableable below it) and re-enables inline_css;
disabled/cheat/.htaccess tries the same overrides .htaccess used above and
is denied by the parent directory's forbid-all.

The <Directory> block and .htaccess inheritance are Apache-specific
configuration mechanisms, but ForbidAllDisabledFilters itself is shared
module behaviour, so this is gated on the debug_conf_dirs lane fixture
rather than restricted to Apache by server type.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    require_status_ok,
)

_BLOCKING = {"X-PSA-Blocking-Rewrite": "psatest"}
_ENABLE_FORBIDDEN = "+remove_quotes,+remove_comments,+collapse_whitespace"


def _fetch(client: PageSpeedClient, path: str, headers: dict) -> str:
    """Bash: $WGET $WGET_ARGS -q -O $OUTFILE $HEADER $URL"""
    return require_status_ok(client.get(path, headers=headers), path).text


def _check_forbid_all_disabled(client, test_root, query="", header=None):
    """Bash: test_forbid_all_disabled "$QUERYP" "$HEADER" """
    inline_css = ",-inline_css" if query else "?PageSpeedFilters=-inline_css"
    headers = {**_BLOCKING, **(header or {})}
    url1 = f"{test_root}/forbid_all_disabled/forbidden.html"
    url2 = f"{test_root}/forbid_all_disabled/disabled/forbidden.html"
    url3 = f"{test_root}/forbid_all_disabled/disabled/cheat/forbidden.html"

    # Fetch testing that forbidden filters stay disabled.
    out = _fetch(client, url1 + query + inline_css, headers)
    assert_contains(out, "<link rel=stylesheet")
    assert_not_contains(out, "<!--")
    assert_contains(out, r"(?m)^<li>")
    for url in (url2, url3):
        out = _fetch(client, url + query + inline_css, headers)
        assert_contains(out, '<link rel="stylesheet')
        assert_contains(out, "<!--")
        assert_contains(out, "    <li>")

    # Fetch testing that enabling inline_css for disabled/ directory works.
    assert_not_contains(_fetch(client, url1, headers), "<style>.yellow")
    assert_contains(_fetch(client, url2, headers), "<style>.yellow")
    assert_contains(_fetch(client, url3, headers), "<style>.yellow")


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestForbidAllDisabledFilters:

    def test_baseline(self, client: PageSpeedClient, test_root: str):
        """start_test ForbidAllDisabledFilters baseline check."""
        _check_forbid_all_disabled(client, test_root)

    def test_query_parameters(self, client: PageSpeedClient, test_root: str):
        """start_test ForbidAllDisabledFilters query parameters check."""
        _check_forbid_all_disabled(
            client, test_root, query=f"?PageSpeedFilters={_ENABLE_FORBIDDEN}")

    def test_request_headers(self, client: PageSpeedClient, test_root: str):
        """start_test ForbidAllDisabledFilters request headers check."""
        _check_forbid_all_disabled(
            client, test_root, header={"PageSpeedFilters": _ENABLE_FORBIDDEN})


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
