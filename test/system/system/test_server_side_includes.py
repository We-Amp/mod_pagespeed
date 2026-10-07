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

"""CSS pulled in by a server-side include is combined.

Ported from: pagespeed/system/system_tests/server_side_includes.sh
"""

import re

import pytest

from pagespeed_test_framework import PageSpeedClient
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


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
