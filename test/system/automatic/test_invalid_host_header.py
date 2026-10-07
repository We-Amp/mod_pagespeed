#!/usr/bin/env python3
# Copyright 2026 We-Amp B.V.
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

"""A Host header with invalid bytes gets an HTTP answer, not a dropped connection.

Ported from: pagespeed/automatic/system_tests/invalid_host_header.sh
"""

import re
import socket

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok

_STATUS = r"HTTP/1\.[01] (200 OK|400 Bad Request)"


class TestInvalidHostHeader:

    def test_invalid_host_header_is_answered(
        self, server_config, client: PageSpeedClient, example_root: str
    ):
        """start_test Invalid HOST URL does not crash the server."""
        request = (
            f"GET {example_root}/ HTTP/1.1\n".encode("ascii")
            + b"Host: 127.0.0.\xef\xbf\xbd\n\n"
        )
        with socket.create_connection(
            (server_config.host, server_config.port), timeout=10
        ) as sock:
            sock.sendall(request)
            status_line = sock.makefile("rb").readline().decode("latin-1")

        assert re.search(_STATUS, status_line), (
            f"Expected '{_STATUS}' for an invalid Host header, got "
            f"{status_line!r} (empty means the connection was dropped)"
        )
        require_status_ok(client.get(f"{example_root}/"),
                          "example root after the invalid Host request")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
