#!/usr/bin/env python3
# Copyright 2024 Google LLC
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

"""nginx internally-redirected-request tests.

In-place optimization leaves internally redirected requests to the server:
it does not look them up and does not record them. Response headers already
set on a request (kept by nginx across an internal redirect) survive.
"""

import http.client
import os
import time
from typing import Callable, Dict, Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)
from pagespeed_test_framework.stats import settled_stats

# The statistics assertions below read server-wide counters and one origin
# access log: this file cannot share the server with parallel workers.
if os.environ.get("PYTEST_XDIST_WORKER"):
    pytest.skip(
        "reads server-wide counters; run without parallel workers",
        allow_module_level=True,
    )

UNOPTIMIZED = "margin : 0px"
INSERTED = "ipro_recorder_inserted_into_cache"
NOT_IN_CACHE = "ipro_not_in_cache"


def _origin_log_count() -> int:
    with open(os.environ["PAGESPEED_REDIRECT_ORIGIN_LOG"]) as f:
        return sum(1 for _ in f)


def _wait_for_origin_log_count(expected: int) -> int:
    """Polls the origin's access log until it holds `expected` lines."""
    count = _origin_log_count()
    for _ in range(40):
        if count >= expected:
            break
        time.sleep(0.25)
        count = _origin_log_count()
    return count


@pytest.mark.nginx_only
class TestInternallyRedirectedRequests:
    """Requests nginx redirects internally are served by the server itself.

    The tests that count the origin's access-log lines must not run in
    parallel with anything else that reaches that origin.
    """

    def _dl(self, client: PageSpeedClient, test_root: str, variant: str):
        url = f"{test_root}/redirected_dl/site.css"
        return client.get(url, headers={"X-Variant": variant})

    def _assert_left_to_the_server(
        self,
        fetch: Callable[[], object],
        stats_snapshot: Callable[[], Dict[str, int]],
        marker: str,
        lookups: Optional[bool],
    ):
        """Repeated requests are answered as the server has the resource.

        The body never changes and nothing is inserted into the in-place
        cache. `lookups` states what is known about in-place lookups: False
        when the request is redirected before in-place optimization sees it
        (no lookup at all), True when its first pass is an ordinary request
        (a lookup happens, which also proves the counters are live), None
        when the order of the two is the server's business.
        """
        before = settled_stats(stats_snapshot, [INSERTED, NOT_IN_CACHE])
        for name in (INSERTED, NOT_IN_CACHE):
            assert name in before, f"statistic {name} is missing"
        for _ in range(6):
            response = fetch()
            assert_http_status(response, 200)
            assert marker in response.text, (
                f"expected the resource with {marker!r}, got: {response.text[:80]!r}"
            )
            assert UNOPTIMIZED in response.text, (
                "an internally redirected request must be answered as the "
                "server has the resource"
            )
            time.sleep(0.5)
        after = stats_snapshot()
        assert after[INSERTED] == before[INSERTED], (
            f"{INSERTED} moved from {before[INSERTED]} to {after[INSERTED]}: "
            f"an internally redirected request was recorded"
        )
        if lookups is False:
            assert after[NOT_IN_CACHE] == before[NOT_IN_CACHE], (
                f"{NOT_IN_CACHE} moved from {before[NOT_IN_CACHE]} to "
                f"{after[NOT_IN_CACHE]}: an internally redirected request "
                f"was looked up"
            )
        elif lookups is True:
            assert after[NOT_IN_CACHE] > before[NOT_IN_CACHE], (
                f"{NOT_IN_CACHE} stayed at {before[NOT_IN_CACHE]}: the first "
                f"pass of these requests is an ordinary in-place lookup, so "
                f"the counters are not being read"
            )

    @pytest.mark.requires_module
    def test_headers_reach_the_client_cold_and_warm(
        self,
        client: PageSpeedClient,
        test_root: str,
        flush_cache: Callable[[], None],
    ):
        flush_cache()
        for _ in range(3):
            # The first iteration is the cold pass.
            response = self._dl(client, test_root, "a")
            assert_http_status(response, 200)
            cache_control = response.header("Cache-Control")
            assert "max-age=600" in cache_control, (
                f"the origin's Cache-Control must reach the client, "
                f"got: {cache_control!r}"
            )
            assert response.header("Set-Cookie"), (
                "the origin's Set-Cookie must reach the client"
            )
            assert response.header("Content-Disposition") == "attachment", (
                "the origin's Content-Disposition must reach the client"
            )
            assert response.header("Content-Type") == "text/css; charset=utf-8", (
                f"the origin's Content-Type must reach the client, got: "
                f"{response.header('Content-Type')!r}"
            )
            time.sleep(0.5)

    @pytest.mark.requires_module
    def test_each_request_gets_its_own_file(
        self,
        client: PageSpeedClient,
        test_root: str,
        flush_cache: Callable[[], None],
    ):
        flush_cache()
        for _ in range(4):
            self._dl(client, test_root, "a")
        response_b = self._dl(client, test_root, "b")
        assert_contains(response_b, "variant-b")
        response_a = self._dl(client, test_root, "a")
        assert_contains(response_a, "variant-a")

    @pytest.mark.requires_module
    def test_origin_is_consulted_on_every_request(
        self,
        client: PageSpeedClient,
        test_root: str,
        flush_cache: Callable[[], None],
    ):
        flush_cache()
        before = _origin_log_count()
        for _ in range(5):
            self._dl(client, test_root, "a")
        consulted = _wait_for_origin_log_count(before + 5) - before
        assert consulted == 5, (
            f"the origin must be consulted on every request; "
            f"consulted {consulted} times for 5 requests"
        )

    @pytest.mark.requires_module
    def test_head_is_answered_by_the_server_too(
        self,
        client: PageSpeedClient,
        test_root: str,
        flush_cache: Callable[[], None],
    ):
        flush_cache()
        url = f"{test_root}/redirected_dl/site.css"
        before = _origin_log_count()
        for _ in range(3):
            conn = http.client.HTTPConnection(client.host, client.port, timeout=30.0)
            try:
                conn.request("HEAD", url, headers={"X-Variant": "a"})
                response = conn.getresponse()
                response.read()
                assert response.status == 200, f"expected 200, got {response.status}"
                assert "max-age=600" in (response.getheader("Cache-Control") or "")
            finally:
                conn.close()
        consulted = _wait_for_origin_log_count(before + 3) - before
        assert consulted == 3, (
            f"the origin must be consulted on every HEAD; "
            f"consulted {consulted} times for 3 requests"
        )

    @pytest.mark.requires_module
    def test_redirected_request_is_never_recorded(
        self,
        client: PageSpeedClient,
        test_root: str,
        stats_snapshot: Callable[[], Dict[str, int]],
        flush_cache: Callable[[], None],
    ):
        flush_cache()
        self._assert_left_to_the_server(
            lambda: self._dl(client, test_root, "a"),
            stats_snapshot,
            "variant-a",
            lookups=True,
        )

    @pytest.mark.requires_module
    def test_chained_redirect_is_never_recorded(
        self,
        client: PageSpeedClient,
        test_root: str,
        stats_snapshot: Callable[[], Dict[str, int]],
        flush_cache: Callable[[], None],
    ):
        """A redirect target that is itself rewritten stays with the server."""
        flush_cache()
        self._assert_left_to_the_server(
            lambda: self._dl(client, test_root, "hop"),
            stats_snapshot,
            "variant-a",
            lookups=True,
        )

    @pytest.mark.requires_module
    def test_rewritten_resource_is_left_to_the_server(
        self,
        client: PageSpeedClient,
        test_root: str,
        stats_snapshot: Callable[[], Dict[str, int]],
        flush_cache: Callable[[], None],
    ):
        """A resource reached through a rewrite is served as the server has it."""
        flush_cache()
        url = f"{test_root}/redirected_rewrite/direct.css"
        self._assert_left_to_the_server(
            lambda: client.get(url),
            stats_snapshot,
            "direct",
            lookups=False,
        )

    @pytest.mark.requires_module
    def test_named_location_fallback_is_left_to_the_server(
        self,
        client: PageSpeedClient,
        test_root: str,
        stats_snapshot: Callable[[], Dict[str, int]],
        flush_cache: Callable[[], None],
    ):
        """A try_files fallback into a named location stays with the server."""
        flush_cache()
        url = f"{test_root}/redirected_named/named.css"
        self._assert_left_to_the_server(
            lambda: client.get(url),
            stats_snapshot,
            "named",
            lookups=None,
        )

    @pytest.mark.requires_module
    def test_direct_resource_still_optimized_in_place(
        self, client: PageSpeedClient, test_root: str
    ):
        """A plain resource (no redirect) is still optimized in place."""
        url = f"{test_root}/redirected/direct.css"
        response = client.fetch_until(
            url,
            condition=lambda r: UNOPTIMIZED not in r.text and "direct" in r.text,
            timeout=60.0,
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
