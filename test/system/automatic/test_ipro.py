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

"""In-place resource optimization (IPRO) tests.

Ported from: pagespeed/automatic/system_tests/ipro.sh

These tests verify that IPRO correctly optimizes resources in-place
without changing their URLs.
"""

import re
import random

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
    assert_file_size,
)


class TestIPRO:
    """Tests for in-place resource optimization.

    Bash original::

        start_test In-place resource optimization
        URL=$TEST_ROOT/ipro/test_image_dont_reuse.png
        THRESHOLD_SIZE=13000
        fetch_until -save $URL "wc -c" $THRESHOLD_SIZE "--save-headers" "-lt"
        check_file_size $FETCH_FILE -lt $THRESHOLD_SIZE
    """

    def test_ipro_compresses_image(
        self, client: PageSpeedClient, test_root: str
    ):
        """IPRO should compress images under the original URL."""
        # Use a random query param to ensure we're not reusing a cached version
        url = f"{test_root}/ipro/test_image_dont_reuse.png?r={random.randint(1, 100000)}"
        threshold_size = 13000

        # Fetch until the image is compressed
        response = client.fetch_until(
            url,
            condition=lambda r: len(r.body) < threshold_size,
            timeout=60.0,
        )
        assert_http_status(response, 200)
        assert_file_size(
            response, "<", threshold_size,
            "IPRO should compress the image",
        )

    def test_ipro_short_cache_lifetime(
        self, client: PageSpeedClient, test_root: str
    ):
        """IPRO resources should have short cache lifetime.

        The origin server must NOT set an explicit long Cache-Control for
        IPRO resources, so PSOL uses implicit_cache_ttl_ms (default 300s).

        Bash original::

            check [ "$(tr -d '\\r' < $FETCH_FILE | \\
                       sed -n 's/Cache-Control: max-age=\\([0-9]*\\)$/\\1/p')" \\
                    -lt 1000 ]
        """
        url = f"{test_root}/ipro/test_image_dont_reuse.png?r={random.randint(1, 100000)}"
        threshold_size = 13000

        # Wait for IPRO to complete
        response = client.fetch_until(
            url,
            condition=lambda r: len(r.body) < threshold_size,
            timeout=60.0,
        )
        assert_http_status(response, 200)

        # Check cache control header
        cache_control = response.header("Cache-Control")
        assert cache_control, "Should have Cache-Control header"

        match = re.search(r"max-age=(\d+)", cache_control)
        if match:
            max_age = int(match.group(1))
            assert max_age < 1000, \
                f"IPRO resources should have short cache lifetime, got max-age={max_age}"

    def test_original_image_larger(
        self, client: PageSpeedClient, test_root: str
    ):
        """Original image (with PageSpeed=off) should be larger.

        Bash original::

            check $WGET_DUMP -O $FETCHED $URL?PageSpeed=off
            check_file_size $FETCHED -gt $THRESHOLD_SIZE
        """
        url = f"{test_root}/ipro/test_image_dont_reuse.png?PageSpeed=off"
        threshold_size = 13000

        response = client.get(url)
        assert_http_status(response, 200)

        assert_file_size(
            response, ">", threshold_size,
            "Original image should be larger than threshold",
        )


def _vary_tokens(response) -> list:
    """Lowercased ``Vary`` field tokens of a response; [] when there is none.

    Multiple ``Vary`` lines and comma-joined values are equivalent (RFC 9110
    12.5.5), and both spellings occur on these responses, so the tokens are
    collected set-wise.
    """
    vary = response.header("Vary")
    if not vary:
        return []
    return [token.strip().lower() for token in vary.split(",") if token.strip()]


@pytest.mark.apache_only
class TestIPROVaryParity:
    """A revalidating 304 must state the same encoding axis as its 200.

    The 200 for an in-place-optimized compressible resource carries ``Vary:
    Accept-Encoding`` -- stamped by the serving chain's compressor, which the
    module's bytes are written through.  The 304 that revalidated the same
    resource carried no such token (a 304 has no body, and the compressor's
    small-response shortcut fires on zero length before the line that states
    the axis), so a shared cache updating its stored header fields from the
    304 (RFC 9111 4.3.4) was told the response varies on a narrower set than
    the one it had keyed on, and could collapse encoding variants.  Parity is
    asserted for both request polarities, and in the negative for a resource
    whose media type the compressor is never handed.
    """

    @staticmethod
    def _served_by_ipro(response) -> bool:
        # The module's in-place serve carries its own weak validator
        # (W/"PSA-...); while the metadata cache is cold the origin's own
        # response answers instead, and THAT 200 is not the one under test.
        return response.header("ETag").startswith('W/"PSA-')

    def _warm(self, client: PageSpeedClient, url: str):
        return client.fetch_until(
            url,
            condition=self._served_by_ipro,
            timeout=60.0,
            detail_fn=lambda r: f"etag={r.header('ETag')!r}",
        )

    def test_css_pair_with_gzip_states_the_same_vary(
        self, client: PageSpeedClient, test_root: str
    ):
        """With Accept-Encoding: gzip, the 304 states the 200's token."""
        url = f"{test_root}/ipro/mod_deflate/big.css"
        self._warm(client, url)

        two_hundred = client.get(
            url, headers={"Accept-Encoding": "gzip"}
        )
        assert_http_status(two_hundred, 200)
        # Sanity half of the pair: with the client advertising gzip, the 200
        # for a compressible in-place resource carries the encoding token.
        # If this fails, the fixture stopped exercising the compressor and
        # the parity assertions would pass vacuously.
        assert "accept-encoding" in _vary_tokens(two_hundred), (
            "the 200 for an in-place CSS resource no longer carries "
            "Vary: Accept-Encoding; the parity test's precondition is gone"
        )

        conditional = client.get(
            url,
            headers={
                "Accept-Encoding": "gzip",
                "If-None-Match": two_hundred.header("ETag"),
            },
        )
        assert_http_status(conditional, 304)
        assert "accept-encoding" in _vary_tokens(conditional), (
            "the 304 revalidating an in-place CSS resource dropped the "
            "encoding axis its 200 states, so a shared cache updating its "
            "stored headers (RFC 9111 4.3.4) narrows the response's "
            "negotiation surface and may collapse encoding variants"
        )

    def test_css_pair_without_gzip_states_the_same_vary(
        self, client: PageSpeedClient, test_root: str
    ):
        """Without Accept-Encoding, the pair still agrees on the axis."""
        url = f"{test_root}/ipro/mod_deflate/big.css"
        self._warm(client, url)

        two_hundred = client.get(url)
        assert_http_status(two_hundred, 200)
        conditional = client.get(
            url, headers={"If-None-Match": two_hundred.header("ETag")}
        )
        assert_http_status(conditional, 304)
        # The compressor states the encoding axis before it scans the
        # request's Accept-Encoding, so on this server the 200 carries the
        # token here too -- but the assertion is PARITY, not presence, so a
        # server whose compressor decides differently fails on the
        # disagreement rather than on an assumption about which way.
        assert ("accept-encoding" in _vary_tokens(conditional)) == (
            "accept-encoding" in _vary_tokens(two_hundred)
        ), (
            "the 304 and the 200 for the same in-place resource disagree "
            "about the encoding axis (no Accept-Encoding in the request)"
        )

    def test_image_pair_states_no_encoding_axis_on_either_leg(
        self, client: PageSpeedClient, test_root: str
    ):
        """A media type the compressor is never handed has no axis at all."""
        url = f"{test_root}/ipro/test_image_dont_reuse.png"
        two_hundred = self._warm(client, url)
        assert_http_status(two_hundred, 200)
        assert "accept-encoding" not in _vary_tokens(two_hundred)

        conditional = client.get(
            url,
            headers={
                "Accept-Encoding": "gzip",
                "If-None-Match": two_hundred.header("ETag"),
            },
        )
        assert_http_status(conditional, 304)
        # The fix composes against the compressor's own decision and is not
        # an unconditional stamp: an in-place image resource has no encoding
        # axis on either leg.
        assert "accept-encoding" not in _vary_tokens(conditional), (
            "the 304 for an in-place image resource states an encoding axis "
            "its 200 never had"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
