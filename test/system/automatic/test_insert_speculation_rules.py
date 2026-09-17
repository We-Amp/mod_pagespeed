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

"""Insert speculation rules filter tests.

These tests verify that the insert_speculation_rules filter injects exactly
one <script type="speculationrules"> ruleset at the end of the body.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)

SCRIPT_TAG_PATTERN = r'<script type=["\']?speculationrules["\']?>'


class TestInsertSpeculationRules:
    """Tests for the insert_speculation_rules filter."""

    def test_injects_ruleset_exactly_once(
        self, client: PageSpeedClient, example_root: str
    ):
        """The speculation-rules script should be injected exactly once."""
        url = (
            f"{example_root}/insert_speculation_rules.html"
            "?PageSpeedFilters=insert_speculation_rules"
        )

        response = client.fetch_until_count(
            url,
            pattern=SCRIPT_TAG_PATTERN,
            expected_count=1,
            timeout=30.0,
            case_insensitive=True,
        )
        assert_http_status(response, 200)

    def test_ruleset_prefetches_same_origin(
        self, client: PageSpeedClient, example_root: str
    ):
        """The injected ruleset should be the same-origin prefetch ruleset."""
        url = (
            f"{example_root}/insert_speculation_rules.html"
            "?PageSpeedFilters=insert_speculation_rules"
        )

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_contains(
            response,
            SCRIPT_TAG_PATTERN,
            "Speculation-rules script should be injected",
        )
        assert_contains(
            response,
            r'"href_matches":"/\*"',
            "Ruleset should prefetch same-origin URLs",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
