# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Pytest fixtures for mod_pagespeed stress tests.

Manages Docker Compose services as a session-scoped fixture and provides
HTTP clients, IPC clients, and metrics helpers.
"""

import os
import subprocess
import sys
import time
from dataclasses import dataclass

import metrics_helpers
import pytest
import requests

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from common.worker_health import wait_worker_socket

STRESS_DIR = os.path.dirname(os.path.abspath(__file__))
# STRESS_COMPOSE_FILE env var overrides the default path (needed when pytest
# runs inside a container but docker compose was started on the host).
# Supports multiple colon-separated paths (e.g. "base.yml:override.yml").
_compose_env = os.environ.get("STRESS_COMPOSE_FILE", "")
if _compose_env:
    COMPOSE_FILES = _compose_env.split(":")
else:
    COMPOSE_FILES = [os.path.join(STRESS_DIR, "docker-compose.yml")]
STRESS_NGINX_PORT = os.environ.get("STRESS_NGINX_PORT", "8190")
STRESS_ORIGIN_PORT = os.environ.get("STRESS_ORIGIN_PORT", "8191")
NGINX_URL = f"http://localhost:{STRESS_NGINX_PORT}"
ORIGIN_URL = f"http://localhost:{STRESS_ORIGIN_PORT}"
STARTUP_TIMEOUT = 90  # seconds to wait for services


@dataclass
class ServiceInfo:
    """Addresses of running stress test services."""

    nginx_url: str
    origin_url: str


class PageSpeedClient:
    """HTTP client wrapping requests.Session with PageSpeed helpers."""

    def __init__(self, base_url):
        self.base_url = base_url
        self.session = requests.Session()

    def get(self, path, headers=None, **kwargs):
        url = self.base_url + path
        return self.session.get(url, headers=headers or {}, **kwargs)

    def poll_for_hit(
        self, path, timeout=15.0, interval=0.5, headers=None, expect_smaller_than=0
    ):
        """Poll until X-PageSpeed: HIT is returned, or timeout."""
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
        assert last_response is not None, f"No response received for {path}"
        return last_response

    @staticmethod
    def assert_miss(response):
        assert response.headers.get("X-PageSpeed") == "MISS", (
            f"Expected MISS, got: {response.headers.get('X-PageSpeed', '<absent>')}"
        )

    @staticmethod
    def assert_hit(response):
        assert response.headers.get("X-PageSpeed") == "HIT", (
            f"Expected HIT, got: {response.headers.get('X-PageSpeed', '<absent>')}"
        )


def _compose_file_args():
    """Return list of -f arguments for docker compose."""
    args = []
    for f in COMPOSE_FILES:
        args.extend(["-f", f])
    return args


def compose_run(*args):
    """Run docker compose command."""
    return subprocess.run(
        ["docker", "compose"] + _compose_file_args() + list(args),
        capture_output=True,
        text=True,
    )


def compose_check(*args):
    """Run docker compose command, raising on failure."""
    return subprocess.run(
        ["docker", "compose"] + _compose_file_args() + list(args),
        capture_output=True,
        text=True,
        check=True,
    )


def docker_exec(service, command):
    """Run a command in a running Docker Compose service container."""
    if isinstance(command, str):
        cmd_parts = command.split()
    else:
        cmd_parts = list(command)
    return subprocess.run(
        ["docker", "compose"]
        + _compose_file_args()
        + ["exec", "-T", service]
        + cmd_parts,
        capture_output=True,
        text=True,
    )


def _wait_for_nginx(url, timeout=STARTUP_TIMEOUT):
    """Wait for nginx to respond with a non-5xx status."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.get(url + "/index.html", timeout=2)
            if r.status_code < 500:
                return
        except requests.RequestException:
            pass
        time.sleep(1)
    raise TimeoutError(f"Nginx not ready at {url} after {timeout}s")


@pytest.fixture(scope="session")
def stress_services():
    """Start Docker Compose services, wait for readiness, yield, tear down."""
    # Set compose file paths for metrics_helpers
    metrics_helpers.COMPOSE_FILES = COMPOSE_FILES

    # Check if stack is already running (for iterative development)
    already_running = False
    try:
        r = requests.get(NGINX_URL + "/index.html", timeout=2)
        if r.status_code < 500:
            already_running = True
    except requests.RequestException:
        pass

    if not already_running:
        # Build images
        compose_check("build")
        # Start all services
        compose_check("up", "-d")

    try:
        _wait_for_nginx(NGINX_URL)
        # Poll worker health instead of blind wait
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                health = metrics_helpers.get_health_via_docker(timeout=3)
                if "OK" in health:
                    break
            except Exception:
                pass
            time.sleep(0.5)
        # Confirm the worker is actually ready so stress runs don't time
        # out with vague "expected HIT" errors.
        wait_worker_socket(docker_exec, timeout=30)
        yield ServiceInfo(nginx_url=NGINX_URL, origin_url=ORIGIN_URL)
    finally:
        logs = compose_run("logs", "--no-color")
        if logs.stdout:
            log_file = os.path.join(STRESS_DIR, "stress_test_logs.txt")
            with open(log_file, "w") as f:
                f.write(logs.stdout)
        if not already_running:
            compose_run("down", "--remove-orphans", "-v")


@pytest.fixture
def client(stress_services):
    """HTTP client pointed at nginx."""
    return PageSpeedClient(stress_services.nginx_url)


@pytest.fixture
def origin_client(stress_services):
    """HTTP client pointed directly at origin."""
    session = requests.Session()
    session._origin_url = stress_services.origin_url
    return session


@pytest.fixture
def stats_client(stress_services):
    """Metrics helper functions (uses docker exec for socket access)."""
    return metrics_helpers


@pytest.fixture
def warm_cache(client):
    """Pre-populate cache with common test paths and wait for HITs."""
    paths = [
        "/images/img-100k.jpg",
        "/images/img-500k.jpg",
        "/css/style-1k.css",
        "/js/app-1k.js",
        "/html/minimal.html",
    ]
    # Trigger initial MISSes
    for path in paths:
        try:
            client.get(path)
        except requests.RequestException:
            pass

    # Poll each path until it becomes a HIT (best effort)
    for path in paths:
        try:
            client.poll_for_hit(path, timeout=15.0)
        except (AssertionError, requests.RequestException):
            pass  # Best effort — some may stay MISS

    return paths
