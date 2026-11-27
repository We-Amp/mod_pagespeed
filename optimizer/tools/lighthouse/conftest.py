# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Pytest fixtures for Lighthouse A/B validation tests.

Provides session-scoped Lighthouse results for both origin (unoptimized)
and nginx (optimized) endpoints.  Results are cached for the entire test
session to avoid redundant Lighthouse invocations.

Ports match the E2E docker-compose stack:
  - origin: http://localhost:8081
  - nginx:  http://localhost:8080
"""

import json
import os
import shutil
import subprocess
import tempfile

import pytest

LIGHTHOUSE_DIR = os.path.dirname(os.path.abspath(__file__))
BUDGET_FILE = os.path.join(LIGHTHOUSE_DIR, "budget.json")

NGINX_URL = "http://localhost:8080"
ORIGIN_URL = "http://localhost:8081"

# Default number of Lighthouse runs for median selection.
DEFAULT_RUNS = 3

# Timeout per Lighthouse invocation (seconds).
LIGHTHOUSE_TIMEOUT = 180


def _lighthouse_installed() -> bool:
    """Return True if the ``lighthouse`` CLI is on PATH."""
    return shutil.which("lighthouse") is not None


def run_lighthouse(url: str, runs: int = DEFAULT_RUNS) -> dict | None:
    """Run Lighthouse CLI against *url* and return the median result JSON.

    Executes *runs* Lighthouse audits with mobile / simulated-throttling
    settings, picks the median by performance score, and returns the full
    JSON report dict for that run.  Returns ``None`` when no successful
    run was recorded.
    """
    results = []
    for i in range(runs):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
            tmp_path = tmp.name
        try:
            proc = subprocess.run(
                [
                    "lighthouse",
                    url,
                    "--output=json",
                    f"--output-path={tmp_path}",
                    "--chrome-flags=--headless --no-sandbox",
                    "--only-categories=performance",
                    "--throttling-method=simulate",
                    "--preset=mobile",
                    "--quiet",
                ],
                capture_output=True,
                text=True,
                timeout=LIGHTHOUSE_TIMEOUT,
            )
            if proc.returncode != 0:
                print(
                    f"  Lighthouse run {i + 1}/{runs} for {url} failed: "
                    f"{proc.stderr[:300]}"
                )
                continue

            with open(tmp_path) as fh:
                data = json.load(fh)
            results.append(data)
        except (
            subprocess.TimeoutExpired,
            json.JSONDecodeError,
            OSError,
        ) as exc:
            print(f"  Lighthouse run {i + 1}/{runs} for {url} error: {exc}")
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    if not results:
        return None

    # Pick the median by performance score.
    def _perf_score(report: dict) -> float:
        return report.get("categories", {}).get("performance", {}).get("score", 0.0)

    results.sort(key=_perf_score)
    return results[len(results) // 2]


def get_audit(report: dict, audit_id: str) -> dict | None:
    """Extract a specific audit entry from a Lighthouse report.

    Returns the audit dict (containing ``score``, ``numericValue``,
    ``details``, etc.) or ``None`` if the audit is absent.
    """
    return report.get("audits", {}).get(audit_id)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture(scope="session")
def lighthouse_available():
    """Skip the entire session if the ``lighthouse`` CLI is not installed."""
    if not _lighthouse_installed():
        pytest.skip("lighthouse CLI not found on PATH")


@pytest.fixture(scope="session")
def lighthouse_origin_results(lighthouse_available) -> dict:
    """Session-cached Lighthouse report for the *origin* (unoptimized).

    Runs Lighthouse against ``http://localhost:8081/index.html``.
    """
    url = f"{ORIGIN_URL}/index.html"
    print(f"\n  Running Lighthouse against origin: {url}")
    report = run_lighthouse(url)
    if report is None:
        pytest.skip(f"All Lighthouse runs failed for origin URL {url}")
    return report


@pytest.fixture(scope="session")
def lighthouse_nginx_results(lighthouse_available) -> dict:
    """Session-cached Lighthouse report for *nginx* (optimized).

    Runs Lighthouse against ``http://localhost:8080/index.html``.
    """
    url = f"{NGINX_URL}/index.html"
    print(f"\n  Running Lighthouse against nginx: {url}")
    report = run_lighthouse(url)
    if report is None:
        pytest.skip(f"All Lighthouse runs failed for nginx URL {url}")
    return report


@pytest.fixture(scope="session")
def budget() -> list[dict]:
    """Load the performance budget from ``budget.json``."""
    with open(BUDGET_FILE) as fh:
        return json.load(fh)
