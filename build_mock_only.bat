@echo off
cd /d C:\pagespeed
echo Building cross-platform mock IIS library...
bazel build --config=windows --config=clang-cl //test/pagespeed/iis:mock_iis //test/pagespeed/iis:iis_test_base
echo Build status: %errorlevel%
