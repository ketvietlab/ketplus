@echo off
rem Runs a command inside the KetPlus Windows build environment:
rem MSVC x64 (vcvars64), CMake, Ninja, Qt (QT_ROOT) and OpenSSL (OPENSSL_ROOT_DIR).
rem
rem   vsenv.cmd cmake --preset lite-dev
rem
rem KETPLUS_TOOLS_ROOT is where tools\windows\install-toolchain.ps1 installed
rem everything (default C:\ketplus-tools). With no arguments it only prints the
rem resolved environment. QT_ROOT / OPENSSL_ROOT_DIR already set are kept;
rem KETPLUS_QT_VERSION (e.g. 6.11.3) selects a Qt version.
setlocal
if not defined KETPLUS_TOOLS_ROOT set "KETPLUS_TOOLS_ROOT=C:\ketplus-tools"
rem Qt: QT_ROOT wins; else KETPLUS_QT_VERSION picks <root>\Qt\<version>\msvc2022_64;
rem else the highest installed version (compared as version numbers, so 6.11 > 6.9).
if not defined QT_ROOT if defined KETPLUS_QT_VERSION set "QT_ROOT=%KETPLUS_TOOLS_ROOT%\Qt\%KETPLUS_QT_VERSION%\msvc2022_64"
if not defined QT_ROOT (
    for /f "usebackq delims=" %%Q in (`powershell -NoProfile -Command "Get-ChildItem -LiteralPath $env:KETPLUS_TOOLS_ROOT\Qt -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName 'msvc2022_64\bin\qmake.exe') } | Sort-Object { [version]$_.Name } | Select-Object -Last 1 -ExpandProperty FullName"`) do set "QT_ROOT=%%Q\msvc2022_64"
)
if not defined OPENSSL_ROOT_DIR set "OPENSSL_ROOT_DIR=%KETPLUS_TOOLS_ROOT%\openssl"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo vsenv: MSVC Build Tools 2022 not found 1>&2
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "PATH=%KETPLUS_TOOLS_ROOT%\cmake\bin;%KETPLUS_TOOLS_ROOT%\ninja;%KETPLUS_TOOLS_ROOT%\python;%QT_ROOT%\bin;%OPENSSL_ROOT_DIR%\bin;%PATH%"

if "%~1"=="" (
    echo QT_ROOT=%QT_ROOT%
    echo OPENSSL_ROOT_DIR=%OPENSSL_ROOT_DIR%
    where cl cmake ninja python
    exit /b 0
)
%*
exit /b %errorlevel%
