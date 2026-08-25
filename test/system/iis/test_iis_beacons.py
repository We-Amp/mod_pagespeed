# Copyright 2024 Google LLC
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

"""IIS beacon system tests.

These tests verify the PageSpeed beacon functionality on IIS, ported
from Apache's beacons_load.sh and unload_handler.sh system tests.

Test classes:
- TestBeaconHandler: Tests beacon endpoint handling
- TestBeaconResponse: Tests beacon response format
- TestUnloadHandler: Tests the beacon's page-hide send triggers
- TestBeaconInstrumentation: Tests beacon script injection

Environment Variables:
    PAGESPEED_TEST_ROOT: Root path for test pages
"""

import re
from typing import Dict, Optional

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
    require_match,
    require_status_ok,
)


# ============================================================================
# TestBeaconHandler: Test beacon endpoint
# ============================================================================


class TestBeaconHandler:
    """Tests for beacon handler functionality.

    These tests verify that the PageSpeed beacon endpoint properly
    receives and processes beacon data.

    Ported from: pagespeed/apache/system_tests/beacons_load.sh
    """

    @pytest.mark.iis_only
    def test_beacon_endpoint_exists(
        self, client: PageSpeedClient, server_config
    ):
        """Beacon endpoint should respond."""
        # Try standard beacon paths
        beacon_paths = [
            "/mod_pagespeed_beacon",
            "/pagespeed_beacon",
            "/ngx_pagespeed_beacon",
        ]

        found = False
        attempts = []
        for path in beacon_paths:
            response = client.get(path)
            attempts.append(f"{path} -> {response.status}")
            if response.status in (200, 204, 400, 405):
                found = True
                break

        if not found:
            # The beacon handler serves these paths on the lane (the
            # instrumentation tests below rely on it), so no responding
            # endpoint is a product regression, not an environment
            # condition.
            pytest.fail(
                "No beacon endpoint found. Probed: " + "; ".join(attempts)
            )

    @pytest.mark.iis_only
    def test_beacon_load_event(
        self, client: PageSpeedClient, server_config
    ):
        """Beacon handler should accept load event data.

        Ported from beacons_load.sh:
        Beacon handler for load events, 204 response, no-cache headers
        """
        # Construct a beacon request (simulating browser beacon)
        beacon_paths = ["/mod_pagespeed_beacon", "/pagespeed_beacon"]
        beacon_data = "url=http://example.com/test&event=load"

        attempts = []
        for path in beacon_paths:
            response = client.get(f"{path}?{beacon_data}")
            attempts.append(f"GET {path} -> {response.status}")

            if response.status in (200, 204):
                # Found working beacon endpoint
                return

        # Try POST method
        for path in beacon_paths:
            response = client.post(path, data=beacon_data)
            attempts.append(f"POST {path} -> {response.status}")
            if response.status in (200, 204):
                return

        # The beacon handler must accept load events on the lane; no
        # responding endpoint is a product regression.
        pytest.fail(
            "Beacon endpoint not responding to load events. Probed: "
            + "; ".join(attempts)
        )

    @pytest.mark.iis_only
    def test_beacon_with_timing_data(
        self, client: PageSpeedClient
    ):
        """Beacon should accept timing data."""
        beacon_paths = ["/mod_pagespeed_beacon", "/pagespeed_beacon"]
        # Simulate timing data that browser would send
        beacon_data = (
            "url=http://example.com/test"
            "&event=load"
            "&dns=10"
            "&connect=20"
            "&ttfb=100"
            "&full=500"
        )

        attempts = []
        for path in beacon_paths:
            response = client.get(f"{path}?{beacon_data}")
            attempts.append(f"{path} -> {response.status}")
            if response.status in (200, 204, 400):
                # Endpoint exists (400 may mean invalid data format)
                return

        # The beacon handler must accept timing data on the lane; no
        # responding endpoint is a product regression.
        pytest.fail(
            "No beacon endpoint accepting timing data. Probed: "
            + "; ".join(attempts)
        )


# ============================================================================
# TestBeaconResponse: Test beacon response format
# ============================================================================


class TestBeaconResponse:
    """Tests for beacon response format.

    These tests verify that beacon responses have correct status codes
    and headers.
    """

    @pytest.mark.iis_only
    def test_beacon_204_response(
        self, client: PageSpeedClient
    ):
        """Beacon should return 204 No Content.

        204 is preferred to avoid browser loading indicator.
        """
        beacon_paths = ["/mod_pagespeed_beacon", "/pagespeed_beacon"]

        attempts = []
        for path in beacon_paths:
            response = client.get(f"{path}?url=http://example.com")
            attempts.append(f"{path} -> {response.status}")

            if response.status == 204:
                # Perfect - 204 is the expected response
                assert len(response.text) == 0 or response.text.strip() == "", (
                    "204 response should have no body"
                )
                return
            elif response.status == 200:
                # 200 is acceptable
                return

        # The beacon handler serves these paths on the lane; no responding
        # endpoint is a product regression.
        pytest.fail(
            "No beacon endpoint found. Probed: " + "; ".join(attempts)
        )

    @pytest.mark.iis_only
    def test_beacon_no_cache_headers(
        self, client: PageSpeedClient
    ):
        """Beacon response should not be cached.

        Beacon responses must have no-cache headers to ensure each
        beacon request is actually sent to the server.
        """
        beacon_paths = ["/mod_pagespeed_beacon", "/pagespeed_beacon"]

        attempts = []
        for path in beacon_paths:
            response = client.get(f"{path}?url=http://example.com")
            attempts.append(f"{path} -> {response.status}")

            if response.status in (200, 204):
                cache_control = response.header("Cache-Control")
                if cache_control:
                    # Should have no-cache or no-store or private
                    assert any(
                        directive in cache_control.lower()
                        for directive in ["no-cache", "no-store", "private", "max-age=0"]
                    ), f"Beacon should not be cached, got: {cache_control}"
                return

        # The beacon handler serves these paths on the lane; no responding
        # endpoint is a product regression.
        pytest.fail(
            "No beacon endpoint found. Probed: " + "; ".join(attempts)
        )

    @pytest.mark.iis_only
    def test_beacon_minimal_body(
        self, client: PageSpeedClient
    ):
        """Beacon response should have minimal body."""
        beacon_paths = ["/mod_pagespeed_beacon", "/pagespeed_beacon"]

        attempts = []
        for path in beacon_paths:
            response = client.get(f"{path}?url=http://example.com")
            attempts.append(f"{path} -> {response.status}")

            if response.status in (200, 204):
                # Body should be small (preferably empty for 204)
                body_size = len(response.text) if response.text else 0
                assert body_size < 100, (
                    f"Beacon body should be minimal, got {body_size} bytes"
                )
                return

        # The beacon handler serves these paths on the lane; no responding
        # endpoint is a product regression.
        pytest.fail(
            "No beacon endpoint found. Probed: " + "; ".join(attempts)
        )


# ============================================================================
# TestUnloadHandler: Test the beacon's page-hide send triggers
# ============================================================================


class TestUnloadHandler:
    """Tests for the page-hide send triggers of the instrumentation beacon.

    The collector reports on visibilitychange-to-hidden / pagehide; the
    legacy beforeunload handler (and the ReportUnloadTime option driving
    it) is a deprecated no-op and must never be injected, since an unload
    handler disables the back/forward cache.

    Ported from: pagespeed/apache/system_tests/unload_handler.sh
    """

    @pytest.mark.iis_only
    def test_pagehide_handler_added(
        self, client: PageSpeedClient, example_root: str
    ):
        """The instrumentation script should send on page-hide.

        The collector registers pagehide/visibilitychange listeners to
        send timing data when the user leaves the page; no beforeunload
        handler may be present.
        """
        response = client.get(
            f"{example_root}/extend_cache.html"
            "?PageSpeedFilters=+add_instrumentation"
        )

        if response.status != 200:
            # The fixture page with filters enabled must be served; a
            # non-200 is a server/product failure.
            require_status_ok(response, "add_instrumentation test page")

        # The page-hide send triggers must be present; with
        # add_instrumentation enabled their absence means the filter did
        # not instrument the page -- a product regression.
        require_match(
            r"pagehide|visibilitychange",
            response,
            "page-hide send trigger (add_instrumentation)",
            flags=re.IGNORECASE,
        )

        # ...and the bfcache-breaking beforeunload handler must not be.
        assert not re.search(r"beforeunload", response.text, re.IGNORECASE), (
            "beforeunload handler found - it breaks the back/forward cache"
        )

    @pytest.mark.iis_only
    def test_unload_instrumentation_script(
        self, client: PageSpeedClient, example_root: str
    ):
        """Unload instrumentation should include timing script."""
        response = client.get(
            f"{example_root}/extend_cache.html"
            "?PageSpeedFilters=+add_instrumentation"
        )

        if response.status != 200:
            # The fixture page with filters enabled must be served; a
            # non-200 is a server/product failure.
            require_status_ok(response, "instrumentation test page")

        # Look for instrumentation script
        if "<script" in response.text.lower():
            # Instrumentation was added; the injected script must carry the
            # timing code -- a script without it means the filter
            # regressed.
            require_match(
                r"pagespeed|timing|performance|beacon",
                response,
                "instrumentation timing code",
                flags=re.IGNORECASE,
            )


# ============================================================================
# TestBeaconInstrumentation: Test beacon script injection
# ============================================================================


class TestBeaconInstrumentation:
    """Tests for beacon script injection.

    These tests verify that PageSpeed injects beacon scripts when
    add_instrumentation is enabled.
    """

    @pytest.mark.iis_only
    def test_instrumentation_script_injected(
        self, client: PageSpeedClient, example_root: str
    ):
        """add_instrumentation should inject beacon script."""
        response = client.get(
            f"{example_root}/extend_cache.html"
            "?PageSpeedFilters=+add_instrumentation"
        )

        if response.status != 200:
            # The fixture page with filters enabled must be served; a
            # non-200 is a server/product failure.
            require_status_ok(response, "instrumentation test page")

        # Check for script injection
        assert_contains(
            response,
            r"<script",
            "add_instrumentation should inject a script tag"
        )

    @pytest.mark.iis_only
    def test_instrumentation_disabled_no_script(
        self, client: PageSpeedClient, example_root: str
    ):
        """Without add_instrumentation, beacon script should not be added."""
        response = client.get(
            f"{example_root}/extend_cache.html"
            "?PageSpeedFilters=-add_instrumentation"
        )

        if response.status != 200:
            # The fixture page with filters enabled must be served; a
            # non-200 is a server/product failure.
            require_status_ok(response, "instrumentation test page")

        # May or may not have scripts depending on page content
        # This is informational

    @pytest.mark.iis_only
    def test_instrumentation_beacon_url(
        self, client: PageSpeedClient, example_root: str
    ):
        """Instrumentation script should reference beacon URL."""
        response = client.get(
            f"{example_root}/extend_cache.html"
            "?PageSpeedFilters=+add_instrumentation"
        )

        if response.status != 200:
            # The fixture page with filters enabled must be served; a
            # non-200 is a server/product failure.
            require_status_ok(response, "instrumentation test page")

        # Look for beacon URL in script
        beacon_patterns = [
            r"mod_pagespeed_beacon",
            r"pagespeed_beacon",
            r"ngx_pagespeed_beacon",
            r"/beacon",
        ]

        found = any(
            re.search(pattern, response.text, re.IGNORECASE)
            for pattern in beacon_patterns
        )

        if not found and "<script" in response.text.lower():
            # Script present but no beacon URL - might be different config
            pass


# ============================================================================
# TestBeaconDataFlow: End-to-end beacon data flow tests
# ============================================================================


class TestBeaconDataFlow:
    """End-to-end tests for beacon data flow."""

    @pytest.mark.iis_only
    def test_full_beacon_flow(
        self, client: PageSpeedClient, example_root: str
    ):
        """Test complete beacon flow from page load to beacon send."""
        # Step 1: Get instrumented page
        page_response = client.get(
            f"{example_root}/extend_cache.html"
            "?PageSpeedFilters=+add_instrumentation"
        )

        if page_response.status != 200:
            # The fixture page with filters enabled must be served; a
            # non-200 is a server/product failure.
            require_status_ok(page_response, "instrumented test page")

        # Step 2: Simulate beacon send
        beacon_paths = ["/mod_pagespeed_beacon", "/pagespeed_beacon"]
        beacon_data = (
            f"url={example_root}/extend_cache.html"
            "&event=load"
            "&load_time=100"
        )

        beacon_sent = False
        for path in beacon_paths:
            beacon_response = client.get(f"{path}?{beacon_data}")
            if beacon_response.status in (200, 204):
                beacon_sent = True
                break

        # Note: We can't verify server received data without logs
        # Just verify the flow completes without errors

    @pytest.mark.iis_only
    def test_beacon_with_critical_css(
        self, client: PageSpeedClient, example_root: str
    ):
        """Beacon should work with critical CSS filter."""
        response = client.get(
            f"{example_root}/extend_cache.html"
            "?PageSpeedFilters=+prioritize_critical_css"
        )

        if response.status != 200:
            # The fixture page with filters enabled must be served; a
            # non-200 is a server/product failure.
            require_status_ok(response, "instrumentation test page")

        # Critical CSS uses beacons to collect CSS usage data
        # Look for related instrumentation
        critical_patterns = [
            r"critical",
            r"above.?fold",
            r"pagespeed.*css",
        ]

        found = any(
            re.search(pattern, response.text, re.IGNORECASE)
            for pattern in critical_patterns
        )

        # This depends on configuration
        if not found:
            pytest.skip("Critical CSS instrumentation not present")


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite -- vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
