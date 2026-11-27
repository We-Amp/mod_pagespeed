# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Pytest fixtures for the real-world website proxy test suite.

Assumes the realworld-tests Docker stack is already running.
Configure base URLs via environment variables:

    NGINX_URL      (default: http://localhost:8083)
    WORKER_API_URL (default: http://localhost:9882)

Requires: pytest, requests
"""

import os
import sys
import time

import pytest
import requests

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from common.worker_health import wait_worker_http

NGINX_URL = os.environ.get("NGINX_URL", "http://localhost:8083")
WORKER_API_URL = os.environ.get("WORKER_API_URL", "http://localhost:9882")
# The management API requires a bearer token, and the harness
# worker publishes it off-loopback on this compose network, so every /v1/*
# call except /v1/health carries one.  Kept in lockstep with
# entrypoint-worker-realworld.sh.
WORKER_API_TOKEN = os.environ.get("PAGESPEED_API_TOKEN", "realworld-test-token")
STARTUP_TIMEOUT = int(os.environ.get("STARTUP_TIMEOUT", "90"))


class PageSpeedClient:
    """HTTP client for nginx with PageSpeed helpers."""

    def __init__(self, base_url: str):
        self.base_url = base_url
        self.session = requests.Session()

    def get(
        self, path: str, headers: dict[str, str] | None = None, **kwargs
    ) -> requests.Response:
        kwargs.setdefault("timeout", 15)
        return self.session.get(self.base_url + path, headers=headers or {}, **kwargs)

    def poll_for_hit(
        self,
        path: str,
        timeout: float = 30.0,
        interval: float = 0.5,
        headers: dict[str, str] | None = None,
        expect_smaller_than: int = 0,
        expect_content_type: str = "",
    ) -> requests.Response:
        """Poll until X-PageSpeed: HIT (and optionally smaller body / content type).

        Returns the last response even on timeout so callers can assert
        on what actually happened.
        """
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                resp = self.get(path, headers=headers)
                last = resp
                if resp.headers.get("X-PageSpeed") == "HIT":
                    if expect_content_type:
                        ct = resp.headers.get("Content-Type", "")
                        if expect_content_type not in ct:
                            time.sleep(interval)
                            continue
                    if expect_smaller_than > 0:
                        if len(resp.content) < expect_smaller_than:
                            return resp
                    else:
                        return resp
            except requests.RequestException:
                pass
            time.sleep(interval)
        assert last is not None, f"No response received for {path} after {timeout}s"
        return last

    def poll_for_response(
        self,
        path: str,
        timeout: float = 30.0,
        interval: float = 0.5,
        headers: dict[str, str] | None = None,
    ) -> requests.Response:
        """Poll until any non-5xx response (for slow real sites)."""
        deadline = time.time() + timeout
        last_exc = None
        while time.time() < deadline:
            try:
                resp = self.get(path, headers=headers)
                if resp.status_code < 500:
                    return resp
            except requests.RequestException as e:
                last_exc = e
            time.sleep(interval)
        if last_exc:
            raise last_exc
        raise TimeoutError(f"No non-5xx response for {path} after {timeout}s")


class WorkerApiClient:
    """HTTP client for the worker /v1/* API."""

    def __init__(self, base_url: str, token: str = WORKER_API_TOKEN):
        self.base_url = base_url
        self.session = requests.Session()
        if token:
            self.session.headers["Authorization"] = f"Bearer {token}"

    def health(self) -> dict:
        """GET /v1/health -- status, uptime, readiness."""
        r = self.session.get(f"{self.base_url}/v1/health", timeout=5)
        r.raise_for_status()
        return r.json()

    def stats(self) -> dict:
        """GET /v1/stats -- processing counters and metrics."""
        r = self.session.get(f"{self.base_url}/v1/stats", timeout=5)
        r.raise_for_status()
        return r.json()

    def cache_urls(self, limit: int = 100) -> dict:
        """GET /v1/cache/urls -- list cached URLs with alternate counts."""
        r = self.session.get(
            f"{self.base_url}/v1/cache/urls", params={"limit": limit}, timeout=10
        )
        r.raise_for_status()
        return r.json()


def _wait_for_service(url: str, path: str, timeout: float):
    """Poll until the service returns a non-5xx response."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.get(url + path, timeout=5)
            if r.status_code < 500:
                return
        except requests.RequestException:
            pass
        time.sleep(1)
    raise TimeoutError(f"Service not ready at {url}{path} after {timeout}s")


def _get_nested_stat(stats: dict, keys: list[str]) -> int:
    """Extract a nested stat value, returning 0 if any key is missing."""
    val = stats
    for key in keys:
        val = val.get(key, {}) if isinstance(val, dict) else 0
    return int(val) if isinstance(val, (int, float)) else 0


def _poll_stat(
    worker_api: WorkerApiClient,
    keys: list[str],
    min_value: int = 1,
    timeout: float = 30,
) -> int:
    """Poll a nested worker stats counter until it reaches min_value."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            stats = worker_api.stats()
            val = _get_nested_stat(stats, keys)
            if val >= min_value:
                return val
        except Exception:
            pass
        time.sleep(1)
    return 0


@pytest.fixture(scope="session")
def nginx(wait_for_services) -> PageSpeedClient:
    return PageSpeedClient(NGINX_URL)


@pytest.fixture(scope="session")
def worker_api(wait_for_services) -> WorkerApiClient:
    return WorkerApiClient(WORKER_API_URL)


@pytest.fixture(scope="session")
def wait_for_services():
    """Wait for both nginx and worker API to be reachable."""
    _wait_for_service(NGINX_URL, "/health", STARTUP_TIMEOUT)
    _wait_for_service(WORKER_API_URL, "/v1/health", STARTUP_TIMEOUT)
    # Before any optimization-dependent test runs, confirm the worker
    # reports ready — a still-starting worker would otherwise manifest
    # as spurious cache-miss failures.
    wait_worker_http(WORKER_API_URL, timeout=STARTUP_TIMEOUT)
