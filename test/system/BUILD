licenses(["notice"])  # Apache 2

load("//bazel:pagespeed_python_test.bzl", "pagespeed_py_test")

# =============================================================================
# System Test Runner
# =============================================================================
#
# Complete system test workflow: builds module, starts Apache, runs tests.
#
# Run with:
#   bazel run //test/system:run_system_tests
#
# Or run directly in Docker:
#   ./test/system/run_system_tests.sh
#
# Options:
#   --build-only    Only build, don't run tests
#   --skip-build    Skip building, run tests only
#   --keep-running  Keep Apache running after tests
#
# Examples:
#   bazel run //test/system:run_system_tests -- -k sanity
#   bazel run //test/system:run_system_tests -- automatic/test_extend_cache.py

sh_binary(
    name = "run_system_tests",
    srcs = ["run_system_tests.sh"],
    data = [
        "//:libmod_pagespeed.so",
        "setup_apache_test.sh",
        "conftest.py",
        "//install:mod_pagespeed_example",
        "//install:mod_pagespeed_test",
        "//install:do_not_modify",
        "//test/system/automatic:all_test_files",
        "//test/system/pagespeed_test_framework:framework_files",
    ],
    visibility = ["//visibility:public"],
)

# =============================================================================
# Bash System Tests (Legacy)
# =============================================================================
#
# Apache mod_pagespeed system tests (bash-based)
#
# These tests require:
#   - Apache installed on the system
#   - sudo access (for Apache control)
#   - Network access (localhost)
#
# Run with:
#   bazel test //test/system:apache_system_test --test_output=streamed
#
# Or run a specific test:
#   bazel test //test/system:apache_system_test --test_arg=extend_cache --test_output=streamed
#
# Note: These tests are tagged "manual" because they require system setup
# that isn't available in all environments. They also require the module
# to be built first:
#   bazel build //:libmod_pagespeed.so

sh_test(
    name = "apache_system_test",
    srcs = ["apache_system_test.sh"],
    args = [],
    data = [
        "//:libmod_pagespeed.so",
        "//install:mod_pagespeed_example",
        "//install:mod_pagespeed_test",
        "//install:do_not_modify",
        "//pagespeed/automatic:system_test_files",
        "//pagespeed/apache:system_test_files",
        "//pagespeed/system:system_test_files",
    ],
    # These tests require system access
    tags = [
        "local",        # Must run locally, not remote
        "manual",       # Don't run in default test suite
        "no-sandbox",   # Needs network and system access
        "requires-network",
    ],
    # System tests can take a long time
    timeout = "eternal",  # No timeout limit
    deps = ["@bazel_tools//tools/bash/runfiles"],
)

# Filegroup for individual test scripts that can be run
filegroup(
    name = "system_test_scripts",
    srcs = [
        "apache_system_test.sh",
        "run_system_tests.sh",
        "setup_apache_test.sh",
    ],
    visibility = ["//visibility:public"],
)

# =============================================================================
# Python System Tests
# =============================================================================
#
# Python-based system tests using pytest.
#
# Run with:
#   bazel test //test/system:test_example --test_output=streamed \
#       --test_env=PAGESPEED_HOST=localhost --test_env=PAGESPEED_PORT=8080
#
# Or run directly with pytest (for development):
#   cd test/system
#   PAGESPEED_HOST=localhost PAGESPEED_PORT=8080 python -m pytest -v

# pytest configuration file
py_library(
    name = "conftest",
    srcs = ["conftest.py"],
    deps = ["//test/system/pagespeed_test_framework"],
    visibility = ["//visibility:public"],
)

# Example test demonstrating the framework
pagespeed_py_test(
    name = "test_example",
    srcs = ["test_example.py"],
    deps = [":conftest"],
)
