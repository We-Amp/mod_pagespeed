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

"""Prioritize critical images filter system tests.

Covers the full prioritize_critical_images loop against a live server:
instrumented page -> critical-images beacon POST (`ci=<url hash>`) ->
`fetchpriority="high"` on the beaconed image. Before any beacon data
arrives the filter must be a strict no-op.

The beacon mechanics mirror test_critical_css_beacon.py in this directory;
the images beacon init snippet is `pagespeed.CriticalImages.Run(...)` rather
than `criticalCssBeaconInit(...)`, so the parameters are parsed locally here.

This test lives in the system suite (not automatic) because it requires a
server with critical-images beaconing enabled; server configurations without
a beacon endpoint (e.g. Envoy) never emit the instrumentation these tests
poll for.
"""

import random
import re
import urllib.parse

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)

# The above-the-fold image on prioritize_critical_images.html that the test
# beacons back as critical.
CRITICAL_IMAGE_SRC = "images/Puzzle.jpg"


def _instrumented_url(example_root: str) -> str:
    """Example page URL with the filter enabled and a cache-busting param.

    The random param gives each test its own property-cache entry, so tests
    don't have to wait out the rebeaconing interval of a previous test.
    """
    return (
        f"{example_root}/prioritize_critical_images.html"
        f"?PageSpeedFilters=prioritize_critical_images"
        f"&test_id={random.randint(1, 1000000000)}"
    )


def _extract_images_beacon_params(html: str) -> dict:
    """Parse the critical-images beacon init snippet.

    The injected snippet has the form:
        pagespeed.CriticalImages.Run('<path>','<url>','<options hash>',
                                     <bool>,<bool>,'<nonce>');
    """
    match = re.search(
        r"pagespeed\.CriticalImages\.Run\('([^']*)','([^']*)','([^']*)',"
        r"(?:true|false),(?:true|false),'([^']*)'\);",
        html,
    )
    assert match, "CriticalImages.Run present but parameters not parseable"
    return {
        "path": match.group(1),
        "url": match.group(2),
        "hash": match.group(3),
        "nonce": match.group(4),
    }


def _extract_image_url_hash(html: str, src: str) -> str:
    """Scrape the data-pagespeed-url-hash the beacon assigned to `src`.

    The beacon JS reports critical images by this hash (`ci=<hash>`); the
    server computed it, so scrape it rather than reimplementing the hash.
    """
    match = re.search(
        rf'<img src="{re.escape(src)}"[^>]*data-pagespeed-url-hash="(\d+)"',
        html,
    )
    assert match, f"no data-pagespeed-url-hash found for {src}"
    return match.group(1)


def _fetch_instrumented(client: PageSpeedClient, url: str):
    """Fetch url until the critical-images beacon snippet appears."""
    response = client.fetch_until_contains(
        url,
        pattern=r"pagespeed\.CriticalImages\.Run",
        timeout=60.0,
    )
    assert_http_status(response, 200)
    return response


class TestPrioritizeCriticalImages:
    """End-to-end fetchpriority annotation from a beacon response.

    Modeled on the legacy critical-image beaconing flow
    (pagespeed/system/system_tests/critical_image_beaconing.sh) and the
    critical CSS beacon round-trip test.
    """

    def test_no_op_before_beacon_data(
        self, client: PageSpeedClient, example_root: str
    ):
        """Without beacon data the filter must not annotate anything.

        Anchored on <img>: the example page's explanatory prose mentions
        the literal attribute, so a bare string match would always trip.
        """
        response = _fetch_instrumented(client, _instrumented_url(example_root))
        assert_not_contains(response, r'<img[^>]*fetchpriority="high"')

    def test_beacon_response_sets_fetchpriority(
        self, client: PageSpeedClient, example_root: str
    ):
        """Beacon an image as critical; it must get fetchpriority="high"."""
        url = _instrumented_url(example_root)
        response = _fetch_instrumented(client, url)
        params = _extract_images_beacon_params(response.text)
        image_hash = _extract_image_url_hash(response.text, CRITICAL_IMAGE_SRC)

        # POST beacon data the way the client JS does (url= in the query).
        path = (
            f"{params['path']}"
            f"?url={urllib.parse.quote(params['url'], safe='')}"
        )
        data = f"oh={params['hash']}&n={params['nonce']}&ci={image_hash}"
        post_response = client.post(
            path,
            data=data,
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        assert_http_status(post_response, 204)

        # Once the beacon data lands in the property cache, the beaconed
        # image is annotated with fetchpriority="high". Poll for the
        # annotated <img> itself: the page prose mentions the attribute, so
        # a bare string pattern would match before any rewrite happened.
        annotated_img = (
            rf'<img src="{re.escape(CRITICAL_IMAGE_SRC)}"[^>]*'
            rf'fetchpriority="high"'
        )
        response = client.fetch_until_contains(
            url,
            pattern=annotated_img,
            timeout=60.0,
        )
        assert_http_status(response, 200)
        assert_contains(response, annotated_img)


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite -- vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
