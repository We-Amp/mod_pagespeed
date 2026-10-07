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

"""In-place rewrite deadline (InPlaceRewriteDeadlineMs).

Ported from: pagespeed/system/system_tests/rewrite_deadline_ipro.sh

With InPlaceWaitForOptimized on, the first request for a resource is always
served unoptimized (it records the resource); later requests wait up to the
deadline for the optimization. A long deadline makes the second response
optimized, a 1 ms deadline makes it unoptimized until the rewrite finishes.
Each case uses a fresh URL and runs twice so an earlier fetch in the same
lane session cannot pre-warm it.

The tests go through secondary_client, the lane's default secondary vhost
with its own cache, where the bash used the primary, to keep the cache
state isolated from the rest of the lane.
"""

import uuid

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok

ORIGINAL_PUZZLE_THRESHOLD = 100000


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestInPlaceRewriteDeadline:

    @pytest.mark.parametrize("attempt", [1, 2])
    def test_two_pass_ipro_with_long_deadline(
        self, secondary_client: PageSpeedClient, test_root: str, attempt: int
    ):
        """Bash: 2-pass ipro with long ModPagespeedInPlaceRewriteDeadline.

        With this lane's small purple.css, the rewrite completes within the
        default deadline too, so this case documents the contract rather
        than discriminating the long-deadline override -- inherited from
        the bash's choice of asset.
        """
        path = f"{test_root}/ipro/wait/long/purple.css?cachebust={uuid.uuid4().hex}"
        first = secondary_client.get(path)
        require_status_ok(first, "first pass")
        assert "background: MediumPurple;" in first.text, (
            f"first pass: the fetch must occur first regardless of the deadline; "
            f"got {first.text[:200]!r}"
        )
        second = secondary_client.get(path)
        require_status_ok(second, "second pass")
        assert "body{background:#9370db}" in second.text, (
            f"second pass: a long deadline and an easy optimization should give "
            f"an optimized result; got {second.text[:200]!r}"
        )

    @pytest.mark.parametrize("attempt", [1, 2])
    def test_three_pass_ipro_with_short_deadline(
        self, secondary_client: PageSpeedClient, test_root: str, attempt: int
    ):
        """Bash: 3-pass ipro with short ModPagespeedInPlaceRewriteDeadline."""
        path = f"{test_root}/ipro/wait/short/Puzzle.jpg?cachebust={uuid.uuid4().hex}"
        first = secondary_client.get(path)
        require_status_ok(first, "first pass")
        assert len(first.body) > ORIGINAL_PUZZLE_THRESHOLD, (
            f"first pass: expected the unoptimized image, got {len(first.body)} bytes"
        )
        second = secondary_client.get(path)
        require_status_ok(second, "second pass")
        assert len(second.body) > ORIGINAL_PUZZLE_THRESHOLD, (
            f"second pass: a 1 ms deadline should miss the image optimization, "
            f"got {len(second.body)} bytes"
        )
        # A byte-count condition alone would also be met by a small error
        # page, so the poll requires a 200 as well.
        optimized = secondary_client.fetch_until(
            path,
            lambda r: r.status == 200 and len(r.body) < ORIGINAL_PUZZLE_THRESHOLD,
            detail_fn=lambda r: (
                f"status={r.status} bytes={len(r.body)} expected<{ORIGINAL_PUZZLE_THRESHOLD}"
            ),
        )
        require_status_ok(optimized, "third pass")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
