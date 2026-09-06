@echo off
REM SPDX-License-Identifier: Apache-2.0
REM Copyright (c) 2024-2026 We-Amp B.V.

:: compile_rc.bat -- find a usable rc.exe / llvm-rc.exe across runner
:: configurations and invoke it. Used by pagespeed_iis_res_gen genrule.
::
:: Usage: compile_rc.bat <output.res> <input.rc>
::
:: Probe order:
::   1. rc.exe on PATH (vcvarsall.bat-prepared shells)
::   2. llvm-rc.exe on PATH (clang-cl installs bundle it)
::   3. llvm-rc.exe next to clang-cl on PATH (Bazel's --config=clang-cl)
::   4. rc.exe under Windows Kits 10\bin\<version>\x64\rc.exe
::      (highest lexically-sorting version wins -- correct for SDKs)
::
:: Runners differ:
::   one host:    Windows SDK 10.0.22621.0 with rc.exe in its x64 bin,
::                 LLVM 19 at C:\Program Files\LLVM\bin\
::   another host: SDKs installed without binaries (headers-only flavor),
::                 LLVM 19 at C:\Program Files\LLVM\bin\
::
:: llvm-rc.exe is a drop-in replacement for rc.exe -- accepts the same
:: /nologo /fo /D flags and produces compatible .res output.

setlocal enabledelayedexpansion

set "OUT=%~1"
set "IN=%~2"
if "%OUT%"=="" goto :usage
if "%IN%"=="" goto :usage

set "RC="

:: 1. rc.exe on PATH.
where rc.exe >nul 2>&1
if not errorlevel 1 (
    for /f "delims=" %%P in ('where rc.exe') do set "RC=%%P" & goto :have_rc
)

:: 2. llvm-rc.exe on PATH.
where llvm-rc.exe >nul 2>&1
if not errorlevel 1 (
    for /f "delims=" %%P in ('where llvm-rc.exe') do set "RC=%%P" & goto :have_rc
)

:: 3. llvm-rc.exe next to clang-cl on PATH.
where clang-cl.exe >nul 2>&1
if not errorlevel 1 (
    for /f "delims=" %%P in ('where clang-cl.exe') do (
        for %%F in ("%%P") do (
            if exist "%%~dpFllvm-rc.exe" set "RC=%%~dpFllvm-rc.exe" & goto :have_rc
        )
    )
)

:: 4. Probe Windows Kits 10. Iterate version dirs; keep the
::    lexically-highest one that has x64\rc.exe (SDK version numbers
::    are zero-padded so lexical = semantic ordering).
set "KITS=C:\Program Files (x86)\Windows Kits\10\bin"
if exist "%KITS%" (
    for /f "delims=" %%V in ('dir /b /ad /on "%KITS%" 2^>nul') do (
        if exist "%KITS%\%%V\x64\rc.exe" set "RC=%KITS%\%%V\x64\rc.exe"
    )
)
if defined RC goto :have_rc

echo compile_rc.bat: could not locate rc.exe or llvm-rc.exe ^(checked PATH, 1>&2
echo                 clang-cl sibling dir, and %KITS%\^*\x64\rc.exe^) 1>&2
exit /b 1

:have_rc
echo compile_rc.bat: using "!RC!"
"!RC!" /nologo /fo "%OUT%" "%IN%"
exit /b %errorlevel%

:usage
echo Usage: compile_rc.bat ^<output.res^> ^<input.rc^> 1>&2
exit /b 2
