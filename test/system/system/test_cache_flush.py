#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Touching cache.flush in the cache directory flushes the cache.

Ported from: pagespeed/apache/system_tests/cache_flushing.sh (the
cache_flush_count half of "Cache flushing works by touching cache.flush in
cache directory."; lifted from nginx/test_nginx_cache.py::TestCacheFlush).
TestCacheFlushFile ports the same case's file-touch half: a changed CSS
input stays invisible until cache.flush is touched in a vhost's cache
directory, and each vhost with its own cache directory flushes
independently.
"""

import os
import pathlib
import re
import shutil
import time
import uuid
from typing import Callable

import pytest

from pagespeed_test_framework import PageSpeedClient, assert_stat_increased, get_stat
from pagespeed_test_framework.client import VhostClient

# Apache polls cache.flush every CacheFlushPollIntervalSec (default 5 s).
FLUSH_DETECTION_TIMEOUT_S = 15.0

SECONDARY_VHOST = "secondary.example.com"


@pytest.mark.requires_stats
@pytest.mark.requires_fixture("cache_flush")
class TestCacheFlushCount:
    """cache_flush_count increments after touching cache.flush."""

    def test_touching_cache_flush_increments_cache_flush_count(
        self, flush_cache, stats_snapshot
    ):
        before = stats_snapshot()
        flush_cache()
        deadline = time.monotonic() + FLUSH_DETECTION_TIMEOUT_S
        after = stats_snapshot()
        while (
            after.get("cache_flush_count", 0) <= before.get("cache_flush_count", 0)
            and time.monotonic() < deadline
        ):
            time.sleep(0.5)
            after = stats_snapshot()
        assert_stat_increased(before, after, "cache_flush_count", min_increase=1)


def _css(color: str) -> str:
    return f".class myclass {{ color: {color}; }}\n"


def _secondary_cache_dir() -> pathlib.Path:
    """PAGESPEED_SECONDARY_CACHE_DIR: the secondary vhost's FileCachePath.

    No default -- every lane uses a different path, same as
    conftest.flush_cache's treatment of PAGESPEED_CACHE_DIR. A configuration
    error must fail loudly, never skip.
    """
    raw = os.environ.get("PAGESPEED_SECONDARY_CACHE_DIR")
    if not raw:
        pytest.fail("PAGESPEED_SECONDARY_CACHE_DIR is not set")
    return pathlib.Path(raw)


@pytest.mark.requires_secondary
@pytest.mark.requires_stats
@pytest.mark.requires_fixture(
    "cache_flush", "doc_root_scratch", "secondary_vhosts", "debug_conf_dirs"
)
class TestCacheFlushFile:
    """Touching cache.flush invalidates each vhost's cache independently.

    Ported from: pagespeed/apache/system_tests/cache_flushing.sh ("Cache
    flushing works by touching cache.flush in cache directory.", the
    cache.flush-touch half; the cache_flush_count half is
    TestCacheFlushCount above).

    The colors embed the wall clock so a stale process can never show the
    "right" value by luck.
    """

    @pytest.mark.timeout(400)
    def test_touching_cache_flush_invalidates_each_vhost_cache(
        self,
        client: PageSpeedClient,
        vhost_client: Callable[[str], VhostClient],
        doc_root: pathlib.Path,
        stats_snapshot,
        flush_cache,
    ):
        secondary = vhost_client(SECONDARY_VHOST)
        suffix = time.strftime("%H,%M,%S)")
        color0, color1 = f"rgb({suffix}", f"rgb(1{suffix}"
        secondary_flush = _secondary_cache_dir() / "cache.flush"

        # Clear out our existing state before we begin the test.
        flush_cache()
        secondary_flush.touch()
        time.sleep(1)

        name = uuid.uuid4().hex[:12]
        test_dir = doc_root / "cache_flush" / name
        url_path = f"/cache_flush/{name}/cache_flush_test.html"
        template = (doc_root / "cache_flush" / "cache_flush_test.html").read_text()
        test_dir.mkdir(parents=True, exist_ok=True)
        try:
            (test_dir / "cache_flush_test.html").write_text(template)
            (test_dir / "update.css").write_text(_css(color0))
            client.fetch_until_count(url_path, re.escape(color0), 1)
            secondary.fetch_until_count(url_path, re.escape(color0), 1)
            initial_flushes = get_stat(stats_snapshot(), "cache_flush_count")

            (test_dir / "update.css").write_text(_css(color1))
            # 1-second file timestamps: let time pass since the last touch.
            time.sleep(2)
            flush_cache()
            client.fetch_until_count(url_path, re.escape(color1), 1)

            new_flushes = get_stat(stats_snapshot(), "cache_flush_count") - initial_flushes
            assert 1 <= new_flushes < 20, (
                f"cache_flush_count moved by {new_flushes}; expected 1..19"
            )

            # The secondary vhost has its own cache; flush it too.
            secondary_flush.touch()
            time.sleep(1)
            secondary.fetch_until_count(url_path, re.escape(color1), 1)
        finally:
            shutil.rmtree(test_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
