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

"""Regression test for the Cyclone background-thread leak on graceful restart.

Background
----------
The Cyclone cache backend runs a HitTracker flush thread per worker/child
process. Before the StopCacheBackgroundThreads() fix, that thread was joined
ONLY when the RewriteDriverFactory was destroyed, which on Apache happens only
on full process teardown (PoolDestroyed()). On a per-child graceful recycle
(``apache2ctl graceful``) the factory is NOT destroyed, so the joinable thread
kept the old child alive forever.

Each stuck child holds an Apache scoreboard slot. With a small MPM (this test
relies on the prefork MPM pinned by setup_apache_test.sh with a tight
ServerLimit/MaxRequestWorkers), the scoreboard fills within a few graceful
cycles and Apache logs ``AH03490: scoreboard is full, not at MaxRequestWorkers``
and can no longer fork new workers.

The fix calls SystemRewriteDriverFactory::StopCacheBackgroundThreads() from
pagespeed_child_exit() on EVERY child exit (not just PoolDestroyed()), which
joins the Cyclone thread so the child can terminate.

Why this test fails pre-fix and passes post-fix
------------------------------------------------
- PRE-fix: after each ``graceful``, the old children that have started a
  Cyclone HitTracker thread (because we sent them a pagespeed-enabled request)
  never exit. ``_apache_child_pids()`` keeps returning the old PIDs, so the
  "old PIDs exited" assertion fails; the child count grows past baseline; and
  once the scoreboard is exhausted the error log contains AH03490.
- POST-fix: pagespeed_child_exit() joins the thread, the old children exit
  promptly, the child set returns to baseline each cycle, and AH03490 never
  appears.

Environment
-----------
This test only runs on Apache and needs to drive ``apache2ctl graceful`` and
read the Apache error log. It self-skips unless the harness exports:
  PAGESPEED_APACHE_CTL        - apachectl/apache2ctl command (default apache2ctl)
  PAGESPEED_APACHE_ERROR_LOG  - path to the Apache error log
and passwordless ``sudo`` is available for the control command. run_system_tests.sh
exports these and the CI container grants sudo.
"""

import os
import re
import shutil
import subprocess
import time
from typing import List, Set

import pytest

# How many graceful cycles to run. Pre-fix, the small (ServerLimit 6) prefork
# scoreboard is exhausted in well under this many cycles; post-fix the child set
# returns to baseline on every cycle.
GRACEFUL_CYCLES = 25

# Per-cycle budget for old children to exit after `graceful`. Generous so the
# assertion is robust to async graceful shutdown under load, not an instant
# snapshot. Post-fix old children exit in well under a second.
CHILD_EXIT_TIMEOUT_S = 20.0
CHILD_EXIT_POLL_S = 0.25


def _apache_ctl() -> str:
    return os.environ.get("PAGESPEED_APACHE_CTL", "apache2ctl")


def _error_log_path() -> str:
    return os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "/var/log/apache2/error.log")


def _have_sudo() -> bool:
    if shutil.which("sudo") is None:
        return False
    try:
        # Non-interactive: succeeds only if sudo needs no password.
        return subprocess.run(
            ["sudo", "-n", "true"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
        ).returncode == 0
    except (subprocess.SubprocessError, OSError):
        return False


def _apache_child_pids() -> Set[int]:
    """Return the set of apache2/httpd CHILD PIDs (excludes the master).

    The master is the apache2/httpd process whose PPID is not itself an apache
    process (typically PID 1 / the init under Docker). Children are the worker
    processes whose parent is the master. We identify children as apache
    processes whose PPID is also an apache process.
    """
    out = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,comm="],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        timeout=15,
    ).stdout

    apache_pids = {}
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        pid_s, ppid_s, comm = parts
        if comm.strip() in ("apache2", "httpd"):
            try:
                apache_pids[int(pid_s)] = int(ppid_s)
            except ValueError:
                continue

    # Children are apache processes whose parent is also an apache process.
    children = {
        pid for pid, ppid in apache_pids.items() if ppid in apache_pids
    }
    return children


def _run_graceful() -> None:
    subprocess.run(
        ["sudo", "-n", _apache_ctl(), "graceful"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=30,
        check=False,
    )


def _read_error_log_tail(num_bytes: int = 200_000) -> str:
    path = _error_log_path()
    try:
        with open(path, "rb") as f:
            try:
                f.seek(-num_bytes, os.SEEK_END)
            except OSError:
                f.seek(0)
            return f.read().decode("utf-8", errors="replace")
    except (FileNotFoundError, PermissionError):
        # Best-effort fallback via sudo if the log isn't world-readable.
        try:
            res = subprocess.run(
                ["sudo", "-n", "tail", "-c", str(num_bytes), path],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=15,
            )
            return res.stdout
        except (subprocess.SubprocessError, OSError):
            return ""


def _wait_for_pids_to_exit(pids: Set[int], timeout_s: float) -> Set[int]:
    """Poll until none of `pids` are alive, or timeout. Return survivors."""
    deadline = time.time() + timeout_s
    survivors = set(pids)
    while survivors and time.time() < deadline:
        time.sleep(CHILD_EXIT_POLL_S)
        survivors = pids & _apache_child_pids()
    return survivors


@pytest.mark.apache_only
@pytest.mark.process_leak
@pytest.mark.requires_module
@pytest.mark.slow
class TestGracefulRestartThreadLeak:
    """Cyclone HitTracker thread must not leak Apache children on graceful."""

    def test_graceful_restart_no_thread_leak(
        self,
        client,
        server_config,
        example_root: str,
    ):
        # Hard gate: this test is meaningless on non-Apache servers.
        if server_config.server_type not in ("apache", "auto"):
            pytest.skip("graceful-restart leak test only runs on Apache")

        if not _have_sudo():
            pytest.skip("passwordless sudo required to drive apachectl graceful")

        error_log = _error_log_path()
        if not (os.path.exists(error_log) or _read_error_log_tail()):
            pytest.skip(f"Apache error log not readable: {error_log}")

        # A pagespeed-enabled URL — fetching it makes the serving child touch
        # the Cyclone cache, which starts the HitTracker flush thread in that
        # child. Without a started thread there is nothing to leak.
        warm_url = f"{example_root}/?PageSpeedFilters=collapse_whitespace"

        def warm_children() -> None:
            # Several requests so the small prefork pool spins up workers and
            # each that serves a request starts its Cyclone thread.
            for _ in range(8):
                try:
                    client.get(warm_url)
                except Exception:
                    pass

        # Baseline: warm up, then record the current child set + count.
        warm_children()
        time.sleep(1.0)
        baseline_children = _apache_child_pids()
        baseline_count = len(baseline_children)
        assert baseline_count > 0, (
            "Expected at least one Apache child process at baseline; "
            "is Apache running with a forking MPM?"
        )

        # Record how much of the error log already exists so we only inspect
        # lines produced during this test.
        pre_log_len = len(_read_error_log_tail())

        max_child_count = baseline_count
        leak_failures: List[str] = []

        for cycle in range(GRACEFUL_CYCLES):
            # Children that must die after this graceful (those alive now and
            # that have served a pagespeed request, i.e. started a Cyclone
            # thread). We warm before snapshotting so the live children have a
            # thread to leak.
            warm_children()
            pre_graceful_children = _apache_child_pids()

            _run_graceful()

            # Poll (do NOT snapshot instantly) for the pre-graceful children to
            # exit. Apache graceful is asynchronous: old generations finish
            # in-flight requests then exit. The leak signal is an old child that
            # NEVER exits within the budget.
            survivors = _wait_for_pids_to_exit(
                pre_graceful_children, CHILD_EXIT_TIMEOUT_S
            )
            if survivors:
                leak_failures.append(
                    f"cycle {cycle}: {len(survivors)} pre-graceful child PID(s) "
                    f"{sorted(survivors)} still alive after "
                    f"{CHILD_EXIT_TIMEOUT_S}s (leaked Cyclone thread keeps them "
                    f"from exiting)"
                )
                # Don't keep hammering a wedged server for all 25 cycles.
                break

            # Re-warm so the next generation has live children to recycle, and
            # track total child count for the monotonic-growth assertion.
            warm_children()
            time.sleep(0.5)
            now_count = len(_apache_child_pids())
            max_child_count = max(max_child_count, now_count)

        # --- Assertions ---------------------------------------------------

        # 1. No old child ever survived its graceful cycle.
        assert not leak_failures, "Children failed to exit on graceful:\n" + "\n".join(
            leak_failures
        )

        # 2. Child count did not grow monotonically / unboundedly. With a clean
        #    recycle the count stays near baseline; allow generous slack for
        #    transient overlap of an exiting old generation and a new one, but
        #    catch true accumulation (pre-fix the count climbs to ServerLimit
        #    and stays pinned there). baseline+ServerLimit headroom is a safe
        #    upper bound; anything approaching 2x ServerLimit means leaking.
        assert max_child_count <= baseline_count + 8, (
            f"Apache child count grew to {max_child_count} (baseline "
            f"{baseline_count}); old children are accumulating instead of "
            f"exiting on graceful (Cyclone thread leak)."
        )

        # 3. The error log must not report a full scoreboard. This is the
        #    user-visible end state of the leak.
        new_log = _read_error_log_tail()[pre_log_len:]
        assert "AH03490" not in new_log, (
            "Apache logged AH03490 (scoreboard is full) during the graceful "
            "loop — children are not exiting, the Cyclone HitTracker thread is "
            "leaking. Offending log lines:\n"
            + "\n".join(
                ln for ln in new_log.splitlines() if "AH03490" in ln
            )
        )

        # 4. After the loop settles, the child set should return to ~baseline.
        time.sleep(1.0)
        final_count = len(_apache_child_pids())
        assert final_count <= baseline_count + 8, (
            f"After the graceful loop, Apache still has {final_count} children "
            f"(baseline {baseline_count}); stuck children were not reaped."
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
