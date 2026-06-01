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

"""Bazel rules for PageSpeed Python system tests.

These rules provide py_test wrappers configured for PageSpeed system testing.
"""

def pagespeed_py_test(
        name,
        srcs,
        deps = [],
        data = [],
        args = [],
        env = {},
        tags = [],
        timeout = "moderate",
        size = "medium",
        **kwargs):
    """Define a PageSpeed Python system test.

    This rule creates a py_test with the pagespeed_test_framework dependency
    and common configuration for system tests.

    Args:
        name: Test name
        srcs: Python source files
        deps: Additional dependencies
        data: Data files needed by the test
        args: Additional pytest arguments
        env: Environment variables
        tags: Test tags
        timeout: Test timeout (default "moderate")
        size: Test size (default "medium")
        **kwargs: Additional py_test arguments
    """
    # Default pytest arguments
    default_args = [
        "-v",
        "--tb=short",
    ]

    # Default tags for system tests
    default_tags = [
        "local",        # Must run locally
        "manual",       # Don't run in default test suite
        "requires-network",
    ]

    native.py_test(
        name = name,
        srcs = srcs,
        main = srcs[0] if len(srcs) == 1 else None,
        deps = deps + [
            "//test/system/pagespeed_test_framework",
        ],
        data = data,
        args = default_args + args,
        env = env,
        tags = tags + default_tags,
        timeout = timeout,
        size = size,
        python_version = "PY3",
        **kwargs
    )


def pagespeed_py_test_suite(
        name,
        srcs,
        deps = [],
        data = [],
        tags = [],
        **kwargs):
    """Define a suite of PageSpeed Python system tests.

    Creates individual test targets for each source file plus a
    test_suite target that groups them all.

    Args:
        name: Suite name
        srcs: List of Python test files
        deps: Additional dependencies
        data: Data files needed by tests
        tags: Additional test tags
        **kwargs: Additional arguments passed to each test
    """
    test_names = []

    for src in srcs:
        # Derive test name from filename: test_foo.py -> test_foo
        test_name = src.replace(".py", "")
        test_names.append(test_name)

        pagespeed_py_test(
            name = test_name,
            srcs = [src],
            deps = deps,
            data = data,
            tags = tags,
            **kwargs
        )

    # Create a test suite grouping all tests
    native.test_suite(
        name = name,
        tests = [":" + t for t in test_names],
        tags = tags,
    )
