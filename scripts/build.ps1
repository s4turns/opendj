<#
    Configure and build OpenDJ on Windows using the toolchain that ships with
    Visual Studio Build Tools, so nothing extra has to be installed.

    Usage:  pwsh scripts/build.ps1 [-Config RelWithDebInfo] [-Clean] [-Run]
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Clean,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe not found. Install Visual Studio 2022 Build Tools with the C++ workload.'
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw 'No Visual Studio installation with the MSVC x64 toolset was found.'
}

# Prefer the CMake and Ninja bundled with Visual Studio over anything on PATH,
# so a machine with no separate CMake install still builds.
$vsCMake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vsNinja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'

$cmake = if (Test-Path $vsCMake) { $vsCMake } else { (Get-Command cmake).Source }
$generatorArgs = if (Test-Path $vsNinja) {
    @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$vsNinja", "-DCMAKE_BUILD_TYPE=$Config")
} else {
    @("-DCMAKE_BUILD_TYPE=$Config")
}

# Import the MSVC environment so Ninja can find cl.exe, the linker and headers.
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$envDump = & "$env:ComSpec" /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
    }
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "Configuring ($Config)..." -ForegroundColor Cyan
& $cmake -S $repoRoot -B $buildDir @generatorArgs
if ($LASTEXITCODE -ne 0) { throw "Configure failed with exit code $LASTEXITCODE." }

Write-Host 'Building...' -ForegroundColor Cyan
& $cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

$exe = Get-ChildItem -Path $buildDir -Filter 'OpenDJ.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($exe) {
    Write-Host "Built $($exe.FullName)" -ForegroundColor Green
    if ($Run) { & $exe.FullName }
} else {
    Write-Warning 'Build reported success but OpenDJ.exe was not found.'
}
