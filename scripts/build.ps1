<#
    Configure and build OpenDJ on Windows using the toolchain that ships with
    Visual Studio Build Tools, so nothing extra has to be installed.

    Usage:  pwsh scripts/build.ps1 [-Config RelWithDebInfo] [-Clean] [-Test] [-Run [files]]
                                   [-Setup] [-Asio [path to the Steinberg ASIO SDK]]

    A machine missing Visual Studio Build Tools or git is offered the install
    through scripts/setup-windows.ps1 before building. -Setup does only that
    and exits, like build.sh --deps.

    -Asio needs Steinberg's SDK, which is the headers and cannot be shipped with
    anything. It is not the same thing as an ASIO driver: ASIO4ALL, FL Studio
    ASIO and the driver that came with an interface are drivers, and none of
    them contains the SDK. Download it from steinberg.net, unpack it anywhere,
    and pass the folder once. Without a path the usual unpack locations are
    checked.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Clean,
    [switch]$Test,
    [switch]$Run,
    [switch]$Setup,
    [switch]$Asio,
    [string]$AsioSdkPath = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$TrackFiles
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build'

# OpenVINO from winget is an MSIX package, which is not on any search path, so
# say where it is. Nothing is passed when it is not installed; the build then
# simply goes without stem separation.
$openvinoArgs = @()
if (-not $env:OpenVINO_DIR) {
    $openvino = Get-AppxPackage -Name 'Intel.OpenVINOToolkit*' -ErrorAction SilentlyContinue |
        Sort-Object Version -Descending | Select-Object -First 1
    $openvinoCmake = if ($openvino) { Join-Path $openvino.InstallLocation 'runtime\cmake' }
    if ($openvinoCmake -and (Test-Path (Join-Path $openvinoCmake 'OpenVINOConfig.cmake'))) {
        $openvinoArgs = @("-DOpenVINO_DIR=$openvinoCmake")
    }
}

$asioArgs = @()

if ($Asio) {
    $candidates = @($AsioSdkPath) + @(
        (Join-Path $repoRoot 'external\asiosdk'),
        (Join-Path $env:USERPROFILE 'Downloads\asiosdk'),
        'C:\SDKs\asiosdk')

    # Recognised by the one header JUCE actually needs, so a folder that merely
    # has the right name is not mistaken for the SDK.
    $sdk = $candidates | Where-Object { $_ } |
           Where-Object { Test-Path (Join-Path $_ 'common\iasiodrv.h') } |
           Select-Object -First 1

    if (-not $sdk) {
        $looked = ($candidates | Where-Object { $_ }) -join ', '
        throw ("-Asio needs Steinberg's ASIO SDK and none was found. Looked in: $looked. " +
               'An ASIO driver such as ASIO4ALL is not the SDK: drivers are what you play ' +
               'through, the SDK is the headers needed to build support for them. Download ' +
               'it from steinberg.net, unpack it, and pass -AsioSdkPath <folder>.')
    }

    Write-Host "ASIO enabled, SDK at $sdk"
    $asioArgs = @('-DOPENDJ_ENABLE_ASIO=ON', "-DOPENDJ_ASIO_SDK_PATH=$sdk")
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

function Get-VsPath {
    if (-not (Test-Path $vswhere)) { return $null }
    & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
}

$vsPath = Get-VsPath
$needsSetup = $Setup -or -not $vsPath -or -not (Get-Command git -ErrorAction SilentlyContinue)

if ($needsSetup) {
    & (Join-Path $PSScriptRoot 'setup-windows.ps1')
    if ($Setup) { return }
    $vsPath = Get-VsPath
}

if (-not $vsPath) {
    throw 'No Visual Studio installation with the MSVC x64 toolset was found. Run scripts/setup-windows.ps1.'
}

# Prefer the CMake and Ninja bundled with Visual Studio over anything on PATH,
# so a machine with no separate CMake install still builds.
$vsCMake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vsNinja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'

$vsCTest = Join-Path (Split-Path -Parent $vsCMake) 'ctest.exe'

$cmake = if (Test-Path $vsCMake) { $vsCMake } else { (Get-Command cmake).Source }
$ctest = if (Test-Path $vsCTest) { $vsCTest } else { (Get-Command ctest -ErrorAction SilentlyContinue).Source }
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
& $cmake -S $repoRoot -B $buildDir @generatorArgs @asioArgs @openvinoArgs
if ($LASTEXITCODE -ne 0) { throw "Configure failed with exit code $LASTEXITCODE." }

Write-Host 'Building...' -ForegroundColor Cyan
& $cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

$exe = Get-ChildItem -Path $buildDir -Filter 'OpenDJ.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $exe) {
    throw 'Build reported success but OpenDJ.exe was not found.'
}

Write-Host "Built $($exe.FullName)" -ForegroundColor Green

if ($Test) {
    if (-not $ctest) { throw 'ctest was not found alongside cmake.' }

    Write-Host 'Running tests...' -ForegroundColor Cyan
    & $ctest --test-dir $buildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE." }
}

if ($Run) {
    if ($TrackFiles) { & $exe.FullName @TrackFiles } else { & $exe.FullName }
}
