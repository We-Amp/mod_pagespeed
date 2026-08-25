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

"""Defensive regression test for worker leaks across nginx reloads.

nginx is NOT affected by the Cyclone HitTracker thread-leak bug that hit Apache
(Apache's pagespeed_child_exit() only reset the factory on PoolDestroyed(); the
nginx port stops the thread on the normal child-exit path). This test exists to
LOCK IN that correct behaviour so a future change cannot reintroduce the leak on
nginx the way it existed on Apache.

What it does
------------
With ``worker_processes`` pinned to a small fixed number (run_nginx_tests.sh
pins 2 by default), it loops ~20 times:
  1. issue a few pagespeed-enabled requests (so each worker touches the Cyclone
     cache and starts its HitTracker flush thread),
  2. send SIGHUP to the master (graceful reload),
  3. assert every OLD worker PID exits within a bounded timeout,
  4. assert the live worker count returns to worker_processes,
and finally asserts summed worker RSS stayed within a tolerance band (no
monotonic growth from accumulating stuck workers).

A leak (old worker that never exits) is the failure signal: a survivor PID, a
worker count above worker_processes, or RSS growth beyond the tolerance band.

Environment
-----------
Runs only on nginx and self-skips unless run_nginx_tests.sh exports:
  PAGESPEED_NGINX_PID_FILE         - master pid file
  PAGESPEED_NGINX_WORKER_PROCESSES - fixed integer worker count (not "auto")
"""

import os
import signal
import time
from typing import Dict, List, Set

import pytest

RELOAD_CYCLES = 20

# Per-cycle budget for old workers to exit after SIGHUP. Generous so the
# assertion is robust to nginx's graceful worker shutdown (old workers finish
# in-flight requests then exit), not an instant snapshot.
WORKER_EXIT_TIMEOUT_S = 20.0
WORKER_EXIT_POLL_S = 0.25

# Tolerance for summed-worker-RSS growth across the whole loop. A leak would
# show unbounded growth (each cycle adds a stuck worker's full RSS); a healthy
# server stays within normal heap jitter.
RSS_GROWTH_TOLERANCE = 0.15  # 15%


def _master_pid() -> int:
    pid_file = os.environ.get("PAGESPEED_NGINX_PID_FILE", "")
    with open(pid_file, "r") as f:
        return int(f.read().strip())


def _worker_pids(master_pid: int) -> Set[int]:
    """Return nginx worker PIDs: nginx processes whose PPID is the master."""
    workers: Set[int] = set()
    try:
        out = _ps()
    except OSError:
        return workers
    for pid, ppid, comm in out:
        if ppid == master_pid and "nginx" in comm:
            workers.add(pid)
    return workers


def _ps():
    import subprocess

    res = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,comm="],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        timeout=15,
    )
    rows = []
    for line in res.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        try:
            rows.append((int(parts[0]), int(parts[1]), parts[2].strip()))
        except ValueError:
            continue
    return rows


def _worker_rss_kb(worker_pids: Set[int]) -> int:
    """Summed RSS (KB) of the given worker PIDs, read from /proc."""
    total = 0
    for pid in worker_pids:
        try:
            with open(f"/proc/{pid}/statm", "r") as f:
                # statm: size resident shared ... in pages
                resident_pages = int(f.read().split()[1])
            total += resident_pages * (os.sysconf("SC_PAGE_SIZE") // 1024)
        except (FileNotFoundError, ValueError, IndexError, OSError):
            continue
    return total


def _send_sighup(master_pid: int) -> None:
    os.kill(master_pid, signal.SIGHUP)


def _wait_for_pids_to_exit(
    pids: Set[int], master_pid: int, timeout_s: float
) -> Set[int]:
    deadline = time.time() + timeout_s
    survivors = set(pids)
    while survivors and time.time() < deadline:
        time.sleep(WORKER_EXIT_POLL_S)
        survivors = pids & _worker_pids(master_pid)
    return survivors


@pytest.mark.nginx_only
@pytest.mark.process_leak
@pytest.mark.requires_module
@pytest.mark.slow
class TestNginxReloadWorkerLeak:
    """Old nginx workers must exit on SIGHUP reload (no Cyclone thread leak)."""

    def test_nginx_reload_no_worker_leak(
        self,
        client,
        server_config,
        example_root: str,
    ):
        if not server_config.is_nginx:
            pytest.skip("nginx reload leak test only runs on nginx")

        pid_file = os.environ.get("PAGESPEED_NGINX_PID_FILE", "")
        if not pid_file or not os.path.exists(pid_file):
            pytest.skip(
                "PAGESPEED_NGINX_PID_FILE not set/readable; run via "
                "run_nginx_tests.sh"
            )

        worker_processes_env = os.environ.get(
            "PAGESPEED_NGINX_WORKER_PROCESSES", "auto"
        )
        if not worker_processes_env.isdigit():
            pytest.skip(
                "worker_processes is not a fixed integer "
                f"({worker_processes_env!r}); set NGINX_WORKER_PROCESSES=2"
            )
        expected_workers = int(worker_processes_env)

        try:
            master_pid = _master_pid()
        except (OSError, ValueError) as e:
            pytest.skip(f"Could not read nginx master PID: {e}")

        warm_url = f"{example_root}/?PageSpeedFilters=collapse_whitespace"

        def warm_workers() -> None:
            for _ in range(6):
                try:
                    client.get(warm_url)
                except Exception:
                    pass

        # Baseline.
        warm_workers()
        time.sleep(1.0)
        baseline_workers = _worker_pids(master_pid)
        assert len(baseline_workers) > 0, (
            "Expected nginx worker processes at baseline; is nginx running with "
            f"worker_processes {expected_workers}?"
        )
        baseline_rss = _worker_rss_kb(baseline_workers)

        max_rss = baseline_rss
        leak_failures: List[str] = []
        count_failures: List[str] = []

        for cycle in range(RELOAD_CYCLES):
            warm_workers()
            pre_reload_workers = _worker_pids(master_pid)

            _send_sighup(master_pid)

            survivors = _wait_for_pids_to_exit(
                pre_reload_workers, master_pid, WORKER_EXIT_TIMEOUT_S
            )
            if survivors:
                leak_failures.append(
                    f"cycle {cycle}: old worker PID(s) {sorted(survivors)} "
                    f"still alive {WORKER_EXIT_TIMEOUT_S}s after SIGHUP "
                    f"(leaked background thread keeps them from exiting)"
                )
                break

            # New generation should have spun up to exactly worker_processes.
            warm_workers()
            time.sleep(0.5)
            now_workers = _worker_pids(master_pid)
            if len(now_workers) != expected_workers:
                count_failures.append(
                    f"cycle {cycle}: {len(now_workers)} live workers, expected "
                    f"{expected_workers}"
                )
            max_rss = max(max_rss, _worker_rss_kb(now_workers))

        # --- Assertions ---------------------------------------------------

        # 1. No old worker survived its reload cycle.
        assert not leak_failures, "Workers failed to exit on reload:\n" + "\n".join(
            leak_failures
        )

        # 2. Worker count stayed equal to worker_processes after each reload.
        assert not count_failures, "Worker count drifted from worker_processes:\n" + "\n".join(
            count_failures
        )

        # 3. Summed worker RSS stayed within the tolerance band (no monotonic
        #    growth from accumulating stuck workers).
        if baseline_rss > 0:
            growth = (max_rss - baseline_rss) / baseline_rss
            assert growth <= RSS_GROWTH_TOLERANCE, (
                f"Summed nginx worker RSS grew {growth * 100:.1f}% "
                f"(baseline {baseline_rss} KB -> peak {max_rss} KB), exceeding "
                f"the {RSS_GROWTH_TOLERANCE * 100:.0f}% tolerance — workers may "
                f"be accumulating across reloads."
            )

        # 4. Final state: workers back to exactly worker_processes.
        time.sleep(1.0)
        final_workers = _worker_pids(master_pid)
        assert len(final_workers) == expected_workers, (
            f"After the reload loop, nginx has {len(final_workers)} workers, "
            f"expected {expected_workers}; stuck workers were not reaped."
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
