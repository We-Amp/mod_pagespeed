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

"""In-place optimization of a gzip-compressed, reverse-proxied origin.

Ported from: pagespeed/apache/system_tests/statistics.sh ("ipro with
reverse proxy of compressed content")

The backend is the lane's unplugged mpsunplugged.example.com vhost reached
through the secondary port, where the bash config ran a backend vhost on a
third port.
"""

import pytest

from pagespeed_test_framework import require_status_ok


@pytest.mark.apache_only  # Apache mod_proxy + mod_deflate in front of the module (bash: apache/)
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestIproReverseProxy:
    """Bash: ipro with reverse proxy of compressed content."""

    def test_compressed_proxied_css_is_optimized_in_place(self, vhost_client):
        vhost = vhost_client("ipro-proxy.example.com")
        vhost.fetch_until(
            "/big.css",
            condition=lambda r: (
                r.header("Content-Encoding") == "gzip" and b"color:#00f" in r.body
            ),
            use_gzip=True,
            timeout=100.0,
            detail_fn=lambda r: (
                f"Content-Encoding={r.header('Content-Encoding')!r} body={r.body[:80]!r}"
            ),
        )
        rewritten = vhost.get("/A.big.css.pagespeed.cf.0.css")
        require_status_ok(rewritten, "A.big.css.pagespeed.cf.0.css")
        assert "big{color:#00f}" in rewritten.text, rewritten.body[:200]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
