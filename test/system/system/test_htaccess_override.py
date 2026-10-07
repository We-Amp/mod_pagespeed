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

"""ModPagespeedDisallow / Allow in an .htaccess hierarchy.

Ported from: pagespeed/apache/system_tests/htaccess_override.sh
"""

import re

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

OPTIMIZED = re.escape("background:#9370db")


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestHtaccessOverride:
    """Bash: Make sure Disallow/Allow overrides work in htaccess hierarchies."""

    def test_disallow_and_allow_follow_the_htaccess_hierarchy(
        self, client: PageSpeedClient, test_root: str
    ):
        disallowed = client.get(f"{test_root}/htaccess/purple.css")
        require_status_ok(disallowed, "htaccess/purple.css")
        assert "MediumPurple" in disallowed.text, (
            f"a Disallowed stylesheet was optimized: {disallowed.text[:200]!r}"
        )
        client.fetch_until(
            f"{test_root}/htaccess/override/purple.css",
            condition=lambda r: count_matching_lines(r.text, OPTIMIZED) == 1,
            timeout=100.0,
            detail_fn=lambda r: f"body={r.text[:120]!r}",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
