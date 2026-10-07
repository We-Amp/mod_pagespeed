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
        ":external_origin_files",
    ],
    visibility = ["//visibility:public"],
)

# The external-origin stand-in content setup_test_content() copies into
# /var/www/external_origin (see setup_apache_test.sh); not runfiles by
# default, so the Bazel-run entry point needs it listed explicitly.
filegroup(
    name = "external_origin_files",
    srcs = glob(["external_origin/**"]),
    visibility = ["//visibility:public"],
)

# Filegroup for individual test scripts that can be run
filegroup(
    name = "system_test_scripts",
    srcs = [
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
    # Put test/system/ on dependents' sys.path so they can `import conftest`,
    # mirroring how direct pytest runs (cwd on sys.path) resolve it. Without
    # this the dep adds the file to the runfiles but the import still fails.
    imports = ["."],
    deps = ["//test/system/pagespeed_test_framework"],
    visibility = ["//visibility:public"],
)

# Example test demonstrating the framework
pagespeed_py_test(
    name = "test_example",
    srcs = ["test_example.py"],
    deps = [":conftest"],
)
