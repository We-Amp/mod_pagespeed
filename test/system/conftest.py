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

"""pytest configuration and fixtures for PageSpeed system tests.

This module provides fixtures that match the environment variables and
setup from the bash system_test_helpers.sh.

Environment Variables:
    PAGESPEED_HOST: Primary server hostname (default: localhost)
    PAGESPEED_PORT: Primary server port (default: 80 for Apache)
    PAGESPEED_HTTPS_HOST: HTTPS server hostname
    PAGESPEED_HTTPS_PORT: HTTPS server port (default: 8443)
    PAGESPEED_SECONDARY_HOST: Secondary server for proxy tests
    PAGESPEED_SECONDARY_PORT: Secondary server port
    PAGESPEED_CACHE_DIR: Cache directory for flush tests
    PAGESPEED_STATS_ENABLED: Whether statistics are enabled (default: 1)
    PAGESPEED_TEST_ROOT: Root path for test pages (default: /mod_pagespeed_test)
    PAGESPEED_EXAMPLE_ROOT: Root path for example pages (default: /mod_pagespeed_example)
"""

import os
import pathlib
import time
from dataclasses import dataclass
from typing import Callable, Dict, Optional

import pytest

from pagespeed_test_framework.client import PageSpeedClient, ProxiedPageSpeedClient


@dataclass
class ServerConfig:
    """Configuration for PageSpeed test servers."""

    # Primary server
    host: str
    port: int

    # HTTPS server (optional)
    https_host: Optional[str]
    https_port: int

    # Secondary server for proxy tests (optional)
    secondary_host: Optional[str]
    secondary_port: Optional[int]

    # Paths
    test_root: str
    example_root: str
    cache_dir: str

    # Features
    stats_enabled: bool

    @property
    def primary_url(self) -> str:
        """Base URL for primary server."""
        return f"http://{self.host}:{self.port}"

    @property
    def https_url(self) -> Optional[str]:
        """Base URL for HTTPS server."""
        if self.https_host:
            return f"https://{self.https_host}:{self.https_port}"
        return None


@pytest.fixture(scope="session")
def server_config() -> ServerConfig:
    """Server configuration loaded from environment variables.

    This fixture reads configuration from environment variables to allow
    tests to run against different server setups.
    """
    return ServerConfig(
        host=os.environ.get("PAGESPEED_HOST", "localhost"),
        port=int(os.environ.get("PAGESPEED_PORT", "80")),
        https_host=os.environ.get("PAGESPEED_HTTPS_HOST"),
        https_port=int(os.environ.get("PAGESPEED_HTTPS_PORT", "8443")),
        secondary_host=os.environ.get("PAGESPEED_SECONDARY_HOST"),
        secondary_port=(
            int(os.environ.get("PAGESPEED_SECONDARY_PORT"))
            if os.environ.get("PAGESPEED_SECONDARY_PORT")
            else None
        ),
        test_root=os.environ.get("PAGESPEED_TEST_ROOT", "/mod_pagespeed_test"),
        example_root=os.environ.get("PAGESPEED_EXAMPLE_ROOT", "/mod_pagespeed_example"),
        cache_dir=os.environ.get("PAGESPEED_CACHE_DIR", "/var/cache/pagespeed"),
        stats_enabled=os.environ.get("PAGESPEED_STATS_ENABLED", "1") == "1",
    )


@pytest.fixture(scope="session")
def client(server_config: ServerConfig) -> PageSpeedClient:
    """Primary server HTTP client.

    Equivalent to: $WGET_DUMP requests in bash tests
    """
    return PageSpeedClient(
        host=server_config.host,
        port=server_config.port,
    )


@pytest.fixture(scope="session")
def https_client(server_config: ServerConfig) -> PageSpeedClient:
    """HTTPS server client.

    Equivalent to: $WGET_DUMP_HTTPS requests in bash tests
    """
    if not server_config.https_host:
        pytest.skip("HTTPS server not configured (set PAGESPEED_HTTPS_HOST)")

    return PageSpeedClient(
        host=server_config.https_host,
        port=server_config.https_port,
        use_https=True,
    )


@pytest.fixture(scope="session")
def secondary_client(server_config: ServerConfig) -> ProxiedPageSpeedClient:
    """Secondary server client for proxy tests.

    Equivalent to: http_proxy=$SECONDARY_HOSTNAME requests in bash tests
    """
    if not server_config.secondary_host:
        pytest.skip("Secondary server not configured (set PAGESPEED_SECONDARY_HOST)")

    return ProxiedPageSpeedClient(
        host=server_config.host,
        port=server_config.port,
        proxy_host=server_config.secondary_host,
        proxy_port=server_config.secondary_port,
    )


@pytest.fixture
def example_root(server_config: ServerConfig) -> str:
    """Root path for example pages.

    Equivalent to: $EXAMPLE_ROOT in bash tests
    """
    return server_config.example_root


@pytest.fixture
def test_root(server_config: ServerConfig) -> str:
    """Root path for test pages.

    Equivalent to: $TEST_ROOT in bash tests
    """
    return server_config.test_root


@pytest.fixture
def rewritten_root(server_config: ServerConfig) -> str:
    """Root path for rewritten resources.

    Equivalent to: $REWRITTEN_ROOT in bash tests
    This is typically the same as example_root.
    """
    return os.environ.get("PAGESPEED_REWRITTEN_ROOT", server_config.example_root)


@pytest.fixture
def stats_snapshot(client: PageSpeedClient, server_config: ServerConfig) -> Callable[[], Dict[str, int]]:
    """Factory fixture for capturing statistics snapshots.

    Usage:
        def test_something(stats_snapshot):
            old_stats = stats_snapshot()
            # do something
            new_stats = stats_snapshot()
            assert_stat_delta(old_stats, new_stats, "counter", 1)

    Equivalent to: $WGET_DUMP $STATISTICS_URL in bash tests
    """
    if not server_config.stats_enabled:
        pytest.skip("Statistics not enabled (set PAGESPEED_STATS_ENABLED=1)")

    def _capture() -> Dict[str, int]:
        return client.get_statistics()

    return _capture


@pytest.fixture
def flush_cache(server_config: ServerConfig) -> Callable[[], None]:
    """Fixture to flush the PageSpeed cache.

    Equivalent to: touch $MOD_PAGESPEED_CACHE/cache.flush in bash tests

    Usage:
        def test_cache(flush_cache):
            flush_cache()
            # cache is now cleared
    """

    def _flush() -> None:
        cache_flush_path = pathlib.Path(server_config.cache_dir) / "cache.flush"

        if not cache_flush_path.parent.exists():
            pytest.skip(f"Cache directory not found: {server_config.cache_dir}")

        # Touch the cache.flush file
        cache_flush_path.touch()

        # Wait for cache flush to be detected (poll interval)
        time.sleep(1.5)

    return _flush


@pytest.fixture
def webp_client(client: PageSpeedClient) -> PageSpeedClient:
    """Client configured for WebP image requests.

    Equivalent to: --user-agent=webp* in bash tests
    """
    return client.with_webp()


# Markers for test categorization
def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line(
        "markers", "requires_secondary: test requires secondary server"
    )
    config.addinivalue_line(
        "markers", "requires_https: test requires HTTPS server"
    )
    config.addinivalue_line(
        "markers", "requires_stats: test requires statistics enabled"
    )
    config.addinivalue_line(
        "markers", "slow: test takes a long time to run"
    )


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
