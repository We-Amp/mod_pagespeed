#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""HTML cache conformance: every `live` row of
test/system/daemon_html/conformance_cases.json against this lane's server.

The runner core (pagespeed_test_framework.daemon_html_conformance) drives the
rows; a per-lane provider drives the server. A lane's provider is the module
`pagespeed_test_framework.daemon_html_provider_<port>` exposing
`make_provider(server_config) -> Provider`. A row is skipped only when its
`ports` leave this lane's port out. Until a lane ships its provider, every
row of that lane is skipped with the reason below.
"""

import importlib
from typing import Optional

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


def _provider(server_config) -> Optional[conf.Provider]:
    name = f"pagespeed_test_framework.daemon_html_provider_{_port(server_config)}"
    try:
        module = importlib.import_module(name)
    except ModuleNotFoundError as e:
        if e.name != name:
            raise
        return None
    provider = module.make_provider(server_config)
    assert provider.port == _port(server_config), provider.port
    return provider


@pytest.fixture(scope="module")
def provider(server_config):
    p = _provider(server_config)
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
