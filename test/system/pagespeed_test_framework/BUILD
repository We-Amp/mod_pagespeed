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
        "stats.py",
    ],
    visibility = ["//visibility:public"],
)

# Unit tests for the framework itself
# These tests don't require a running server
py_test(
    name = "test_framework",
    srcs = ["test_framework.py"],
    deps = [":pagespeed_test_framework"],
    python_version = "PY3",
)

# Filegroup for all framework files (used by run_system_tests.sh)
filegroup(
    name = "framework_files",
    srcs = glob(["*.py"]),
    visibility = ["//visibility:public"],
)
