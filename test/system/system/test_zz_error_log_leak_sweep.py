#!/usr/bin/env python3
# Copyright (c) 2026 We-Amp B.V.
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

"""End-of-run sweep of the whole Apache error log.

Three checks ride on the one read of the log:

1. test_no_leaked_rewrite_drivers_on_shutdown -- the RewriteDriver leak
   gate, ported from: install/Makefile.tests, where every apache_debug_*_test
   target ended with `[ -z "`grep leaked_rewrite_drivers $(APACHE_LOG)`" ]`
   after stopping Apache.

   ServerContext::~ServerContext() (net/instaweb/rewriter/server_context.cc)
   logs "%d leaked_rewrite_drivers on destruction" whenever a RewriteDriver
   is still alive when its ServerContext is torn down. On Apache that
   destructor runs from ApacheRewriteDriverFactory::PoolDestroyed(), which
   pagespeed_child_exit() (pagespeed/apache/mod_instaweb.cc) invokes on
   every child-process exit -- not only at final module unload. This lane's
   test_graceful_restart_leak.py already drives 25 `apache2ctl graceful`
   cycles earlier in the same run, each recycling children, so by the time
   this test runs the log already reflects a whole run's worth of
   ServerContext teardowns. The retired bash harness grepped for this
   message at the end of every apache_debug_*_test target; nothing in the
   pytest suite did until this file.

   Deviation from the bash: the bash's grep had no positive control, so a
   truncated, rotated or otherwise-empty log passed vacuously. Every test
   here requires the log to be non-empty and to contain the module's own
   Apache startup line (the thread-resolution message
   test_mpm_thread_resolution.py matches) before it trusts an absent needle
   as meaningful.

2. test_no_child_crash_or_fatal_lines -- a child that dies on a signal
   leaves AH00052 ("child pid N exit signal ..."), with AH00050/AH00051
   covering the other child-loss reports, and the module's own fatal
   failures log [FATAL] / "Check failed:" (base/logging.h). None of these
   appear in a healthy run.

3. test_child_set_matches_last_restart -- the death that leaves NO log
   line. httpd logs nothing for a child killed by SIGKILL; the parent
   re-forks a replacement within about a second, so the loss shows up only
   as a child younger than the rest of the pool. The same check in the
   other direction catches a worker that never exited an EARLIER graceful
   restart: a worker born just before the final "resuming normal
   operations" line that then failed to exit sits inside the window of
   that last restart and cannot be distinguished from a legitimate child
   of it.
   Every child must have been forked within CHILD_START_WINDOW_S of the
   last AH00163 "resuming normal operations" line (or of the master's own
   start when the log has none). The lane's prefork pool is fixed
   (setup_apache_test.sh pins StartServers=MinSpareServers=
   MaxSpareServers=MaxRequestWorkers=16 and MaxConnectionsPerChild=0), so
   no legitimate fork lands outside that window.

This module is named to sort last in test/system/system/, the second and
last directory run_system_tests.sh hands to pytest by default
(`automatic/ system/ -v`) -- so it is the last test collected and run on the
Apache lane, after every other Apache test's traffic has had a chance to
leave something behind in the log. A pytest_sessionfinish hook would run
after the pass/fail verdict for the session is already fixed, which is why
this is an ordinary (last-sorting) test instead.

Apache only: PAGESPEED_APACHE_ERROR_LOG is Apache's log path, and this
module reads it directly rather than through a fixture. The leak message
and the startup-line positive control are both logged by server-generic
code (net/instaweb/rewriter/server_context.cc and
pagespeed/system/system_rewrite_driver_factory.cc, the latter overridden
per-binding), so the same sweep is mechanically possible on nginx via
PAGESPEED_NGINX_ERROR_LOG -- but that lane's own pytest invocation, log
rotation and child/worker-recycle behavior (there is no nginx analogue of
test_graceful_restart_leak.py's 25 graceful cycles exercising this path
mid-run) are unverified here, so this stays Apache-only rather than
guessing at a second lane.
"""

import os
import re
import subprocess
import time
from typing import Dict, Optional, Tuple

import pytest

# Transcribed from net/instaweb/rewriter/server_context.cc
# ServerContext::~ServerContext(). The full message is "ServerContext: %d
# leaked_rewrite_drivers on destruction"; matching the substring alone is
# exactly what the retired bash's `grep leaked_rewrite_drivers` did, and
# stays immune to the leak count and to the per-driver detail lines a debug
# build logs after it.
LEAK_MESSAGE = "leaked_rewrite_drivers"

# Transcribed from pagespeed/apache/apache_rewrite_driver_factory.cc /
# pagespeed/system/system_rewrite_driver_factory.cc LogThreadCountResolution():
# logged unconditionally at kWarning on every Apache startup and again on
# every graceful restart, so its presence is the positive control that this
# is actually the log Apache wrote to in this run. Same prefix
# test_mpm_thread_resolution.py matches.
STARTUP_LINE = "PageSpeed optimization threads:"

# httpd's child-death reports (AH00052 is the crash line, "child pid N exit
# signal ..."; AH00050/AH00051 cover the other child losses), the signal text
# itself so a crash is caught independent of the AH code, and the module's
# own fatal lines (base/logging.h LOG(FATAL) and CHECK). The bare word
# "aborted" is deliberately NOT matched: the benign fetch-cancellation
# message "Fetch failed (Operation was aborted by an application callback)"
# -- logged when a graceful restart cancels a start-up fetch -- contains it.
CRASH_LINE = re.compile(r"AH0005[012]|exit signal|\[FATAL\]|Check failed:")

# Half-width of the fork-time window every child must fall into, measured
# against the last graceful restart (test_child_set_matches_last_restart).
# 60 s is generous for slow hosts -- a graceful cycle on a loaded one takes
# 10-13 s, and fork spread across the fixed 16-child pool adds little --
# while the lane runs for many minutes after its last restart, so a
# mid-run replacement (silent death) or a worker left over from an earlier
# restart cycle sits far outside it.
CHILD_START_WINDOW_S = 60

# Apache error-log line prefix: "[Sun Oct  5 08:24:28.823456 2026]" (the day
# is space-padded). Log and ps share the container's clock, so time.mktime
# (local time) is the right conversion.
_LOG_TIMESTAMP = re.compile(
    r"\[(\w{3} \w{3}\s+\d{1,2} \d{2}:\d{2}:\d{2})(?:\.\d+)? (\d{4})\]"
)


def _read_error_log() -> str:
    # Same helper as test_handler_quoting.py / test_remote_config.py (two
    # identical copies in test/system/system/; no copy lives in
    # pagespeed_test_framework). Reproduced here rather than imported: test
    # modules are not meant to be imported as library code, and there is no
    # framework helper to import instead.
    path = os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "/var/log/apache2/error.log")
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        out = subprocess.run(["sudo", "-n", "cat", path], capture_output=True)
        if out.returncode != 0:
            pytest.fail(f"cannot read {path}: {out.stderr!r}")
        return out.stdout.decode("utf-8", "replace")


def _read_checked_error_log() -> str:
    """The error log plus the positive control shared by every sweep below:
    an empty or truncated log, or one without the module's own startup line,
    must fail loudly rather than satisfy an absence assertion vacuously the
    way the retired bash's ungated grep did.
    """
    log = _read_error_log()
    assert log, "Apache error log is empty; cannot sweep it for a leak"
    assert STARTUP_LINE in log, (
        "Apache error log does not contain its own startup line "
        f"({STARTUP_LINE!r}); the log this test read is not the one "
        "Apache actually wrote to -- see PAGESPEED_APACHE_ERROR_LOG. "
        f"Log tail: {log[-2000:]!r}"
    )
    return log


def _log_line_epoch(line: str) -> Optional[float]:
    """Epoch seconds of an Apache error-log line's [timestamp], else None."""
    m = _LOG_TIMESTAMP.match(line)
    if not m:
        return None
    try:
        parsed = time.strptime(
            f"{m.group(1)} {m.group(2)}", "%a %b %d %H:%M:%S %Y"
        )
    except ValueError:
        return None
    return time.mktime(parsed)


def _last_resume_epoch(log: str) -> Optional[float]:
    """Epoch of the last AH00163 "resuming normal operations" line, or None.

    AH00163 is logged by the new generation on every completed start and
    graceful restart, so its last occurrence dates the fork round the
    current child set belongs to.
    """
    last = None
    for line in log.splitlines():
        if "AH00163" not in line or "resuming normal operations" not in line:
            continue
        epoch = _log_line_epoch(line)
        if epoch is not None:
            last = epoch
    return last


def _apache_processes() -> Dict[int, Tuple[int, int]]:
    """Map pid -> (ppid, age in seconds) for every apache2/httpd process.

    Same ps shape as test_graceful_restart_leak.py's _apache_child_pids(),
    plus etimes so each process's fork time can be compared against the log.
    """
    out = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,etimes=,comm="],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        timeout=15,
    ).stdout
    procs: Dict[int, Tuple[int, int]] = {}
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) < 4:
            continue
        pid_s, ppid_s, age_s, comm = parts
        if comm.strip() not in ("apache2", "httpd"):
            continue
        try:
            procs[int(pid_s)] = (int(ppid_s), int(age_s))
        except ValueError:
            continue
    return procs


@pytest.mark.apache_only  # PAGESPEED_APACHE_ERROR_LOG is Apache's log path
@pytest.mark.requires_module  # the startup line is only logged with the module installed
class TestErrorLogLeakSweep:
    """Bash: the `grep leaked_rewrite_drivers $(APACHE_LOG)` gate that ended
    every apache_debug_*_test target in install/Makefile.tests, plus the
    crash-line and child-set sweeps described in the module docstring."""

    def test_no_leaked_rewrite_drivers_on_shutdown(self):
        log = _read_checked_error_log()

        offending = [line for line in log.splitlines() if LEAK_MESSAGE in line]
        assert not offending, (
            "Apache error log contains a RewriteDriver leak on ServerContext "
            "shutdown (ServerContext::~ServerContext(), "
            "net/instaweb/rewriter/server_context.cc). Offending line(s):\n"
            + "\n".join(offending)
        )

    def test_no_child_crash_or_fatal_lines(self):
        log = _read_checked_error_log()

        offending = [line for line in log.splitlines() if CRASH_LINE.search(line)]
        assert not offending, (
            "Apache error log contains child-crash or module-fatal lines "
            "(AH00050/AH00051/AH00052, exit signal, [FATAL], Check failed:) "
            "-- a child died on a signal or a CHECK failed during the run. "
            "Offending line(s):\n" + "\n".join(offending)
        )

    def test_child_set_matches_last_restart(self):
        log = _read_checked_error_log()

        procs = _apache_processes()
        # Children are apache processes whose parent is also an apache
        # process (the master); see test_graceful_restart_leak.py.
        children = {
            pid: age for pid, (ppid, age) in procs.items() if ppid in procs
        }
        assert children, (
            "No apache2/httpd child processes found; is Apache running with "
            "a forking MPM?"
        )

        now = time.time()
        ref = _last_resume_epoch(log)
        ref_desc = "the last AH00163 (resuming normal operations) line"
        if ref is None:
            masters = [
                age for pid, (ppid, age) in procs.items() if ppid not in procs
            ]
            if len(masters) != 1:
                pytest.skip(
                    "no AH00163 line in the error log and "
                    f"{len(masters)} apache2/httpd master processes; cannot "
                    "date the fork round the current children belong to"
                )
            ref = now - masters[0]
            ref_desc = "the master process's start (no AH00163 line in the log)"

        offending = []
        for pid, age in sorted(children.items()):
            offset = (now - age) - ref
            if offset < -CHILD_START_WINDOW_S:
                offending.append(
                    f"child pid {pid} started {-offset:.0f}s BEFORE {ref_desc}"
                    " -- a worker that never exited a graceful restart"
                )
            elif offset > CHILD_START_WINDOW_S:
                offending.append(
                    f"child pid {pid} started {offset:.0f}s AFTER {ref_desc}"
                    " -- a replacement for a child that died without a log"
                    " line (SIGKILL leaves none)"
                )
        assert not offending, (
            "Apache child set does not date from the last restart:\n"
            + "\n".join(offending)
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
