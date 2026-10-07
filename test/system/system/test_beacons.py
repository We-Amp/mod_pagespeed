#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Beacon handler responses and the deprecated ReportUnloadTime option.

Ported from: pagespeed/apache/system_tests/beacons_load.sh
             pagespeed/apache/system_tests/unload_handler.sh
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok

SECONDARY_VHOST = "secondary.example.com"
ADD_INSTRUMENTATION = "/mod_pagespeed_test/add_instrumentation.html"


def _lines_containing(text: str, needle: str) -> int:
    """grep -c NEEDLE"""
    return sum(1 for line in text.splitlines() if needle in line)


class TestBeaconLoad:
    """add_instrumentation beacons load."""

    def test_load_beacon_is_204_and_not_cacheable(
        self, client: PageSpeedClient, server_config
    ):
        response = client.get(f"{server_config.beacon_path}?ets=load:13")
        assert response.status == 204, (
            f"{server_config.beacon_path}?ets=load:13 returned {response.status}, "
            f"expected 204 No Content"
        )
        assert "max-age=0, no-cache" in response.header("Cache-Control"), (
            f"Cache-Control: {response.header('Cache-Control')!r}"
        )


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestReportUnloadTimeIsNoOp:
    """add_instrumentation with the deprecated no-op ReportUnloadTime enabled."""

    def test_output_matches_plain_add_instrumentation(self, vhost_client, server_config):
        client = vhost_client(SECONDARY_VHOST)
        response = client.get(f"{ADD_INSTRUMENTATION}?PageSpeedFilters=add_instrumentation")
        require_status_ok(response, f"http://{SECONDARY_VHOST}{ADD_INSTRUMENTATION}")
        text = response.text
        assert text.count("<script") == 2, f"expected 2 <script tags; body: {text[:800]}"
        init = (
            f"pagespeed.addInstrumentationInit('{server_config.beacon_path}', '', "
            f"'http://{SECONDARY_VHOST}{ADD_INSTRUMENTATION}');"
        )
        assert _lines_containing(text, init) == 1, f"expected one {init!r}; body: {text[:800]}"
        assert _lines_containing(text, "beforeunload") == 0, "beforeunload handler injected"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
