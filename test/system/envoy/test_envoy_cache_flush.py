#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Touching cache.flush flushes the Envoy filter's cache, under per-host
options as under the server's.

The test server (setup_envoy_test.sh) configures a PageSpeed virtual host for
127.0.0.1 that repeats the server's rewrite level, so a request to
127.0.0.1:<port> is served with per-host options and one to localhost:<port>
with the server's, and the two cases differ only in that. The page inlines
update.css; a changed update.css shows only once the cache is flushed, since
the cached copy is still fresh. The colors are random, so an earlier run's
content cannot match.

Environment (exported by run_envoy_tests.sh):
    PAGESPEED_ENVOY_CACHE_DIR: the filter's FileCachePath
    PAGESPEED_ENVOY_DOC_ROOT: the test server's document root
"""

import os
import pathlib
import re
import shutil
import time
import uuid

import pytest

from pagespeed_test_framework import PageSpeedClient

# CacheFlushPollIntervalSec defaults to 5 seconds on Envoy.
FLUSH_TIMEOUT_S = 30.0


def _required_dir(name: str) -> pathlib.Path:
    raw = os.environ.get(name)
    if not raw:
        pytest.fail(f"{name} is not set (run_envoy_tests.sh exports it)")
    path = pathlib.Path(raw)
    if not path.is_dir():
        pytest.fail(f"{name}={path} is not a directory")
    return path


def _unshortenable_hex_color() -> str:
    """A random #rrggbb the CSS minifier keeps as it is (not #rgb-shortenable)."""
    while True:
        color = f"#{uuid.uuid4().int & 0xFFFFFF:06x}"
        if any(color[i] != color[i + 1] for i in (1, 3, 5)):
            return color


@pytest.mark.envoy_only
class TestEnvoyCacheFlush:
    @pytest.mark.timeout(300)
    @pytest.mark.parametrize(
        "host",
        [
            pytest.param("localhost", id="server-options"),
            pytest.param("127.0.0.1", id="per-host-options"),
        ],
    )
    def test_touching_cache_flush_shows_changed_css(self, server_config, host):
        cache_dir = _required_dir("PAGESPEED_ENVOY_CACHE_DIR")
        test_root = _required_dir("PAGESPEED_ENVOY_DOC_ROOT") / "mod_pagespeed_test"
        client = PageSpeedClient(host=host, port=server_config.port)
        template = (test_root / "cache_flush" / "cache_flush_test.html").read_text()
        name = f"envoy-flush-{uuid.uuid4().hex[:12]}"
        test_dir = test_root / "cache_flush" / name
        url_path = f"/mod_pagespeed_test/cache_flush/{name}/cache_flush_test.html"
        before, after = _unshortenable_hex_color(), _unshortenable_hex_color()
        test_dir.mkdir(parents=True, exist_ok=True)
        try:
            (test_dir / "cache_flush_test.html").write_text(template)
            (test_dir / "update.css").write_text(
                f".class myclass {{ color: {before}; }}\n"
            )
            client.fetch_until_count(url_path, re.escape(before), 1)

            (test_dir / "update.css").write_text(
                f".class myclass {{ color: {after}; }}\n"
            )
            # cache.flush is compared by its mtime, in whole seconds.
            time.sleep(2)
            (cache_dir / "cache.flush").touch()
            # Well short of the five-minute lifetime of the cached
            # update.css, so only the flush can bring in the new color.
            client.fetch_until_count(
                url_path, re.escape(after), 1, timeout=FLUSH_TIMEOUT_S
            )
        finally:
            shutil.rmtree(test_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
