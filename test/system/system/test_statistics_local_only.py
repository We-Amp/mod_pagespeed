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

"""Statistics are served to loopback clients only, whatever the query.

Ported from: pagespeed/system/system_tests/statistics_are_local_only.sh

The lane's own statistics handler is open to the test runner, so the
restricted handler lives on stats-local-only.example.com ("Require ip
127.0.0.1 ::1", the bash config's "Allow from localhost / 127.0.0.1"); the
non-local client is this host connecting to its own non-loopback address,
as the bash did. The bash's second probe appends ".pagespeed.ce.<hash>.css"
to a statistics URL that already carries "?PageSpeed=off", so it is the same
handler with a different query, not a .pagespeed. URL; it is ported as
written. The bash's Host: localhost variant would reach the secondary port's
default vhost and is not ported.
"""

import socket
import subprocess

import pytest

from pagespeed_test_framework import VhostClient, require_status_ok

HOST = "stats-local-only.example.com"
STATS = "/mod_pagespeed_statistics?PageSpeed=off"


def _non_loopback_address() -> str:
    """This host's first non-127 IPv4 address (the bash's ifconfig lookup)."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("10.255.255.255", 1))  # no packet is sent
        address = probe.getsockname()[0]
    except OSError:
        address = ""
    finally:
        probe.close()
    if address and not address.startswith("127."):
        return address
    out = subprocess.run(["hostname", "-I"], capture_output=True, text=True)
    for candidate in out.stdout.split():
        if "." in candidate and not candidate.startswith("127."):
            return candidate
    pytest.fail("this host has no non-loopback IPv4 address to make a non-local request from")


@pytest.mark.apache_only  # Apache access control on the handler (bash: Apache config)
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestStatisticsLocalOnly:
    """Bash: Non-local access to statistics fails."""

    def test_non_local_access_to_statistics_fails(self, vhost_client, server_config):
        # Control: a loopback client gets the statistics.
        require_status_ok(vhost_client(HOST).get(STATS), "loopback statistics")

        remote = VhostClient(HOST, proxy_host=_non_loopback_address(),
                             proxy_port=server_config.secondary_port)
        css = remote.get(
            "/mod_pagespeed_example/styles/W.rewrite_css_images.css.pagespeed.cf.Hash.css"
        )
        require_status_ok(css, "non-local .pagespeed. stylesheet")
        assert "background-image" in css.text, css.text[:300]

        for path in (STATS, STATS + ".pagespeed.ce.8CfGBvwDhH.css"):
            response = remote.get(path)
            assert response.status >= 400, (
                f"{path} was served to a non-local client: HTTP {response.status}"
            )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
