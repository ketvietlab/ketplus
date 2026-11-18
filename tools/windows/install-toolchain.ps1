<#
.SYNOPSIS
  Installs the KetPlus Windows build toolchain on a Windows 10/11 x64 machine.

.DESCRIPTION
  Run in an elevated PowerShell (or over ssh as an administrator). Installs
  Python 3, CMake, Ninja, Visual Studio 2022 Build Tools (C++ x64), Qt and
  OpenSSL 3 under -Root, then prints the versions. Safe to re-run: steps whose
  result already exists are skipped. Nothing here is machine specific; pass the
  locations as parameters.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File install-toolchain.ps1 -Root C:\ketplus-tools
#>
param(
    [string]$Root = 'C:\ketplus-tools',
    [string]$QtVersion = '6.11.3',
    [string]$QtArch = 'win64_msvc2022_64',
    [string[]]$QtModules = @('qtwebsockets', 'qtwebview', 'qt5compat', 'qtshadertools', 'qtimageformats'),
    [string]$PythonVersion = '3.12.8',
    [string]$CMakeVersion = '3.31.6',
    [string]$NinjaVersion = '1.12.1',
    [switch]$SkipMsvc,
    [switch]$SkipQt,
    [switch]$SkipOpenSsl
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Force -Path $Root, "$Root\dl" | Out-Null

function Get-File([string]$Url, [string]$Name) {
    $dest = Join-Path "$Root\dl" $Name
    if (-not (Test-Path $dest)) { Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $dest }
    return $dest
}

# Python, installed for all users into $Root\python. PATH is not modified
# (PrependPath=0); tools\windows\vsenv.cmd adds it for builds.
$py = "$Root\python\python.exe"
if (-not (Test-Path $py)) {
    $exe = Get-File "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-amd64.exe" "python-$PythonVersion.exe"
    $p = Start-Process $exe -Wait -PassThru -ArgumentList "/quiet InstallAllUsers=1 TargetDir=$Root\python PrependPath=0 Include_test=0 Include_launcher=0"
    if ($p.ExitCode -notin 0, 3010 -or -not (Test-Path $py)) { throw "Python installer failed: $($p.ExitCode)" }
}

# CMake
if (-not (Test-Path "$Root\cmake\bin\cmake.exe")) {
    $zip = Get-File "https://github.com/Kitware/CMake/releases/download/v$CMakeVersion/cmake-$CMakeVersion-windows-x86_64.zip" "cmake-$CMakeVersion.zip"
    Expand-Archive $zip -DestinationPath "$Root\dl\cmake-x" -Force
    Move-Item (Get-ChildItem "$Root\dl\cmake-x")[0].FullName "$Root\cmake" -Force
}

# Ninja
if (-not (Test-Path "$Root\ninja\ninja.exe")) {
    $zip = Get-File "https://github.com/ninja-build/ninja/releases/download/v$NinjaVersion/ninja-win.zip" "ninja-$NinjaVersion.zip"
    Expand-Archive $zip -DestinationPath "$Root\ninja" -Force
}

# MSVC Build Tools 2022 (x64) + Windows SDK. Qt 6.11 needs a recent toolset
# (MSVC 14.4x, Visual Studio 17.10 or newer): an older install is updated.
if (-not $SkipMsvc) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $component = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    $have = (Test-Path $vswhere) -and (& $vswhere -products * -latest -version '[17.10,)' -requires $component -property installationPath)
    if (-not $have) {
        $bt = Get-File 'https://aka.ms/vs/17/release/vs_BuildTools.exe' 'vs_BuildTools.exe'
        $old = if (Test-Path $vswhere) { & $vswhere -products * -latest -version '[17.0,)' -property installationPath } else { $null }
        $add = @('--add', $component, '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621')
        $common = @('--quiet', '--wait', '--norestart', '--nocache')
        if ($old) {
            $p = Start-Process $bt -Wait -PassThru -ArgumentList (@('update', '--installPath', "`"$old`"") + $common)
            if ($p.ExitCode -notin 0, 3010) { throw "vs_BuildTools update failed: $($p.ExitCode)" }
            $p = Start-Process $bt -Wait -PassThru -ArgumentList (@('modify', '--installPath', "`"$old`"") + $common + $add)
        } else {
            $p = Start-Process $bt -Wait -PassThru -ArgumentList ($common + $add)
        }
        if ($p.ExitCode -notin 0, 3010) { throw "vs_BuildTools failed: $($p.ExitCode)" }
    }
}

# Qt: tools/windows/install_qt.py reads the online repository directly
# (aqtinstall does not support the Qt 6.11 repository layout).
if (-not $SkipQt) {
    $qtDir = "$Root\Qt\$QtVersion\msvc2022_64"
    if (-not (Test-Path "$qtDir\bin\qmake.exe")) {
        & $py -m pip install --quiet py7zr
        $qtArgs = @("$PSScriptRoot\install_qt.py", '--version', $QtVersion, '--dest', "$Root\Qt")
        foreach ($m in $QtModules) { $qtArgs += @('--module', $m) }
        & $py @qtArgs
        if ($LASTEXITCODE) { throw 'Qt install failed' }
    }
}

# OpenSSL 3: the prebuilt MSVC build Qt publishes (unpacked into $Root\openssl).
if (-not $SkipOpenSsl) {
    if (-not (Test-Path "$Root\openssl\include\openssl\ssl.h")) {
        & $py -m pip install --quiet py7zr
        & $py "$PSScriptRoot\install_qt.py" --version $QtVersion --dest "$Root\Qt" --openssl-only --openssl-dest "$Root\openssl"
        if ($LASTEXITCODE) { throw 'OpenSSL install failed' }
    }
}

Write-Host '--- installed ---'
& $py --version
& "$Root\cmake\bin\cmake.exe" --version | Select-Object -First 1
Write-Host "ninja $(& "$Root\ninja\ninja.exe" --version)"
if (Test-Path "$Root\Qt\$QtVersion\msvc2022_64\bin\qmake.exe") { Write-Host "Qt $QtVersion at $Root\Qt\$QtVersion\msvc2022_64" }
if (Test-Path "$Root\openssl\include\openssl\opensslv.h") { Select-String -Path "$Root\openssl\include\openssl\opensslv.h" -Pattern 'OPENSSL_VERSION_STR' | Select-Object -First 1 | ForEach-Object Line }
Write-Host "Set QT_ROOT=$Root\Qt\$QtVersion\msvc2022_64 and OPENSSL_ROOT_DIR=$Root\openssl (tools\windows\vsenv.cmd sets both and the compiler environment)."
