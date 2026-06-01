@echo off
C:
cd \pagespeed
echo Current directory: %CD%
echo.
echo Running IIS unit tests...
bazel test --config=windows --config=clang-cl //test/pagespeed/iis:mock_iis_test --test_output=all
echo.
echo Exit code: %ERRORLEVEL%
