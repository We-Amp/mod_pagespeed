@echo off
REM SPDX-License-Identifier: Apache-2.0
REM Copyright (c) 2024-2026 We-Amp B.V.

cd /d C:\pagespeed
echo Running IIS tests...
bazel info 2>&1
echo.
echo === Now running tests ===
bazel test --config=windows --config=clang-cl //test/pagespeed/iis:mock_iis_test --test_output=all 2>&1
