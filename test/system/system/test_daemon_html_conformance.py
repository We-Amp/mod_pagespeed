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
A lane outside that set skips every row until its provider lands.

Skips (enforced by test/system/conftest.py once the lane provides the
daemon_html_conformance fixture): the Apache lane collects only the rows
whose `ports` include it (the others are reported deselected) and fails the
run on any skip; another lane may skip only a row whose `ports` leave it
out.
"""

import os

import pytest

from pagespeed_test_framework import daemon_html_conformance as conf

CASES = conf.load_cases()
ROWS = conf.live_rows(CASES)


def _port(server_config):
    return conf.lane_port(server_config.server_type)


# What each port's provider needs from its lane, as a lane fixture: without
# it (a lane that runs no optimizer worker) the rows have nothing to run on.
PROVIDER_FIXTURES = {
    "apache": "daemon_html_conformance",
    "nginx": "daemon_html_conformance",
    # The IIS provider runs inside the IIS rig's guest
    # (test/system/iis/html_conformance/); the IIS system-test lane does not
    # advertise the fixture, so it skips these rows.
    "iis": "daemon_html_conformance",
}


@pytest.fixture(scope="module")
def provider(server_config):
    port = _port(server_config)
    needed = PROVIDER_FIXTURES.get(port)
    provided = {n.strip() for n in os.environ.get("PAGESPEED_LANE_FIXTURES", "").split(",")}
    if needed is not None and needed not in provided:
        pytest.skip(f"lane fixture {needed} not provided")
    p = conf.load_provider(port, server_config) if port is not None else None
    if p is None:
        pytest.skip(f"no HTML cache conformance provider for the {server_config.server_type} lane yet")
    return p


@pytest.mark.parametrize("row", ROWS, ids=[r["id"] for r in ROWS])
def test_live_row(provider, row):
    reason = conf.skip_reason(row, provider.port)
    if reason is not None:
        if provider.port in conf.ZERO_SKIP_PORTS:
            pytest.fail(f"{reason}: the {provider.port} lane runs no row for another port")
        pytest.skip(reason)
    runner = conf.RowRunner(provider, row, CASES["bodies"])
    failures = runner.run()
    # What each window step saw, for the terminal summary (conftest).
    for w in runner.windows:
        conf.WINDOW_LOG.append(dict(w, row=row["id"]))
    assert failures == [], "\n".join(failures)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
