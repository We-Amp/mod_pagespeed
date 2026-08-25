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

licenses(["notice"])  # Apache 2

# PageSpeed Python system test framework
#
# This library provides utilities for writing system tests in Python,
# replacing the bash-based system_test_helpers.sh.

py_library(
    name = "pagespeed_test_framework",
    srcs = [
        "__init__.py",
        "assertions.py",
        "client.py",
        "pytest_main.py",
        "require.py",
        "stats.py",
    ],
    # Put test/system/ on dependents' sys.path so tests can
    # `from pagespeed_test_framework import ...` (without this, only the
    # workspace root is importable and every dependent py_test fails at
    # import with ModuleNotFoundError).
    imports = [".."],
    visibility = ["//visibility:public"],
)

# Unit tests for the framework itself
# These tests don't require a running server
py_test(
    name = "test_framework",
    srcs = ["test_framework.py"],
    deps = [
        ":pagespeed_test_framework",
        # test_framework.py's TestPytestTimeoutScaling does `import conftest`
        # (test/system/conftest.py). Without the dep it is missing from the
        # runfiles and those cases fail under bazel while direct pytest --
        # which has test/system/ on sys.path -- passes.
        "//test/system:conftest",
    ],
    python_version = "PY3",
)

# Regression guard for: the py_test wrapper must propagate
# pytest's exit status, so a failing case turns the bazel target red.
# test_framework.py rides along in srcs so the guard can subprocess-run the
# exact file bazel uses as the test_framework target's main -- welding the
# guard to the shipped entry point, so a partial revert of just that file's
# __main__ hunk is caught here.
py_test(
    name = "test_pytest_exit_status",
    # Two subprocess legs, each allowed up to 120s.
    size = "medium",
    srcs = [
        "test_framework.py",
        "test_pytest_exit_status.py",
    ],
    main = "test_pytest_exit_status.py",
    deps = [":pagespeed_test_framework"],
    python_version = "PY3",
)

# Filegroup for all framework files (used by run_system_tests.sh)
filegroup(
    name = "framework_files",
    srcs = glob(["*.py"]),
    visibility = ["//visibility:public"],
)
