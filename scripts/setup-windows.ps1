<#
    Install what OpenDJ needs to build on Windows, so a fresh machine can go
    from nothing to a working build with one command.

    Usage:  pwsh scripts/setup-windows.ps1 [-Check] [-Yes] [-DryRun]

    What a build needs:
      * Visual Studio 2022 Build Tools with the C++ workload: the MSVC x64
        toolset and the Windows SDK. The "C++ CMake tools" component comes with
        it and brings the CMake and Ninja that build.ps1 prefers, so neither
        has to be installed separately.
      * git, because CMake clones JUCE at configure time.
    And to run RTMP broadcasts rather than build:
      * ffmpeg with libx264 (Gyan.FFmpeg), which the broadcaster launches.
    Everything else is fetched by CMake during the build.

    Not installed here: Steinberg's ASIO SDK. Its licence forbids
    redistribution, so it stays opt-in; see build.ps1 -Asio.

    Options:
        -Check    Report what is present and what is missing; install nothing.
        -Yes      Do not ask before installing. The Build Tools are several GB.
        -DryRun   Print the install commands instead of running them.
        -ForceMissing  Treat these as missing ('git', 'msvc', 'ffmpeg'); for testing.
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Yes,
    [switch]$DryRun,
    [ValidateSet('git', 'msvc', 'ffmpeg')]
    [string[]]$ForceMissing = @()
)

$ErrorActionPreference = 'Stop'

function Test-Git { Get-Command git -ErrorAction SilentlyContinue }

# winget's shim and package folder count too: a session that predates the
# install has a stale PATH, and the app looks in the same places.
function Test-Ffmpeg {
    if (Get-Command ffmpeg -ErrorAction SilentlyContinue) { return $true }
    $winget = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet'
    if (Test-Path (Join-Path $winget 'Linksfmpeg.exe')) { return $true }
    [bool](Get-ChildItem (Join-Path $winget 'Packages') -Filter 'Gyan.FFmpeg*' -ErrorAction SilentlyContinue |
        Get-ChildItem -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1)
}

function Get-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
}

# A freshly installed tool is on the machine PATH but not on this session's, and
# restarting the terminal just to build would defeat the point of this script.
function Update-SessionPath {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$machine;$user"
}

$missing = [ordered]@{}
if ($ForceMissing -contains 'ffmpeg' -or -not (Test-Ffmpeg)) { $missing['ffmpeg'] = 'ffmpeg' }
if ($ForceMissing -contains 'git' -or -not (Test-Git)) { $missing['git'] = 'git' }
if ($ForceMissing -contains 'msvc' -or -not (Get-VsInstall)) {
    $missing['msvc'] = 'Visual Studio 2022 Build Tools (C++ workload, CMake, Ninja, Windows SDK)'
}

Write-Host 'Build requirements:' -ForegroundColor Cyan
foreach ($item in @(@('git', 'git'), @('msvc', 'Visual Studio 2022 Build Tools (C++ workload)'), @('ffmpeg', 'ffmpeg (RTMP broadcast)'))) {
    if ($missing.Contains($item[0])) {
        Write-Host "  [missing] $($item[1])" -ForegroundColor Yellow
    } else {
        Write-Host "  [ok]      $($item[1])" -ForegroundColor Green
    }
}

if ($missing.Count -eq 0) {
    Write-Host 'Nothing to install.' -ForegroundColor Green
    return
}

if ($Check) {
    Write-Host 'Run scripts/setup-windows.ps1 to install the missing pieces.'
    exit 1
}

$vsArgs = '--passive --wait --norestart ' +
    '--add Microsoft.VisualStudio.Workload.VCTools ' +
    '--add Microsoft.VisualStudio.Component.VC.CMake.Project ' +
    '--includeRecommended'

$common = @('--exact', '--accept-package-agreements', '--accept-source-agreements')
$commands = @()
if ($missing.Contains('git')) {
    $commands += , (@('install', '--id', 'Git.Git') + $common)
}
if ($missing.Contains('ffmpeg')) {
    $commands += , (@('install', '--id', 'Gyan.FFmpeg') + $common)
}
if ($missing.Contains('msvc')) {
    $commands += , (@('install', '--id', 'Microsoft.VisualStudio.2022.BuildTools') + $common +
                    @('--override', $vsArgs))
}

if ($DryRun) {
    foreach ($c in $commands) { Write-Host "winget $($c -join ' ')" }
    return
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw ('winget was not found. It ships with Windows 11 as part of App Installer; ' +
           'install or update "App Installer" from the Microsoft Store, or install the ' +
           'missing pieces by hand: Visual Studio 2022 Build Tools with the "Desktop ' +
           'development with C++" workload, and git.')
}

if (-not $Yes) {
    if ([Console]::IsInputRedirected) {
        throw 'Missing requirements and no terminal to ask on. Re-run with -Yes to install them.'
    }
    $answer = Read-Host 'Install the missing requirements with winget? The Build Tools are several GB. [Y/n]'
    if ($answer -and $answer -notmatch '^(y|yes)$') { throw 'Cancelled.' }
}

# The Visual Studio installer needs administrator rights. Ask once, here, rather
# than letting it fail halfway through with an error about the workload.
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Administrator rights are needed; accept the UAC prompt.' -ForegroundColor Yellow
    $shell = (Get-Process -Id $PID).Path
    $relaunch = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Yes')
    foreach ($m in $ForceMissing) { $relaunch += @('-ForceMissing', $m) }
    $proc = Start-Process -FilePath $shell -ArgumentList $relaunch -Verb RunAs -Wait -PassThru
    Update-SessionPath
    if ($proc.ExitCode -ne 0) { throw "The elevated installer exited with code $($proc.ExitCode)." }
    return
}

foreach ($c in $commands) {
    Write-Host "winget $($c -join ' ')" -ForegroundColor Cyan
    & winget @c
    # 0x8A15002B (already installed, nothing newer) is fine. Anything else is not.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
        throw "winget failed with exit code $LASTEXITCODE."
    }
}

# An existing Build Tools install without the C++ workload is reported by winget
# as already installed and left alone, so add the workload to it directly.
if ($missing.Contains('msvc') -and -not (Get-VsInstall)) {
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    $existing = & (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe') `
        -latest -products * -property installationPath
    if ($existing -and (Test-Path $installer)) {
        Write-Host "Adding the C++ workload to $existing" -ForegroundColor Cyan
        $p = Start-Process -FilePath $installer -Wait -PassThru `
            -ArgumentList "modify --installPath `"$existing`" $vsArgs"
        if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {
            throw "Visual Studio Installer exited with code $($p.ExitCode)."
        }
    }
}

Update-SessionPath

$stillMissing = @()
if (-not (Test-Git)) { $stillMissing += 'git' }
if (-not (Test-Ffmpeg)) { $stillMissing += 'ffmpeg' }
if (-not (Get-VsInstall)) { $stillMissing += 'Visual Studio Build Tools (C++ workload)' }

if ($stillMissing.Count -gt 0) {
    throw "Still missing after install: $($stillMissing -join ', '). A reboot may be needed; run this again afterwards."
}

Write-Host 'Requirements installed. Build with: pwsh scripts/build.ps1' -ForegroundColor Green
