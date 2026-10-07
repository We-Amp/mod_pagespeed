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

"""JavaScript minification, cache extension and source maps.

Ported from: pagespeed/system/system_tests/source_maps.sh
"""

import re
import time
import urllib.parse
from typing import Tuple

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_file_size,
    assert_header_contains,
    assert_not_contains,
    require_match,
)
from pagespeed_test_framework.client import Response

_PAGE = "/experimental_js_minifier/index.html"
_ORIGINAL_HTML_SIZE = 1484
_JM_LINE = r"src=.*\.pagespeed\.jm\."
_JM_SRC = r'src="([^"]*\.pagespeed\.jm\.[^"]*)"'
_ONE_YEAR = r"(?i)^max-age=31536000"


def _lines_matching(text: str, pattern: str) -> int:
    """Bash: grep -c (counts lines)."""
    return sum(1 for line in text.splitlines() if re.search(pattern, line))


def _path_of(url: str) -> str:
    parts = urllib.parse.urlsplit(url)
    return parts.path + (f"?{parts.query}" if parts.query else "")


def _is_optimized(response: Response) -> bool:
    """The optimized variant: a 200 carrying the one-year cache lifetime."""
    return response.status == 200 and bool(
        re.search(_ONE_YEAR, response.header("Cache-Control")))


def _fetch_optimized(client: PageSpeedClient, path: str, what: str) -> Response:
    """Poll a .pagespeed. resource until PageSpeed serves the optimized variant.

    A fetch of a .pagespeed. URL that is not served from the cache
    reconstructs the resource under the location's default options, and two
    outcomes answer with ``max-age=300,private``
    (RewriteContext::FixFetchFallbackHeaders) instead of the one-year TTL
    (for the ``sm`` source-map URL a hash mismatch is a 404 instead, since
    the source-map filter fails on mismatch):

    - the reconstruction missed the deadline and the unoptimized input was
      served (issue #1129). The background rewrite still completes, so the
      next poll is a cache hit and the wait converges;
    - the reconstruction produced a different hash. The page here is
      rewritten with ``include_js_source_maps`` on its query string, which
      the bare script URL does not carry, so a reconstruction can never
      reproduce its hash: the URL is served optimized only from the cache
      entry the HTML rewrite wrote, and polling cannot repair a missing one.
      That entry is stale on arrival when it is written in the same second
      a cache flush was stamped (test_show_cache flushes just before this
      file; flush_cache now leaves the flush second before returning, see
      conftest.wait_until_second_after).

    Only the one-year Cache-Control marks the resource as optimized, so wait
    for that, bounded by fetch_until's budget, the way the other system tests
    do. The caller's assertions on the returned response are unchanged.
    """
    start = time.monotonic()
    try:
        return client.fetch_until(
            path,
            _is_optimized,
            detail_fn=lambda r: (
                f"status={r.status} Cache-Control={r.header('Cache-Control')!r}"),
        )
    except TimeoutError as err:
        elapsed = time.monotonic() - start
        raise TimeoutError(
            f"timed out after {elapsed:.0f} s waiting for the optimized resource "
            f"({what} at {path}): {err}"
        ) from err


def _fetch_page_and_script(
    client: PageSpeedClient, test_root: str, filters: str
) -> Tuple[Response, Response]:
    """Bash: fetch_until -save -recursive $URL 'grep -c src=.*\\.pagespeed\\.jm\\.' 1"""
    page = f"{test_root}{_PAGE}?PageSpeedFilters={filters}"
    html = client.fetch_until(
        page,
        lambda r: _lines_matching(r.text, _JM_LINE) == 1,
        detail_fn=lambda r: (
            f"lines matching {_JM_LINE!r}: {_lines_matching(r.text, _JM_LINE)} expected=1"),
    )
    src = require_match(_JM_SRC, html, "rewritten script.js URL").group(1)
    script_url = urllib.parse.urljoin(f"http://{client.host}{page}", src)
    script = _fetch_optimized(client, _path_of(script_url), "rewritten script.js")
    return html, script


def _assert_minified_and_extended(html: Response, script: Response) -> None:
    # No comments should remain.
    assert_not_contains(html, "removed")
    assert_not_contains(script, "removed")
    # Net savings.
    assert_file_size(html, "-lt", _ORIGINAL_HTML_SIZE)
    # Rewritten JS is cache-extended.
    assert_header_contains(script, "Cache-Control", _ONE_YEAR)
    assert script.header("Expires"), "rewritten script.js carries no Expires header"


class TestSourceMaps:

    def test_js_minification_and_cache_extension(
        self, client: PageSpeedClient, test_root: str
    ):
        """start_test JS minification and cache extension"""
        html, script = _fetch_page_and_script(client, test_root, "rewrite_javascript")
        _assert_minified_and_extended(html, script)
        # Contents of <script src=> element kept.
        assert_contains(html.text + script.text, "preserved")

    def test_source_map(self, client: PageSpeedClient, test_root: str):
        """start_test Source map tests"""
        html, script = _fetch_page_and_script(
            client, test_root, "rewrite_javascript,include_js_source_maps")
        _assert_minified_and_extended(html, script)
        # No source map for inline JS.
        assert_not_contains(html, "sourceMappingURL")
        # Yes source_map for external JS.
        map_url = require_match(
            r"sourceMappingURL=(http://\S+)", script, "source map URL").group(1)
        source_map = _fetch_optimized(client, _path_of(map_url), "source map")
        assert_header_contains(source_map, "Cache-Control", _ONE_YEAR)  # Long cache
        assert_contains(source_map, re.escape("script.js?PageSpeed=off"))  # Has source URL.
        assert_contains(source_map, re.escape('"mappings":'))  # Has mappings.


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
