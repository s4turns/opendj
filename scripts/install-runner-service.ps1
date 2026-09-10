<#
    Turn an already-registered Gitea Actions runner into a Windows service, so
    Windows CI runs whether or not anyone is logged in.

    Run scripts/setup-windows-runner.ps1 first to register the runner. That
    leaves it starting from a task at logon, which means CI only works while
    someone is signed in and stops the moment they sign out.

    This needs Administrator. Run it from an elevated prompt, or let it ask:

        pwsh scripts/install-runner-service.ps1

    Options:
        -InstallDir    Where the runner lives (default C:\gitea-runner)
        -ServiceName   Service name (default GiteaRunner)
        -Uninstall     Remove the service and put the logon task back
#>
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\gitea-runner',
    [string]$ServiceName = 'GiteaRunner',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$nssmVersion = '2.24'
$taskName = 'Gitea Actions runner'

# ---------------------------------------------------------------------------
# Elevation
# ---------------------------------------------------------------------------
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)

if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Installing a service needs Administrator. Asking for elevation...' -ForegroundColor Yellow

    $arguments = @('-NoProfile', '-File', $PSCommandPath,
                   '-InstallDir', $InstallDir, '-ServiceName', $ServiceName)
    if ($Uninstall) { $arguments += '-Uninstall' }

    Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $arguments -Verb RunAs -Wait
    return
}

$exePath = Join-Path $InstallDir 'act_runner.exe'
$nssmPath = Join-Path $InstallDir 'nssm.exe'

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------
if ($Uninstall) {
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        Write-Host "Removing service '$ServiceName'..." -ForegroundColor Cyan
        & $nssmPath stop $ServiceName confirm 2>&1 | Out-Null
        & $nssmPath remove $ServiceName confirm
    } else {
        Write-Host "No service named '$ServiceName'." -ForegroundColor Yellow
    }

    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Enable-ScheduledTask -TaskName $taskName | Out-Null
        Start-ScheduledTask -TaskName $taskName
        Write-Host "Logon task '$taskName' re-enabled and started." -ForegroundColor Green
    }

    return
}

if (-not (Test-Path $exePath)) {
    throw "No runner at $exePath. Run scripts/setup-windows-runner.ps1 first."
}

if (-not (Test-Path (Join-Path $InstallDir '.runner'))) {
    throw "The runner in $InstallDir is not registered. Run scripts/setup-windows-runner.ps1 first."
}

# ---------------------------------------------------------------------------
# Only one runner may be live at a time
#
# The service and the logon task would both start the same registration, and
# two processes claiming the same runner id fight over jobs. The task goes.
# ---------------------------------------------------------------------------
if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Write-Host "Stopping and disabling the logon task '$taskName'..." -ForegroundColor Cyan
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Disable-ScheduledTask -TaskName $taskName | Out-Null
}

Get-Process act_runner -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "Stopping runner process $($_.Id)..." -ForegroundColor Cyan
    Stop-Process -Id $_.Id -Force
}

Start-Sleep -Seconds 2

# ---------------------------------------------------------------------------
# NSSM
#
# act_runner is a console program and does not speak to the service control
# manager, so it cannot be installed with sc.exe alone: Windows would keep
# reporting it as failing to start even while it worked. NSSM is a shim that
# runs it as a child and does the service protocol on its behalf.
# ---------------------------------------------------------------------------
if (-not (Test-Path $nssmPath)) {
    $zip = Join-Path $env:TEMP "nssm-$nssmVersion.zip"
    $extract = Join-Path $env:TEMP "nssm-$nssmVersion-extracted"

    Write-Host "Downloading NSSM $nssmVersion..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri "https://nssm.cc/release/nssm-$nssmVersion.zip" -OutFile $zip -UseBasicParsing

    if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
    Expand-Archive -Path $zip -DestinationPath $extract -Force

    $found = Get-ChildItem -Path $extract -Filter 'nssm.exe' -Recurse |
        Where-Object { $_.FullName -match 'win64' } | Select-Object -First 1

    if (-not $found) { throw 'nssm.exe (win64) was not found in the download.' }

    Copy-Item $found.FullName $nssmPath
    Remove-Item -Recurse -Force $extract, $zip -ErrorAction SilentlyContinue
} else {
    Write-Host "NSSM already present at $nssmPath" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Install and configure
# ---------------------------------------------------------------------------
if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    Write-Host "Service '$ServiceName' exists; reconfiguring it." -ForegroundColor Yellow
    & $nssmPath stop $ServiceName confirm 2>&1 | Out-Null
} else {
    Write-Host "Installing service '$ServiceName'..." -ForegroundColor Cyan
    & $nssmPath install $ServiceName $exePath 'daemon'
    if ($LASTEXITCODE -ne 0) { throw "nssm install failed with exit code $LASTEXITCODE." }
}

$logDir = Join-Path $InstallDir 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$settings = @(
    @('Application',        $exePath),
    @('AppParameters',      'daemon'),
    @('AppDirectory',       $InstallDir),
    @('DisplayName',        'Gitea Actions runner'),
    @('Description',        'Runs Gitea Actions jobs for OpenDJ'),
    @('Start',              'SERVICE_AUTO_START'),
    @('ObjectName',         'LocalSystem'),
    @('AppStdout',          (Join-Path $logDir 'runner.log')),
    @('AppStderr',          (Join-Path $logDir 'runner.err.log')),
    @('AppRotateFiles',     '1'),
    @('AppRotateBytes',     '10485760'),
    @('AppExit',            'Default', 'Restart'),
    @('AppRestartDelay',    '5000'),
    # A build can take ten minutes; do not kill it on a stop request too early.
    @('AppStopMethodConsole', '30000')
)

foreach ($s in $settings) {
    & $nssmPath set $ServiceName @s | Out-Null
}

Write-Host 'Starting the service...' -ForegroundColor Cyan
& $nssmPath start $ServiceName | Out-Null
Start-Sleep -Seconds 6

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
$process = Get-Process act_runner -ErrorAction SilentlyContinue

if ($service.Status -eq 'Running' -and $process) {
    Write-Host "Service '$ServiceName' is running as $($service.StartType) startup." -ForegroundColor Green
    Write-Host 'Windows CI now runs without anyone being logged in.' -ForegroundColor Green
    Write-Host "Logs: $logDir" -ForegroundColor Green
} else {
    Write-Warning "Service status is '$($service.Status)'. Check $logDir for why."
}
