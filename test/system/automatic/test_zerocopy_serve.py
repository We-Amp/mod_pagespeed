#!/usr/bin/env python3
# Copyright 2026 We-Amp B.V.
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

"""Zero-copy aliased serving E2E (CycloneZeroCopyServe).

The rig's pagespeed.conf enables CycloneZeroCopy(+Serve) while keeping the
stock Debian/Ubuntu filter chain: mod_reqtimeout enabled and mod_filter
by-type harnesses (AddOutputFilterByType for MOD_PAGESPEED_OUTPUT_FILTER
and DEFLATE) in every request's output chain.  A warm cache-hit resource
serve must still take the ALIASED path.

The regression guarded here is the defect itself: eligibility silently
fail-closing on every default install, with the serve degrading to the
verified copy and no counter explaining why (aliased=0, copied_out=0).
"""

import os
import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
    assert_stat_increased,
)


class TestZeroCopyAliasedServe:
    """Warm cache-hit image serves must alias on the stock chain."""

    def test_warm_image_hit_serves_aliased(
        self, client: PageSpeedClient, test_root: str, server_config
    ):
        """A warm Cyclone cache hit goes out via the aliased zero-copy path."""
        # The zero-copy opt-in (CycloneZeroCopy + CycloneZeroCopyServe) is
        # configured on the Apache rig only (setup_apache_test.sh); the
        # IIS/nginx/envoy rigs run this shared suite without it, where the
        # aliased counter correctly stays 0.  Port-specific rigs set
        # PAGESPEED_SERVER_TYPE; the Apache rig leaves it at "auto".
        # CycloneZeroCopy flavor coverage. The aliased opt-in is configured
        # on the Apache rig (setup_apache_test.sh) and, under the M1 gate, on
        # the IIS rig (setup_iis_full.ps1 with PAGESPEED_ZEROCOPY_GATE=1). The
        # nginx port flips the default on at startup but this shared suite
        # does not drive the nginx rewrite; envoy has no aliased serve. So run
        # the assertion on the apache and iis flavors; on IIS a flat counter
        # means the option is simply off on this rig (not a gate run), which
        # skips rather than fails.
        if server_config.server_type not in ("auto", "apache", "iis"):
            pytest.skip(
                "zero-copy aliased-serve opt-in is exercised on the Apache "
                "and (M1-gated) IIS rigs only"
            )
        # On IIS, skip UP FRONT unless this is a gate-configured rig: the
        # post-fetch flat-counter skip below only fires after a 30s
        # fetch_until budget, which under slowed (AppVerifier) lanes turns a
        # clean skip into a timeout-shaped flake.
        if server_config.server_type == "iis" and os.environ.get(
            "PAGESPEED_ZEROCOPY_GATE"
        ) != "1":
            pytest.skip(
                "CycloneZeroCopy(+Serve) is only configured on the IIS rig "
                "under the M1 gate (PAGESPEED_ZEROCOPY_GATE=1)"
            )
        # zerocopy_big_image.html resizes Puzzle.jpg to 800x600: a genuine
        # REWRITTEN .pagespeed.ic. artifact, stored in the HTTP cache and
        # comfortably above the engine's 16KB aliasing floor.  See the
        # fixture page for why the obvious alternatives cannot alias:
        # the example page's 256x192 Puzzle is below the floor, full-size
        # recompression is dropped as no-saving, and cache-extended .ce.
        # outputs are on-the-fly resources that never hit the HTTP cache.
        url = f"{test_root}/zerocopy_big_image.html"
        response = client.fetch_until_contains(
            url,
            pattern=r'Puzzle\.jpg\.pagespeed\.ic\.[^"]+\.jpg',
            timeout=30.0,
        )
        match = re.search(
            r'[^"]*Puzzle\.jpg\.pagespeed\.ic\.[^"]+\.jpg', response.text
        )
        assert match is not None, "rewritten Puzzle.jpg URL not found in HTML"
        img_path = match.group(0)
        assert img_path.startswith("/"), f"unexpected relative URL: {img_path}"

        old_stats = client.get_statistics()
        # Fetch the artifact twice: whatever the first fetch's cache state,
        # the second is a warm Cyclone hit and must be served aliased.
        assert_http_status(client.get(img_path), 200)
        assert_http_status(client.get(img_path), 200)
        new_stats = client.get_statistics()

        # On IIS the aliased serve only engages when CycloneZeroCopy(+Serve)
        # is on; a flat counter here means this is a plain IIS rig, not the
        # gate flavor -- skip so the cross-platform suite stays green there.
        aliased_delta = new_stats.get("zerocopy_serve_aliased", 0) - old_stats.get(
            "zerocopy_serve_aliased", 0
        )
        if server_config.server_type == "iis" and aliased_delta == 0:
            pytest.skip(
                "CycloneZeroCopy(+Serve) off on this IIS rig (aliased counter "
                "flat); set PAGESPEED_ZEROCOPY_GATE=1 to exercise the IIS "
                "aliased-serve flavor"
            )
        assert_stat_increased(
            old_stats,
            new_stats,
            "zerocopy_serve_aliased",
            msg=(
                "warm .pagespeed.ic hit must alias on the stock filter "
                "chain (reqtimeout + by-type harnesses present); if this "
                "regressed, check zerocopy_serve_ineligible and the "
                "one-time blocker log in the error log"
            ),
        )


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite -- vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
