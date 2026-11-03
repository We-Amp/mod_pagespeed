#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""nginx daemon lane: store decisions use the stricter caching headers.

A response is kept in the optimizer's shared cache only when both the
origin's and the server's caching headers allow it, for the shorter of the
two lifetimes.  daemon-origin.example.com proxies /daemon_origin/ to an
application origin (daemon-origin-src.example.com, pagespeed off) whose
images each carry their own Cache-Control, and sets its own caching headers
for browsers.  This checks, on the wire under nginx with the worker running,
that

  * a response the origin does not allow a shared cache to keep is never
    answered from it: no Age header, the origin's bytes on every request,
    and the client still gets the server's field;
  * a response both allow storing is answered from it;
  * a stored copy is never answered with an Age past the shorter lifetime.

The same cases run against the packaged nginx and Apache in
test/system/shared_cache/origin-headers.sh.

Needs the lane fixtures rule_packs and daemon_arm (PAGESPEED_DAEMON_LANE=1 or
require); other lanes skip it.
"""

import os
import time

import pytest

from pagespeed_test_framework import VhostClient, require_status_ok

pytestmark = [
    pytest.mark.nginx_only,
    pytest.mark.requires_fixture("rule_packs", "daemon_arm"),
]

HOST = "daemon-origin.example.com"
SOURCE_HOST = "daemon-origin-src.example.com"
DIRECTIVE = "max-age=3600"
REPEATS = 10
STORE_TIMEOUT_S = 90.0
HEADERS = {
    "Accept": "image/jpeg",
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/130.0 Safari/537.36"
    ),
}


def _client(server_config, host) -> VhostClient:
    raw = os.environ.get("PAGESPEED_RULE_PACKS_PORT")
    if not raw:
        pytest.fail("lane fixture rule_packs is provided but PAGESPEED_RULE_PACKS_PORT is not set")
    return VhostClient(host, proxy_host=server_config.host, proxy_port=int(raw))


@pytest.fixture(scope="module")
def site(server_config) -> VhostClient:
    return _client(server_config, HOST)


@pytest.fixture(scope="module")
def source(server_config) -> VhostClient:
    return _client(server_config, SOURCE_HOST)


@pytest.mark.parametrize("name", ["private", "nostore"])
def test_an_origin_private_or_no_store_response_is_never_stored(site, source, name):
    path = f"/daemon_origin/{name}.jpg"
    origin = source.get(path)
    require_status_ok(origin, f"{SOURCE_HOST}{path}")
    for i in range(REPEATS):
        response = site.get(path, headers=HEADERS)
        require_status_ok(response, f"{HOST}{path}")
        assert not response.header("Age"), (name, i, dict(response.headers))
        assert response.body == origin.body, (name, i, len(response.body), len(origin.body))
        assert response.header("Cache-Control") == DIRECTIVE, (name, i, dict(response.headers))
        time.sleep(1.0)


def test_an_origin_public_response_is_stored(site):
    path = "/daemon_origin/public.jpg"
    response = site.fetch_until(
        path,
        lambda r: r.status == 200 and bool(r.header("Age")),
        timeout=STORE_TIMEOUT_S,
        interval=1.0,
        headers=HEADERS,
    )
    assert response.header("Cache-Control") == DIRECTIVE, dict(response.headers)


def test_the_stored_copy_keeps_the_origins_freshness(site):
    path = "/daemon_origin/short.jpg"
    site.fetch_until(
        path,
        lambda r: r.status == 200 and bool(r.header("Age")),
        timeout=STORE_TIMEOUT_S,
        interval=1.0,
        headers=HEADERS,
    )
    largest = 0
    end = time.time() + 45.0
    while time.time() < end:
        response = site.get(path, headers=HEADERS)
        require_status_ok(response, f"{HOST}{path}")
        age = response.header("Age")
        if age:
            largest = max(largest, int(age))
        time.sleep(1.0)
    assert largest <= 16, f"served from the shared cache at Age {largest}, past the origin's 15 s"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
