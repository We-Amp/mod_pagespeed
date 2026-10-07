#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Apache static-asset route: answers, not-found responses and statistics.

A request under the static-asset prefix is answered by the module only when
the server's path and the module's own reading of the request path name the
same file directly under the prefix. Any other request under the prefix gets
the server's own not-found response (404) and does not count in
resource_404_count -- the same outcome as on nginx. A well-formed name the
module does not know still gets the module's not-found response and counts,
as before.

The child-process set is read (never signalled) before and after the
requests: the fixed prefork pool (setup_apache_test.sh) must keep the same
children, i.e. no child was lost while handling them.
"""

import os
import subprocess
import time
from typing import Set

import pytest

from pagespeed_test_framework import PageSpeedClient, assert_http_status
from pagespeed_test_framework.stats import settled_stats

PREFIX = "/pagespeed_static/"

# A request whose two readings of the path differ: the server decodes the
# escaped space, the module's reading keeps it escaped.
DIFFERING_READINGS = PREFIX + "no%20such.0.js"

# A well-formed name (three dot-separated parts) that names no asset; both
# readings agree.
UNKNOWN_ASSET = PREFIX + "no_such_asset.0.js"

# A well-formed name of an asset the module serves.
KNOWN_ASSET = PREFIX + "js_defer.0.js"

COUNTER = "resource_404_count"


def _apache_child_pids() -> Set[int]:
    """Read the apache2/httpd child PIDs (processes whose parent is also an
    apache2/httpd process). Read-only: nothing is signalled."""
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
        if len(parts) < 3 or parts[2].strip() not in ("apache2", "httpd"):
            continue
        try:
            apache[int(parts[0])] = int(parts[1])
        except ValueError:
            continue
    return {pid for pid, ppid in apache.items() if ppid in apache}


def _read_error_log() -> str:
    path = os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "/var/log/apache2/error.log")
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        out = subprocess.run(["sudo", "-n", "cat", path], capture_output=True)
        if out.returncode != 0:
            pytest.fail(f"cannot read {path}: {out.stderr!r}")
        return out.stdout.decode("utf-8", "replace")


def _child_exit_lines(log_text: str) -> int:
    return sum(1 for line in log_text.splitlines() if "exit signal" in line)


@pytest.mark.apache_only
@pytest.mark.requires_stats
class TestApacheStaticAssetRoute:
    def test_differing_readings_get_the_servers_not_found(
        self, client: PageSpeedClient, stats_snapshot
    ):
        """404 from the server, not counted, and the child set is unchanged."""
        children_before = _apache_child_pids()
        if not children_before:
            pytest.fail("no apache2/httpd child processes found with ps")
        exits_before = _child_exit_lines(_read_error_log())
        old = settled_stats(stats_snapshot, [COUNTER])

        response = client.get(DIFFERING_READINGS)
        assert_http_status(response, 404)

        new = stats_snapshot()
        assert new.get(COUNTER, 0) == old.get(COUNTER, 0), (
            f"{COUNTER} moved from {old.get(COUNTER, 0)} to "
            f"{new.get(COUNTER, 0)}; the server's own 404 is not counted"
        )

        # The server keeps serving, from the same children.
        probe = client.get("/mod_pagespeed_example/index.html")
        assert probe.status in (200, 404)
        time.sleep(0.5)
        children_after = _apache_child_pids()
        lost = children_before - children_after
        assert not lost, f"apache children gone after the request: {sorted(lost)}"
        assert _child_exit_lines(_read_error_log()) == exits_before, (
            "a child exit was logged while handling the request"
        )

    def test_unknown_asset_gets_the_modules_not_found(
        self, client: PageSpeedClient, stats_snapshot
    ):
        """Unchanged: the module's 404, counted once."""
        old = settled_stats(stats_snapshot, [COUNTER])

        response = client.get(UNKNOWN_ASSET)
        assert_http_status(response, 404)

        new = stats_snapshot()
        assert new.get(COUNTER, 0) == old.get(COUNTER, 0) + 1, (
            f"{COUNTER} should increment by 1, was {old.get(COUNTER, 0)}, "
            f"now {new.get(COUNTER, 0)}"
        )

    def test_known_asset_is_served(self, client: PageSpeedClient):
        response = client.get(KNOWN_ASSET)
        assert_http_status(response, 200)
        assert len(response.body) > 0


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
