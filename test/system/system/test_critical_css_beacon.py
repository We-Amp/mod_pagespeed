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

"""Critical CSS beacon system tests.

Ported from: pagespeed/automatic/system_test_helpers.sh
             (test_prioritize_critical_css / test_prioritize_critical_css_final)

Covers the full prioritize_critical_css loop against a live server:
instrumented page -> beacon POST -> critical rules inlined; plus the
beacon overflow signal (`of=1`) added in the same change, which must increment
the beacon_overflow_count statistic (and only for the literal value "1").

The client-side halves of this pipeline (viewport-aware selector
criticality, payload truncation + rotation) run in a real browser and are
covered by test/browser/critical_css_beacon_test.mjs.
"""

import random
import urllib.parse

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    assert_stat_delta,
)
from pagespeed_test_framework.stats import extract_beacon_params

# Selectors present as candidates on prioritize_critical_css.html; the
# same set the legacy bash test beacons back.
CRITICAL_SELECTORS = ".big,.blue,.bold,.foo"


def _instrumented_url(example_root: str) -> str:
    """Example page URL with the filter enabled and a cache-busting param.

    The random param gives each test its own property-cache entry, so tests
    don't have to wait out the rebeaconing interval of a previous test.
    """
    return (
        f"{example_root}/prioritize_critical_css.html"
        f"?PageSpeedFilters=prioritize_critical_css"
        f"&test_id={random.randint(1, 1000000000)}"
    )


def _fetch_beacon_params(client: PageSpeedClient, url: str) -> dict:
    """Fetch url until the criticalCssBeaconInit snippet appears; parse it."""
    response = client.fetch_until_contains(
        url,
        pattern=r"criticalCssBeaconInit",
        timeout=60.0,
    )
    assert_http_status(response, 200)
    params = extract_beacon_params(response.text)
    assert params, "criticalCssBeaconInit present but parameters not parseable"
    return params


def _post_beacon(client: PageSpeedClient, params: dict, data: str):
    """POST beacon data the way the client JS does (url= in the query)."""
    path = f"{params['path']}?url={urllib.parse.quote(params['url'], safe='')}"
    return client.post(
        path,
        data=data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )


class TestBeaconOverflowStat:
    """The `of=1` truncation signal must be visible in server statistics.

    The beacon JS appends `&of=1` when the selector payload was truncated
    to fit MAX_POST_SIZE; the server counts these so operators can see
    pages whose candidate selector set exceeds the beacon budget.
    """

    @pytest.mark.requires_stats
    def test_overflow_flag_increments_counter(
        self, client: PageSpeedClient, example_root: str, stats_snapshot
    ):
        params = _fetch_beacon_params(client, _instrumented_url(example_root))

        stats_before = stats_snapshot()
        data = f"oh={params['hash']}&n={params['nonce']}&cs={CRITICAL_SELECTORS}&of=1"
        response = _post_beacon(client, params, data)
        assert_http_status(response, 204)
        stats_after = stats_snapshot()

        assert_stat_delta(stats_before, stats_after, "beacon_overflow_count", 1)

    @pytest.mark.requires_stats
    def test_no_overflow_flag_does_not_increment(
        self, client: PageSpeedClient, example_root: str, stats_snapshot
    ):
        params = _fetch_beacon_params(client, _instrumented_url(example_root))

        stats_before = stats_snapshot()
        data = f"oh={params['hash']}&n={params['nonce']}&cs={CRITICAL_SELECTORS}"
        response = _post_beacon(client, params, data)
        assert_http_status(response, 204)
        stats_after = stats_snapshot()

        assert_stat_delta(stats_before, stats_after, "beacon_overflow_count", 0)

    @pytest.mark.requires_stats
    def test_overflow_flag_zero_does_not_increment(
        self, client: PageSpeedClient, example_root: str, stats_snapshot
    ):
        """Only the literal `of=1` counts as an overflow report."""
        params = _fetch_beacon_params(client, _instrumented_url(example_root))

        stats_before = stats_snapshot()
        data = f"oh={params['hash']}&n={params['nonce']}&cs={CRITICAL_SELECTORS}&of=0"
        response = _post_beacon(client, params, data)
        assert_http_status(response, 204)
        stats_after = stats_snapshot()

        assert_stat_delta(stats_before, stats_after, "beacon_overflow_count", 0)


class TestPrioritizeCriticalCss:
    """End-to-end critical CSS computation from a beacon response.

    Bash original (system_test_helpers.sh):
        BEACON_DATA="oh=${OPTIONS_HASH}&n=${NONCE}&cs=.big,.blue,.bold,.foo"
        OUT=$($CURL -sSi -d "$BEACON_DATA" "$BEACON_URL")
        check_from "$OUT" grep '^HTTP/1.1 204'
        fetch_until $URL 'grep -c <style>[.]blue{[^}]*}</style>' 1
        ...
    """

    def test_beacon_response_inlines_critical_selectors(
        self, client: PageSpeedClient, example_root: str
    ):
        url = _instrumented_url(example_root)
        params = _fetch_beacon_params(client, url)

        data = f"oh={params['hash']}&n={params['nonce']}&cs={CRITICAL_SELECTORS}"
        response = _post_beacon(client, params, data)
        assert_http_status(response, 204)

        # Once the beacon data lands in the property cache, the critical
        # rules for the beaconed selectors are inlined as <style> blocks.
        response = client.fetch_until_count(
            url,
            pattern=r"<style>\.foo\{[^}]*\}</style>",
            expected_count=1,
            timeout=60.0,
        )
        assert_http_status(response, 200)
        assert_contains(response, r"<style>\.blue\{[^}]*\}</style>")
        assert_contains(response, r"<style>\.big\{[^}]*\}</style>")
        assert_contains(response, r"<style>\.blue\{[^}]*\}\.bold\{[^}]*\}</style>")
