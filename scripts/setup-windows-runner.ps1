<#
    Register this Windows machine as a Gitea Actions runner, so the `windows`
    job in .gitea/workflows/build.yml has something to run on.

    The Linux runner cannot take that job: it advertises ubuntu-latest and has
    no MSVC. A Windows job needs a Windows machine.

    Usage:
        pwsh scripts/setup-windows-runner.ps1 -Token <registration token>

    Get the token from Gitea: repository Settings, then Actions, then Runners,
    then "Create new runner". A repository token registers a runner for this
    repository alone; an organisation or site token covers more.

    Options:
        -Instance     Gitea URL (default https://git.interdo.me)
        -Name         Runner name shown in Gitea (default: this machine's name)
        -Labels       Runner labels (default windows-latest:host)
        -InstallDir   Where the runner lives (default C:\gitea-runner)
        -NoAutoStart  Register only; do not create the logon task or start it
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Token,

    [string]$Instance = 'https://git.interdo.me',
    [string]$Name = "$env:COMPUTERNAME-windows",
    [string]$Labels = 'windows-latest:host',
    [string]$InstallDir = 'C:\gitea-runner',
    [switch]$NoAutoStart
)

$ErrorActionPreference = 'Stop'
$runnerVersion = '3.4.2'
$taskName = 'Gitea Actions runner'

# ---------------------------------------------------------------------------
# The runner executes each job directly on this machine, so what the job needs
# has to already be here. Node is what runs actions/checkout, git is what it
# checks out with, and the build script finds MSVC by itself.
# ---------------------------------------------------------------------------
$missing = @()
foreach ($tool in 'node', 'git') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { $missing += $tool }
}

if ($missing.Count -gt 0) {
    throw "These have to be installed and on PATH first: $($missing -join ', ')."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere) -or
    -not (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
    throw 'Visual Studio Build Tools with the C++ workload is required to build OpenDJ.'
}

Write-Host "node $(node --version), git present, MSVC present." -ForegroundColor Green

# ---------------------------------------------------------------------------
# Fetch the runner
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$exePath = Join-Path $InstallDir 'act_runner.exe'

if (-not (Test-Path $exePath)) {
    $url = "https://gitea.com/gitea/runner/releases/download/v$runnerVersion/gitea-runner-$runnerVersion-windows-amd64.exe"
    Write-Host "Downloading runner $runnerVersion..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $url -OutFile $exePath -UseBasicParsing
} else {
    Write-Host "Runner already present at $exePath" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Register
#
# The :host suffix on the label is what makes jobs run directly on this machine
# instead of in a container. Without it the runner tries to start a Windows
# container per job, which is not what this is for.
# ---------------------------------------------------------------------------
Push-Location $InstallDir
try {
    if (Test-Path (Join-Path $InstallDir '.runner')) {
        Write-Host 'Already registered. Delete .runner to register again.' -ForegroundColor Yellow
    } else {
        Write-Host "Registering with $Instance as '$Name' [$Labels]..." -ForegroundColor Cyan
        & $exePath register --no-interactive `
            --instance $Instance `
            --token $Token `
            --name $Name `
            --labels $Labels
        if ($LASTEXITCODE -ne 0) { throw "Registration failed with exit code $LASTEXITCODE." }
    }

    if ($NoAutoStart) {
        Write-Host "Registered. Start it by hand with: $exePath daemon" -ForegroundColor Green
        return
    }

    # A logon task rather than a service: act_runner is a console program and
    # does not speak to the service control manager, so a real service would be
    # reported as failing to start even while it worked.
    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Write-Host "Scheduled task '$taskName' already exists." -ForegroundColor Yellow
    } else {
        $action = New-ScheduledTaskAction -Execute $exePath -Argument 'daemon' -WorkingDirectory $InstallDir
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME

        # Restart it if it dies. It is a console program, so anything that ends
        # the session it inherited takes it with it, and a job that was mid-flight
        # is left with no runner and eventually fails for reasons that have
        # nothing to do with the code.
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
            -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) `
            -RestartCount 999 -RestartInterval ([TimeSpan]::FromMinutes(1)) `
            -MultipleInstances IgnoreNew

        Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
            -Settings $settings -Description 'Runs Gitea Actions jobs for OpenDJ' | Out-Null

        Write-Host "Created scheduled task '$taskName', starting at logon." -ForegroundColor Green
    }

    Start-ScheduledTask -TaskName $taskName
    Start-Sleep -Seconds 3

    if (Get-Process act_runner -ErrorAction SilentlyContinue) {
        Write-Host 'Runner is running. Push to testing and the windows job should be picked up.' -ForegroundColor Green
    } else {
        Write-Warning "The runner did not stay running. Try it in the foreground to see why: $exePath daemon"
    }
} finally {
    Pop-Location
}
