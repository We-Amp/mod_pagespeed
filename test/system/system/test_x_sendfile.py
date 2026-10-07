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

"""Responses handed to the web server's sendfile are not rewritten.

Ported from: pagespeed/system/system_tests/x_sendfile.sh
"""

import pytest

from pagespeed_test_framework import require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

PATH = "/mod_pagespeed_test/normal.js"
BLOCKING = {"X-PSA-Blocking-Rewrite": "psatest"}


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestXSendfile:
    """Bash: check that rewriting only happens without X-Sendfile."""

    @pytest.mark.parametrize(
        "host,header",
        [
            pytest.param("uses-sendfile.example.com", "X-Sendfile", id="x-sendfile"),
            pytest.param("uses-xaccelredirect.example.com", "X-Accel-Redirect",
                         id="x-accel-redirect"),
        ],
    )
    def test_sendfile_responses_are_not_rewritten(self, vhost_client, host, header):
        response = vhost_client(host).get(PATH, headers=BLOCKING)
        require_status_ok(response, f"{host}{PATH}")
        assert response.header_values(header), response.raw_headers
        assert "comment2" in response.text, f"{host}: the script was rewritten"

    def test_responses_without_sendfile_are_rewritten(self, vhost_client):
        vhost_client("doesnt-sendfile.example.com").fetch_until(
            PATH,
            condition=lambda r: count_matching_lines(r.text, r"comment2") == 0,
            timeout=100.0,
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
