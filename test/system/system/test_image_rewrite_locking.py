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

"""A burst of requests for one image rewrites it once, at most twice.

Ported from: pagespeed/system/system_tests/image_rewrite_locking.sh

A bug once made the server ignore a failed rewrite lock and rewrite the
same image for many concurrent requests; the lock is best effort, so the
bash suite accepts one or two rewrites for a burst of twenty.
"""

import random
import time

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.stats import settled_stats


@pytest.mark.requires_stats
class TestImageRewriteLocking:
    """Bash: A burst of image requests should yield only one two rewrites."""

    def test_burst_of_image_requests_rewrites_once_or_twice(
        self, client: PageSpeedClient, example_root: str, stats_snapshot
    ):
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 10 ** 9)}"
        start = settled_stats(stats_snapshot, ["image_rewrites"])
        for _ in range(20):
            require_status_ok(client.get(url), url)
        deadline = time.monotonic() + 30.0
        while stats_snapshot().get("image_rewrites", 0) == start.get("image_rewrites", 0):
            if time.monotonic() > deadline:
                pytest.fail(f"{url}: image_rewrites never moved after the burst")
            time.sleep(0.5)
        end = settled_stats(stats_snapshot, ["image_rewrites"])
        delta = end.get("image_rewrites", 0) - start.get("image_rewrites", 0)
        assert delta in (1, 2), f"a burst of 20 requests rewrote the image {delta} times"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
