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

"""Server-side includes: the include is combined, and never in-place
optimized under the page's URL.

Ported from: pagespeed/system/system_tests/server_side_includes.sh
"""

import re
import time

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_stat_delta,
    require_status_ok,
)
from pagespeed_test_framework.stats import count_matching_lines

COMBINED = re.escape(
    "styles/yellow.css+blue.css+big.css+bold.css.pagespeed.cc.xo4He3_gYf.css"
)


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestServerSideIncludes:
    """Bash: server-side includes."""

    def test_combine_css_across_an_include(self, client: PageSpeedClient, test_root: str):
        page = client.fetch_until(
            f"{test_root}/ssi/ssi.shtml?PageSpeedFilters=combine_css",
            condition=lambda r: count_matching_lines(r.text, re.escape(".pagespeed.")) == 1,
            timeout=100.0,
            detail_fn=lambda r: f"body={r.text[:300]!r}",
        )
        assert count_matching_lines(page.text, COMBINED) == 1, page.text[:600]


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestServerSideIncludesSubrequestSkip:
    """An include subrequest must not run in-place optimization.

    The include runs as an httpd subrequest, and the module used to run the
    in-place paths on it under the INCLUDING page's URL: on a miss a recorder
    was attached to the subrequest, whose pool Apache destroys while the
    protocol filter chain it shares with the main request still points at the
    recorder's filter, so the page's own output ran through freed memory.  The
    subrequest must pass through untouched, and the main request's in-place
    behaviour must stay exactly as it was.
    """

    @pytest.mark.requires_stats
    def test_include_subrequest_takes_no_in_place_path(
        self, client: PageSpeedClient, test_root: str, stats_snapshot
    ):
        # A unique query parameter keeps the page's first in-place lookup a fresh
        # miss even on a warm server, so the miss count below is exact.
        page = f"{test_root}/ssi/ssi_ipro.shtml?ssi_run={time.time_ns()}"
        css = f"{test_root}/ssi/included.css"

        # First request: the page is served whole, as HTML, with the included
        # stylesheet's text expanded into it.
        stats_0 = stats_snapshot()
        page_1 = client.get(page)
        require_status_ok(page_1, "the SSI page")
        assert "text/html" in page_1.header("Content-Type"), page_1.headers
        assert "ssi-ipro-marker" in page_1.text, page_1.text[:300]
        assert "ssi-ipro-page-body" in page_1.text, page_1.text[:300]
        stats_1 = stats_snapshot()

        # Exactly one in-place lookup miss: the main request's own.  The
        # include used to run the in-place path a second time, under the
        # page's URL rather than the stylesheet's own.
        assert_stat_delta(stats_0, stats_1, "ipro_not_in_cache", 1)

        # The server stays up: the same page answers again, still complete.
        page_2 = client.get(page)
        require_status_ok(page_2, "the SSI page, second request")
        assert "ssi-ipro-marker" in page_2.text, page_2.text[:300]

        # The included stylesheet, requested directly (a main request), still
        # optimizes in place under its own URL: the skip must not have
        # over-reached.
        original = client.get(f"{css}?PageSpeed=off")
        require_status_ok(original, "the included stylesheet with PageSpeed=off")
        optimized = client.fetch_until(
            css,
            condition=lambda r: len(r.body) < len(original.body),
            timeout=30.0,
            detail_fn=lambda r: f"len={len(r.body)} want<{len(original.body)}",
        )
        assert "ssi-ipro-marker" in optimized.text, optimized.text[:300]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
