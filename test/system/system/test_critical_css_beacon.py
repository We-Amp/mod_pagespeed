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
beacon overflow signal (`of=1`), which must increment
the beacon_overflow_count statistic (and only for the literal value "1").

The client-side halves of this pipeline (viewport-aware selector
criticality, payload truncation + rotation) run in a real browser and are
covered by test/browser/critical_css_beacon_test.mjs.
"""

import random
import re
import urllib.parse

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    assert_not_contains,
    assert_stat_delta,
)
from pagespeed_test_framework.stats import extract_beacon_params, settled_stats

# Selectors present as candidates on prioritize_critical_css.html; the
# same set the legacy bash test beacons back.
CRITICAL_SELECTORS = ".big,.blue,.bold,.foo"

# On the example page the stylesheet that imports others; its rewritten URL
# shows once the imported files have been merged into it.
EXAMPLE_REWRITTEN = r"all_using_imports\.css\.pagespeed\.cf\."

# The critical selector beaconed back for the cascade-layers page; it lives
# inside an @layer block in styles/layers.css.
LAYER_CRITICAL_SELECTORS = ".layer-critical"

# A deferred stylesheet: the preload link that stands where the stylesheet
# link stood. The link is matched as an element, because the attribute name
# on its own also occurs in the script that turns these links on.
DEFERRED_LINK = r'<link rel="preload"[^>]* as="style" data-pagespeed-deferred-css>'


# User agents the module's own device classification maps to a phone, a
# tablet and a desktop browser (they are in the phone, tablet and desktop
# lists of its user-agent matcher tests). Reports and their results are kept
# per device class.
PHONE_USER_AGENT = (
    "Mozilla/5.0 (iPhone; CPU iPhone OS 5_0_1 like Mac OS X) AppleWebKit/534.46"
    " (KHTML, like Gecko) Version/5.1 Mobile/9A405 Safari/7534.48.3"
)
TABLET_USER_AGENT = (
    "Mozilla/5.0 (iPad; U; CPU OS 3_2 like Mac OS X; en-us) "
    "AppleWebKit/531.21.10 (KHTML, like Gecko) Version/4.0.4 "
    "Mobile/7B334b Safari/531.21.10"
)
DESKTOP_USER_AGENT = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_6_8) AppleWebKit/534.51.22 "
    "(KHTML, like Gecko) Version/5.1.1 Safari/534.51.22"
)


def _as(user_agent: str):
    """Request headers for a browser with this user agent (None: default)."""
    return {"User-Agent": user_agent} if user_agent else None


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


def _fetch_beacon_params(
    client: PageSpeedClient,
    url: str,
    rewritten: str = "",
    skip_nonce: str = "",
    user_agent: str = "",
) -> dict:
    """Fetch url until the criticalCssBeaconInit snippet appears; parse it.

    rewritten: a regex for the rewritten URL of a stylesheet that imports
    another. On a cold cache a response can be instrumented while that
    stylesheet's rewrite is still running; its selectors then do not yet
    include the imported file's. A report sent for that response stops
    counting when the imported selectors arrive, so wait until the rewritten
    URL shows before taking the nonce.

    skip_nonce: wait for a response whose nonce differs from this one (the
    page is instrumented again a few seconds after a report).

    user_agent: fetch as this browser instead of the client's default.
    """

    def ready(r) -> bool:
        params = extract_beacon_params(r.text)
        if not params or params["nonce"] == skip_nonce:
            return False
        return not rewritten or re.search(rewritten, r.text) is not None

    response = client.fetch_until(
        url,
        ready,
        timeout=60.0,
        headers=_as(user_agent),
        detail_fn=lambda r: (
            f"beacon init present: {'criticalCssBeaconInit' in r.text}; "
            f"rewritten stylesheet URL present: "
            f"{not rewritten or re.search(rewritten, r.text) is not None}"
        ),
    )
    assert_http_status(response, 200)
    params = extract_beacon_params(response.text)
    assert params, "criticalCssBeaconInit present but parameters not parseable"
    return params


def _post_beacon(
    client: PageSpeedClient, params: dict, data: str, user_agent: str = ""
):
    """POST beacon data the way the client JS does (url= in the query).

    user_agent: post as this browser instead of the client's default.
    """
    path = f"{params['path']}?url={urllib.parse.quote(params['url'], safe='')}"
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    headers.update(_as(user_agent) or {})
    return client.post(path, data=data, headers=headers)


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

        stats_before = settled_stats(stats_snapshot, ["beacon_overflow_count"])
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

        stats_before = settled_stats(stats_snapshot, ["beacon_overflow_count"])
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

        stats_before = settled_stats(stats_snapshot, ["beacon_overflow_count"])
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
        params = _fetch_beacon_params(client, url, rewritten=EXAMPLE_REWRITTEN)

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
        # Each full stylesheet is preloaded where its link stood and marked
        # for the script that turns it on, followed by a copy of the link
        # for browsers without scripts. Nothing is parked at the end of the
        # body any more.
        assert_contains(
            response, DEFERRED_LINK + r'<noscript><link rel="stylesheet"'
        )
        assert_contains(response, r"pagespeed\.CriticalCssLoader\.Run\(\);")
        assert_not_contains(response, r"psa_add_styles")

    def test_truncated_report_returns_the_page_to_blocking_stylesheets(
        self, client: PageSpeedClient, example_root: str
    ):
        """A report that was cut short is not taken for a complete one.

        A complete report first: the page is served with deferred
        stylesheets. Then a report flagged as cut short (of=1), sent for a
        later instrumented response: the selectors that did not fit would
        look unmatched, so the page goes back to its ordinary stylesheet
        links until a complete report arrives.
        """
        url = _instrumented_url(example_root)
        params = _fetch_beacon_params(client, url, rewritten=EXAMPLE_REWRITTEN)
        data = f"oh={params['hash']}&n={params['nonce']}&cs={CRITICAL_SELECTORS}"
        assert_http_status(_post_beacon(client, params, data), 204)

        deferred = re.compile(DEFERRED_LINK)
        client.fetch_until(
            url,
            lambda r: deferred.search(r.text) is not None,
            timeout=60.0,
            detail_fn=lambda r: (
                f"deferred stylesheets: {len(deferred.findall(r.text))}"
            ),
        )

        # The page is instrumented again a few seconds after a report.
        again = _fetch_beacon_params(
            client, url, rewritten=EXAMPLE_REWRITTEN, skip_nonce=params["nonce"]
        )
        data = f"oh={again['hash']}&n={again['nonce']}&cs=.foo&of=1"
        assert_http_status(_post_beacon(client, again, data), 204)

        client.fetch_until(
            url,
            lambda r: deferred.search(r.text) is None,
            timeout=60.0,
            detail_fn=lambda r: (
                f"deferred stylesheets: {len(deferred.findall(r.text))}"
            ),
        )
        # It stays that way: no later response is served from the cut report.
        for _ in range(3):
            page = client.get(url)
            assert_http_status(page, 200)
            assert_not_contains(page, DEFERRED_LINK)
            assert_not_contains(page, r'<noscript><link rel="stylesheet"')
            assert_contains(page, r'<link rel="stylesheet"')


class TestReportsAreKeptPerDeviceClass:
    """A browser's report counts for its own device class, and only for it.

    The server keeps what browsers report per device class: phone, tablet and
    desktop. The nonce handed out with an instrumented response belongs to
    the class of the browser that response went to, and a report is accepted
    only when it is posted by a browser of that same class. So phones are
    served from what phones reported, and a report from one class neither
    optimizes nor disturbs the pages served to another.
    """

    @staticmethod
    def _report(client, url, user_agent, post_user_agent=""):
        """Fetch as user_agent, then report as post_user_agent (default: same)."""
        params = _fetch_beacon_params(
            client, url, rewritten=EXAMPLE_REWRITTEN, user_agent=user_agent
        )
        data = f"oh={params['hash']}&n={params['nonce']}&cs={CRITICAL_SELECTORS}"
        response = _post_beacon(
            client, params, data, user_agent=post_user_agent or user_agent
        )
        assert_http_status(response, 204)
        return params, data

    @staticmethod
    def _wait_until_optimized(client, url, user_agent):
        deferred = re.compile(DEFERRED_LINK)
        return client.fetch_until(
            url,
            lambda r: deferred.search(r.text) is not None,
            timeout=60.0,
            headers=_as(user_agent),
            detail_fn=lambda r: (
                f"deferred stylesheets: {len(deferred.findall(r.text))}"
            ),
        )

    @staticmethod
    def _assert_not_optimized(client, url, user_agent):
        """Page views of this class still get their ordinary stylesheets.

        Called after another class's view has turned optimized, so the
        report has been stored by then; a few views, so a result that is
        about to show would be seen.
        """
        for _ in range(3):
            page = client.get(url, headers=_as(user_agent))
            assert_http_status(page, 200)
            assert_not_contains(page, DEFERRED_LINK)
            assert_contains(page, r'<link rel="stylesheet"')

    @pytest.mark.parametrize(
        "user_agent",
        [
            pytest.param(PHONE_USER_AGENT, id="phone"),
            pytest.param(TABLET_USER_AGENT, id="tablet"),
        ],
    )
    def test_report_from_a_phone_or_tablet_optimizes_that_class_only(
        self, client: PageSpeedClient, example_root: str, user_agent: str
    ):
        url = _instrumented_url(example_root)
        self._report(client, url, user_agent)

        page = self._wait_until_optimized(client, url, user_agent)
        assert_contains(page, r"<style>\.foo\{[^}]*\}</style>")
        # No desktop browser has reported for this page yet.
        self._assert_not_optimized(client, url, DESKTOP_USER_AGENT)

    def test_report_from_a_desktop_browser_optimizes_desktop_only(
        self, client: PageSpeedClient, example_root: str
    ):
        url = _instrumented_url(example_root)
        self._report(client, url, DESKTOP_USER_AGENT)

        page = self._wait_until_optimized(client, url, DESKTOP_USER_AGENT)
        assert_contains(page, r"<style>\.foo\{[^}]*\}</style>")
        # No phone has reported for this page yet: phones are not served
        # from what a desktop browser reported.
        self._assert_not_optimized(client, url, PHONE_USER_AGENT)

    def test_nonce_from_one_class_is_rejected_when_posted_by_another(
        self, client: PageSpeedClient, example_root: str
    ):
        """A phone's nonce posted by a desktop browser changes nothing.

        The rejected post is answered 204 like any other, so the rejection
        is shown by its effects: the nonce is still good for the phone that
        was given it, and desktop page views stay un-optimized after the
        phone's own report has been stored.
        """
        url = _instrumented_url(example_root)
        params, data = self._report(
            client, url, PHONE_USER_AGENT, post_user_agent=DESKTOP_USER_AGENT
        )

        # The same nonce, now posted by the class it was handed to.
        response = _post_beacon(client, params, data, user_agent=PHONE_USER_AGENT)
        assert_http_status(response, 204)
        self._wait_until_optimized(client, url, PHONE_USER_AGENT)

        self._assert_not_optimized(client, url, DESKTOP_USER_AGENT)


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
        # The non-critical rule is only in the full stylesheet, which is
        # preloaded in place; it is never in an inline <style>.
        assert_not_contains(response, r"\.layer-noncritical\{")
        assert_contains(response, DEFERRED_LINK)


# Every selector of the test page's stylesheets that matches something in
# the page: what a browser reports under the default selection.
NO_FLASH_SELECTORS = [
    "body",
    ".ncf-title",
    ".ncf-lede",
    ".ncf-spacer",
    ".ncf-list",
    ".ncf-list li",
    ".ncf-card",
    ".ncf-card h2",
    ".ncf-card p",
    ".ncf-imported",
]


class TestStyledBelowTheFirstScreen:
    """Rules for content far below the first screen are in the inline CSS.

    critical_css_no_flash/ has three stylesheets (one imports a fourth file)
    and, 1,600 px down, a list, cards and a paragraph they style. After a
    browser has reported every matching selector, all three stylesheets are
    deferred in place and the rules for the content below the first screen
    are inline, including the ones that came from the imported file.
    """

    def test_rules_for_content_below_the_first_screen_are_inline(
        self, client: PageSpeedClient, test_root: str
    ):
        url = (
            f"{test_root}/critical_css_no_flash/index.html"
            f"?PageSpeedFilters=prioritize_critical_css"
            f"&test_id={random.randint(1, 1000000000)}"
        )
        # layout.css imports imported.css: take the nonce only once the
        # import has been merged in, so the report covers its selectors.
        params = _fetch_beacon_params(
            client, url, rewritten=r"layout\.css\.pagespeed\.cf\."
        )

        selectors = ",".join(
            urllib.parse.quote(s, safe="") for s in NO_FLASH_SELECTORS
        )
        data = f"oh={params['hash']}&n={params['nonce']}&cs={selectors}"
        response = _post_beacon(client, params, data)
        assert_http_status(response, 204)

        # Three stylesheet links, three preloads in their places.
        response = client.fetch_until_count(
            url,
            pattern=DEFERRED_LINK,
            expected_count=3,
            timeout=60.0,
        )
        assert_http_status(response, 200)
        assert_contains(response, r"\.ncf-card\{[^}]*padding:24px")
        assert_contains(response, r"\.ncf-list\{[^}]*list-style-type:none")
        assert_contains(response, r"\.ncf-imported\{[^}]*margin-top:48px")
        # Rules that match nothing in the page stay out of the inline CSS
        # (the brace keeps this from matching the list of selectors an
        # instrumented response carries).
        assert_not_contains(response, r"\.ncf-unused-[a-z]+\{")
        # Nothing is parked at the end of the body.
        assert_not_contains(response, r"psa_add_styles")


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite --
    # vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
