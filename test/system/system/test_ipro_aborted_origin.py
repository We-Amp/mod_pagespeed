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

"""An origin response that ends abnormally is never recorded in place.

The lane's flushing origin (pagespeed_test_framework/flush_origin.py,
reverse-proxied at /mod_pagespeed_test/flush_origin/ with flushpackets=on)
answers the first request for a key of abort_mid_body.css with the head of
a cacheable stylesheet and then drops the connection, the way a backend that
crashes or restarts mid-transfer does. Apache's proxy then ends the response
with an error. The in-place recorder has already buffered the part that
arrived; it must throw it away rather than store it as the resource, so the
next request for the same URL goes back to the origin and gets the whole
stylesheet instead of the cut-short copy from the cache.
"""

import secrets
import socket

import pytest

from pagespeed_test_framework import assert_stat_delta, require_status_ok
from pagespeed_test_framework.stats import settled_stats

ROUTE = "/mod_pagespeed_test/flush_origin/abort_mid_body.css"
TAIL_SELECTOR = "tail-of-stylesheet"
COUNTERS = [
    "ipro_recorder_resources",
    "ipro_recorder_inserted_into_cache",
    "ipro_recorder_failed",
]


def _raw_get(host: str, port: int, target: str, timeout: float = 30.0) -> bytes:
    """GET target and return every byte until the server closes, however the
    response ends; an HTTP client library would raise on the cut-short body."""
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.sendall(
            (f"GET {target} HTTP/1.1\r\nHost: {host}\r\n"
             "Accept-Encoding: identity\r\nConnection: close\r\n\r\n").encode("latin-1"))
        raw = b""
        while True:
            try:
                data = sock.recv(65536)
            except ConnectionResetError:
                break
            if not data:
                break
            raw += data
        return raw
    finally:
        sock.close()


@pytest.mark.apache_only  # mod_proxy ends the brigade with an error bucket (Apache recorder filters)
@pytest.mark.requires_stats
@pytest.mark.requires_fixture("flush_origin")
class TestIproAbortedOrigin:
    """A truncated origin response is not stored or served by in-place optimization."""

    @pytest.mark.parametrize("mode", ["length", "chunked"])
    def test_cut_short_response_is_not_recorded_or_served(
        self, client, stats_snapshot, server_config, mode
    ):
        target = f"{ROUTE}?key=aborted-{mode}-{secrets.token_hex(8)}&mode={mode}"
        before = settled_stats(stats_snapshot, COUNTERS)

        first = _raw_get(server_config.host, server_config.port, target)
        status_line = first.split(b"\r\n", 1)[0]
        assert b" 200" in status_line, first[:300]
        assert b".abort-rule-" in first, first[:300]
        assert TAIL_SELECTOR.encode() not in first, (
            "the origin was meant to drop the first response before its last rule")

        after_first = settled_stats(stats_snapshot, COUNTERS)
        # The recorder did engage on this response...
        assert_stat_delta(before, after_first, "ipro_recorder_resources", 1)
        inserted = (after_first.get("ipro_recorder_inserted_into_cache", 0)
                    - before.get("ipro_recorder_inserted_into_cache", 0))

        # ...and the same URL again must reach the origin, whose second answer
        # is whole, instead of a recorded cut-short copy.
        second = client.get(target)
        require_status_ok(second, target)
        assert inserted == 0 and TAIL_SELECTOR in second.text, (
            f"cut-short response recorded: ipro_recorder_inserted_into_cache "
            f"delta={inserted}, second response has the closing rule="
            f"{TAIL_SELECTOR in second.text}, body tail={second.text[-120:]!r}")
        assert_stat_delta(before, after_first, "ipro_recorder_failed", 1)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
