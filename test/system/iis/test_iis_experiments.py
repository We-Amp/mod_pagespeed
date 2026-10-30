# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""IIS system test: an A/B experiment applies to the rewritten HTML.

A visitor is assigned an experiment arm (and given the experiment cookie) on
the module's first pass over a request. The options used to rewrite the HTML
are built again later in the request and must carry the same arm; otherwise
the HTML is rewritten without the arm's filters.

The test runs a one-arm experiment that enables ``remove_comments``, a filter
the site's core filter set does not include, and checks that an HTML comment
is removed and that the cookie is written exactly once. It changes the test
site's pagespeed.config for its own duration (restored, pool recycled, on every
exit path).

Environment variables (optional; defaults match the standard lane):
    PAGESPEED_IIS_APPPOOL: application pool of the site (PageSpeedTestPool)
    PAGESPEED_IIS_SITE_CONFIG: the pagespeed.config the site reads
        (C:\\inetpub\\pagespeed_test\\pagespeed.config)
"""

import os
import subprocess
import time

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok

APPPOOL = os.environ.get("PAGESPEED_IIS_APPPOOL", "PageSpeedTestPool")
SITE_CONFIG = os.environ.get(
    "PAGESPEED_IIS_SITE_CONFIG", r"C:\inetpub\pagespeed_test\pagespeed.config"
)
APPCMD = os.path.join(
    os.environ.get("SystemRoot", r"C:\Windows"), "system32", "inetsrv", "appcmd.exe"
)
PAGE = "remove_comments.html"
COMMENT = "This comment will be removed"
COOKIE = "PageSpeedExperiment="


def _recycle_pool() -> None:
    subprocess.run(
        [APPCMD, "recycle", "apppool", f"/apppool.name:{APPPOOL}"],
        capture_output=True,
        text=True,
        timeout=120,
    )
    time.sleep(3)


@pytest.mark.iis_only
@pytest.mark.slow
def test_experiment_arm_applies_to_rewritten_html(client: PageSpeedClient, example_root: str):
    try:
        with open(SITE_CONFIG, "rb") as handle:
            raw_original = handle.read()
    except OSError:
        pytest.skip(f"site config {SITE_CONFIG} not readable")
    original = raw_original.decode("utf-8", errors="replace")

    # Prepended: the lane's config ends in "host:" match blocks, and
    # directives appended there would fall inside the last block.
    injected = (
        "pagespeed RunExperiment on\n"
        "pagespeed ExperimentSpec id=1;percent=100;default;enable=remove_comments\n"
        + original
    )
    try:
        with open(SITE_CONFIG, "w", encoding="utf-8", newline="") as handle:
            handle.write(injected)
        _recycle_pool()

        deadline = time.time() + 60
        response = client.get(f"{example_root}/{PAGE}")
        while COMMENT in response.text and time.time() < deadline:
            time.sleep(2)
            response = client.get(f"{example_root}/{PAGE}")
        require_status_ok(response, PAGE)

        assert COMMENT not in response.text, (
            "the experiment arm's remove_comments filter was not applied to the "
            "rewritten HTML"
        )
        cookies = [v for v in response.header_values("Set-Cookie") if v.startswith(COOKIE)]
        assert len(cookies) == 1, f"experiment cookie written {len(cookies)} times: {cookies}"
    finally:
        with open(SITE_CONFIG, "wb") as handle:
            handle.write(raw_original)
        _recycle_pool()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
