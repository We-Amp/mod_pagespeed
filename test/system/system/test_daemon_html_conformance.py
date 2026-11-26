#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""HTML cache conformance: every `live` row of
test/system/daemon_html/conformance_cases.json against this lane's server.

The runner core (pagespeed_test_framework.daemon_html_conformance) drives the
rows; a per-lane provider drives the server. A lane whose port is in
daemon_html_conformance.PROVIDER_PORTS must ship the module
`pagespeed_test_framework.daemon_html_provider_<port>` exposing
`make_provider(server_config) -> Provider`; a missing one fails the lane.
A lane outside that set skips every row until its provider lands. Within a
lane with a provider, a row is skipped only when its `ports` leave the
lane's port out.
"""

import os

import pytest

from pagespeed_test_framework import daemon_html_conformance as conf

CASES = conf.load_cases()
ROWS = conf.live_rows(CASES)


def _port(server_config) -> str:
    if server_config.is_iis:
        return "iis"
    if server_config.is_nginx:
        return "nginx"
    return "apache"


# What each port's provider needs from its lane, as a lane fixture: without
# it (a lane that runs no optimizer worker) the rows have nothing to run on.
PROVIDER_FIXTURES = {
    "apache": "daemon_html_conformance",
    "nginx": "daemon_html_conformance",
}


@pytest.fixture(scope="module")
def provider(server_config):
    port = _port(server_config)
    needed = PROVIDER_FIXTURES.get(port)
    provided = {n.strip() for n in os.environ.get("PAGESPEED_LANE_FIXTURES", "").split(",")}
    if needed is not None and needed not in provided:
        pytest.skip(f"lane fixture {needed} not provided")
    p = conf.load_provider(port, server_config)
    if p is None:
        pytest.skip(f"no HTML cache conformance provider for the {_port(server_config)} lane yet")
    return p


@pytest.mark.parametrize("row", ROWS, ids=[r["id"] for r in ROWS])
def test_live_row(provider, row):
    reason = conf.skip_reason(row, provider.port)
    if reason is not None:
        pytest.skip(reason)
    failures = conf.run_row(provider, row, CASES["bodies"])
    assert failures == [], "\n".join(failures)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
