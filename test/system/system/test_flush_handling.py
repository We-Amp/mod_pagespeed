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

"""Flush events in the middle of an HTTP response.

Ported from: pagespeed/system/system_tests/flush_handling.sh and
pagespeed/system/system_tests/image_rewrite_with_flush.sh

The chunked, slow-flushing test origin
(test/system/pagespeed_test_framework/flush_origin.py, reverse-proxied at
/mod_pagespeed_test/flush_origin/ with flushpackets=on) reproduces the PHP
pages of the bash suite: it sets a response header and, for the "with
flush" routes, flushes mid-body. The header must survive the flush exactly
once, and image rewriting must still reach elements on both sides of the
flush.
"""

import socket

import pytest

from pagespeed_test_framework import fetch_chunks, require_status_ok

ROUTE = "/mod_pagespeed_test/flush_origin"


@pytest.mark.requires_fixture("flush_origin")
class TestFlushHandlingHeaders:
    """Bash: Headers are not destroyed by a flush event (flush_handling.sh)."""

    @pytest.mark.parametrize("route, header_value", [
        pytest.param("withoutflush", "without_flush", id="without-flush"),
        pytest.param("withflush", "with_flush", id="with-flush"),
    ])
    def test_headers_are_not_destroyed_by_a_flush_event(
        self, client, route, header_value
    ):
        response = client.get(f"{ROUTE}/{route}")
        require_status_ok(response, route)
        pagespeed_headers = (
            response.header_values("X-Mod-Pagespeed")
            + response.header_values("X-Page-Speed")
        )
        assert len(pagespeed_headers) == 1, (
            f"{route}: expected exactly one PageSpeed header, got {pagespeed_headers}"
        )
        assert response.header_values("X-My-PHP-Header") == [header_value], (
            f"{route}: X-My-PHP-Header "
            f"{response.header_values('X-My-PHP-Header')}, expected [{header_value!r}]"
        )


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("flush_origin", "secondary_vhosts")
class TestImageRewriteWithFlush:
    """Bash: Image rewrite with flush (image_rewrite_with_flush.sh)."""

    def test_image_rewrite_with_flush(self, vhost_client):
        """Bash: fetch_until -save $URL 'fgrep -c .pagespeed.ic' 2."""
        vhost_client("image-rewrite-with-flush.example.com").fetch_until_count(
            f"{ROUTE}/image_rewrite_with_flush", r"\.pagespeed\.ic", 2,
        )


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("flush_origin", "secondary_vhosts")
class TestFollowFlushes:
    """Bash: Follow flushes does what it should do (follow_flushes.sh).

    check_flushing flush 2.2 5: through flush.example.com every chunk
    arrives within 2.2 s of the previous read and at least five chunks
    arrive. The flushing origin stands in for the PHP page.
    """

    def test_follow_flushes_streams_timely_chunks(self, vhost_client, server_config):
        precheck = vhost_client("flush.example.com").get(f"{ROUTE}/withoutflush")
        require_status_ok(precheck, "flush.example.com withoutflush")
        try:
            response = fetch_chunks(
                server_config.secondary_host,
                server_config.secondary_port,
                f"http://flush.example.com{ROUTE}/slow_flushing_html_response",
                "flush.example.com",
                timeout=2.2,
            )
        except socket.timeout as err:
            pytest.fail(f"a chunk took longer than 2.2 s ({err}): flushes were not followed")
        assert response.status == 200 and response.chunked, (
            f"status {response.status}, chunked={response.chunked}"
        )
        assert len(response.chunks) >= 5, (
            f"expected at least 5 chunks, got {len(response.chunks)}: "
            f"{[c.data[:40] for c in response.chunks]}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
