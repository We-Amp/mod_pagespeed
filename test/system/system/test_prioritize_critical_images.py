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


# User agents the module's own device classification maps to a phone, a
# desktop browser and a tablet (they are in the phone, desktop and tablet
# lists of its user-agent matcher tests). Reports and their results are kept
# per device class.
PHONE_USER_AGENT = (
    "Mozilla/5.0 (iPhone; CPU iPhone OS 5_0_1 like Mac OS X) AppleWebKit/534.46"
    " (KHTML, like Gecko) Version/5.1 Mobile/9A405 Safari/7534.48.3"
)
DESKTOP_USER_AGENT = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_6_8) AppleWebKit/534.51.22 "
    "(KHTML, like Gecko) Version/5.1.1 Safari/534.51.22"
)
TABLET_USER_AGENT = (
    "Mozilla/5.0 (iPad; U; CPU OS 3_2 like Mac OS X; en-us) "
    "AppleWebKit/531.21.10 (KHTML, like Gecko) Version/4.0.4 "
    "Mobile/7B334b Safari/531.21.10"
)

# The beaconed image once it has been annotated. Matched as the <img>
# itself: the page prose mentions the attribute.
ANNOTATED_IMG = (
    rf'<img src="{re.escape(CRITICAL_IMAGE_SRC)}"[^>]*fetchpriority="high"'
)


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


def _fetch_instrumented(client: PageSpeedClient, url: str, user_agent: str = ""):
    """Fetch url until the critical-images beacon snippet appears.

    user_agent: fetch as this browser instead of the client's default.
    """
    response = client.fetch_until_contains(
        url,
        pattern=r"pagespeed\.CriticalImages\.Run",
        timeout=60.0,
        headers={"User-Agent": user_agent} if user_agent else None,
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


class TestReportsAreKeptPerDeviceClass:
    """A browser's critical-image report counts for its own device class.

    Which images are on the first screen depends on the screen, so the
    server keeps these reports per device class (phone, tablet, desktop): a
    report is accepted when it is posted by a browser of the class the
    instrumented response went to, and it is used for page views of that
    class only.

    The "that class only" half is checked with a tablet. Within one device
    class a server may reuse critical-image data between URLs that differ
    only in their query string, and every test here fetches the same page;
    no test reports as a tablet, so a tablet page view has no data of its
    own class to pick up, whatever ran before.
    """

    @staticmethod
    def _report_and_wait(client, url, user_agent):
        """Fetch as user_agent, report the image, wait for the annotation."""
        headers = {"User-Agent": user_agent}
        response = _fetch_instrumented(client, url, user_agent=user_agent)
        params = _extract_images_beacon_params(response.text)
        image_hash = _extract_image_url_hash(response.text, CRITICAL_IMAGE_SRC)

        path = (
            f"{params['path']}"
            f"?url={urllib.parse.quote(params['url'], safe='')}"
        )
        data = f"oh={params['hash']}&n={params['nonce']}&ci={image_hash}"
        post_response = client.post(
            path,
            data=data,
            headers={
                "Content-Type": "application/x-www-form-urlencoded",
                **headers,
            },
        )
        assert_http_status(post_response, 204)

        response = client.fetch_until_contains(
            url, pattern=ANNOTATED_IMG, timeout=60.0, headers=headers
        )
        assert_http_status(response, 200)

    @staticmethod
    def _assert_tablet_not_annotated(client, url):
        """Tablet page views have no critical-image data.

        Called after another class's view has been annotated, so that
        class's report has been stored by then.
        """
        for _ in range(3):
            page = client.get(url, headers={"User-Agent": TABLET_USER_AGENT})
            assert_http_status(page, 200)
            assert_not_contains(page, r'<img[^>]*fetchpriority="high"')

    def test_report_from_a_phone_is_used_for_phones(
        self, client: PageSpeedClient, example_root: str
    ):
        url = _instrumented_url(example_root)
        self._report_and_wait(client, url, PHONE_USER_AGENT)
        self._assert_tablet_not_annotated(client, url)

    def test_report_from_a_desktop_browser_is_not_used_for_another_class(
        self, client: PageSpeedClient, example_root: str
    ):
        url = _instrumented_url(example_root)
        self._report_and_wait(client, url, DESKTOP_USER_AGENT)
        self._assert_tablet_not_annotated(client, url)


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite --
    # vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
