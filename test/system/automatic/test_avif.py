#!/usr/bin/env python3
# Copyright 2024 Google LLC
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

"""AVIF image optimization tests.

End-to-end proof that the AVIF encode path is reachable through a real server:
the filter resolves from its config name, capability detection reads the Accept
header, and the encoder actually runs and emits AVIF bytes.

Two groups of cases:

* TestAvifContentNegotiation -- three cases asserting mutually exclusive output
  formats (AVIF, WebP, JPEG) driven purely by the Accept header, so a setup bug
  that collapses capability detection (say, an always-true SupportsAvif) cannot
  satisfy all three at once.
* TestAvifPickSmaller -- the case where BOTH encoders run and the rewriter has
  to choose between two real candidates on size.
"""

import re
from urllib.parse import urljoin

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
    assert_image_format,
    assert_stat_increased,
    assert_stat_unchanged,
)

# A one-image page pointing at a large lossy photographic JPEG. Size matters:
# the AVIF candidate is adopted via pick-smaller against the WebP/recompressed
# JPEG candidate, so the source has to be one AVIF actually wins on.
_HTML_PAGE = "optimize_for_bandwidth/avif_urls/rewrite_avif.html"

# The rewritten <img src>, whatever extension the rewriter settled on. Matching
# any extension is deliberate: the extension is one of the things under test,
# so the convergence condition must not pre-judge it.
_REWRITTEN_SRC = re.compile(r'src="([^"]*\.pagespeed\.ic\.[^"]*)"')

# Accept headers. Capability detection does EXACT per-token matching on the
# comma-split Accept header, so every token must be bare: "image/avif;q=0.8"
# would NOT register as AVIF support -- which is why none of these carry a
# q-value, even though a real browser's Accept header usually would.
#
# The first three name at most one of the two modern formats, keeping the
# content-negotiation cases mutually exclusive. _ACCEPT_AVIF_AND_WEBP names
# both, which is the only way to get both encoders to run on one request.
_ACCEPT_AVIF = "text/html, image/avif"
_ACCEPT_WEBP = "text/html, image/webp"
_ACCEPT_LEGACY = "text/html"
_ACCEPT_AVIF_AND_WEBP = "text/html, image/avif, image/webp"

# Filter sets. The WebP and legacy cases enable BOTH conversion filters, so
# capability detection -- not filter configuration -- is the only thing that can
# suppress AVIF there; asserting "no AVIF rewrite" with convert_jpeg_to_avif
# switched off would be vacuously true. Neither case is ambiguous: the Accept
# header names at most one modern format, so at most one encoder is ever set up
# and per-image pick-smaller never gets a say.
_FILTERS_AVIF = "recompress_jpeg,convert_jpeg_to_avif"
_FILTERS_BOTH = "recompress_jpeg,convert_jpeg_to_avif,convert_jpeg_to_webp"

_AVIF_REWRITES = "image_avif_rewrites"
# Server-side AVIF encode budget (AvifTimeoutMs, 5000ms by default) is NOT
# scaled by PAGESPEED_TEST_TIMEOUT_MULTIPLIER. For a STILL image the budget is
# enforced as an up-front admission test (estimated cost from pixel count and
# speed), not as a frame-boundary abort, so this counter ticks when an encode
# was REFUSED before it started. Puzzle.jpg is ~0.78 Mpx, estimated ~392ms at
# the default speed 6, so it should be admitted with a wide margin -- a nonzero
# delta here means the estimate or the budget is misconfigured, which from out
# here is indistinguishable from a broken AVIF path. Report it on failure.
_AVIF_JPEG_TIMEOUTS = "image_avif_conversion_jpeg_timeouts"


def _fetch_rewritten_image(
    client: PageSpeedClient,
    test_root: str,
    filters: str,
    accept: str,
):
    """Rewrite the one-image page under `filters`/`accept`, return the image.

    Two steps, both carrying the same Accept header: poll the HTML until the
    <img src> has been replaced by a .pagespeed.ic. URL, then fetch that URL.
    The image response body is what the format assertions run on, so they read
    real encoder output rather than a URL extension or a mime-map guess.

    The counter assertions (not the format assertions) rely on a cold rewrite
    cache, which is what the lanes give us: the Apache setup clears the cache
    before every run and the nginx/IIS lanes run in a fresh container. The
    flush_cache fixture is deliberately not used -- it needs a writable
    PAGESPEED_CACHE_DIR that the Apache lane does not export, so it would
    silently turn these tests into skips.

    The three cases cannot alias each other in cache: the request's AVIF/WebP
    capability level is encoded into the rewritten resource URL and rides in
    the ResourceContext that keys the rewrite, so a differing Accept header is
    already a differing cache entry regardless of the filter set.
    """
    html_url = f"{test_root}/{_HTML_PAGE}?PageSpeedFilters={filters}"
    html = client.fetch_until(
        html_url,
        condition=lambda r: _REWRITTEN_SRC.search(r.text) is not None,
        timeout=60.0,
        headers={"Accept": accept},
        detail_fn=lambda r: f"no rewritten src in {len(r.text)} bytes of HTML",
    )
    assert_http_status(html, 200)

    src = _REWRITTEN_SRC.search(html.text).group(1)
    image_url = urljoin(html_url, src)
    image = client.get(image_url, headers={"Accept": accept})
    assert_http_status(image, 200)
    return image


@pytest.mark.requires_stats
class TestAvifContentNegotiation:
    """AVIF is served to, and only to, clients that advertise Accept: image/avif.

    AVIF capability is Accept-only -- no User-Agent matching is involved -- so
    the Accept header is the single input that has to decide these three.
    """

    def test_avif_accept_yields_avif(
        self, client: PageSpeedClient, test_root: str, stats_snapshot
    ):
        """Accept: image/avif with the AVIF filter on produces AVIF bytes."""
        old_stats = stats_snapshot()
        response = _fetch_rewritten_image(
            client, test_root, _FILTERS_AVIF, _ACCEPT_AVIF
        )
        new_stats = stats_snapshot()

        # Primary: server-side proof that the AVIF encode path actually ran,
        # independent of mime config and body parsing.
        timeouts = new_stats.get(_AVIF_JPEG_TIMEOUTS, 0) - old_stats.get(
            _AVIF_JPEG_TIMEOUTS, 0
        )
        assert_stat_increased(
            old_stats, new_stats, _AVIF_REWRITES, 1,
            "AVIF-capable request should drive an AVIF rewrite "
            f"(jpeg->avif encode timeouts in this window: {timeouts})",
        )
        # Corroborating: the bytes really are AVIF.
        assert_image_format(
            response, "avif",
            "AVIF-capable request should be served AVIF",
        )

    def test_webp_accept_yields_webp(
        self, client: PageSpeedClient, test_root: str, stats_snapshot
    ):
        """Accept: image/webp (no AVIF token) produces WebP, never AVIF.

        convert_jpeg_to_avif is enabled here too, so the absence of AVIF is
        attributable to capability detection rather than to the filter simply
        being off.
        """
        old_stats = stats_snapshot()
        response = _fetch_rewritten_image(
            client, test_root, _FILTERS_BOTH, _ACCEPT_WEBP
        )
        new_stats = stats_snapshot()

        assert_stat_unchanged(
            old_stats, new_stats, _AVIF_REWRITES,
            "WebP-only request must not drive an AVIF rewrite",
        )
        assert_image_format(
            response, "webp",
            "WebP-capable request should be served WebP",
        )

    def test_legacy_accept_yields_jpeg(
        self, client: PageSpeedClient, test_root: str, stats_snapshot
    ):
        """An Accept naming neither format keeps the image a JPEG.

        Both conversion filters are enabled here, so capability detection is
        the only thing standing between this request and a converted image.
        """
        old_stats = stats_snapshot()
        response = _fetch_rewritten_image(
            client, test_root, _FILTERS_BOTH, _ACCEPT_LEGACY
        )
        new_stats = stats_snapshot()

        assert_stat_unchanged(
            old_stats, new_stats, _AVIF_REWRITES,
            "Legacy request must not drive an AVIF rewrite",
        )
        assert_image_format(
            response, "jpeg",
            "Legacy request should be served recompressed JPEG",
        )


@pytest.mark.requires_stats
class TestAvifPickSmaller:
    """AVIF is adopted over a competing WebP candidate on SIZE, not by default.

    THIS IS THE CASE THAT COVERS THE SIZE-COMPARISON BRANCH -- do not fold it
    back into test_avif_accept_yields_avif to "remove duplication". They look
    similar and they are not:

    In the AVIF-only case, no WebP candidate is ever produced, so the adoption
    guard `(!ok || avif_out.size() < output_contents_.size())` is satisfied
    through its `!ok` arm. That arm is unconditional -- it would adopt an AVIF
    candidate of ANY size, including one larger than the alternative. The
    size comparison itself is never evaluated there.

    Enabling both filters AND advertising both formats is the only
    configuration that produces two real candidates and forces the rewriter to
    actually compare them. If the comparison were inverted or dropped, this
    case would fail and no other would.

    The margin is not marginal, so this is not a coin-flip test: for
    Puzzle.jpg the AVIF candidate measures 35,004 bytes against WebP's 45,258
    -- AVIF is ~23% smaller.
    """

    def test_avif_wins_on_size_against_webp(
        self, client: PageSpeedClient, test_root: str, stats_snapshot
    ):
        """With both encoders running, the smaller (AVIF) candidate is served."""
        old_stats = stats_snapshot()
        response = _fetch_rewritten_image(
            client, test_root, _FILTERS_BOTH, _ACCEPT_AVIF_AND_WEBP
        )
        new_stats = stats_snapshot()

        # Primary: the AVIF encode ran AND its output was the one adopted.
        # image_avif_rewrites counts adoptions, not attempts, so a losing AVIF
        # candidate would leave this flat.
        timeouts = new_stats.get(_AVIF_JPEG_TIMEOUTS, 0) - old_stats.get(
            _AVIF_JPEG_TIMEOUTS, 0
        )
        assert_stat_increased(
            old_stats, new_stats, _AVIF_REWRITES, 1,
            "AVIF should beat WebP on size and be adopted "
            f"(jpeg->avif encode timeouts in this window: {timeouts})",
        )
        # Corroborating: the bytes served really are AVIF and not WebP.
        assert_image_format(
            response, "avif",
            "Both formats accepted: the smaller AVIF candidate should win",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
