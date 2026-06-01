@echo off
cd /d C:\pagespeed
echo Building mock_iis library...
bazel build --config=windows --config=clang-cl //test/pagespeed/iis:mock_iis
if %errorlevel% neq 0 (
    echo Build failed with error %errorlevel%
    exit /b %errorlevel%
)
echo Build succeeded!
echo.
echo Running mock_iis_test...
bazel test --config=windows --config=clang-cl //test/pagespeed/iis:mock_iis_test --test_output=all
echo Test completed with error %errorlevel%
