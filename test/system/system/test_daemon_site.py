#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Optimizer-worker cache entries are kept per site.

nginx (run_nginx_tests.sh) and Apache (setup_apache_test.sh) each run two
server blocks / virtual hosts, daemon-a.example.test on
PAGESPEED_DAEMON_SITE_PORT_A and daemon-b.example.test on
PAGESPEED_DAEMON_SITE_PORT_B, each the only one on its listener, in front of
one optimizer worker. Each document root holds stylesheets with a rule only
that root has (.daemon-site-a-only, .daemon-site-b-only).

  * A resource recorded through A is never served to a request B admitted,
    whatever Host the client sends: one naming A, or one carrying A's port.
  * A location-level (nginx) or per-directory (Apache) CacheFragment does not
    move the site an entry is recorded under.
  * A URL purge through the worker's management API (PAGESPEED_DAEMON_API)
    still finds the entry after the worker restarts
    (PAGESPEED_DAEMON_RESTART); on nginx from
    nginx/test_nginx_worker_restart_purge.py.

Needs the lane fixture daemon_arm (PAGESPEED_DAEMON_LANE=1 or require).
"""

import hashlib
import json
import os
import re
import subprocess
import time

import pytest

from pagespeed_test_framework.client import PageSpeedClient

pytestmark = [
    pytest.mark.not_iis,
    pytest.mark.not_envoy,
    pytest.mark.requires_fixture("daemon_arm"),
]

A_NAME = "daemon-a.example.test"
B_NAME = "daemon-b.example.test"
A_RULE = "daemon-site-a-only"
B_RULE = "daemon-site-b-only"
SITE_ID = re.compile(r"^[0-9a-f]{64}$")
DEADLINE_S = 120.0
LOCATION_FRAGMENT = "daemon-site-location"


def _port(name: str) -> int:
    value = os.environ.get(name, "")
    if not value:
        pytest.fail(f"lane fixture daemon_arm is provided but {name} is not set")
    return int(value)


@pytest.fixture(scope="module")
def a() -> PageSpeedClient:
    return PageSpeedClient("127.0.0.1", _port("PAGESPEED_DAEMON_SITE_PORT_A"))


@pytest.fixture(scope="module")
def b() -> PageSpeedClient:
    return PageSpeedClient("127.0.0.1", _port("PAGESPEED_DAEMON_SITE_PORT_B"))


@pytest.fixture(scope="module")
def api() -> PageSpeedClient:
    value = os.environ.get("PAGESPEED_DAEMON_API", "")
    host, _, port = value.rpartition(":")
    if not host or not port.isdigit():
        pytest.fail(f"PAGESPEED_DAEMON_API must be host:port, got {value!r}")
    return PageSpeedClient(host, int(port))


def _from_worker(response) -> bool:
    """Served from the optimizer worker's cache: the cache's Age header."""
    return response.status == 200 and response.header("Age") != ""


def _record_through(client: PageSpeedClient, path: str, host: str, rule: str):
    """Fetch through `client` until the worker's cache answers with `rule`."""
    return client.fetch_until(
        path,
        condition=lambda r: _from_worker(r) and rule in r.text,
        timeout=DEADLINE_S,
        headers={"Host": host},
    )


def _assert_not_served_the_other_sites_entry(client, path, host):
    for _ in range(3):
        response = client.get(path, headers={"Host": host})
        assert response.status == 200, (host, response.status, dict(response.headers))
        assert A_RULE not in response.text, (host, response.text, dict(response.headers))
        assert B_RULE in response.text, (host, response.text)
        time.sleep(0.5)


def _purge(api: PageSpeedClient, path: str, host: str) -> dict:
    """POST a URL purge, retrying while the worker does not know the host yet
    or its API is not up again."""
    body = json.dumps({"url": path, "hostname": host})
    deadline = time.time() + DEADLINE_S
    last = None
    while time.time() < deadline:
        try:
            last = api.post(
                "/v1/cache/purge",
                data=body,
                # The API's guard for state-changing requests.
                headers={"Content-Type": "application/json", "X-Requested-With": "XMLHttpRequest"},
            )
        except Exception as exc:  # the API restarting
            last = exc
            time.sleep(0.5)
            continue
        if last.status == 200:
            return json.loads(last.text)
        time.sleep(0.5)
    pytest.fail(f"purge of {host}{path} did not succeed: {last!r}")


def test_a_host_naming_the_other_block_is_not_served_its_entries(a, b):
    # Recorded through A under A's own name; asked of B under the same name.
    _record_through(a, "/named.css", A_NAME, A_RULE)
    _assert_not_served_the_other_sites_entry(b, "/named.css", A_NAME)


def test_a_host_with_the_other_blocks_port_is_not_served_its_entries(a, b):
    a_port = _port("PAGESPEED_DAEMON_SITE_PORT_A")
    for host in (f"{A_NAME}:{a_port}", f"{B_NAME}:{a_port}"):
        _record_through(a, "/port.css", host, A_RULE)
        _assert_not_served_the_other_sites_entry(b, "/port.css", host)


def test_a_directory_fragment_does_not_move_the_site(a, api):
    _record_through(a, "/plain.css", A_NAME, A_RULE)
    _record_through(a, "/loc/plain.css", A_NAME, A_RULE)
    block = _purge(api, "/plain.css", A_NAME)
    location = _purge(api, "/loc/plain.css", A_NAME)
    block_sites = [s["site"] for s in block.get("sites", [])]
    location_sites = [s["site"] for s in location.get("sites", [])]
    assert len(block_sites) == 1 and SITE_ID.match(block_sites[0]), block
    assert location_sites == block_sites, (block, location)
    assert block_sites[0] != hashlib.sha256(LOCATION_FRAGMENT.encode()).hexdigest()


def check_a_url_purge_finds_the_entry_after_a_worker_restart(a, api):
    """Shared with nginx/test_nginx_worker_restart_purge.py."""
    restart = os.environ.get("PAGESPEED_DAEMON_RESTART", "")
    if not restart:
        pytest.fail("lane fixture daemon_arm is provided but PAGESPEED_DAEMON_RESTART is not set")
    # Optimized by the worker (the stylesheet minified onto one line), so the
    # worker has seen the host and the site.
    a.fetch_until(
        "/restart.css",
        condition=lambda r: _from_worker(r) and A_RULE in r.text and "\n" not in r.text.strip(),
        timeout=DEADLINE_S,
        headers={"Host": A_NAME},
    )
    result = subprocess.run([restart], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, (result.stdout, result.stderr)
    _record_through(a, "/after.css", A_NAME, A_RULE)
    purged = _purge(api, "/restart.css", A_NAME)
    assert purged.get("deleted", 0) > 0, purged


# On nginx this case runs from nginx/test_nginx_worker_restart_purge.py, after
# the other cases that use the worker.
@pytest.mark.apache_only
def test_a_url_purge_finds_the_entry_after_a_worker_restart(a, api):
    check_a_url_purge_finds_the_entry_after_a_worker_restart(a, api)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
