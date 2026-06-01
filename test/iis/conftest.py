#!/usr/bin/env python3
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

"""Pytest configuration for IIS PageSpeed integration tests.

This configuration extends the shared test framework to run against IIS Express
or a full IIS server. It imports and re-exports fixtures from the shared
test/system/conftest.py to enable running the shared automatic tests.

Environment Variables:
    PAGESPEED_HOST - Server hostname (default: localhost)
    PAGESPEED_PORT - Server port (default: 8080 for IIS Express)
    PAGESPEED_TEST_ROOT - Test pages root (default: /)
    PAGESPEED_EXAMPLE_ROOT - Example pages root (default: /)
    PAGESPEED_CACHE_DIR - Cache directory (default: C:\\PageSpeed\\cache)
    PAGESPEED_STATS_ENABLED - Whether stats are enabled (default: 1)
    IIS_EXPRESS - Set to 1 when running against IIS Express

Usage:
    # Run IIS-specific tests
    pytest test/iis/

    # Run shared automatic tests against IIS
    pytest test/system/automatic/ --confcutdir=test/iis

    # Or use the runner script
    ./test/iis/Run-IISTests.ps1
"""

import os
import sys
import pathlib

import pytest

# Add the shared test framework to the path
_test_system_dir = pathlib.Path(__file__).parent.parent / "system"
sys.path.insert(0, str(_test_system_dir))

# Import the shared fixtures - this makes them available for all tests
from conftest import (  # noqa: E402
    ServerConfig,
    server_config,
    client,
    https_client,
    secondary_client,
    example_root,
    test_root,
    rewritten_root,
    stats_snapshot,
    flush_cache,
    webp_client,
)

from pagespeed_test_framework.client import PageSpeedClient


# Override server_config for IIS-specific defaults
@pytest.fixture(scope="session")
def server_config() -> ServerConfig:
    """Server configuration with IIS-specific defaults.

    IIS Express uses different defaults than Apache:
    - Default port is 8080 (not 80)
    - Test root is / (not /mod_pagespeed_test)
    - Cache dir is C:\\PageSpeed\\cache
    """
    is_iis_express = os.environ.get("IIS_EXPRESS", "0") == "1"

    return ServerConfig(
        host=os.environ.get("PAGESPEED_HOST", "localhost"),
        port=int(os.environ.get("PAGESPEED_PORT", "8080" if is_iis_express else "80")),
        https_host=os.environ.get("PAGESPEED_HTTPS_HOST"),
        https_port=int(os.environ.get("PAGESPEED_HTTPS_PORT", "44300")),
        secondary_host=os.environ.get("PAGESPEED_SECONDARY_HOST"),
        secondary_port=(
            int(os.environ.get("PAGESPEED_SECONDARY_PORT"))
            if os.environ.get("PAGESPEED_SECONDARY_PORT")
            else None
        ),
        # IIS test site uses standard paths
        test_root=os.environ.get("PAGESPEED_TEST_ROOT", "/mod_pagespeed_test"),
        example_root=os.environ.get("PAGESPEED_EXAMPLE_ROOT", "/mod_pagespeed_example"),
        cache_dir=os.environ.get("PAGESPEED_CACHE_DIR", "C:\\PageSpeed\\cache"),
        stats_path=os.environ.get("PAGESPEED_STATS_PATH", "/pagespeed_statistics"),
        admin_path=os.environ.get("PAGESPEED_ADMIN_PATH", "/pagespeed_admin"),
        server_type="iis",
        stats_enabled=os.environ.get("PAGESPEED_STATS_ENABLED", "1") == "1",
    )


def pytest_configure(config):
    """Configure custom markers for IIS tests."""
    config.addinivalue_line(
        "markers", "slow: marks tests as slow (deselect with '-m \"not slow\"')"
    )
    config.addinivalue_line(
        "markers", "admin: marks tests that require admin path access"
    )
    config.addinivalue_line(
        "markers", "license: marks tests that require a valid license key"
    )
    config.addinivalue_line(
        "markers", "ipro: marks IPRO (In-Place Resource Optimization) tests"
    )
    config.addinivalue_line(
        "markers", "html_rewrite: marks HTML rewriting tests"
    )
    config.addinivalue_line(
        "markers", "iis_only: marks tests specific to IIS (not shared)"
    )
    config.addinivalue_line(
        "markers", "requires_secondary: test requires secondary server"
    )
    config.addinivalue_line(
        "markers", "requires_https: test requires HTTPS server"
    )
    config.addinivalue_line(
        "markers", "requires_stats: test requires statistics enabled"
    )


# IIS-specific fixtures

@pytest.fixture(scope="session")
def admin_path(server_config: ServerConfig) -> str:
    """Return the URL to the PageSpeed admin UI."""
    return f"{server_config.primary_url}/pagespeed_admin"


@pytest.fixture(scope="session")
def statistics_path(server_config: ServerConfig) -> str:
    """Return the URL to the PageSpeed statistics endpoint."""
    return f"{server_config.primary_url}/pagespeed_statistics"


@pytest.fixture
def iis_cache_dir(server_config: ServerConfig) -> pathlib.Path:
    """Return the IIS-specific cache directory path."""
    return pathlib.Path(server_config.cache_dir)


@pytest.fixture
def flush_iis_cache(iis_cache_dir: pathlib.Path):
    """Fixture to flush the IIS PageSpeed cache.

    On Windows, we touch the cache.flush file in the configured cache directory.
    """
    import time

    def _flush():
        cache_flush = iis_cache_dir / "cache.flush"
        if not iis_cache_dir.exists():
            pytest.skip(f"Cache directory not found: {iis_cache_dir}")

        cache_flush.touch()
        time.sleep(1.5)  # Wait for cache flush to be detected

    return _flush


# Skip tests based on server configuration
def pytest_runtest_setup(item):
    """Skip tests based on markers and server configuration."""
    server_cfg = item.funcargs.get("server_config")
    if server_cfg is None:
        return

    if item.get_closest_marker("requires_secondary"):
        if not server_cfg.secondary_host:
            pytest.skip("Secondary server not configured")

    if item.get_closest_marker("requires_https"):
        if not server_cfg.https_host:
            pytest.skip("HTTPS server not configured")

    if item.get_closest_marker("requires_stats"):
        if not server_cfg.stats_enabled:
            pytest.skip("Statistics not enabled")
