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

"""The module's own fetches do not pass access rules that admit by address.

The module fetches the inputs of an optimized resource from the same server,
from the address the request arrived on (or over loopback). An access rule
that admits local clients would admit that fetch, so a non-local client
could read a location restricted to local clients by asking for an
optimized URL whose input lies in it. The optimized URL below is named from
an open directory, so the request itself is outside the restricted location
and only its input is inside.

The non-local client, on one host:
- nginx: connects to loopback from this host's non-loopback address, so its
  peer is not local while the request arrives on loopback, and the module's
  fetch therefore comes from loopback ("allow 127.0.0.1").
- Apache: "Require local" admits a client on the server's own address too,
  so the vhost trusts loopback as a forwarding proxy and the client connects
  from loopback to this host's non-loopback address naming a remote address
  in X-Forwarded-For; the module's fetch goes to that non-loopback address,
  from that address.
"""

import http.client
import os
import socket
import subprocess
from typing import Dict, Tuple

import pytest

HOST = "self-fetch-access.example.com"
BASE = "/mod_pagespeed_test/self_fetch_access"
OPEN_CSS = f"{BASE}/open/plain.css"
HELD_CSS = f"{BASE}/local_only/held.css"
OPEN_MARKER = "sfa-open-marker"
HELD_MARKER = "sfa-held-marker"
# Optimized-resource URLs. ",_" is an escaped "/": the held one's input is
# ../local_only/held.css relative to the open directory. "A." is the
# stylesheet filter's own name prefix.
OPEN_OPTIMIZED = f"{BASE}/open/A.plain.css.pagespeed.cf.0.css"
HELD_OPTIMIZED = f"{BASE}/open/A...,_local_only,_held.css.pagespeed.cf.0.css"
# The packaged admin location's configuration page, named the same way (three
# levels up from the open directory).
ADMIN_OPTIMIZED = f"{BASE}/open/..,_..,_..,_pagespeed_admin,_config.pagespeed.ce.0.css"
# A documentation address (RFC 5737) standing for the remote client.
REMOTE_CLIENT = "192.0.2.77"


def _non_loopback_address() -> str:
    """This host's first non-127 IPv4 address."""
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


def _port() -> int:
    port = os.environ.get("PAGESPEED_SELF_FETCH_ACCESS_PORT")
    if not port:
        pytest.fail("PAGESPEED_SELF_FETCH_ACCESS_PORT is not set by the lane")
    return int(port)


def _remote_get(server_config, path: str) -> Tuple[int, str]:
    """GET path on the self-fetch-access vhost as a non-local client."""
    own = _non_loopback_address()
    headers: Dict[str, str] = {"Host": HOST}
    if server_config.is_nginx:
        connect_to, source = "127.0.0.1", own
    else:
        connect_to, source = own, "127.0.0.1"
        headers["X-Forwarded-For"] = REMOTE_CLIENT
    conn = http.client.HTTPConnection(connect_to, _port(), timeout=30,
                                      source_address=(source, 0))
    try:
        conn.request("GET", path, headers=headers)
        response = conn.getresponse()
        return response.status, response.read().decode("utf-8", "replace")
    finally:
        conn.close()


def _describe(status: int, body: str) -> str:
    return f"HTTP {status}: {body[:300]!r}"


@pytest.mark.requires_fixture("self_fetch_access")
class TestSelfFetchAccess:
    """A non-local client cannot read a local-only location through the module."""

    def test_non_local_client_is_refused_directly(self, server_config):
        # The premise: this client is not local to the location's rule.
        status, body = _remote_get(server_config, HELD_CSS)
        assert status == 403, _describe(status, body)
        assert HELD_MARKER not in body

    def test_unrestricted_stylesheet_is_still_optimized(self, server_config):
        status, body = _remote_get(server_config, OPEN_OPTIMIZED)
        assert status == 200, _describe(status, body)
        assert OPEN_MARKER in body, _describe(status, body)
        # Optimized: the source's comment and spacing are gone.
        assert "/*" not in body, _describe(status, body)

    def test_restricted_stylesheet_is_not_served_through_optimized_url(self, server_config):
        status, body = _remote_get(server_config, HELD_OPTIMIZED)
        assert HELD_MARKER not in body, (
            "a location restricted to local clients was read by a non-local "
            f"client through an optimized URL: {_describe(status, body)}")
        assert status != 200, _describe(status, body)

    @pytest.mark.apache_only  # the packaged Apache configuration's admin blocks
    def test_packaged_admin_location_is_not_served_through_optimized_url(self, server_config):
        status, body = _remote_get(server_config, "/pagespeed_admin/config")
        assert status == 403, _describe(status, body)
        status, body = _remote_get(server_config, ADMIN_OPTIMIZED)
        assert "pagespeed_admin" not in body and "FileCachePath" not in body, (
            "the packaged admin location was read by a non-local client "
            f"through an optimized URL: {_describe(status, body)}")
        assert status != 200, _describe(status, body)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
