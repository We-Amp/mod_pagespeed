#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Statistics logging, the console JSON handler and the console page.

Ported from: pagespeed/apache/system_tests/statistics_logging.sh
"""

import json
import os
import pathlib
import re
import time
from typing import List, Tuple

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok

pytestmark = pytest.mark.requires_stats


def _lines_containing(text: str, needle: str) -> int:
    """grep NEEDLE | wc -l"""
    return sum(1 for line in text.splitlines() if needle in line)


@pytest.fixture(scope="module")
def stats_log_path() -> pathlib.Path:
    raw = os.environ.get("PAGESPEED_STATS_LOG", "")
    if not raw:
        pytest.fail("lane fixture stats_log is provided but PAGESPEED_STATS_LOG is unset")
    return pathlib.Path(raw)


@pytest.fixture(scope="module")
def start_time_ms() -> int:
    raw = os.environ.get("PAGESPEED_LANE_START_MS", "")
    if not raw.isdigit():
        pytest.fail(f"PAGESPEED_LANE_START_MS={raw!r} is not a millisecond timestamp")
    return int(raw)


def _read_stats_log(
    path: pathlib.Path, client: PageSpeedClient, timeout: float = 30.0
) -> Tuple[str, List[int]]:
    """The log text and its logged timestamps, once the logger has dumped at least once.

    TIMESTAMPS=($(sed -n '/timestamp: /s/[^0-9]*//gp' $MOD_PAGESPEED_STATS_LOG))

    The logger dumps from request processing, so the wait keeps requests
    flowing through `client` (the bash case ran after the whole suite).
    """
    deadline = time.monotonic() + timeout
    while True:
        text = path.read_text(errors="replace") if path.is_file() else ""
        stamps = [
            int(re.sub(r"[^0-9]", "", line))
            for line in text.splitlines()
            if "timestamp: " in line
        ]
        if stamps:
            return text, stamps
        if time.monotonic() > deadline:
            listing = sorted(os.listdir(path.parent)) if path.parent.is_dir() else "missing"
            pytest.fail(f"no 'timestamp: ' line in {path} after {timeout}s; {path.parent}: {listing}")
        client.get("/mod_pagespeed_example/index.html")
        time.sleep(0.5)


@pytest.mark.requires_fixture("stats_log")
class TestStatisticsLogging:
    """Statistics logging works."""

    def test_statistics_log_records_this_run(
        self, client: PageSpeedClient, stats_log_path, start_time_ms
    ):
        text, stamps = _read_stats_log(stats_log_path, client)
        assert _lines_containing(text, "timestamp: ") >= 1
        early = [stamp for stamp in stamps if stamp < start_time_ms]
        assert not early, f"timestamps before this run started ({start_time_ms}): {early[:5]}"
        assert _lines_containing(text, "num_flushes: ") >= 1
        assert _lines_containing(text, "histogram#") == 0, "histograms must not be logged"
        assert _lines_containing(text, "image_ongoing_rewrites: ") >= 1


@pytest.mark.requires_fixture("stats_log")
class TestConsoleJsonHandler:
    """Statistics logging JSON handler works; JSON handler does not mirror HTML."""

    def test_json_handler_returns_the_logged_series(
        self, client: PageSpeedClient, server_config, stats_log_path
    ):
        _, stamps = _read_stats_log(stats_log_path, client)
        url = (
            f"{server_config.admin_path}/console?json&granularity=0"
            "&var_titles=num_flushes,image_ongoing_rewrites"
        )
        response = client.get(url)
        require_status_ok(response, "console JSON handler")
        text = response.text
        # Each variable we ask for shows up once.
        assert _lines_containing(text, '"num_flushes": ') == 1
        assert _lines_containing(text, '"image_ongoing_rewrites": ') == 1
        assert _lines_containing(text, '"timestamps": ') == 1
        json_stamps = json.loads(text)["timestamps"]
        # The handler may have logged more since; only compare what the log had.
        assert len(json_stamps) >= len(stamps)
        assert json_stamps[: len(stamps)] == stamps

    def test_json_handler_does_not_mirror_html(self, client: PageSpeedClient, server_config):
        response = client.get(
            f"{server_config.admin_path}/console?json&granularity=0&var_titles=<boo>"
        )
        assert "<boo>" not in response.text, "raw <boo> mirrored by the JSON handler"
        assert "&lt;boo&gt;" in response.text, (
            f"expected the escaped &lt;boo&gt;; body: {response.text[:500]}"
        )


@pytest.mark.requires_fixture("admin_handlers")
class TestStatisticsConsole:
    """Statistics console is available."""

    def test_console_page_is_available(self, client: PageSpeedClient):
        response = client.get("/pagespeed_console")
        require_status_ok(response, "/pagespeed_console")
        assert "console" in response.text


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
