<#
.SYNOPSIS
    Build WhiteRhino (this repo) on native Windows with MSVC, including the
    kload resource step.

.DESCRIPTION
    kload needs kernelreloaded's kloader.elf bundled into
    bin/resources/kernelreloaded/kloader.elf before CMake configures --
    CMakeLists.txt globs bin/resources/ at configure time and copies whatever
    it finds, so that step has to run first, not as part of the CMake build
    itself. build-kloader-resource.sh does that (via the kernelreloaded
    Docker toolchain -- Windows still needs it, since kloader.elf is cross-
    compiled MIPS, not something MSVC produces); this script calls it through
    WSL, then does the native MSVC build the same way this repo's build was
    proven working under WSL2/PCSX2 earlier.

    Assumes prerequisites are already installed: Visual Studio 2022 with the
    C++ workload, Python, Git for Windows, and Docker Desktop with its WSL2
    backend (needed by build-kloader-resource.sh regardless of host OS).

.PARAMETER SkipDeps
    Skip build-dependencies.bat. Only correct on a re-run where deps\ already
    exists -- it is the multi-hour step.

.PARAMETER SkipKloaderResource
    Skip rebuilding kloader.elf. Only correct when
    bin/resources/kernelreloaded/kloader.elf is already current.

.EXAMPLE
    .\build-windows.ps1
    .\build-windows.ps1 -SkipDeps -SkipKloaderResource   # fast rebuild, PCSX2 side only
#>
[CmdletBinding()]
param(
    [switch]$SkipDeps,
    [switch]$SkipKloaderResource
)

$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot | Split-Path -Parent   # whiterhino/ root (this script lives in whiterhino/tools/)

function Say($msg) { Write-Host "`n=== $msg" -ForegroundColor Cyan }
function Note($msg) { Write-Host "    $msg" -ForegroundColor DarkGray }
function Die($msg) { Write-Host "`n*** $msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
if (-not $SkipKloaderResource) {
    Say "building kloader.elf via WSL (kernelreloaded's Docker toolchain)"
    Note "cross-compiled MIPS output -- MSVC has no part in this step"
    wsl.exe bash -lc "'$src/tools/build-kloader-resource.sh' | cat"
    if ($LASTEXITCODE -ne 0) { Die "build-kloader-resource.sh failed (exit $LASTEXITCODE)" }
}
else {
    Say "skipping kloader.elf rebuild (-SkipKloaderResource)"
    $resourceElf = Join-Path $src 'bin\resources\kernelreloaded\kloader.elf'
    if (-not (Test-Path $resourceElf)) { Die "-SkipKloaderResource given but $resourceElf does not exist" }
}

# ---------------------------------------------------------------------------
Say "checking prerequisites"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Die "Visual Studio Installer not found -- install VS2022 with the C++ workload first" }
$vsPath = & $vswhere -version "[17,18)" -latest -property installationPath 2>$null | Select-Object -First 1
if (-not $vsPath) { Die "no VS2022 install found via vswhere" }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { Die "vcvars64.bat not found at $vcvars -- C++ workload missing" }
Note "vcvars64: $vcvars"

$pythonDir = $null
foreach ($hive in 'HKCU', 'HKLM') {
    foreach ($view in Get-ChildItem "${hive}:\SOFTWARE\Python\PythonCore" -ErrorAction SilentlyContinue) {
        $p = (Get-ItemProperty "$($view.PSPath)\InstallPath" -ErrorAction SilentlyContinue).'(default)'
        if ($p -and (Test-Path (Join-Path $p 'python.exe'))) { $pythonDir = $p.TrimEnd('\'); break }
    }
    if ($pythonDir) { break }
}
if (-not $pythonDir) { Die "Python not found in the registry -- install it, open a new shell, and re-run" }
Note "python: $pythonDir\python.exe"

function Invoke-VsBatch([string]$Body, [string]$What) {
    $bat = Join-Path $env:TEMP ("whiterhino-" + [guid]::NewGuid().ToString('N') + ".bat")
    @"
@echo off
set "PATH=%PATH%;${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$pythonDir"
call "$vcvars" >nul || exit /b 1
$Body
"@ | Set-Content -Encoding ASCII $bat
    & cmd.exe /c $bat
    $rc = $LASTEXITCODE
    Remove-Item $bat -Force -ErrorAction SilentlyContinue
    if ($rc -ne 0) { Die "$What failed (exit $rc)" }
}

# ---------------------------------------------------------------------------
if (-not $SkipDeps) {
    Say "building dependencies (Qt, FFmpeg, SDL3, shaderc) -- hours, not minutes"
    Note "Re-run with -SkipDeps to skip this once deps\ exists."
    Invoke-VsBatch @"
cd /d "$src"
call .github\workflows\scripts\windows\build-dependencies.bat || exit /b 1
"@ "build-dependencies.bat"
}
else {
    Say "skipping dependency build (-SkipDeps)"
    if (-not (Test-Path (Join-Path $src 'deps'))) { Die "-SkipDeps given but deps\ does not exist" }
}

# ---------------------------------------------------------------------------
Say "configuring and building PCSX2"
Invoke-VsBatch @"
cd /d "$src"
cmake . -B build -DCMAKE_PREFIX_PATH="$src\deps" -DQT_BUILD=ON -DCMAKE_BUILD_TYPE=Release -DDISABLE_ADVANCE_SIMD=ON -G Ninja || exit /b 1
cmake --build build --config Release || exit /b 1
cmake --install build --config Release || exit /b 1
"@ "cmake build"

# ---------------------------------------------------------------------------
Say "done"
$exe = Get-ChildItem -Path (Join-Path $src 'bin') -Filter 'pcsx2-qt*.exe' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if ($exe) {
    Write-Host "`nBuilt: $($exe.FullName)" -ForegroundColor Green
    Write-Host "Timestamp: $($exe.LastWriteTime)" -ForegroundColor Green
}
else {
    Note "pcsx2-qt.exe not found under $src\bin -- check the log above"
}
