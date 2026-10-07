#!/usr/bin/env python3
# Copyright (c) 2026 We-Amp B.V.
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

"""IPRO s-maxage tagging of unoptimized in-place responses.

Ported from: pagespeed/automatic/system_tests/smaxage.sh

While an in-place resource is not optimized yet, PageSpeed serves the origin
bytes with a short shared-cache lifetime (s-maxage=10) merged into the origin
Cache-Control, so shared caches do not keep the unoptimized copy for long.
Once optimized, the response carries a plain max-age. Origins that forbid
shared caching or transformation (private, no-cache, no-store, no-transform)
are never optimized and keep their Cache-Control as sent.

The fixture directories are server-level <Directory> blocks in
test/system/setup_apache_test.sh (from install/debug.conf.template); the
tests go through the secondary vhost, whose IPRO cache is its own.
"""

import re
import time
from typing import Optional

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.client import _read_timeout_multiplier

IPRO_ROOT = "/mod_pagespeed_test/ipro"
# Bytes of every example.css under mod_pagespeed_test/ipro/cc*/ and ipro/nocc/.
UNOPTIMIZED_LENGTH = 41


def check_ipro_s_maxage(
    client: PageSpeedClient,
    path: str,
    expect_optimization: bool,
    expected_unoptimized_cache_control: str,
    expected_optimized_cache_control: Optional[str],
    unoptimized_length: int = UNOPTIMIZED_LENGTH,
) -> None:
    """Poll path until it is optimized, or for 3 s when it must not be.

    Bash original (smaxage.sh, check_ipro_s_maxage), per poll:
      - more than one Cache-Control header -> fail
      - body longer than the unoptimized length -> fail
      - unoptimized body: Cache-Control equals expected_unoptimized_cache_control
        exactly, and the response has Content-Length: <unoptimized length>
        or Transfer-Encoding: chunked
      - optimized body: fail unless expect_optimization; else Cache-Control
        matches expected_optimized_cache_control (re.match) and
        X-Original-Content-Length is the unoptimized length
      - timeout: 20 s when optimization is expected (fail), 3 s otherwise (pass)
    """
    timeout_s = (20.0 if expect_optimization else 3.0) * _read_timeout_multiplier()
    deadline = time.monotonic() + timeout_s
    while True:
        response = client.get(path)
        require_status_ok(response, f"IPRO fetch of {path}")
        cache_controls = response.header_values("Cache-Control")
        assert len(cache_controls) <= 1, (
            f"Got more than one Cache-Control header for {path}: {cache_controls}"
        )
        cache_control = cache_controls[0] if cache_controls else ""
        body_length = len(response.body)
        assert body_length <= unoptimized_length, (
            f"Received {body_length} bytes for {path}; unoptimized content "
            f"should be {unoptimized_length}"
        )
        if body_length == unoptimized_length:
            assert cache_control == expected_unoptimized_cache_control, (
                f"Got bad cache control [{cache_control}] for unoptimized "
                f"{path}, expecting [{expected_unoptimized_cache_control}]"
            )
            content_length = response.header("Content-Length")
            chunked = response.header("Transfer-Encoding") == "chunked"
            assert content_length == str(unoptimized_length) or (
                not content_length and chunked
            ), (
                f"Unoptimized {path}: expected Content-Length: "
                f"{unoptimized_length} or Transfer-Encoding: chunked, got "
                f"Content-Length [{content_length}] Transfer-Encoding "
                f"[{response.header('Transfer-Encoding')}]"
            )
        else:
            assert expect_optimization, (
                f"Got unexpected optimization of {path}: {body_length} bytes, "
                f"Cache-Control [{cache_control}]"
            )
            assert re.match(expected_optimized_cache_control, cache_control), (
                f"Optimized {path}: Cache-Control [{cache_control}] does not "
                f"match {expected_optimized_cache_control!r}"
            )
            assert response.header("X-Original-Content-Length") == str(
                unoptimized_length
            ), (
                f"Optimized {path}: X-Original-Content-Length "
                f"[{response.header('X-Original-Content-Length')}], expected "
                f"{unoptimized_length}"
            )
            return
        if time.monotonic() > deadline:
            assert not expect_optimization, (
                f"Timed out after {timeout_s:.0f}s: {path} never got optimized"
            )
            return
        time.sleep(0.1)


# (directory under ipro/, expect optimization, unoptimized Cache-Control,
#  optimized Cache-Control pattern)
DIRECTIVE_CASES = [
    pytest.param("cc200", True, "max-age=200, s-maxage=10",
                 r"max-age=[0-9]*$", id="max-age-200"),
    pytest.param("nocc", True, "s-maxage=10",
                 r"max-age=[0-9]*$", id="no-cache-control-header"),
    pytest.param("cc200p", False, "private, max-age=200",
                 None, id="private"),
    pytest.param("cc200nc", False, "no-cache, max-age=200",
                 None, id="no-cache"),
    pytest.param("cc200ns", False, "no-store, max-age=200",
                 None, id="no-store"),
    pytest.param("cc200nt", False, "no-transform, max-age=200",
                 None, id="no-transform"),
    pytest.param("cc200sma5", True, "s-maxage=5, max-age=200",
                 r"max-age=[0-9]*$", id="s-maxage-5"),
]


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestIproSMaxAgeDirectives:
    """Which origin Cache-Control directives allow in-place optimization.

    Bash original: smaxage.sh "ipro resources tagged with s-maxage, CC: ..."
    """

    @pytest.mark.parametrize(
        "directory, expect_optimization, unoptimized_cc, optimized_cc",
        DIRECTIVE_CASES,
    )
    def test_ipro_s_maxage(
        self,
        secondary_client: PageSpeedClient,
        directory: str,
        expect_optimization: bool,
        unoptimized_cc: str,
        optimized_cc: Optional[str],
    ):
        check_ipro_s_maxage(
            secondary_client,
            f"{IPRO_ROOT}/{directory}/example.css",
            expect_optimization,
            unoptimized_cc,
            optimized_cc,
        )


MERGING_CASES = [
    pytest.param("cc200sma50", "s-maxage=10, max-age=200",
                 r"max-age=[0-9]*$", id="s-maxage-50"),
    pytest.param("cc200sma50nsp", "s-maxage=10, max-age=200",
                 r"max-age=[0-9]*$", id="s-maxage-50-no-space"),
    pytest.param("cc9", "max-age=9", r"max-age=[0-9]$", id="max-age-9"),
    # smaxage.sh's "CC: max-age=200, max-age=9" case fetches ipro/cc9/ too
    # (no ipro/cc200cc9/ content exists); ported as written.
    pytest.param("cc9", "max-age=9", r"max-age=[0-9]$",
                 id="max-age-200-and-9-as-written"),
    pytest.param("cc200sma50sma5", "max-age=200, s-maxage=10, s-maxage=5",
                 r"max-age=[0-9]*$", id="multiple-existing-s-maxage"),
    pytest.param("cc200sma50cc9", "max-age=200, s-maxage=10, max-age=9",
                 r"max-age=[0-9]", id="multiple-existing-max-age"),
    pytest.param("cc200sma50cc9nsp", "max-age=200, s-maxage=10, max-age=9",
                 r"max-age=[0-9]", id="no-spaces"),
    pytest.param("cc200sma50sma51", "max-age=200, s-maxage=10, s-maxage=10",
                 r"max-age=[0-9]", id="multiple-high-s-maxage"),
]


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestIproSMaxAgeMerging:
    """How the added s-maxage merges with an origin's own s-maxage/max-age.

    Bash original: smaxage.sh "ipro resources tagged with s-maxage, ..."
    An origin s-maxage above 10 is lowered to 10, one below is kept; every
    case is optimized eventually and then carries a plain max-age.
    """

    @pytest.mark.parametrize(
        "directory, unoptimized_cc, optimized_cc", MERGING_CASES
    )
    def test_ipro_s_maxage(
        self,
        secondary_client: PageSpeedClient,
        directory: str,
        unoptimized_cc: str,
        optimized_cc: str,
    ):
        check_ipro_s_maxage(
            secondary_client,
            f"{IPRO_ROOT}/{directory}/example.css",
            True,
            unoptimized_cc,
            optimized_cc,
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
