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

"""CSS with an id is combined only when the id is permitted.

Ported from: pagespeed/system/system_tests/css_combining_authorization.sh
"""

import re

import pytest

from pagespeed_test_framework import PageSpeedClient, assert_contains
from pagespeed_test_framework.stats import count_matching_lines

COMBINED = re.escape("styles/big.css+bold.css.pagespeed.cc")


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestCssCombiningAuthorization:
    """Bash: can combine css with authorized ids only."""

    def test_only_permitted_ids_are_combined(self, client: PageSpeedClient, test_root: str):
        url = f"{test_root}/combine_css_with_ids.html?PageSpeedFilters=combine_css"
        page = client.fetch_until(
            url,
            condition=lambda r: count_matching_lines(r.text, COMBINED) == 1,
            timeout=100.0,
            detail_fn=lambda r: f"combined lines: {count_matching_lines(r.text, COMBINED)}",
        )
        assert_contains(page, re.escape('/styles/yellow.css" id='))
        assert_contains(page, re.escape('/styles/blue.css" id='))


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
