# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Shared fixtures for production readiness tests.

Manages Docker Compose lifecycle, provides HTTP clients for
nginx proxy and direct origin access, and metrics helpers.
"""

import os
import subprocess
import sys
import time

import pytest
import requests
from helpers import metrics as metrics_module
from helpers.client import ProductionTestClient

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from common.worker_health import wait_worker_socket

COMPOSE_DIR = os.path.dirname(__file__)
PRODTEST_NGINX_PORT = os.environ.get("PRODTEST_NGINX_PORT", "8200")
PRODTEST_ORIGIN_PORT = os.environ.get("PRODTEST_ORIGIN_PORT", "8201")
NGINX_URL = f"http://localhost:{PRODTEST_NGINX_PORT}"
ORIGIN_URL = f"http://localhost:{PRODTEST_ORIGIN_PORT}"


def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "p0: Priority 0 — hard gate for production")
    config.addinivalue_line("markers", "p1: Priority 1 — soft gate, review exceptions")
    config.addinivalue_line("markers", "p2: Priority 2 — rare scenarios, completeness")


def _wait_for_service(url, timeout=60):
    """Wait for a service to respond to HTTP requests."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.get(url, timeout=2)
            if r.status_code < 500:
                return True
        except requests.RequestException:
            pass
        time.sleep(1)
    return False


def _is_stack_running():
    """Check if the Docker Compose stack is already running."""
    try:
        r = requests.get(f"{ORIGIN_URL}/health", timeout=2)
        return r.status_code == 200
    except requests.RequestException:
        return False


def docker_exec(service, command):
    """Run a command inside a running Docker Compose service container."""
    if isinstance(command, str):
        cmd_parts = command.split()
    else:
        cmd_parts = list(command)
    return subprocess.run(
        ["docker", "compose", "exec", "-T", service] + cmd_parts,
        cwd=COMPOSE_DIR,
        capture_output=True,
        text=True,
    )


@pytest.fixture(scope="session")
def stack():
    """Start Docker Compose stack for the session."""
    metrics_module.COMPOSE_DIR = COMPOSE_DIR

    if _is_stack_running():
        print("\nStack already running, reusing existing services")
        yield
        return

    print("\nStarting production test stack...")
    subprocess.run(
        ["docker", "compose", "up", "-d", "--build"],
        cwd=COMPOSE_DIR,
        check=True,
        timeout=300,
    )

    # Wait for origin
    if not _wait_for_service(f"{ORIGIN_URL}/health", timeout=60):
        subprocess.run(["docker", "compose", "logs"], cwd=COMPOSE_DIR)
        pytest.fail("Origin service did not become healthy")

    # Wait for nginx
    if not _wait_for_service(f"{NGINX_URL}/health", timeout=60):
        subprocess.run(["docker", "compose", "logs"], cwd=COMPOSE_DIR)
        pytest.fail("Nginx service did not become healthy")

    # Extra sleep for worker to fully initialize
    time.sleep(3)

    # Confirm the worker is actually ready so production tests report
    # the real cause, not a "stale artifact" false positive.
    wait_worker_socket(docker_exec, timeout=30)

    yield

    # Collect logs on teardown
    subprocess.run(
        ["docker", "compose", "logs"],
        cwd=COMPOSE_DIR,
        stdout=open(os.path.join(COMPOSE_DIR, "test-logs.txt"), "w"),
        stderr=subprocess.STDOUT,
    )


@pytest.fixture
def client(stack):
    """HTTP client pointed at nginx proxy (default port 8200)."""
    return ProductionTestClient(NGINX_URL, origin_url=ORIGIN_URL)


@pytest.fixture
def origin_client(stack):
    """HTTP client pointed directly at origin (default port 8201)."""
    return ProductionTestClient(ORIGIN_URL)


@pytest.fixture
def metrics_client(stack):
    """Metrics helper module for querying worker stats."""
    return metrics_module


@pytest.fixture
def purge(stack):
    """Cache purge helper."""

    def _purge(url, hostname="localhost"):
        return metrics_module.purge(url, hostname=hostname)

    return _purge
