#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Defensive regression test for the nginx static-asset route.

The static-asset handler answers only when the server's reading of the
request path and the module's own reading agree on a file directly under the
static-asset prefix. A request whose two readings disagree must be left to the
server (an ordinary response), and the worker must keep serving afterwards.
"""

import os
import time

import pytest

from pagespeed_test_framework import PageSpeedClient

# A handful of request targets whose server-side path and module-side path do
# not name the same file under the static-asset prefix. Each must be left to
# the server (any ordinary status), never routed into the static-asset
# handler, and must not disturb the worker.
DISAGREEING_TARGETS = [
    "/pagespeed_static/..%2Fx",
    "/pagespeed_static/..%2F..%2Fx",
    "/pagespeed_static/%2e%2e%2Fy",
    "/pagespeed_static/a%2Fb.0.js",
]

# A well-formed static-asset request: both readings agree on a file directly
# under the prefix. Served by the handler on a correctly built install.
WELL_FORMED_TARGET = "/pagespeed_static/js_defer.0.js"


def _error_log_tail() -> str:
    path = os.environ.get("PAGESPEED_NGINX_ERROR_LOG", "")
    if not path or not os.path.exists(path):
        pytest.fail(
            "PAGESPEED_NGINX_ERROR_LOG is unset or points nowhere; "
            "cannot check for worker exits"
        )
    with open(path, "r", errors="replace") as f:
        return f.read()


def _worker_exit_lines(log_text: str) -> list:
    # A clean graceful shutdown logs "exited with code 0"; that is not a
    # worker loss. Count signal exits and non-zero exit codes only.
    out = []
    for line in log_text.splitlines():
        if "exited on signal" in line:
            out.append(line)
            continue
        marker = "exited with code "
        if "worker process" in line and marker in line:
            code = line.rsplit(marker, 1)[1].strip()
            if code != "0":
                out.append(line)
    return out


@pytest.mark.nginx_only
class TestStaticAssetRoute:
    def test_disagreeing_targets_are_left_to_the_server(
        self, client: PageSpeedClient
    ):
        """A request the two readings disagree on is answered normally and the
        worker keeps serving."""
        baseline = len(_worker_exit_lines(_error_log_tail()))

        for target in DISAGREEING_TARGETS:
            response = client.get(target)
            # The server answers (no reset / dropped connection). The handler
            # must not have claimed the request; any ordinary status is fine.
            assert response.status >= 200

        # The worker that handled those requests is still alive: a follow-up
        # request succeeds, and no new worker-exit line appeared in the log.
        probe = client.get("/mod_pagespeed_example/index.html")
        assert probe.status in (200, 404)

        # Give nginx a moment to flush any crash record, then re-read the log.
        time.sleep(0.5)
        after = len(_worker_exit_lines(_error_log_tail()))
        assert after == baseline, (
            "an nginx worker exited while handling the request(s); "
            f"new worker-exit log lines: {after - baseline}"
        )

    def test_well_formed_static_asset_is_served(self, client: PageSpeedClient):
        """A well-formed static-asset request is still served (the fix does not
        change the handler's normal behaviour)."""
        response = client.get(WELL_FORMED_TARGET)
        assert response.status == 200
        assert len(response.body) > 0


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
