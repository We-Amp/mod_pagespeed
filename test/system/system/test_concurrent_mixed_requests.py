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

"""A fixed mix of concurrent requests, each checked against its own outcome.

No other Apache system test sends requests in parallel, so under the event MPM
(several threads in one child) the per-request code paths barely overlap. This
test warms a handful of URLs until each has its settled answer, then sends a
bounded number of requests for all of them at once and checks every response
against the answer of its own URL. Option state leaking from one request to a
neighbour shows up as a wrong body, not only as a 5xx. It also checks that the
set of Apache child processes is the same before and after (a crashed child
would be re-forked under a new pid). It runs under either MPM.
"""

import json
import os
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Callable, List, Set, Tuple

import pytest

from pagespeed_test_framework import PageSpeedClient, VhostClient, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

# The request count and the concurrency stay under the lane's sixteen workers,
# which also have to serve the module's own fetches back to the server.
ROUNDS = 12
CONCURRENCY = 12
DAEMON_ROUNDS = 12
DAEMON_CONCURRENCY = 8
WARM_TIMEOUT_S = 100.0
DAEMON_WARM_TIMEOUT_S = 90.0

OPTIMIZED = re.escape("background:#9370db")
COMBINED = re.escape("styles/yellow.css+blue.css+big.css+bold.css.pagespeed.cc.xo4He3_gYf.css")

DAEMON_HOST = "daemon.example.com"
DAEMON_CSS_PATH = "/daemon_lane/lane.css"
DAEMON_HTML_PATH = "/daemon_lane/lane.html"
OVERRIDE_HOST = "request-option-override.example.com"
OVERRIDE_PAGE = "/mod_pagespeed_test/forbidden.html"
OVERRIDE_QUERY = (
    "?ModPagespeed=on&ModPagespeedFilters=+collapse_whitespace,+remove_comments"
    "&PageSpeedRequestOptionOverride="
)

# One request: (name, fetch, check). fetch returns a Response; check returns an
# empty string when the response is the expected one, else a description.
Request = Tuple[str, Callable[[], object], Callable[[object], str]]


def _apache_child_pids() -> Set[int]:
    """The pids of the Apache children (processes whose parent is also Apache)."""
    out = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,comm="],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        timeout=15,
    ).stdout
    apache = {}
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[2].strip() in ("apache2", "httpd"):
            try:
                apache[int(parts[0])] = int(parts[1])
            except ValueError:
                continue
    return {pid for pid, ppid in apache.items() if ppid in apache}


def _run_all(requests: List[Request], concurrency: int, rounds: int) -> List[str]:
    """Send `rounds` copies of every request, `concurrency` at a time.

    Returns one description per response that was not the expected one.
    """
    jobs = [req for _ in range(rounds) for req in requests]

    def _one(job: Request) -> str:
        name, fetch, check = job
        try:
            problem = check(fetch())
        except Exception as err:  # a reset or timeout is a failure of this request
            return f"{name}: {type(err).__name__}: {err}"
        return f"{name}: {problem}" if problem else ""

    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        results = list(pool.map(_one, jobs))
    return [r for r in results if r]


def _status_and(check: Callable[[str], str]) -> Callable[[object], str]:
    def _check(response) -> str:
        if response.status != 200:
            return f"status {response.status}"
        return check(response.text)

    return _check


def _must_contain(needle: str) -> Callable[[str], str]:
    return lambda body: "" if needle in body else f"missing {needle!r}: {body[:120]!r}"


def _must_not_contain(needle: str) -> Callable[[str], str]:
    return lambda body: f"unexpected {needle!r}: {body[:120]!r}" if needle in body else ""


def _must_match_once(pattern: str) -> Callable[[str], str]:
    return lambda body: (
        "" if count_matching_lines(body, pattern) == 1 else f"pattern {pattern!r} not once: {body[:120]!r}"
    )


def _is_json_object(body: str) -> str:
    try:
        return "" if isinstance(json.loads(body), dict) else "not a JSON object"
    except ValueError:
        return f"not JSON: {body[:80]!r}"


def _assert_same_children(before: Set[int]) -> None:
    after = _apache_child_pids()
    assert after == before, (
        f"the Apache child set changed during the concurrent requests: "
        f"before={sorted(before)} after={sorted(after)} (a child exited and was replaced)"
    )


def _fresh(server_config) -> PageSpeedClient:
    """A client of its own, so no connection state is shared between threads."""
    return PageSpeedClient(host=server_config.host, port=server_config.port)


@pytest.mark.apache_only
@pytest.mark.requires_module
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("debug_conf_dirs", "secondary_vhosts")
class TestConcurrentMixedRequests:
    """Rewritten pages, option overrides, an .htaccess hierarchy and admin pages at once."""

    def test_each_response_matches_its_own_url(self, client, server_config, test_root, vhost_client):
        ssi = f"{test_root}/ssi/ssi.shtml?PageSpeedFilters=combine_css"
        disallowed = f"{test_root}/htaccess/purple.css"
        allowed = f"{test_root}/htaccess/override/purple.css"
        admin = f"{server_config.admin_path}/statistics"

        # Warm up: each URL gets its settled answer before the burst.
        client.fetch_until(
            ssi,
            condition=lambda r: count_matching_lines(r.text, re.escape(".pagespeed.")) == 1,
            timeout=WARM_TIMEOUT_S,
            detail_fn=lambda r: f"body={r.text[:300]!r}",
        )
        client.fetch_until(
            allowed,
            condition=lambda r: count_matching_lines(r.text, OPTIMIZED) == 1,
            timeout=WARM_TIMEOUT_S,
            detail_fn=lambda r: f"body={r.text[:120]!r}",
        )
        require_status_ok(client.get(disallowed), disallowed)
        require_status_ok(client.get(admin), admin)

        def override(secret: str):
            return lambda: vhost_client(OVERRIDE_HOST).get(OVERRIDE_PAGE + OVERRIDE_QUERY + secret)

        requests: List[Request] = [
            ("combined ssi page", lambda: _fresh(server_config).get(ssi),
             _status_and(_must_match_once(COMBINED))),
            ("disallowed stylesheet", lambda: _fresh(server_config).get(disallowed),
             _status_and(lambda b: _must_contain("MediumPurple")(b) or _must_not_contain("background:#9370db")(b))),
            ("allowed stylesheet", lambda: _fresh(server_config).get(allowed),
             _status_and(_must_match_once(OPTIMIZED))),
            ("override with the right secret", override("abc"),
             _status_and(_must_not_contain("<!--"))),
            ("override with a wrong secret", override("notabc"),
             _status_and(_must_contain("<!--"))),
            ("admin statistics", lambda: _fresh(server_config).get(admin),
             _status_and(_is_json_object)),
        ]

        children = _apache_child_pids()
        problems = _run_all(requests, CONCURRENCY, ROUNDS)
        assert not problems, f"{len(problems)} wrong response(s):\n" + "\n".join(problems[:10])
        _assert_same_children(children)


@pytest.mark.apache_only
@pytest.mark.requires_module
@pytest.mark.requires_fixture("daemon_arm")
class TestConcurrentDaemonRequests:
    """Per-request handles to the optimizer daemon, requested in parallel."""

    def test_daemon_served_responses_agree(self, server_config):
        port = int(os.environ.get("PAGESPEED_RULE_PACKS_PORT", "0") or 0)
        if not port:
            pytest.fail("lane fixture daemon_arm is provided but PAGESPEED_RULE_PACKS_PORT is not set")

        def site() -> VhostClient:
            return VhostClient(DAEMON_HOST, proxy_host=server_config.host, proxy_port=port)

        # Warm up until the daemon serves the stylesheet (Age set, no module
        # version header), then take that body as the expected one.
        warm = site().fetch_until(
            DAEMON_CSS_PATH,
            condition=lambda r: r.status == 200 and bool(r.header("Age"))
            and not r.header("X-Mod-Pagespeed") and not r.header("X-Page-Speed"),
            timeout=DAEMON_WARM_TIMEOUT_S,
            detail_fn=lambda r: f"status={r.status} headers={dict(r.headers)}",
        )
        expected_css = warm.text
        assert expected_css, "the daemon-served stylesheet is empty"
        require_status_ok(site().get(DAEMON_HTML_PATH), DAEMON_HOST + DAEMON_HTML_PATH)

        def css_check(response) -> str:
            if response.status != 200:
                return f"status {response.status}"
            return "" if response.text == expected_css else f"body differs: {response.text[:80]!r}"

        def html_check(response) -> str:
            if response.status != 200:
                return f"status {response.status}"
            return "" if "<html" in response.text.lower() else f"not a page: {response.text[:80]!r}"

        requests: List[Request] = [
            ("daemon stylesheet", lambda: site().get(DAEMON_CSS_PATH), css_check),
            ("daemon-host page", lambda: site().get(DAEMON_HTML_PATH), html_check),
        ]
        children = _apache_child_pids()
        problems = _run_all(requests, DAEMON_CONCURRENCY, DAEMON_ROUNDS)
        assert not problems, f"{len(problems)} wrong response(s):\n" + "\n".join(problems[:10])
        _assert_same_children(children)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
