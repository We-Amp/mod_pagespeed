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

import inspect
import os
import re
import shutil
import subprocess
import sys
import time
from typing import List, Optional, Set

import pytest

# How many graceful cycles to run. Pre-fix, the small (ServerLimit 6) prefork
# scoreboard is exhausted in well under this many cycles; post-fix the child set
# returns to baseline on every cycle.
GRACEFUL_CYCLES = 25

# Tail window (bytes) read from the Apache error log for the AH03490 probe.
# Named so the baseline probe can refuse to reason from a SATURATED window:
# when the pre-test log already fills it, the post-test slice of "new" lines
# is empty by construction and the absence assertion passes vacuously.
ERROR_LOG_TAIL_BYTES = 200_000

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


def _read_error_log_tail(
    num_bytes: int = ERROR_LOG_TAIL_BYTES,
) -> Optional[str]:
    """Return the decoded log tail, or None when the log cannot be read.

    None -- not "" -- is the unreadable signal on purpose: the leak test
    asserts the ABSENCE of a needle (AH03490), and an empty string would
    satisfy that assertion vacuously, making a broken probe (for example a
    sudo tail that fails with no passwordless sudo, its return code ignored)
    indistinguishable from a clean run. Callers must check for None before
    drawing any conclusion from the returned text.
    """
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
            # Binary, not text=True: the tail window is not guaranteed to be
            # valid UTF-8. Apache logs raw request/response fragments, so a
            # gzip or image payload puts arbitrary bytes in the file and
            # text=True's strict decode raises UnicodeDecodeError, killing
            # the test. Decode permissively -- the needle this test greps
            # for (AH03490) is plain ASCII and survives errors="replace".
            res = subprocess.run(
                ["sudo", "-n", "tail", "-c", str(num_bytes), path],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=15,
            )
            # A failing sudo (no passwordless sudo, a sudoers change) exits
            # non-zero with empty stdout. Treating that as an empty log is
            # the masking hole: report the probe as not having run instead.
            if res.returncode != 0:
                return None
            return res.stdout.decode("utf-8", errors="replace")
        except (subprocess.SubprocessError, OSError):
            return None


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
        # exists() is not readability: a present-but-unreadable log whose
        # sudo fallback also fails must skip here, not sail through with a
        # vacuously-empty log.
        if _read_error_log_tail() is None:
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
        # lines produced during this test. The probe must actually run: None
        # means unreadable, and measuring "new" lines against a failed probe
        # would silently corrupt the absence assertion below.
        pre_tail = _read_error_log_tail()
        assert pre_tail is not None, (
            f"Apache error log at {error_log} became unreadable mid-test; "
            "refusing to measure new log lines against a failed probe"
        )
        pre_log_len = len(pre_tail)
        # Fail closed on a saturated tail window. The AH03490 assertion below
        # reasons over post_tail[pre_log_len:]; when the pre-test log already
        # fills the tail window, pre_log_len equals the window size and that
        # slice is EMPTY BY CONSTRUCTION -- the AH03490 absence assertion
        # passes on an empty slice no matter what the graceful loop logged.
        # This is not theoretical: the default probe target is the
        # persistent system error log, which grows past the window on a
        # reused runner. Refuse to run the absence assertion from a
        # saturated probe.
        assert pre_log_len < ERROR_LOG_TAIL_BYTES, (
            f"Apache error log at {error_log} already fills the "
            f"{ERROR_LOG_TAIL_BYTES}-byte tail window before this test runs; "
            "the post-test slice of new lines would be empty by construction "
            "and the AH03490 absence assertion would pass vacuously. "
            "Truncate or rotate the log (or raise ERROR_LOG_TAIL_BYTES) to "
            "run this test."
        )

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
        #    user-visible end state of the leak. The assertion has ABSENCE
        #    polarity, so it is only as strong as the probe behind it: an
        #    unreadable log would satisfy it vacuously. Fail closed instead
        #    of concluding "no AH03490" from a log we could not read.
        post_tail = _read_error_log_tail()
        assert post_tail is not None, (
            f"Apache error log at {error_log} unreadable after the graceful "
            "loop; refusing to conclude AH03490 is absent from a log this "
            "test could not read"
        )
        new_log = post_tail[pre_log_len:]
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


def test_read_error_log_tail_fallback_tolerates_binary_log_content(
    monkeypatch, tmp_path
):
    """Unit regression for the sudo-tail fallback crashing on binary logs.

    The fallback used ``text=True``, whose strict UTF-8 decode raises
    UnicodeDecodeError when the tailed window holds arbitrary bytes (a gzip
    or image payload Apache logged raw request/response fragments of).
    That exception is not a SubprocessError/OSError, so it escaped the
    fallback's own except clause and killed the test. This feeds the
    fallback a gzip-magic-bytes payload through a subprocess.run stub that
    reproduces text=True's strict-decode behaviour exactly, so the test is
    red on the unfixed code and needs neither sudo nor a running Apache.
    """
    # A tailed window with gzip magic bytes (\x1f\x8b) amid plain-ASCII log
    # lines, including the needle the leak test greps for.
    needle = b"AH03490: scoreboard is full, not at MaxRequestWorkers"
    payload = (
        b"[notice] Apache/2.4 configured -- resuming normal operations\n"
        b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\xed\xc0\x81\x08\x00"
        + needle
        + b"\n\xff\xfe binary tail bytes\n"
    )

    def fake_run(cmd, **kwargs):
        assert cmd[:3] == ["sudo", "-n", "tail"], f"unexpected command: {cmd}"
        if kwargs.get("text"):
            # subprocess.run with text=True decodes the child's stdout with a
            # strict codec; on non-UTF-8 bytes it raises UnicodeDecodeError.
            # Decode strictly here so the stub fails exactly the way the real
            # call does on the unfixed code.
            payload.decode("utf-8")
        return subprocess.CompletedProcess(cmd, 0, stdout=payload, stderr=b"")

    # Point the reader at a nonexistent log so the primary open() path raises
    # FileNotFoundError and the sudo-tail fallback under test is taken.
    monkeypatch.setenv(
        "PAGESPEED_APACHE_ERROR_LOG", str(tmp_path / "nonexistent-error.log")
    )
    monkeypatch.setattr(subprocess, "run", fake_run)

    tail = _read_error_log_tail()

    assert isinstance(tail, str)
    # The ASCII needle must survive the decode...
    assert needle.decode("ascii") in tail
    # ...and the binary bytes must have been replaced, not raised on.
    assert "�" in tail


def test_read_error_log_tail_returns_none_when_sudo_tail_fails(
    monkeypatch, tmp_path
):
    """A failed sudo tail must read as "probe did not run", not as "".

    The leak test asserts the ABSENCE of AH03490, so a helper that reports
    a failed probe as an empty string lets a completely broken run pass
    vacuously -- the gate-cannot-go-red property. On the unfixed helper
    (return code ignored, "" on failure) this case is red.
    """

    def fake_run(cmd, **kwargs):
        assert cmd[:3] == ["sudo", "-n", "tail"], f"unexpected command: {cmd}"
        # sudo refusing a non-interactive run: empty stdout, non-zero status.
        return subprocess.CompletedProcess(
            cmd, 1, stdout=b"", stderr=b"sudo: a password is required"
        )

    monkeypatch.setenv(
        "PAGESPEED_APACHE_ERROR_LOG", str(tmp_path / "nonexistent-error.log")
    )
    monkeypatch.setattr(subprocess, "run", fake_run)

    assert _read_error_log_tail() is None


def test_leak_test_fails_closed_on_an_unreadable_log():
    """Pin the polarity: the AH03490 absence check must prove its probe ran.

    Absence assertions pass vacuously on empty input, so the consumer side
    of the contract matters as much as the helper: the test must check the
    probe (the None contract above) BEFORE it concludes anything from the
    log text. Weld that ordering to the shipped test's own source so a
    revert of just the consumer half -- helper returning None again but the
    AH03490 check reading it blind -- goes red here.
    """
    source = inspect.getsource(
        TestGracefulRestartThreadLeak.test_graceful_restart_no_thread_leak
    )
    probe_check = source.find("post_tail is not None")
    needle_check = source.find('"AH03490" not in')
    assert probe_check != -1, "leak test lost its probe check"
    assert needle_check != -1, "leak test lost its AH03490 absence assertion"
    assert probe_check < needle_check, (
        "the AH03490 absence assertion runs before the probe is checked; "
        "an unreadable log would satisfy it vacuously"
    )


def test_leak_test_fails_closed_on_a_saturated_log_tail(monkeypatch):
    """A pre-test log that fills the tail window must not read as green.

    When pre_tail saturates the tail window, post_tail[pre_log_len:] is
    empty BY CONSTRUCTION, so "AH03490" not in "" passes even with AH03490
    freshly logged during the graceful loop -- the same gate-cannot-go-red
    shape as the None contract above, one level up. Drive the shipped test
    end to end with a saturated pre-test tail and a fresh AH03490 in the
    post-test tail: it must fail closed on the saturation assert. On code
    without that assert the method completes green and this test is red.
    """
    this_module = sys.modules[__name__]

    needle = "AH03490: scoreboard is full, not at MaxRequestWorkers\n"
    pre_tail = "x" * ERROR_LOG_TAIL_BYTES
    post_tail = "y" * (ERROR_LOG_TAIL_BYTES - len(needle)) + needle

    monkeypatch.setattr(this_module, "_have_sudo", lambda: True)

    reads = []

    def fake_read(num_bytes=ERROR_LOG_TAIL_BYTES):
        reads.append(1)
        # Calls 1 (readability gate) and 2 (baseline) see the saturated
        # pre-test log; call 3 (post-loop) sees a fresh AH03490 in the tail.
        return pre_tail if len(reads) <= 2 else post_tail

    monkeypatch.setattr(this_module, "_read_error_log_tail", fake_read)

    # A fake child set that recycles cleanly on every graceful, so the ONLY
    # thing that can fail is the log reasoning under test.
    current_pids = {1000}
    monkeypatch.setattr(
        this_module, "_apache_child_pids", lambda: set(current_pids)
    )
    cycles = {"n": 0}

    def fake_graceful():
        cycles["n"] += 1
        current_pids.clear()
        current_pids.add(1000 + cycles["n"])

    monkeypatch.setattr(this_module, "_run_graceful", fake_graceful)
    monkeypatch.setattr(this_module, "GRACEFUL_CYCLES", 2)
    monkeypatch.setattr(time, "sleep", lambda *a: None)

    class _Config:
        server_type = "apache"

    class _Client:
        def get(self, url):
            return None

    with pytest.raises(AssertionError, match="tail window"):
        TestGracefulRestartThreadLeak().test_graceful_restart_no_thread_leak(
            _Client(), _Config(), "http://example"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
