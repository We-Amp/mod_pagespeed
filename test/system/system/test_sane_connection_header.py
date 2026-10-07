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

"""The Connection header of an in-place response is not doubled.

Ported from: pagespeed/system/system_tests/sane_connection_header.sh

The bug depended on seeing a resource for the first time on the in-place
path, where the origin response headers are cached; the query string
busts earlier cache entries.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient


class TestSaneConnectionHeader:
    """Bash: Sane Connection header (sane_connection_header.sh:22-27)."""

    def test_connection_header_is_not_doubled(self, client: PageSpeedClient, test_root: str):
        url = f"{test_root}/normal.js?q=cachebust"
        # wget's default request carries "Connection: Keep-Alive"; Apache
        # only echoes the Connection header back when the request sent one.
        response = client.fetch_until(
            url,
            condition=lambda r: 'W/"PSA-aj-' in r.header("ETag"),
            timeout=100.0,
            headers={"Connection": "Keep-Alive"},
            detail_fn=lambda r: f"etag={r.header('ETag')!r}",
        )
        connection = ", ".join(response.header_values("Connection"))
        assert "keep-alive, keep-alive" not in connection.lower(), (
            f"doubled Connection header: {response.header_values('Connection')}"
        )
        assert "keep-alive" in connection.lower(), (
            f"no keep-alive Connection header: {response.raw_headers}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
