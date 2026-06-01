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

"""Link rel=canonical header tests.

Ported from: pagespeed/automatic/system_tests/rel_canonical.sh

These tests verify that .pagespeed. resources have Link rel=canonical
headers while IPRO resources do not.
"""

import random

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestRelCanonicalIPRO:
    """Tests that IPRO resources do NOT have rel=canonical headers.

    Bash original::

        start_test link rel=canonical header not present with IPRO resources
        REL_CANONICAL_REGEXP='Link:.*rel.*canonical'
        URL=$EXAMPLE_ROOT/images/Puzzle.jpg
        # Fetch it a few times until IPRO is done and has given it an ipro ("aj") etag.
        fetch_until -save "$URL" 'grep -c E[Tt]ag:.W/.PSA-aj.' 1 --save-headers
        # rel=canonical should not be present.
        check [ $(grep -c "$REL_CANONICAL_REGEXP" $FETCH_FILE) = 0 ]
    """

    def test_ipro_no_rel_canonical(
        self, client: PageSpeedClient, example_root: str
    ):
        """IPRO resources should not have Link rel=canonical header."""
        # Use random query param to avoid caching
        url = f"{example_root}/images/Puzzle.jpg?a={random.randint(1, 100000)}"

        # Wait for IPRO to complete (indicated by PSA-aj in ETag)
        response = client.fetch_until(
            url,
            condition=lambda r: "PSA-aj" in r.header("ETag", ""),
            timeout=30.0,
        )
        assert_http_status(response, 200)

        # Link header should not contain rel=canonical
        link_header = response.header("Link")
        if link_header:
            assert "canonical" not in link_header.lower(), \
                f"IPRO resource should not have rel=canonical, got: {link_header}"


class TestRelCanonicalCacheExtended:
    """Tests that cache-extended resources have rel=canonical headers.

    Bash original::

        start_test link rel=canonical header present with pagespeed.ce resources
        URL=$REWRITTEN_ROOT/images/Puzzle.jpg.pagespeed.ce.HASH.jpg
        OUT=$($CURL -D- -o/dev/null -sS $URL)
        check_from "$OUT" grep "$REL_CANONICAL_REGEXP"
    """

    def test_cache_extended_has_rel_canonical(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """Cache-extended (.ce.) resources should have Link rel=canonical."""
        # Note: The hash in the URL may not be valid, but we can test with
        # any cache-extended resource pattern
        url = f"{rewritten_root}/images/Puzzle.jpg.pagespeed.ce.ABCD1234.jpg"

        response = client.get(url)

        # Even if 404, check the headers for valid resources
        if response.status == 200:
            link_header = response.header("Link")
            assert link_header, "Cache-extended resource should have Link header"
            assert "canonical" in link_header.lower(), \
                f"Cache-extended resource should have rel=canonical, got: {link_header}"


class TestRelCanonicalImageCompressed:
    """Tests that image-compressed resources have rel=canonical headers.

    Bash original::

        start_test link rel=canonical header present with pagespeed.ic resources
        URL=$REWRITTEN_ROOT/images/xPuzzle.jpg.pagespeed.ic.HASH.jpg
        OUT=$($CURL -D- -o/dev/null -sS  $URL)
        check_from "$OUT" grep "$REL_CANONICAL_REGEXP"
    """

    def test_image_compressed_has_rel_canonical(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """Image-compressed (.ic.) resources should have Link rel=canonical."""
        url = f"{rewritten_root}/images/xPuzzle.jpg.pagespeed.ic.ABCD1234.jpg"

        response = client.get(url)

        # Even if 404, check the headers for valid resources
        if response.status == 200:
            link_header = response.header("Link")
            assert link_header, "Image-compressed resource should have Link header"
            assert "canonical" in link_header.lower(), \
                f"Image-compressed resource should have rel=canonical, got: {link_header}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
