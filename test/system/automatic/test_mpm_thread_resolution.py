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

"""Regression test for MPM thread-count resolution at Apache post-config.

Background
----------
The optimization thread counts are computed from two inputs: the
effective CPU budget, and how many httpd children share the machine. The
second one comes from ``ap_mpm_query(AP_MPMQ_MAX_DAEMONS)``.

Before the #535/#604 fix that query was made during Apache's *config read*
pass. At that point a DSO MPM -- which is what every distribution package
ships -- has not yet computed its child count, so the query answers 0.
``ProcessConcurrencyFromMpmInfo()`` correctly refuses to divide by that and
reports ``kUnknownProcessConcurrency``, and the policy then takes its
one-thread-per-pool floor. The server came up with 1 + 1 threads on every
machine, whatever its size, and the only trace was a log line saying the
child-process count could not be read -- which nobody was looking for.

The fix moved resolution to ``pagespeed_post_config()``, after
``check_config`` has run and the MPM can answer. Nothing in the test tree
asserted the *outcome* of that resolution, so this file does.

What is asserted
----------------
``ApacheRewriteDriverFactory::LogThreadCountResolution()`` emits exactly one
of two lines at kWarning (chosen so it survives Apache's default
``LogLevel warn``):

  healthy   ``... Effective CPU budget: N whole cores (limited by S);
            httpd children: P; MPM: non-threaded (ThreadsPerChild=T). ...``
  degraded  ``... Could not read the configured child-process count from the
            MPM, so the minimum was used. ... Effective CPU budget: N whole
            cores (limited by S).``

Note that BOTH contain the text ``Effective CPU budget:``. A substring
assertion on that field alone therefore passes in the failure case, which is
why this test matches the healthy line as a whole and asserts the degraded
sentence is absent by its own distinct wording.

On this lane ``setup_apache_test.sh`` pins the prefork MPM with
``ServerLimit 6`` / ``MaxRequestWorkers 6``, so ``AP_MPMQ_MAX_DAEMONS`` is 6
and prefork's ``AP_MPMQ_MAX_THREADS`` is a hardcoded 1. Post-fix the log must
therefore say ``httpd children: 6`` and ``MPM: non-threaded
(ThreadsPerChild=1)``; pre-fix it says it could not read the count at all.

Why the thread *counts* are derived from the log line
-----------------------------------------------------
The counts depend on ``effective_cores``, which is
``min(online CPUs, sched_getaffinity mask, cgroup quota)`` -- not
``nproc``. Under ``docker --cpus=`` or systemd ``CPUQuota=`` those diverge,
and a test that reimplemented the detection would go red for the wrong
reason. So this test parses the ``Effective CPU budget: N whole cores`` value
out of the very line it is asserting and feeds it back through the policy
formula, the same trick the ``Expected()`` helper in
``test/pagespeed/system/system_rewrite_driver_factory_test.cc`` uses.

With 6 children most runners floor to 1 + 1 anyway, so the discriminating
power of the number itself is low on this lane -- the real signal is the
healthy-vs-degraded shape and the ``httpd children``/``MPM`` fields.

Why "at least one match", never "exactly one"
---------------------------------------------
httpd runs its whole configuration sequence TWICE at startup (pass 1 and
pass 2 of the restart loop in ``main.c``), and ``pagespeed_post_config()``
has no second-pass guard, so the line is emitted at least twice per start --
plus once more per ``apache2ctl graceful``, which
``test_graceful_restart_leak.py`` issues 25 times earlier in the same run.
This test therefore asserts "at least one match, and EVERY match is healthy".

(Relatedly: ``apache2ctl -t`` cannot be used to drive this. httpd exits after
``ap_run_test_config`` in pass 1, before ``open_logs`` and ``post_config``,
so ``-t`` never reaches the code under test.)

Environment
-----------
Apache only. Reads the Apache error log; self-skips unless the harness
exports (run_system_tests.sh does):
  PAGESPEED_APACHE_ERROR_LOG  - path to the Apache error log
Passwordless ``sudo`` is used only as a fallback when the log is not
world-readable.
"""

import os
import re
import subprocess
from typing import List, Optional, Tuple

import pytest

# ---------------------------------------------------------------------------
# The literals below are transcribed from the product source. Keep them in
# sync with:
#   pagespeed/apache/apache_rewrite_driver_factory.cc  LogThreadCountResolution
#   pagespeed/system/system_rewrite_driver_factory.cc  LogThreadCountResolution
#                                                      WarnIfThreadCountsNotFinalized
# ---------------------------------------------------------------------------

# Every resolution line, healthy or degraded, starts with this.
RESOLUTION_PREFIX = "PageSpeed optimization threads:"

# The healthy Apache line, in full. Anchoring on the whole sentence (rather
# than on "Effective CPU budget:", which the degraded line also contains) is
# the point of this regex.
HEALTHY_RE = re.compile(
    r"PageSpeed optimization threads: (?P<rewrite>\d+) rewrite, "
    r"(?P<expensive>\d+) expensive rewrite \(per httpd child\)\. "
    r"Effective CPU budget: (?P<cores>\d+) whole cores "
    r"\(limited by (?P<source>[^)]+)\); "
    r"httpd children: (?P<children>\d+); "
    r"MPM: (?P<mpm>threaded|non-threaded) "
    r"\(ThreadsPerChild=(?P<threads_per_child>\d+)\)\. "
    r"Override with NumRewriteThreads and NumExpensiveRewriteThreads\."
)

# The Apache degraded branch: the MPM could not be asked for its child count.
# This is the legacy outcome and is what this test exists to catch.
DEGRADED_APACHE = (
    "Could not read the configured child-process count from the MPM, "
    "so the minimum was used."
)

# The base-class degraded branch. On Apache the override above always wins, so
# seeing this means ApacheRewriteDriverFactory::LogThreadCountResolution() is
# no longer being called at all.
DEGRADED_BASE = "Could not determine how many server processes share this machine"

# SystemRewriteDriverFactory::WarnIfThreadCountsNotFinalized(): a worker pool
# was built before resolution ran, so every pool silently got one thread. This
# is what deleting the FinalizeThreadCounts() call site produces.
NOT_FINALIZED = (
    "PageSpeed optimization worker pool created before the thread counts "
    "were resolved"
)

# the design record, pagespeed/system/optimization_thread_policy.h.
CPU_SHARE_NUMERATOR = 1
CPU_SHARE_DENOMINATOR = 2

# What setup_apache_test.sh pins for the prefork MPM on this lane. If those
# values move, this test must move with them -- and so must
# test_graceful_restart_leak.py, which depends on the same pinning.
EXPECTED_HTTPD_CHILDREN = 6

# prefork answers AP_MPMQ_IS_THREADED with AP_MPMQ_NOT_SUPPORTED, so
# QueryMpmThreadInfo() leaves max_threads at its default of 1 and
# IsThreadedFromMpmInfo() reports false.
EXPECTED_MPM_CLASS = "non-threaded"
EXPECTED_THREADS_PER_CHILD = 1


def _error_log_path() -> str:
    return os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "/var/log/apache2/error.log")


def _read_error_log() -> str:
    """Read the Apache error log whole.

    Whole, not tailed: the resolution line is emitted at server startup, which
    is long before this test runs, and on a lane where the graceful-restart
    test has already run a tail can push the startup lines out of the window.
    The CI container starts from an empty log, so the file is small.
    """
    path = _error_log_path()
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", errors="replace")
    except (FileNotFoundError, PermissionError, IsADirectoryError):
        # Best-effort fallback via sudo if the log isn't world-readable.
        try:
            # Binary, not text=True: the error log is not guaranteed to be
            # valid UTF-8. Apache logs raw request/response fragments, so a
            # gzip or image payload puts arbitrary bytes in the file and a
            # strict decode raises UnicodeDecodeError. Decode permissively --
            # every string this test matches on is plain ASCII.
            res = subprocess.run(
                ["sudo", "-n", "cat", path],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=30,
            )
            return res.stdout.decode("utf-8", errors="replace")
        except (subprocess.SubprocessError, OSError):
            return ""


def _expected_budget(effective_cores: int, concurrent_processes: int) -> int:
    """Mirror of ComputeOptimizationThreadCounts() in
    pagespeed/system/optimization_thread_policy.cc.

    budget = max(1, floor(effective_cores * share / concurrent_processes))
    and both pools get the whole budget.
    """
    if effective_cores < 1:
        effective_cores = 1
    if concurrent_processes <= 0:
        return 1
    numerator = effective_cores * CPU_SHARE_NUMERATOR
    denominator = CPU_SHARE_DENOMINATOR * concurrent_processes
    return max(1, numerator // denominator)


def _resolution_lines(log_text: str) -> List[str]:
    return [line for line in log_text.splitlines() if RESOLUTION_PREFIX in line]


def _find_lines(log_text: str, needle: str) -> List[str]:
    return [line for line in log_text.splitlines() if needle in line]


def _parse_healthy(line: str) -> Optional[Tuple[int, int, int, str, int, int]]:
    """(rewrite, expensive, cores, mpm_class, threads_per_child, children)."""
    m = HEALTHY_RE.search(line)
    if m is None:
        return None
    return (
        int(m.group("rewrite")),
        int(m.group("expensive")),
        int(m.group("cores")),
        m.group("mpm"),
        int(m.group("threads_per_child")),
        int(m.group("children")),
    )


@pytest.mark.apache_only
@pytest.mark.requires_module
class TestMpmThreadCountResolution:
    """The MPM must be queryable at post-config, and the log must say so."""

    def test_thread_counts_resolve_from_the_mpm(self, server_config):
        # Hard gate: the log line under test is Apache-specific (the nginx and
        # IIS ports emit the base-class wording).
        if server_config.server_type not in ("apache", "auto"):
            pytest.skip("MPM thread-resolution test only runs on Apache")

        error_log = _error_log_path()
        log_text = _read_error_log()
        if not log_text:
            pytest.skip(f"Apache error log not readable or empty: {error_log}")

        lines = _resolution_lines(log_text)

        # --- 1. The degraded branch must not have been taken --------------
        #
        # Checked first and by its own distinct wording, because the degraded
        # line ALSO contains "Effective CPU budget:" -- so this is the
        # assertion a careless substring check would miss.
        degraded = _find_lines(log_text, DEGRADED_APACHE)
        assert not degraded, (
            "Apache took the DEGRADED thread-resolution path: it could not read "
            "the child-process count from the MPM, so every pool fell back to "
            "one thread. This is the #535/#604 defect -- the counts are being "
            "resolved during the config-read pass, when a DSO MPM's statics are "
            "still zero, instead of at post-config. Offending log line(s):\n"
            + "\n".join(degraded)
        )

        base_degraded = _find_lines(log_text, DEGRADED_BASE)
        assert not base_degraded, (
            "The BASE-class thread-resolution message was logged on Apache, so "
            "ApacheRewriteDriverFactory::LogThreadCountResolution() is no longer "
            "being called and the Apache-specific MPM divisor is not being used. "
            "Offending log line(s):\n" + "\n".join(base_degraded)
        )

        # --- 2. No pool was built before resolution ran -------------------
        not_finalized = _find_lines(log_text, NOT_FINALIZED)
        assert not not_finalized, (
            "A PageSpeed worker pool was created before the thread counts were "
            "resolved, so it silently got one thread. The port's "
            "FinalizeThreadCounts() call site (pagespeed_post_config() in "
            "pagespeed/apache/mod_instaweb.cc) is missing or runs too late. "
            "Offending log line(s):\n" + "\n".join(not_finalized)
        )

        # --- 3. The healthy line is present -------------------------------
        assert lines, (
            "No PageSpeed thread-count resolution line in the Apache error log "
            f"({error_log}). The resolution logs unconditionally at kWarning on "
            "every startup, so its complete absence means resolution never ran "
            "at all -- e.g. the FinalizeThreadCounts() call was removed from "
            "pagespeed_post_config() -- or the module is not loaded."
        )

        parsed = []
        unrecognized = []
        for line in lines:
            fields = _parse_healthy(line)
            if fields is None:
                unrecognized.append(line)
            else:
                parsed.append((line, fields))

        # Every resolution line must be the healthy one. Never a count of
        # exactly 1: httpd runs the config sequence twice at startup and once
        # more per graceful restart, so the line legitimately repeats.
        assert not unrecognized, (
            "Thread-resolution line(s) in the Apache error log matched neither "
            "the healthy nor the known degraded format. Either the message "
            "wording changed (update HEALTHY_RE in this file to match "
            "ApacheRewriteDriverFactory::LogThreadCountResolution()) or an "
            "unexpected branch was taken:\n" + "\n".join(unrecognized)
        )
        assert parsed  # implied by the two assertions above; stated for clarity

        # --- 4. Every healthy line reports the pinned prefork shape -------
        for line, (rewrite, expensive, cores, mpm, tpc, children) in parsed:
            assert children == EXPECTED_HTTPD_CHILDREN, (
                f"Resolution reported 'httpd children: {children}', expected "
                f"{EXPECTED_HTTPD_CHILDREN}. setup_apache_test.sh pins the "
                f"prefork MPM to ServerLimit/MaxRequestWorkers "
                f"{EXPECTED_HTTPD_CHILDREN}, which is what "
                f"AP_MPMQ_MAX_DAEMONS must answer once check_config has run. "
                f"A different number means either the MPM pinning moved (update "
                f"this constant AND check test_graceful_restart_leak.py, which "
                f"depends on the same pinning) or the query is being made at "
                f"the wrong time.\n  {line}"
            )
            assert mpm == EXPECTED_MPM_CLASS, (
                f"Resolution classified the MPM as '{mpm}', expected "
                f"'{EXPECTED_MPM_CLASS}': this lane pins prefork, which reports "
                f"AP_MPMQ_NOT_SUPPORTED for AP_MPMQ_IS_THREADED.\n  {line}"
            )
            assert tpc == EXPECTED_THREADS_PER_CHILD, (
                f"Resolution reported ThreadsPerChild={tpc}, expected "
                f"{EXPECTED_THREADS_PER_CHILD}: prefork's AP_MPMQ_MAX_THREADS "
                f"is a hardcoded 1.\n  {line}"
            )

            # --- 5. The counts follow the policy, using the line's own
            #        effective-core figure as the input (never nproc).
            expected = _expected_budget(cores, children)
            assert rewrite == expected and expensive == expected, (
                f"Resolution computed {rewrite} rewrite / {expensive} expensive "
                f"threads, but the design record's policy on {cores} effective cores "
                f"across {children} httpd children gives {expected} for each "
                f"pool (max(1, floor(cores * "
                f"{CPU_SHARE_NUMERATOR}/{CPU_SHARE_DENOMINATOR} / children))).\n"
                f"  {line}"
            )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
