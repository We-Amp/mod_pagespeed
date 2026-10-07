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

"""Cache-Control: no-transform handling.

Ported from: pagespeed/system/system_tests/no_transform.sh

A no-transform response is neither rewritten nor stripped of no-transform,
unless DisableRewriteOnNoTransform is turned off for its directory.

The tests go through secondary_client, the lane's default secondary vhost
with its own cache, where the bash used the primary, to keep the cache
state isolated from the rest of the lane.
"""

import re

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

BLOCKING_REWRITE = {"X-PSA-Blocking-Rewrite": "psatest"}


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestNoTransform:

    def test_no_transform_resource_is_not_rewritten(
        self, secondary_client: PageSpeedClient, test_root: str
    ):
        """Bash: HonorNoTransform cache-control: no-transform."""
        page = secondary_client.get(
            f"{test_root}/no_transform/image.html", headers=BLOCKING_REWRITE
        )
        require_status_ok(page, "no_transform/image.html")
        assert ".pagespeed." not in page.text, (
            f"no-transform page was rewritten: {page.text[:500]}"
        )
        image = secondary_client.get(
            f"{test_root}/no_transform/BikeCrashIcn.png", headers=BLOCKING_REWRITE
        )
        require_status_ok(image, "no_transform/BikeCrashIcn.png")
        assert re.search(r"no-transform", image.header("Cache-Control")), (
            f"no-transform was dropped: Cache-Control [{image.header('Cache-Control')}]"
        )

    def test_rewrite_despite_no_transform_when_disabled(
        self, secondary_client: PageSpeedClient, test_root: str
    ):
        """Bash: rewrite on Cache-control: no-transform (grep -c style == 2)."""
        url = f"{test_root}/disable_no_transform/index.html?PageSpeedFilters=inline_css"
        secondary_client.fetch_until(
            url,
            lambda r: count_matching_lines(r.text, "style") == 2,
            detail_fn=lambda r: (
                f"lines with 'style'={count_matching_lines(r.text, 'style')} expected=2"
            ),
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
