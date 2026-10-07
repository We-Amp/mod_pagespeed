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

"""X-PSA-Blocking-Rewrite with the configured key, and with a wrong one.

Ported from: pagespeed/apache/system_tests/blocking_rewrite.sh
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_stat_delta,
    require_status_ok,
)
from pagespeed_test_framework.stats import count_matching_lines, settled_stats

COUNTERS = ["image_rewrites", "cache_hits", "cache_misses", "cache_inserts"]


@pytest.mark.requires_stats
class TestBlockingRewrite:
    """Bash: Blocking rewrite enabled (blocking_rewrite.sh:15-39)."""

    def test_blocking_rewrite_rewrites_in_the_request(
        self, client: PageSpeedClient, test_root: str, stats_snapshot
    ):
        url = f"{test_root}/blocking_rewrite.html?PageSpeedFilters=rewrite_images"
        old = settled_stats(stats_snapshot, COUNTERS)
        response = client.get(url, headers={"X-PSA-Blocking-Rewrite": "psatest"})
        require_status_ok(response, url)
        # The blocking key's whole point: the rewrite happens synchronously,
        # so the response to THIS request already carries the rewritten
        # image URL -- not just a stat that has moved by the time we look
        # again below.
        assert_contains(
            response,
            r"\.pagespeed\.ic\.",
            "the blocking-key response must already carry the rewritten "
            "image URL",
        )
        new = settled_stats(stats_snapshot, COUNTERS)
        assert_stat_delta(old, new, "image_rewrites", 1)
        assert_stat_delta(old, new, "cache_hits", 0)
        assert_stat_delta(old, new, "cache_misses", 2)
        assert_stat_delta(old, new, "cache_inserts", 2)


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestBlockingRewriteWrongKey:
    """Bash: Blocking rewrite enabled using wrong key (blocking_rewrite.sh:41-50)."""

    URL = "/mod_pagespeed_test/blocking_rewrite_another.html?PageSpeedFilters=rewrite_images"

    def test_wrong_key_does_not_block(self, vhost_client):
        vhost = vhost_client("secondary.example.com")
        first = vhost.get(self.URL, headers={"X-PSA-Blocking-Rewrite": "junk"})
        require_status_ok(first, "wrong-key fetch")
        assert count_matching_lines(first.text, r"[.]pagespeed[.]") < 1, (
            f"a wrong key produced a blocking rewrite: {first.text[:400]!r}"
        )
        vhost.fetch_until(
            self.URL,
            condition=lambda r: count_matching_lines(r.text, r"[.]pagespeed[.]") == 1,
            timeout=100.0,
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
