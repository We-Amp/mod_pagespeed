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

load("//bazel:pagespeed_python_test.bzl", "pagespeed_py_test", "pagespeed_py_test_suite")

# =============================================================================
# Python System Tests - System
# =============================================================================
#
# These tests are ported from pagespeed/system/system_tests/*.sh
# They typically require more complex server configurations.
#
# Run all tests:
#   bazel test //test/system/system:all --test_output=streamed \
#       --test_env=PAGESPEED_HOST=localhost --test_env=PAGESPEED_PORT=8080 \
#       --test_env=PAGESPEED_SECONDARY_HOST=localhost --test_env=PAGESPEED_SECONDARY_PORT=8081

pagespeed_py_test_suite(
    name = "all",
    srcs = [
        "test_critical_css_beacon.py",
        "test_headers.py",
        "test_ipro.py",
        "test_prioritize_critical_images.py",
    ],
    deps = ["//test/system:conftest"],
)
