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

"""A resource the module cannot fetch itself is not optimized.

Ported from: pagespeed/system/system_tests/broken_fetch_ipro_record.sh

broken-fetch.example.com cannot fetch anything (its fetch proxy is
unreachable). Optimized resources are built only from what the module
fetches itself, so its JavaScript is served as the server serves it and is
never optimized, in place or through the page, and that stays so after the
vhost's cache is invalidated through its own cache.flush file (written with
sudo: the module owns that directory).
"""

import pathlib
import re
import subprocess
import time

import pytest

from pagespeed_test_framework import require_match, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

DIR = "/mod_pagespeed_test"


def _touch_cache_flush(cache_dir: pathlib.Path) -> None:
    """Stamp <cache_dir>/cache.flush on a whole second strictly later than
    the current one.

    The module creates the vhost's cache directory owned by the web server
    (0755) when it parses the configuration, so the runner writes the file
    with passwordless sudo -- only ever in this one directory. The cache
    invalidates only entries whose write time is at or before the flush
    time, so a same-second stamp would leave the recorded entry alive; the
    write is held until the wall clock has strictly passed the previous
    stamp before the new one is taken.
    """
    flush = cache_dir / "cache.flush"
    try:
        previous = int(flush.stat().st_mtime)
    except FileNotFoundError:
        previous = 0
    floor = max(previous, int(time.time()))
    deadline = time.monotonic() + 30.0
    while int(time.time()) <= floor:
        if time.monotonic() > deadline:
            pytest.fail(f"clock never passed the previous flush stamp {floor}")
        time.sleep(0.05)
    stamp = int(time.time())
    out = subprocess.run(["sudo", "-n", "touch", "-d", f"@{stamp}", str(flush)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        pytest.fail(f"sudo touch {flush} failed: {out.stderr.strip()}")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "cache_flush")
class TestBrokenFetchIproRecord:
    """Bash: Broken fetches with ipro-recording; now never optimized."""

    def _assert_served_as_recorded(self, vhost, attempts: int) -> None:
        for attempt in range(attempts):
            js = vhost.get(f"{DIR}/broken-fetch.js")
            require_status_ok(js, "broken-fetch.js")
            assert "This comment should be eliminated" in js.text, (
                f"broken-fetch.js was optimized (attempt {attempt}): "
                f"{js.text[:300]!r}")
            assert count_matching_lines(js.text, re.escape("var a = 0;")) == 1, (
                js.text[:300])
            time.sleep(0.5)

    def test_unfetchable_resource_is_not_optimized(
        self, vhost_client, server_config
    ):
        if not server_config.cache_dir:
            pytest.fail("PAGESPEED_CACHE_DIR is not set on this lane")
        cache_dir = pathlib.Path(server_config.cache_dir) / "broken-fetch"
        vhost = vhost_client("broken-fetch.example.com")

        # Several reads: the server serves it, and in-place recording sees it.
        self._assert_served_as_recorded(vhost, 8)
        for _ in range(4):
            page = vhost.get(f"{DIR}/broken-fetch.html")
            require_status_ok(page, "broken-fetch.html")
            src = require_match(r'<script src="([^"]+)"', page, "script URL").group(1)
            assert ".pagespeed." not in src, (
                f"broken-fetch.html links an optimized script: {src}")
            time.sleep(0.5)

        _touch_cache_flush(cache_dir)
        self._assert_served_as_recorded(vhost, 4)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
