#!/usr/bin/env python3
# Copyright 2026 We-Amp B.V.
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

"""Authorized content is served but never cached or optimized.

Ported from: pagespeed/system/system_tests/authorization_basic.sh

mod_pagespeed_test/auth/ requires HTTP Basic auth (user1); the lane config
carries the bash suite's <Directory> block.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_stat_delta,
    require_status_ok,
)
from pagespeed_test_framework.stats import settled_stats

_AUTH = {"Authorization": "Basic dXNlcjE6cGFzc3dvcmQ="}


@pytest.mark.requires_stats
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestAuthorizationBasic:

    def test_authorized_resource_not_cached(
        self, client: PageSpeedClient, test_root: str, stats_snapshot
    ):
        """start_test authorized resources do not get cached and optimized."""
        before = settled_stats(stats_snapshot, ["ipro_recorder_not_cacheable"])
        response = client.get(f"{test_root}/auth/medium_purple.css", headers=_AUTH)
        require_status_ok(response, "authorized auth/medium_purple.css")
        assert_contains(response, "background: MediumPurple;")
        after = stats_snapshot()
        assert_stat_delta(before, after, "ipro_recorder_not_cacheable", 1)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
