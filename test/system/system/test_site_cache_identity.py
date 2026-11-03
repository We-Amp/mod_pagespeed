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

"""Each nginx server block and Apache virtual host keeps its cached resources
to itself, unless they set the same CacheFragment.

The lane (run_nginx_tests.sh, setup_apache_test.sh) serves site.example.test
and shared.example.test from one block or virtual host on each of two
listeners, PAGESPEED_SITE_PORT_A and PAGESPEED_SITE_PORT_B. The two document
roots each hold a style.css with a rule only that root has (.site-a-only,
.site-b-only). The shared.example.test hosts set one explicit CacheFragment.
On Apache, plain.example.test is served the same way by two virtual hosts
that carry no ModPagespeed directive, under the main server's configuration.
"""

import os
import re
import subprocess
from typing import Dict, Optional

import pytest

from pagespeed_test_framework.client import PageSpeedClient

pytestmark = [pytest.mark.not_iis, pytest.mark.not_envoy]

FRAGMENT_LINE = re.compile(r"server \"?([^\s\"]+)\"? uses cache fragment (site:[0-9a-f]+)")


def _client(port_name: str) -> PageSpeedClient:
    value = os.environ.get(port_name, "")
    if not value:
        pytest.skip("the lane runs no site listeners (PAGESPEED_SITE_PORT_A/B)")
    return PageSpeedClient("127.0.0.1", int(value))


def _host(name: str) -> Dict[str, str]:
    return {"Host": name}


def _optimize_in_place(client: PageSpeedClient, host: str):
    return client.fetch_until(
        "/style.css",
        condition=lambda r: r.status == 200 and "{color:red}" in r.text,
        headers=_host(host),
    )


def _apache_error_log() -> Optional[str]:
    """The whole Apache error log, or None where it cannot be read."""
    path = os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "")
    if not path:
        return None
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", errors="replace")
    except OSError:
        try:
            res = subprocess.run(
                ["sudo", "-n", "cat", path],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=30,
            )
        except (subprocess.SubprocessError, OSError):
            return None
        if res.returncode != 0:
            return None
        return res.stdout.decode("utf-8", errors="replace")


def _a_logged_value() -> str:
    """A derived value from the server's log where the lane has one."""
    text = _apache_error_log() or ""
    values = [v for name, v in FRAGMENT_LINE.findall(text) if name == "site.example.test"]
    return values[-2] if len(values) >= 2 else "site:" + "0" * 32


class TestSiteCacheKeyspaces:
    def test_same_names_on_two_listeners_keep_separate_entries(self):
        a, b = _client("PAGESPEED_SITE_PORT_A"), _client("PAGESPEED_SITE_PORT_B")
        optimized = _optimize_in_place(a, "site.example.test")
        assert ".site-a-only{color:red}" in optimized.text, optimized.text

        other = b.get("/style.css", headers=_host("site.example.test"))
        assert other.status == 200, other.raw_headers
        assert "site-a-only" not in other.text, other.text
        assert "site-b-only" in other.text, other.text
        # The other one still optimizes its own copy.
        mine = _optimize_in_place(b, "site.example.test")
        assert ".site-b-only{color:red}" in mine.text, mine.text

    def test_a_request_header_cannot_choose_the_keyspace(self):
        value = _a_logged_value()
        a, b = _client("PAGESPEED_SITE_PORT_A"), _client("PAGESPEED_SITE_PORT_B")
        _optimize_in_place(a, "site.example.test")
        for name in ("PageSpeedCacheFragment", "ModPagespeedCacheFragment"):
            other = b.get("/style.css", headers={"Host": "site.example.test", name: value})
            assert other.status == 200, other.raw_headers
            assert "site-a-only" not in other.text, (name, other.text)

    def test_an_explicit_fragment_is_shared(self):
        a, b = _client("PAGESPEED_SITE_PORT_A"), _client("PAGESPEED_SITE_PORT_B")
        optimized = _optimize_in_place(a, "shared.example.test")
        other = b.get("/style.css", headers=_host("shared.example.test"))
        assert other.status == 200, other.raw_headers
        assert other.text == optimized.text, (optimized.text, other.text)


@pytest.mark.apache_only
class TestVirtualHostsWithoutDirectives:
    def test_plain_virtual_hosts_on_two_listeners_keep_separate_entries(self):
        a, b = _client("PAGESPEED_SITE_PORT_A"), _client("PAGESPEED_SITE_PORT_B")
        optimized = _optimize_in_place(a, "plain.example.test")
        assert ".site-a-only{color:red}" in optimized.text, optimized.text

        other = b.get("/style.css", headers=_host("plain.example.test"))
        assert other.status == 200, other.raw_headers
        assert "site-a-only" not in other.text, other.text
        assert "site-b-only" in other.text, other.text
        mine = _optimize_in_place(b, "plain.example.test")
        assert ".site-b-only{color:red}" in mine.text, mine.text

    def test_each_virtual_host_and_the_main_server_log_their_own_value(self):
        text = _apache_error_log()
        if text is None:
            pytest.fail("the Apache error log is not readable (PAGESPEED_APACHE_ERROR_LOG)")
        lines = FRAGMENT_LINE.findall(text)
        plain = [value for name, value in lines if name == "plain.example.test"]
        others = {value for name, value in lines if name != "plain.example.test"}
        assert len(plain) >= 2, lines
        assert plain[-1] != plain[-2], plain
        assert not set(plain) & others, (plain, others)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
