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

"""Regression guard for the silently-green-lane defect.

The py_test wrapper must propagate pytest's exit status: a failing pytest
case must turn the bazel target red, not pass silently. Before the fix,
the wrapper's __main__ block called pytest.main(...) and dropped the
return code, so the process always exited 0 and bazel reported PASS while
5 cases failed.

These tests drive the SHIPPED entry point -- test_framework.py, the exact
file bazel uses as the test_framework py_test's main -- in a subprocess
with a synthetic case file (its __main__ passes argv through to pytest).
Welding the guard to the shipped path, rather than to a copy of the
mechanism, is what makes a partial revert of just the test_framework.py
__main__ hunk detectable: the failing case below then exits 0 and this
target goes red.

Run with: python -m pytest -v test_pytest_exit_status.py
"""

import os
import pathlib
import subprocess
import sys

from pagespeed_test_framework.pytest_main import run_pytest

_HERE = pathlib.Path(__file__).absolute().parent

# The exact file bazel runs as the test_framework py_test's main, placed in
# this target's runfiles via srcs.
SHIPPED_MAIN = _HERE / "test_framework.py"

_FAILING_CASE = """
def test_deliberately_fails():
    assert False, "deliberate failure: this case must turn the target red"
"""

_PASSING_CASE = """
def test_passes():
    assert True
"""


def _run_shipped_main(tmp_path, case_source):
    """Run the shipped py_test main on a synthetic case, as bazel would."""
    test_file = tmp_path / "test_case.py"
    test_file.write_text(case_source)
    # The subprocess is plain python, not the bazel launcher: give it the
    # runfiles sys.path (the test/system dir holds the framework package)
    # so test_framework.py's own imports resolve.
    env = dict(os.environ)
    env["PYTHONPATH"] = (
        str(_HERE.parent) + os.pathsep + env.get("PYTHONPATH", "")
    )
    return subprocess.run(
        [sys.executable, str(SHIPPED_MAIN), str(test_file)],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )


def test_failing_case_exits_nonzero(tmp_path):
    """A deliberately failing case must propagate out of the shipped main."""
    result = _run_shipped_main(tmp_path, _FAILING_CASE)
    assert result.returncode != 0, (
        "shipped main swallowed pytest's exit status: a failing case "
        f"exited with {result.returncode}.\nstdout:\n{result.stdout}\n"
        f"stderr:\n{result.stderr}"
    )


def test_passing_case_exits_zero(tmp_path):
    """A passing suite still exits 0 (no false red)."""
    result = _run_shipped_main(tmp_path, _PASSING_CASE)
    assert result.returncode == 0, (
        f"shipped main failed a passing case (rc={result.returncode}).\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )


if __name__ == "__main__":
    run_pytest([__file__, "-v"])
