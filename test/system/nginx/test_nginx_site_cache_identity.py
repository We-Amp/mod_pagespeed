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

"""Each server block keeps its cached resources to itself.

The lane (run_nginx_tests.sh) runs name-based server blocks on two listeners,
PAGESPEED_SITE_PORT_A and PAGESPEED_SITE_PORT_B, each with a document root
whose style.css holds a rule only that root has (.site-a-only, .site-b-only,
.site-c-only):

- site.example.test on both listeners, one block each;
- shared.example.test on both listeners, with one explicit CacheFragment;
- shard-page.example.com, sharding to names it serves itself;
- cdn-page.example.com, mapping its resources to cdn.example.com, which
  another block (root c) serves.

The last two groups use names under one registrable domain, as a site and
its shard or CDN names usually are.

A block without a CacheFragment of its own gets one derived from its names
and listen addresses, logged when the configuration is read.  The behaviour
both servers share is in system/test_site_cache_identity.py.
"""

import os
import re
import subprocess
from typing import Dict, List

import pytest

from pagespeed_test_framework.client import PageSpeedClient

pytestmark = pytest.mark.nginx_only

OPTIMIZED_A = ".site-a-only{color:red}"
FRAGMENT_LINE = re.compile(
    r'pagespeed: server "([^"]*)" uses cache fragment (\S+)'
)


def _port(name: str) -> int:
    value = os.environ.get(name, "")
    if not value:
        pytest.skip("run via run_nginx_tests.sh (site listener ports)")
    return int(value)


def _client(port_name: str) -> PageSpeedClient:
    return PageSpeedClient("127.0.0.1", _port(port_name))


def _host(name: str) -> Dict[str, str]:
    return {"Host": name}


def _optimize_in_place(client: PageSpeedClient, host: str, path: str = "/style.css"):
    return client.fetch_until(
        path,
        condition=lambda r: r.status == 200 and "{color:red}" in r.text,
        headers=_host(host),
    )


def _fragments(config_text: str) -> List[tuple]:
    """(server name, fragment) per block, in configuration order, from `nginx -t`."""
    binary = os.environ.get("PAGESPEED_NGINX_BINARY", "")
    conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE", "")
    if not binary or not os.path.exists(conf):
        pytest.skip("run via run_nginx_tests.sh (nginx binary and config)")
    test_conf = os.path.join(os.path.dirname(conf), "nginx_site_identity_test.conf")
    with open(test_conf, "w") as f:
        f.write(config_text)
    try:
        r = subprocess.run(
            [binary, "-t", "-p", os.path.dirname(conf), "-e", "stderr", "-c", test_conf],
            capture_output=True,
            text=True,
            timeout=60,
        )
    finally:
        os.unlink(test_conf)
    assert r.returncode == 0, r.stderr
    return FRAGMENT_LINE.findall(r.stderr)


def _config_text() -> str:
    conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE", "")
    if not conf or not os.path.exists(conf):
        pytest.skip("run via run_nginx_tests.sh (nginx config)")
    with open(conf) as f:
        return f.read()


def _values_for(fragments: List[tuple], name: str) -> List[str]:
    return [value for server, value in fragments if server == name]


class TestServerBlockKeyspaces:
    def test_a_request_cannot_choose_another_blocks_keyspace(self):
        values = _values_for(_fragments(_config_text()), "site.example.test")
        # The first listener's value, as its log line shows it.
        value = values[0] if values else "site:" + "0" * 32
        a, b = _client("PAGESPEED_SITE_PORT_A"), _client("PAGESPEED_SITE_PORT_B")
        _optimize_in_place(a, "site.example.test")
        for name in ("PageSpeedCacheFragment", "ModPagespeedCacheFragment"):
            other = b.get(
                "/style.css", headers={"Host": "site.example.test", name: value}
            )
            assert other.status == 200, other.raw_headers
            assert "site-a-only" not in other.text, (name, other.text)

    def test_output_on_a_shard_the_block_serves_is_found(self):
        a = _client("PAGESPEED_SITE_PORT_A")
        page = a.fetch_until(
            "/page.html",
            condition=lambda r: ".pagespeed.cf." in r.text,
            headers=_host("shard-page.example.com"),
        )
        match = re.search(
            r'href="http://(shard-[12]\.example\.com)(/[^"]*\.pagespeed\.cf\.[^"]*)"',
            page.text,
        )
        assert match, page.text
        output = a.get(match.group(2), headers=_host(match.group(1)))
        assert output.status == 200, output.raw_headers
        assert OPTIMIZED_A in output.text, output.text

    def test_output_mapped_to_a_name_another_block_serves_comes_from_that_block(self):
        a = _client("PAGESPEED_SITE_PORT_A")
        page = a.fetch_until(
            "/page.html",
            condition=lambda r: ".pagespeed.cf." in r.text,
            headers=_host("cdn-page.example.com"),
        )
        match = re.search(
            r'href="http://cdn\.example\.com(/[^"]*\.pagespeed\.cf\.[^"]*)"', page.text
        )
        assert match, page.text
        output = a.get(match.group(1), headers=_host("cdn.example.com"))
        # The receiving block rebuilds the resource from its own input, or
        # declines; it never answers with the other block's entry.
        assert output.status in (200, 404), output.raw_headers
        assert "site-a-only" not in output.text, output.text
        if output.status == 200:
            assert ".site-c-only{color:red}" in output.text, output.text


class TestServerBlockKeyspaceValue:
    def test_value_is_logged_and_differs_between_listeners(self):
        values = _values_for(_fragments(_config_text()), "site.example.test")
        assert len(values) == 2, values
        for value in values:
            assert re.fullmatch(r"site:[0-9a-f]{32}", value), value
        assert values[0] != values[1]

    def test_value_is_stable_across_reads_and_name_order(self):
        text = _config_text()
        first = _values_for(_fragments(text), "site.example.test")
        assert len(first) == 2, first
        again = _values_for(_fragments(text), "site.example.test")
        assert first == again
        # The first block lists its names in another order and case.
        reordered = text.replace(
            "server_name site.example.test www.site.example.test;",
            "server_name WWW.Site.Example.Test Site.Example.Test;",
            1,
        )
        assert reordered != text
        names = {"site.example.test", "www.site.example.test"}
        values = [
            value for server, value in _fragments(reordered) if server.lower() in names
        ]
        assert values == first, (first, values)

    def test_explicit_fragment_is_kept_and_not_logged_as_derived(self):
        fragments = _fragments(_config_text())
        assert _values_for(fragments, "shared.example.test") == []


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
