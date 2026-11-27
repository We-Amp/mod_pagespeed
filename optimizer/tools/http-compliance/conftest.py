# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Pytest fixtures for HTTP compliance tests.

Manages Docker Compose services and provides HTTP clients for nginx
(with pagespeed), baseline nginx (without pagespeed), and direct origin
access.
"""

import os
import subprocess
import sys
import time

import pytest
import requests

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from common.async_css_loader_path import async_css_loader_path_from_source
from common.worker_health import wait_worker_socket

COMPLIANCE_DIR = os.path.dirname(os.path.abspath(__file__))
COMPOSE_FILE = os.path.join(COMPLIANCE_DIR, "docker-compose.yml")
NGINX_PORT = os.environ.get("COMPLIANCE_NGINX_PORT", "8180")
AGGRESSIVE_PORT = os.environ.get("COMPLIANCE_AGGRESSIVE_PORT", "8183")
ORIGIN_PORT = os.environ.get("COMPLIANCE_ORIGIN_PORT", "8181")
NGINX_URL = f"http://localhost:{NGINX_PORT}"
AGGRESSIVE_URL = f"http://localhost:{AGGRESSIVE_PORT}"
ORIGIN_URL = f"http://localhost:{ORIGIN_PORT}"
STARTUP_TIMEOUT = 60

# The async-CSS loader is served at a CONTENT-ADDRESSED path,
# /pagespeed_static/async_css.<hash>.js, where the hash is computed at C++
# compile time over kAsyncCssLoaderJs (src/worker/async_css_loader.h). The
# compliance worker has no critical-CSS pass, so it never injects the loader into
# a page to scrape the path from; and the loader is served unconditionally at
# preaccess regardless of injection. So we recompute the SAME path from the SAME
# source of truth and verify it end-to-end (a wrong recompute fails LOUD with a
# 404, never silently). The recompute is shared with tools/async-css-probe via
# tools/common/ — one copy, so the two harnesses cannot disagree.


class ComplianceClient:
    """HTTP client with helpers for HTTP compliance testing.

    Wraps requests.Session and adds assertion methods for verifying
    HTTP protocol correctness (Content-Length, headers, etc.).
    """

    def __init__(self, base_url: str):
        self.base_url = base_url
        self.session = requests.Session()

    def get(
        self, path: str, headers: dict[str, str] | None = None, **kwargs
    ) -> requests.Response:
        url = self.base_url + path
        return self.session.get(url, headers=headers or {}, **kwargs)

    def head(
        self, path: str, headers: dict[str, str] | None = None, **kwargs
    ) -> requests.Response:
        url = self.base_url + path
        return self.session.head(url, headers=headers or {}, **kwargs)

    def post(
        self,
        path: str,
        data: bytes | str | None = None,
        headers: dict[str, str] | None = None,
        **kwargs,
    ) -> requests.Response:
        url = self.base_url + path
        return self.session.post(url, data=data, headers=headers or {}, **kwargs)

    def put(
        self,
        path: str,
        data: bytes | str | None = None,
        headers: dict[str, str] | None = None,
        **kwargs,
    ) -> requests.Response:
        url = self.base_url + path
        return self.session.put(url, data=data, headers=headers or {}, **kwargs)

    def delete(
        self, path: str, headers: dict[str, str] | None = None, **kwargs
    ) -> requests.Response:
        url = self.base_url + path
        return self.session.delete(url, headers=headers or {}, **kwargs)

    def options(
        self, path: str, headers: dict[str, str] | None = None, **kwargs
    ) -> requests.Response:
        url = self.base_url + path
        return self.session.options(url, headers=headers or {}, **kwargs)

    def request(
        self, method: str, path: str, headers: dict[str, str] | None = None, **kwargs
    ) -> requests.Response:
        url = self.base_url + path
        return self.session.request(method, url, headers=headers or {}, **kwargs)

    def poll_for_hit(
        self,
        path: str,
        timeout: float = 8.0,
        interval: float = 0.25,
        headers: dict[str, str] | None = None,
        expect_smaller_than: int = 0,
    ) -> requests.Response:
        """Poll until X-PageSpeed: HIT is returned."""
        deadline = time.time() + timeout
        last_response = None
        while time.time() < deadline:
            try:
                resp = self.get(path, headers=headers)
                last_response = resp
                if resp.headers.get("X-PageSpeed") == "HIT":
                    if expect_smaller_than > 0:
                        if len(resp.content) < expect_smaller_than:
                            return resp
                    else:
                        return resp
            except requests.RequestException:
                pass
            time.sleep(interval)
        assert last_response is not None, f"No response for {path}"
        return last_response

    @staticmethod
    def assert_content_length_matches_body(response: requests.Response):
        """Assert Content-Length header matches actual body size.

        Skipped when Content-Encoding is present: Content-Length reflects
        the compressed wire size, but response.content is the decompressed
        body (requests library auto-decompresses). RFC 9110 Section 8.6.
        """
        if response.headers.get("Content-Encoding"):
            return  # Content-Length is compressed size; body is decompressed
        cl = response.headers.get("Content-Length")
        if cl is not None:
            expected = int(cl)
            actual = len(response.content)
            assert expected == actual, (
                f"Content-Length {expected} != body size {actual}"
            )

    @staticmethod
    def assert_miss(response: requests.Response):
        assert response.headers.get("X-PageSpeed") == "MISS", (
            f"Expected MISS, got: {response.headers.get('X-PageSpeed', '<absent>')}"
        )

    @staticmethod
    def assert_hit(response: requests.Response):
        assert response.headers.get("X-PageSpeed") == "HIT", (
            f"Expected HIT, got: {response.headers.get('X-PageSpeed', '<absent>')}"
        )

    @staticmethod
    def assert_has_date(response: requests.Response):
        """Assert Date header is present (RFC 9110 Section 6.6.1)."""
        assert "Date" in response.headers, "Missing Date header"

    @staticmethod
    def assert_valid_content_length(response: requests.Response):
        """Assert Content-Length is a valid non-negative decimal integer."""
        cl = response.headers.get("Content-Length")
        if cl is not None:
            assert cl.isdigit(), f"Content-Length not a valid integer: {cl}"
            assert int(cl) >= 0, f"Content-Length negative: {cl}"


def compose_run(*args):
    return subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE, *args],
        capture_output=True,
        text=True,
    )


def compose_check(*args):
    return subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE, *args],
        capture_output=True,
        text=True,
        check=True,
    )


def docker_exec(service, command):
    """Run a command inside a running Docker Compose service container."""
    if isinstance(command, str):
        cmd_parts = command.split()
    else:
        cmd_parts = list(command)
    return subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE, "exec", "-T", service] + cmd_parts,
        capture_output=True,
        text=True,
    )


def _wait_for_service(url: str, timeout: float = STARTUP_TIMEOUT):
    """Wait for an HTTP service to respond."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.get(url + "/small.html", timeout=2)
            if r.status_code < 500:
                return
        except requests.RequestException:
            pass
        time.sleep(0.5)
    raise TimeoutError(f"Service not ready at {url} after {timeout}s")


@pytest.fixture(scope="session")
def compliance_services():
    """Start Docker Compose services, wait for readiness, yield, tear down.

    Set COMPLIANCE_NO_LIFECYCLE=1 to skip build/up/down (when services are
    managed externally, e.g. by CI workflow steps).
    """
    no_lifecycle = os.environ.get("COMPLIANCE_NO_LIFECYCLE", "") == "1"

    if not no_lifecycle:
        compose_check("build")
        compose_check("up", "-d")
    try:
        _wait_for_service(NGINX_URL)
        _wait_for_service(AGGRESSIVE_URL)
        _wait_for_service(ORIGIN_URL)
        # Poll until worker sockets are ready (replaces fixed 2s sleep)
        for _ in range(10):
            try:
                requests.get(NGINX_URL + "/small.html", timeout=2)
                break
            except requests.RequestException:
                time.sleep(0.5)
        # Confirm the worker is actually ready before any compliance test
        # runs — tests that depend on optimization would otherwise time
        # out with unhelpful errors.
        wait_worker_socket(docker_exec, timeout=30)
        yield {
            "nginx_url": NGINX_URL,
            "aggressive_url": AGGRESSIVE_URL,
            "origin_url": ORIGIN_URL,
        }
    finally:
        if not no_lifecycle:
            logs = compose_run("logs", "--no-color")
            if logs.stdout:
                print("\n=== Docker Compose Logs ===")
                tail = logs.stdout[-5000:] if len(logs.stdout) > 5000 else logs.stdout
                print(tail)
            compose_run("down", "--remove-orphans", "-v")


@pytest.fixture
def client(compliance_services) -> ComplianceClient:
    """HTTP client pointed at nginx with pagespeed enabled."""
    return ComplianceClient(compliance_services["nginx_url"])


@pytest.fixture(scope="session")
def async_css_loader_path() -> str:
    """The content-addressed path the worker serves the async-CSS loader at,
    recomputed from src/worker/async_css_loader.h (the single source of truth)."""
    return async_css_loader_path_from_source()


@pytest.fixture
def baseline_client(compliance_services) -> ComplianceClient:
    """HTTP client pointed at nginx with pagespeed disabled (/baseline/)."""
    return ComplianceClient(compliance_services["nginx_url"])


@pytest.fixture
def aggressive_client(compliance_services) -> ComplianceClient:
    """HTTP client pointed at nginx with pagespeed in aggressive mode."""
    return ComplianceClient(compliance_services["aggressive_url"])


@pytest.fixture
def origin_client(compliance_services) -> ComplianceClient:
    """HTTP client pointed directly at the origin (bypass proxy)."""
    return ComplianceClient(compliance_services["origin_url"])
