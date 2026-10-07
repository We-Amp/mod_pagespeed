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
    PAGESPEED_CACHE_DIR: Cache directory for flush tests (no default; the
        flush_cache fixture fails, rather than skips, when it is unset or
        unusable)
    PAGESPEED_STATS_ENABLED: Whether statistics are enabled (default: 1)
    PAGESPEED_TEST_ROOT: Root path for test pages (default: /mod_pagespeed_test)
    PAGESPEED_EXAMPLE_ROOT: Root path for example pages (default: /mod_pagespeed_example)
    PAGESPEED_STATS_PATH: Path to statistics endpoint (default: /mod_pagespeed_statistics)
    PAGESPEED_ADMIN_PATH: Path to admin endpoint (default: /pagespeed_admin)
    PAGESPEED_SERVER_TYPE: Server type: apache, envoy, iis, or nginx (default: auto-detect)
    PAGESPEED_DOC_ROOT: Document root the test runner may write scratch
        content under (lane fixture doc_root_scratch)
    PAGESPEED_BEACON_PATH: Beacon handler path (default: /ngx_pagespeed_beacon
        on nginx, /mod_pagespeed_beacon elsewhere)
    PAGESPEED_LANE_FIXTURES: Comma-separated lane fixtures the lane runner
        provisioned (see KNOWN_LANE_FIXTURES)
"""

import os
import pathlib
import time
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional, Tuple

import pytest

from pagespeed_test_framework.client import (
    PageSpeedClient,
    ProxiedPageSpeedClient,
    VhostClient,
    # Imported (not re-parsed here) so the poll budget in client.py and the
    # pytest cap below can never drift on parsing semantics (invalid/<=0 ->
    # default). See pytest_configure.
    _read_fetch_until_retries,
    _read_timeout_multiplier,
)


# ---------------------------------------------------------------------------
# Lane fixtures
# ---------------------------------------------------------------------------
# A lane fixture is a named piece of test-only server configuration that a
# lane runner provisions and advertises in PAGESPEED_LANE_FIXTURES
# (comma-separated). Tests that depend on one carry
# @pytest.mark.requires_fixture("<name>", ...): they run where the lane
# provides it and skip -- a lane capability, never a product artifact --
# where it does not. A misspelled name is a configuration error and fails
# loudly on both sides (marker and runner), so a typo can never turn into a
# test that silently skips on every lane.
KNOWN_LANE_FIXTURES = frozenset({
    # <Directory>/<Location> test blocks copied from install/debug.conf.template
    "debug_conf_dirs",
    # /alt/admin/path, /pagespeed_console, /mod_pagespeed_global_statistics
    # and the server-scope ModPagespeedMessagesDomains list
    "admin_handlers",
    # statistics log at PAGESPEED_STATS_LOG, run start at PAGESPEED_LANE_START_MS
    "stats_log",
    # ModPagespeedCompressMetadataCache on the primary vhost
    "compressed_metadata_cache",
    # PAGESPEED_CACHE_DIR writable by the test runner (flush_cache works)
    "cache_flush",
    # PAGESPEED_DOC_ROOT with runner-writable purge/ and cache_flush/ trees
    "doc_root_scratch",
    # the named VirtualHosts on the secondary port
    "secondary_vhosts",
    # local stand-ins for selfsigned.modpagespeed.com, www.gstatic.com and
    # (https) www.modpagespeed.com
    "external_origin",
    # chunked slow-flushing origin at /mod_pagespeed_test/flush_origin/
    "flush_origin",
    # the experiment.example.com vhost (formerly install/debug.conf.template's
    # #EXPERIMENT_GA block) on the secondary port
    "experiment_framework",
    # the twelve remote-config vhosts (formerly install/debug.conf.template's
    # #REMOTE_CONFIG block) plus pathological_server.py on RCPORT
    "remote_config",
    # option response headers (PageSpeed / ModPagespeed / PageSpeedFilters)
    # set by the server on option_headers/{off,modpagespeed_off,on}.html
    "option_response_headers",
})


def parse_lane_fixtures(raw: str) -> frozenset:
    """Names in a comma-separated PAGESPEED_LANE_FIXTURES value."""
    return frozenset(name.strip() for name in raw.split(",") if name.strip())


def unknown_lane_fixtures(names: Iterable[str]) -> List[str]:
    """The names (in order) that are not in KNOWN_LANE_FIXTURES."""
    return [name for name in names if name not in KNOWN_LANE_FIXTURES]


def missing_lane_fixtures(names: Iterable[str], provided: frozenset) -> List[str]:
    """The names (in order) the lane did not provide."""
    return [name for name in names if name not in provided]


def validate_advertised_lane_fixtures(raw: str) -> None:
    """Reject a runner that advertises a fixture name conftest does not know."""
    unknown = unknown_lane_fixtures(sorted(parse_lane_fixtures(raw)))
    if unknown:
        raise pytest.UsageError(
            f"PAGESPEED_LANE_FIXTURES names unknown lane fixture(s) {unknown}; "
            f"known: {sorted(KNOWN_LANE_FIXTURES)}"
        )


def wait_until_second_after(
    stamp_sec: int,
    now: Callable[[], float] = time.time,
    sleep: Callable[[float], None] = time.sleep,
    deadline_s: float = 30.0,
) -> bool:
    """Block until the wall clock has left the whole second ``stamp_sec``.

    A cache flush is stamped as a whole second, and the server declares a
    cache entry stale when its Date header -- itself whole-second -- is <=
    that stamp (PurgeSet::IsValid; HTTPCache::Put assumes Date > stamp).
    So everything PageSpeed writes to its caches during the rest of the
    flush second is stale on arrival: an HTML rewrite in that second mints
    .pagespeed. URLs that are never served from cache, and when the page was
    rewritten with options the bare URL does not carry (query-string
    ``include_js_source_maps`` in test_source_maps), reconstruction yields a
    different hash and the URL is served as ``max-age=300,private`` for good.
    flush_cache therefore returns only once this second is over, so no
    later cache write can share it. Returns False if ``deadline_s`` of
    sleeping passes first (clock skew), True otherwise.
    """
    slept = 0.0
    while int(now()) <= stamp_sec:
        if slept >= deadline_s:
            return False
        sleep(0.05)
        slept += 0.05
    return True


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
    # None when PAGESPEED_CACHE_DIR is unset. There is deliberately no default:
    # every lane uses a different FileCachePath, so any built-in guess is wrong
    # somewhere and fails silently at flush time.
    cache_dir: Optional[str]

    # Statistics and admin paths
    stats_path: str
    admin_path: str

    # Server type
    server_type: str  # "apache", "envoy", "iis", "nginx", or "auto"

    # Features
    stats_enabled: bool

    # Document root the test runner may write scratch content under (lane
    # fixture doc_root_scratch). None when PAGESPEED_DOC_ROOT is unset.
    doc_root: Optional[str] = None

    # Beacon handler path: bash $BEACON_HANDLER (mod_pagespeed_beacon on
    # Apache, ngx_pagespeed_beacon on nginx).
    beacon_path: str = "/mod_pagespeed_beacon"

    # The whole-cache purge (purge=*) is answered only by the global admin.
    global_admin_path: str = "/pagespeed_global_admin"

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

    @property
    def is_iis(self) -> bool:
        """Check if running against IIS."""
        return self.server_type == "iis"

    @property
    def is_nginx(self) -> bool:
        """Check if running against nginx."""
        return self.server_type == "nginx"

    @property
    def is_windows(self) -> bool:
        """Check if cache directory is on Windows.

        Infers the platform from the shape of cache_dir, so it is only
        meaningful when PAGESPEED_CACHE_DIR is set; with no cache dir
        configured there is nothing to infer from and this reports False.
        """
        # Windows paths start with drive letter or use backslashes
        cache_dir = self.cache_dir or ""
        return (len(cache_dir) > 1 and cache_dir[1] == ':') or '\\' in cache_dir


@pytest.fixture(scope="session")
def server_config() -> ServerConfig:
    """Server configuration loaded from environment variables.

    This fixture reads configuration from environment variables to allow
    tests to run against different server setups.
    """
    server_type = os.environ.get("PAGESPEED_SERVER_TYPE", "auto")

    # Auto-detect statistics path based on server type
    if server_type in ("iis", "envoy", "nginx"):
        default_stats_path = "/pagespeed_statistics"
        default_admin_path = "/pagespeed_admin"
    else:
        default_stats_path = "/mod_pagespeed_statistics"
        default_admin_path = "/pagespeed_admin"

    default_beacon_path = (
        "/ngx_pagespeed_beacon" if server_type == "nginx" else "/mod_pagespeed_beacon"
    )

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
        # No default: see ServerConfig.cache_dir. Tests that need it fail loudly.
        cache_dir=os.environ.get("PAGESPEED_CACHE_DIR"),
        stats_path=os.environ.get("PAGESPEED_STATS_PATH", default_stats_path),
        admin_path=os.environ.get("PAGESPEED_ADMIN_PATH", default_admin_path),
        global_admin_path=os.environ.get(
            "PAGESPEED_GLOBAL_ADMIN_PATH", "/pagespeed_global_admin"
        ),
        server_type=server_type,
        stats_enabled=os.environ.get("PAGESPEED_STATS_ENABLED", "1") == "1",
        doc_root=os.environ.get("PAGESPEED_DOC_ROOT") or None,
        beacon_path=os.environ.get("PAGESPEED_BEACON_PATH", default_beacon_path),
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


@pytest.fixture(scope="session")
def vhost_client(server_config: ServerConfig) -> Callable[[str], VhostClient]:
    """Factory: a client for one name-based vhost on the secondary port.

    Equivalent to: http_proxy=$SECONDARY_HOSTNAME wget http://<vhost>/...

    Usage:
        @pytest.mark.requires_fixture("secondary_vhosts")
        def test_x(vhost_client):
            client = vhost_client("signed-urls.example.com")
            client.get("/mod_pagespeed_example/index.html")
    """
    if not server_config.secondary_host or not server_config.secondary_port:
        pytest.skip(
            "Secondary server not configured (set PAGESPEED_SECONDARY_HOST "
            "and PAGESPEED_SECONDARY_PORT)"
        )

    def _make(vhost: str) -> VhostClient:
        return VhostClient(
            vhost=vhost,
            proxy_host=server_config.secondary_host,
            proxy_port=server_config.secondary_port,
        )

    return _make


@pytest.fixture
def vhost_stats_snapshot(
    vhost_client: Callable[[str], VhostClient], server_config: ServerConfig
) -> Callable[[str], Dict[str, int]]:
    """Factory: per-vhost statistics of one named vhost.

    Usage:
        before = vhost_stats_snapshot("purge.example.com")
    """
    if not server_config.stats_enabled:
        pytest.skip("Statistics not enabled (set PAGESPEED_STATS_ENABLED=1)")

    def _capture(vhost: str) -> Dict[str, int]:
        return _make_stats_capture(vhost_client(vhost), server_config)()

    return _capture


@pytest.fixture(scope="session")
def doc_root(server_config: ServerConfig) -> pathlib.Path:
    """The primary server's document root (lane fixture doc_root_scratch)."""
    if not server_config.doc_root:
        pytest.skip("Document root not configured (set PAGESPEED_DOC_ROOT)")
    path = pathlib.Path(server_config.doc_root)
    if not path.is_dir():
        pytest.fail(f"PAGESPEED_DOC_ROOT={path} is not a directory")
    return path


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


def _make_stats_capture(
    stats_client: PageSpeedClient, server_config: ServerConfig
) -> Callable[[], Dict[str, int]]:
    """Create a stats-capture callable for the given client.

    Helper used by both stats_snapshot and secondary_stats_snapshot fixtures.
    """
    # nginx admin endpoints don't work with ?PageSpeed=off, so disable it for nginx
    disable_pagespeed = not (server_config.is_nginx or server_config.server_type == "envoy")

    def _capture() -> Dict[str, int]:
        return stats_client.get_statistics(
            stats_path=server_config.stats_path,
            disable_pagespeed=disable_pagespeed
        )

    return _capture


@pytest.fixture
def stats_snapshot(client: PageSpeedClient, server_config: ServerConfig) -> Callable[[], Dict[str, int]]:
    """Factory fixture for capturing statistics snapshots from the primary vhost.

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

    return _make_stats_capture(client, server_config)


@pytest.fixture
def secondary_stats_snapshot(
    secondary_client: PageSpeedClient, server_config: ServerConfig
) -> Callable[[], Dict[str, int]]:
    """Factory fixture for capturing statistics snapshots from the secondary vhost.

    With per-vhost statistics, the secondary vhost (used for IPRO tests) tracks
    its own counters. Tests that send requests via secondary_client must read
    stats from the same vhost to see correct deltas.

    Usage:
        def test_ipro(secondary_client, secondary_stats_snapshot):
            old_stats = secondary_stats_snapshot()
            secondary_client.get(url)
            new_stats = secondary_stats_snapshot()
            assert_stat_delta(old_stats, new_stats, "ipro_served", 1)
    """
    if not server_config.stats_enabled:
        pytest.skip("Statistics not enabled (set PAGESPEED_STATS_ENABLED=1)")

    return _make_stats_capture(secondary_client, server_config)


@pytest.fixture
def flush_cache(server_config: ServerConfig, client: PageSpeedClient) -> Callable[[], None]:
    """Fixture to flush the PageSpeed cache.

    Equivalent to: touch $MOD_PAGESPEED_CACHE/cache.flush in bash tests

    For IIS, we use the admin API to flush cache if the file touch method doesn't work.

    A cache directory that is unset, missing, or unwritable is an environment
    or product defect, not a reason to pass silently: every exit path below
    that did not flush the cache calls pytest.fail().

    Usage:
        def test_cache(flush_cache):
            flush_cache()
            # cache is now cleared
    """

    def _flush_stats() -> Optional[Tuple[Optional[int], Optional[int]]]:
        """(cache_flush_count, cache_flush_timestamp_ms), or None if unreadable.

        Reuses the same read path as stats_snapshot/_make_stats_capture
        (client.get_statistics against server_config.stats_path). Returns
        None -- never raises -- on any read failure, so callers can fall back
        to a fixed sleep instead of polling forever. Either counter may be
        None when the server does not export it.
        """
        try:
            disable_pagespeed = not (
                server_config.is_nginx or server_config.server_type == "envoy"
            )
            stats = client.get_statistics(
                stats_path=server_config.stats_path,
                disable_pagespeed=disable_pagespeed,
            )
        except Exception:
            return None
        return stats.get("cache_flush_count"), stats.get("cache_flush_timestamp_ms")

    def _flush() -> None:
        cache_dir = server_config.cache_dir
        if not cache_dir:
            pytest.fail(
                "Cannot flush cache: PAGESPEED_CACHE_DIR is not set and has no "
                "default -- every lane uses a different FileCachePath (Apache: "
                "/var/cache/mod_pagespeed, nginx/Envoy/IIS: see the lane's "
                "run_*_tests script). Export PAGESPEED_CACHE_DIR to the cache "
                "directory the server under test is actually configured with."
            )

        # Try file-based cache flush first
        cache_flush_path = pathlib.Path(cache_dir) / "cache.flush"
        cache_dir_exists = cache_flush_path.parent.exists()
        touch_error: Optional[OSError] = None

        if cache_dir_exists:
            try:
                # Read the statistics BEFORE touching, so any change the poll
                # below sees is genuinely caused by this flush.
                before = _flush_stats()
                # The server reads cache.flush's mtime at whole-second
                # granularity, so a touch within the same second as the
                # previous flush is invisible to it (bash slept 2 s between
                # touches for this reason). Give the file an mtime on a whole
                # second strictly later than its current one, waiting for the
                # wall clock to reach it so the stamp is never in the future.
                previous_sec = (
                    int(cache_flush_path.stat().st_mtime)
                    if cache_flush_path.exists()
                    else 0
                )
                cache_flush_path.touch()
                target_sec = max(int(time.time()), previous_sec + 1)
                clock_deadline = time.monotonic() + 30.0
                while int(time.time()) < target_sec:
                    if time.monotonic() > clock_deadline:
                        pytest.fail(
                            f"the wall clock did not reach {target_sec} within 30 s "
                            f"(cache.flush carries mtime {previous_sec}; clock skew?)"
                        )
                    time.sleep(0.1)
                os.utime(cache_flush_path, (target_sec, target_sec))
                if server_config.is_iis:
                    # The IIS lane runs with cache purging enabled, where the
                    # server watches cache.purge rather than cache.flush, so
                    # the touch is not reflected in the flush statistics. Keep
                    # the historical fixed wait there.
                    time.sleep(1.5)
                    return
                if before is None:
                    # Statistics unavailable on this lane: detection cannot be
                    # confirmed. The lanes poll cache.flush every second, so
                    # 2.5 s covers the interval with margin.
                    time.sleep(2.5)
                    return
                # Bounded poll for the server to notice the flush. The
                # timestamp statistic is the server's own record of the last
                # flush it applied, so it is compared against the stamp we
                # wrote; the count is a fallback for servers without it (it is
                # shared across vhosts and also moves on purges).
                before_count, _ = before
                deadline = time.monotonic() + 15.0
                after: Optional[Tuple[Optional[int], Optional[int]]] = before
                recorded = False
                while time.monotonic() < deadline:
                    time.sleep(0.5)
                    after = _flush_stats()
                    if after is None:
                        continue
                    after_count, after_ts = after
                    if after_ts is not None:
                        recorded = after_ts >= target_sec * 1000
                    else:
                        recorded = (
                            after_count is not None
                            and before_count is not None
                            and after_count > before_count
                        )
                    if recorded:
                        break
                if not recorded:
                    pytest.fail(
                        f"cache.flush at {cache_flush_path} was touched (mtime "
                        f"{target_sec}) but the server did not record the flush "
                        f"within 15 s (statistics before={before}, after={after})"
                    )
                # The flush is applied. Now leave its second, so no cache
                # entry written by the rest of this test or the next one
                # carries a Date equal to the stamp and is stale on arrival
                # (see wait_until_second_after).
                if not wait_until_second_after(target_sec):
                    pytest.fail(
                        f"the wall clock did not leave second {target_sec} within "
                        f"30 s of the flush being recorded (clock skew?)"
                    )
                return
            except OSError as exc:
                # File method didn't work, try admin API for IIS
                touch_error = exc

        # Try admin API endpoint for cache flush (works for IIS)
        admin_error: Optional[Exception] = None
        if server_config.is_iis:
            try:
                flush_url = f"{server_config.admin_path}?cache_flush=1"
                client.get(flush_url)
                time.sleep(1.0)
                return
            except Exception as exc:
                admin_error = exc

        # Nothing flushed the cache. nginx and Apache both use the file-based
        # flush (touch cache.flush), so the file failure above is terminal.
        detail = f" IIS admin-API fallback also failed: {admin_error!r}." if admin_error else ""
        if not cache_dir_exists:
            pytest.fail(
                f"Cannot flush cache: cache directory does not exist: {cache_dir} "
                f"(PAGESPEED_CACHE_DIR). Point it at the server's FileCachePath, "
                f"and check the server actually created it.{detail}"
            )
        pytest.fail(
            f"Cannot flush cache: cannot write {cache_flush_path}: {touch_error!r}. "
            f"The test runner needs write access to the server's cache "
            f"directory (PAGESPEED_CACHE_DIR).{detail}"
        )

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
    # A runner that advertises a misspelled fixture fails the run up front.
    validate_advertised_lane_fixtures(os.environ.get("PAGESPEED_LANE_FIXTURES", ""))

    # Scale pytest's per-test timeout past fetch_until's worst-case wall
    # clock: ini timeout x PAGESPEED_TEST_TIMEOUT_MULTIPLIER x
    # (1 + PAGESPEED_TEST_FETCH_RETRIES).
    #
    # The multiplier term: slow environments (AppVerif + Page Heap, 5-50x
    # slowdown) must not hit the 120s pytest ceiling before fetch_until's
    # multiplier-scaled budget runs out -- without this, bumping the
    # multiplier is a no-op for any test whose wall clock exceeds 120s.
    #
    # The retry term: with PAGESPEED_TEST_FETCH_RETRIES > 0 one
    # fetch_until costs up to base x multiplier x (1 + retries). At the
    # AppVerif matrix's values (multiplier 6, retries 1) a 60s-base test
    # needs exactly 720s -- equal to the old 120x6 cap, so pytest could fire
    # at the very instant the retry budget ended, pre-empting fetch_until's
    # own TimeoutError (and its evidence capture) or killing a poll seconds
    # before convergence. Two-poll tests (2 x 30s base) needed ~728s > 720s,
    # and 120s-base tests (test_js_blacklist et al.) need 1440s and were
    # killed mid-FIRST attempt, so the widened budget never applied at all.
    # The cap must sit ABOVE fetch_until's worst case so pytest's timeout
    # always loses to the harness's own, better-instrumented one.
    multiplier = _read_timeout_multiplier()
    attempts = 1 + _read_fetch_until_retries()
    if multiplier > 1.0 or attempts > 1:
        base_timeout = int(config.getini("timeout") or 120)
        config.option.timeout = int(base_timeout * multiplier * attempts)

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
    config.addinivalue_line(
        "markers", "apache_only: test only runs on Apache"
    )
    config.addinivalue_line(
        "markers", "envoy_only: test only runs on Envoy"
    )
    config.addinivalue_line(
        "markers", "iis_only: test only runs on IIS with PageSpeed module"
    )
    config.addinivalue_line(
        "markers", "not_iis: test does not run on IIS (missing feature)"
    )
    config.addinivalue_line(
        "markers", "not_envoy: test does not run on Envoy (missing feature)"
    )
    config.addinivalue_line(
        "markers", "nginx_only: test only runs on nginx"
    )
    config.addinivalue_line(
        "markers", "not_nginx: test does not run on nginx (missing feature)"
    )
    config.addinivalue_line(
        "markers", "requires_module: test requires PageSpeed module to be installed"
    )
    config.addinivalue_line(
        "markers",
        "process_leak: process-level worker/child lifecycle regression test "
        "(graceful restart / reload); needs server control env vars",
    )
    config.addinivalue_line(
        "markers",
        "requires_fixture(*names): test needs these lane fixtures "
        "(PAGESPEED_LANE_FIXTURES; see conftest.KNOWN_LANE_FIXTURES)",
    )


# An unknown requires_fixture(...) name is a configuration error, not a
# per-test, per-lane concern -- fail collection for every offending item
# before any test runs, on every lane, so a requires_secondary/https/stats
# skip in pytest_runtest_setup (which can return early) never gets a chance
# to mask a typo'd fixture name behind a clean-looking SKIP.
def pytest_collection_modifyitems(config, items):
    """Hard-fail collection when requires_fixture(...) names an unknown fixture."""
    errors = []
    for item in items:
        for marker in item.iter_markers("requires_fixture"):
            names = list(marker.args)
            unknown = unknown_lane_fixtures(names)
            if not names or unknown:
                errors.append(
                    f"{item.nodeid}: requires_fixture{tuple(names)}: unknown lane "
                    f"fixture name(s) {unknown}"
                )
    if errors:
        raise pytest.UsageError(
            "unknown requires_fixture lane fixture name(s):\n"
            + "\n".join(errors)
            + f"\nknown: {sorted(KNOWN_LANE_FIXTURES)}"
        )


# Skip tests based on server configuration
def pytest_runtest_setup(item):
    """Skip tests based on markers and server configuration."""
    # Get server_type from environment since fixtures aren't resolved yet
    server_type = os.environ.get("PAGESPEED_SERVER_TYPE", "auto")
    secondary_host = os.environ.get("PAGESPEED_SECONDARY_HOST")
    https_host = os.environ.get("PAGESPEED_HTTPS_HOST")
    stats_enabled = os.environ.get("PAGESPEED_STATS_ENABLED", "1") == "1"

    if item.get_closest_marker("requires_secondary"):
        if not secondary_host:
            pytest.skip("Secondary server not configured")

    if item.get_closest_marker("requires_https"):
        if not https_host:
            pytest.skip("HTTPS server not configured")

    if item.get_closest_marker("requires_stats"):
        if not stats_enabled:
            pytest.skip("Statistics not enabled")

    provided = parse_lane_fixtures(os.environ.get("PAGESPEED_LANE_FIXTURES", ""))
    for marker in item.iter_markers("requires_fixture"):
        names = list(marker.args)
        missing = missing_lane_fixtures(names, provided)
        if missing:
            pytest.skip(f"Lane does not provide fixture(s): {', '.join(missing)}")

    # Server-type specific markers
    if item.get_closest_marker("apache_only"):
        if server_type not in ("apache", "auto"):
            pytest.skip("Test only runs on Apache")

    if item.get_closest_marker("envoy_only"):
        if server_type not in ("envoy", "auto"):
            pytest.skip("Test only runs on Envoy")

    if item.get_closest_marker("iis_only"):
        if server_type != "iis":
            pytest.skip("Test only runs on IIS")
        # iis_only tests also require the module to be installed
        if not stats_enabled:
            pytest.skip("Test requires PageSpeed module (stats not enabled)")

    if item.get_closest_marker("not_iis"):
        if server_type == "iis":
            pytest.skip("Test not supported on IIS")

    if item.get_closest_marker("not_envoy"):
        if server_type == "envoy":
            pytest.skip("Test not supported on Envoy")

    if item.get_closest_marker("nginx_only"):
        if server_type != "nginx":
            pytest.skip("Test only runs on nginx")

    if item.get_closest_marker("not_nginx"):
        if server_type == "nginx":
            pytest.skip("Test not supported on nginx")

    if item.get_closest_marker("requires_module"):
        if not stats_enabled:
            pytest.skip("Test requires PageSpeed module (stats not enabled)")
