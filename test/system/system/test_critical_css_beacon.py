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
    assert_not_contains,
    assert_stat_delta,
)
from pagespeed_test_framework.stats import extract_beacon_params

# Selectors present as candidates on prioritize_critical_css.html; the
# same set the legacy bash test beacons back.
CRITICAL_SELECTORS = ".big,.blue,.bold,.foo"

# The critical selector beaconed back for the cascade-layers page; it lives
# inside an @layer block in styles/layers.css.
LAYER_CRITICAL_SELECTORS = ".layer-critical"


def _instrumented_url(
    example_root: str, page: str = "prioritize_critical_css.html"
) -> str:
    """Example page URL with the filter enabled and a cache-busting param.

    The random param gives each test its own property-cache entry, so tests
    don't have to wait out the rebeaconing interval of a previous test.
    """
    return (
        f"{example_root}/{page}"
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
        # The all_using_imports.css stylesheet also declares an @font-face,
        # which the critical-CSS filter always retains and the serializer
        # emits ahead of the critical rulesets.
        assert_contains(
            response,
            r"<style>@font-face\{[^}]*\}\.blue\{[^}]*\}\.bold\{[^}]*\}</style>",
        )


class TestPrioritizeCriticalCssLayers:
    """Cascade-layer sheets: beacon arming plus a layer-aware inline subset.

    styles/layers.css keeps every rule inside @layer blocks (the shape of
    frameworks that wrap their entire output in cascade layers). The page
    must still arm the beacon — with an empty candidate set the server
    answers kDoNotBeacon and never instruments — and after a beacon response
    the inline subset keeps survivors inside their @layer wrappers while
    emptied block-form @layer declarations stay behind, so first-occurrence
    layer order matches the deferred full copy.
    """

    def test_pure_layer_page_arms_and_inlines_layered_subset(
        self, client: PageSpeedClient, example_root: str
    ):
        url = _instrumented_url(
            example_root, page="prioritize_critical_css_layers.html"
        )
        # Arming is itself load-bearing: the candidates can only come from
        # inside the @layer bodies on this page.
        params = _fetch_beacon_params(client, url)

        data = (
            f"oh={params['hash']}&n={params['nonce']}"
            f"&cs={LAYER_CRITICAL_SELECTORS}"
        )
        response = _post_beacon(client, params, data)
        assert_http_status(response, 204)

        response = client.fetch_until_count(
            url,
            pattern=r"@layer base\{\.layer-critical\{[^}]*\}\}",
            expected_count=1,
            timeout=60.0,
        )
        assert_http_status(response, 200)
        # The emptied non-critical layer survives as a bare declaration;
        # dropping it would flip the inline subset's layer order relative to
        # the full stylesheet loaded afterwards.
        assert_contains(response, r"@layer components\{\}")
        # The statement-form declaration rides along verbatim.
        assert_contains(response, r"@layer base, components;")
        # The non-critical rule lives only in the deferred full copy (the
        # cloned <link>), never in an inline <style>.
        assert_not_contains(response, r"\.layer-noncritical\{")


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite -- vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
