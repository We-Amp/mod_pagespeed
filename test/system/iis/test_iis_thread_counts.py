# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""IIS system test: NumRewriteThreads and NumExpensiveRewriteThreads apply.

The worker process logs the sizes of its optimization pools when it starts.
The test sets both directives in the test site's pagespeed.config, recycles
the application pool, and reads the line from the message history (a local admin page). The site's
configuration is restored and the pool recycled on every exit path.

Environment variables (optional; defaults match the standard lane):
    PAGESPEED_IIS_APPPOOL: application pool of the site (PageSpeedTestPool)
    PAGESPEED_IIS_SITE_CONFIG: the pagespeed.config the site reads
        (C:\\inetpub\\pagespeed_test\\pagespeed.config)
"""

import json
import os
import re
import subprocess
import time

import pytest

from pagespeed_test_framework import PageSpeedClient

APPPOOL = os.environ.get("PAGESPEED_IIS_APPPOOL", "PageSpeedTestPool")
SITE_CONFIG = os.environ.get(
    "PAGESPEED_IIS_SITE_CONFIG", r"C:\inetpub\pagespeed_test\pagespeed.config"
)
APPCMD = os.path.join(
    os.environ.get("SystemRoot", r"C:\Windows"), "system32", "inetsrv", "appcmd.exe"
)
LINE = re.compile(
    r"PageSpeed optimization threads: (\d+) rewrite \(([^)]*)\), "
    r"(\d+) expensive rewrite \(([^)]*)\)"
)


def _recycle_pool() -> None:
    out = subprocess.run(
        [APPCMD, "recycle", "apppool", f"/apppool.name:{APPPOOL}"],
        capture_output=True, text=True, timeout=120,
    )
    assert out.returncode == 0, f"appcmd recycle failed: {out.stdout!r} {out.stderr!r}"
    time.sleep(3)


def _resolved_counts(client: PageSpeedClient, example_root: str, admin_path: str):
    """The matching lines in the message history, once at least one is there."""
    deadline = time.time() + 60
    while time.time() < deadline:
        client.get(f"{example_root}/combine_javascript1.js")
        history = client.get(f"{admin_path}/message_history")
        if history.status == 200:
            found = [m for m in (LINE.search(e.get("message", ""))
                                 for e in json.loads(history.text).get("messages", [])) if m]
            if found:
                return found
        time.sleep(2)
    return []


@pytest.mark.iis_only
@pytest.mark.slow
def test_thread_count_directives_set_the_pool_sizes(
    client: PageSpeedClient, example_root: str, server_config
):
    try:
        with open(SITE_CONFIG, "rb") as handle:
            raw_original = handle.read()
    except OSError:
        pytest.skip(f"site config {SITE_CONFIG} not readable")
    original = raw_original.decode("utf-8", errors="replace")

    # Prepended: the lane's config ends in "host:" match blocks.
    injected = (
        "pagespeed NumRewriteThreads 3\n"
        "pagespeed NumExpensiveRewriteThreads 2\n" + original
    )
    try:
        with open(SITE_CONFIG, "w", encoding="utf-8", newline="") as handle:
            handle.write(injected)
        _recycle_pool()
        found = _resolved_counts(client, example_root, server_config.admin_path)
        assert found, "no thread count line in the message history"
        assert len(found) == 1, f"the line is in the message history {len(found)} times"
        match = found[0]
        assert match.group(1) == "3" and match.group(2) == "NumRewriteThreads", match.group(0)
        assert match.group(3) == "2" and match.group(4) == "NumExpensiveRewriteThreads", match.group(0)
    finally:
        with open(SITE_CONFIG, "wb") as handle:
            handle.write(raw_original)
        _recycle_pool()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
