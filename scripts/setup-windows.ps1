<#
    Install what OpenDJ needs to build on Windows, so a fresh machine can go
    from nothing to a working build with one command.

    Usage:  pwsh scripts/setup-windows.ps1 [-Check] [-Yes] [-DryRun] [-SkipStems]

    What a build needs:
      * Visual Studio 2022 Build Tools with the C++ workload: the MSVC x64
        toolset and the Windows SDK. The "C++ CMake tools" component comes with
        it and brings the CMake and Ninja that build.ps1 prefers, so neither
        has to be installed separately.
      * git, because CMake clones JUCE at configure time.
    And to run RTMP broadcasts rather than build:
      * ffmpeg with libx264 (Gyan.FFmpeg), which the broadcaster launches.
    And for stem separation, which a build without them simply goes without:
      * Intel's OpenVINO runtime (winget), which runs the model. build.ps1 finds
        it and copies its DLLs next to OpenDJ.exe, so it needs no PATH entry.
      * The htdemucs model, about 105 MB from Intel/demucs-openvino on Hugging
        Face (MIT licence), into ~/.local/share/opendj/models.
    And, only on a machine with an NVIDIA card, for stems on its GPU:
      * ONNX Runtime with CUDA from Microsoft's GitHub releases, into
        ~/.local/share/opendj/onnxruntime-gpu. build.ps1 builds against it.
      * NVIDIA's CUDA 13 runtime, cuBLAS, cuFFT, NVRTC and cuDNN 9, unpacked
        from NVIDIA's redistributable archives into ~/.local/share/opendj/cuda.
        About 2.2 GB to download; OpenDJ finds the DLLs there itself, so no
        CUDA toolkit install and no PATH entry.
      * The same htdemucs as ONNX, 174 MB from this project's own releases
        (exported with scripts/export-htdemucs-onnx.py).
    Everything else is fetched by CMake during the build.

    Not installed here: Steinberg's ASIO SDK. Its licence forbids
    redistribution, so it stays opt-in; see build.ps1 -Asio.

    Options:
        -Check    Report what is present and what is missing; install nothing.
        -Yes      Do not ask before installing. The Build Tools are several GB.
        -DryRun   Print the install commands instead of running them.
        -SkipStems  Leave out every stem piece: none is checked nor installed.
        -ForceMissing  Treat these as missing ('git', 'msvc', 'ffmpeg', 'openvino',
                       'stemmodel', 'onnxruntime', 'cuda', 'stemmodelonnx');
                       for testing.
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Yes,
    [switch]$DryRun,
    [switch]$SkipStems,
    [string[]]$ForceMissing = @()
)

$ErrorActionPreference = 'Stop'

# `pwsh -File` hands a comma list over as one string, so it is split here, and
# checked here for the same reason: a ValidateSet would reject the whole list.
$ForceMissing = @($ForceMissing | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($name in $ForceMissing) {
    if ($name -notin 'git', 'msvc', 'ffmpeg', 'openvino', 'stemmodel', 'onnxruntime', 'cuda', 'stemmodelonnx') {
        throw ("Unknown -ForceMissing value '$name'. Use git, msvc, ffmpeg, openvino, stemmodel, " +
               'onnxruntime, cuda or stemmodelonnx.')
    }
}

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

# The OpenVINO package from winget is an MSIX, so it is found by asking Windows
# for the package rather than by looking on PATH.
function Test-OpenVino {
    [bool](Get-AppxPackage -Name 'Intel.OpenVINOToolkit*' -ErrorAction SilentlyContinue)
}

# The folder and the file names are the ones StemModel looks for, and the sizes
# are the published ones, so a download that stopped halfway is not mistaken
# for a model.
$modelFolder = Join-Path $HOME '.local\share\opendj\models'
$modelBase = 'https://huggingface.co/Intel/demucs-openvino/resolve/main/htdemucs_v4'
$modelFiles = @(
    @{ Name = 'htdemucs_v4.xml'; Remote = 'htdemucs_fwd.xml'; Bytes = 1477599 },
    @{ Name = 'htdemucs_v4.bin'; Remote = 'htdemucs_fwd.bin'; Bytes = 104552746 }
)

function Test-StemModel {
    foreach ($f in $modelFiles) {
        $path = Join-Path $modelFolder $f.Name
        if (-not (Test-Path $path) -or (Get-Item $path).Length -ne $f.Bytes) { return $false }
    }
    $true
}

function Install-StemModel {
    New-Item -ItemType Directory -Force $modelFolder | Out-Null

    # The progress bar makes a download of this size several times slower.
    $previous = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'

    try {
        foreach ($f in $modelFiles) {
            $path = Join-Path $modelFolder $f.Name
            Write-Host "Downloading $($f.Name) ($([math]::Round($f.Bytes / 1e6, 1)) MB)" -ForegroundColor Cyan
            Invoke-WebRequest -Uri "$modelBase/$($f.Remote)" -OutFile "$path.part" -UseBasicParsing

            if ((Get-Item "$path.part").Length -ne $f.Bytes) {
                Remove-Item "$path.part"
                throw "$($f.Name) came down the wrong size. Run this again."
            }

            Move-Item -Force "$path.part" $path
        }
    } finally {
        $ProgressPreference = $previous
    }
}

# Stems on an NVIDIA card. OpenVINO only drives Intel graphics, so these are
# what put the separation on the GPU there; on any other machine they would be
# gigabytes for nothing, and are neither checked nor offered.
function Test-NvidiaGpu {
    [bool](Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'NVIDIA*' })
}

$onnxRuntimeFolder = Join-Path $HOME '.local\share\opendj\onnxruntime-gpu'
$onnxRuntime = @{
    Url = 'https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/onnxruntime-win-x64-gpu_cuda13-1.31.0.zip'
    Sha256 = 'd332ccf84acb769a11364cddd4e8d2992a168d16be472d5ba70b279b644552f5'
    Bytes = 309484217
}

# The archive names are written beside the DLLs, so a folder from an older
# setup is replaced rather than mixed with the new one.
$cudaFolder = Join-Path $HOME '.local\share\opendj\cuda'
$cudaRedist = 'https://developer.download.nvidia.com/compute'
$cudaArchives = @(
    @{ Path = 'cuda/redist/cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-13.4.92-archive.zip'
       Sha256 = '621a70c4778287b1abf8c27c61b841d797fda2aeff01033374dd19624bcfa249'; Bytes = 2737397 },
    @{ Path = 'cuda/redist/libcublas/windows-x86_64/libcublas-windows-x86_64-13.8.0.4-archive.zip'
       Sha256 = '0974318e9861a61cb9091cf7de8d4c2880e9787fe311cb4bcbd08865663c5f43'; Bytes = 422415931 },
    @{ Path = 'cuda/redist/libcufft/windows-x86_64/libcufft-windows-x86_64-12.4.0.43-archive.zip'
       Sha256 = '69d0ad8dc3a1be66f01a748a8206d0ceaafa56939474663fbe790dc3e91d2009'; Bytes = 159602693 },
    @{ Path = 'cuda/redist/cuda_nvrtc/windows-x86_64/cuda_nvrtc-windows-x86_64-13.4.92-archive.zip'
       Sha256 = '92e4db6e79550665e1dbdc6cc09c934452a667881f5ed49a51681429874a8f0d'; Bytes = 319966721 },
    @{ Path = 'cudnn/redist/cudnn/windows-x86_64/cudnn-windows-x86_64-9.27.0.42_cuda13-archive.zip'
       Sha256 = '15595c7b52896262a0e590f02b3b3851c6a2c9046a72047526deaf59a9cbf7d5'; Bytes = 1330803635 }
)
$cudaManifest = ($cudaArchives | ForEach-Object { Split-Path $_.Path -Leaf }) -join "`n"

$onnxModel = @{
    Name = 'htdemucs_v4.onnx'
    Url = 'https://git.interdo.me/interdome/opendj/releases/download/stem-model-onnx-1/htdemucs_v4.onnx'
    Bytes = 174261060
}

function Test-OnnxRuntime {
    (Test-Path (Join-Path $onnxRuntimeFolder 'lib\onnxruntime_providers_cuda.dll')) -and
        ((Get-Content (Join-Path $onnxRuntimeFolder 'opendj-source.txt') -ErrorAction SilentlyContinue) -eq $onnxRuntime.Url)
}

function Test-Cuda {
    $written = Get-Content -Raw (Join-Path $cudaFolder 'opendj-archives.txt') -ErrorAction SilentlyContinue
    [bool]$written -and $written.Trim() -eq $cudaManifest
}

function Test-StemModelOnnx {
    $path = Join-Path $modelFolder $onnxModel.Name
    (Test-Path $path) -and (Get-Item $path).Length -eq $onnxModel.Bytes
}

# Checks the size, and the hash where one is published, before anything is
# unpacked from a download.
function Get-Download($url, $bytes, $sha256, $target) {
    Write-Host "Downloading $(Split-Path $url -Leaf) ($([math]::Round($bytes / 1e6, 1)) MB)" -ForegroundColor Cyan
    $previous = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try { Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing }
    finally { $ProgressPreference = $previous }

    if ((Get-Item $target).Length -ne $bytes) {
        Remove-Item $target
        throw "$(Split-Path $url -Leaf) came down the wrong size. Run this again."
    }
    if ($sha256 -and (Get-FileHash -Algorithm SHA256 $target).Hash -ne $sha256) {
        Remove-Item $target
        throw "$(Split-Path $url -Leaf) does not match its published checksum."
    }
}

function New-ScratchFolder {
    $path = Join-Path ([IO.Path]::GetTempPath()) "opendj-setup-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory $path | Out-Null
    $path
}

function Install-OnnxRuntime {
    $scratch = New-ScratchFolder
    try {
        $zip = Join-Path $scratch 'onnxruntime.zip'
        Get-Download $onnxRuntime.Url $onnxRuntime.Bytes $onnxRuntime.Sha256 $zip
        Expand-Archive $zip (Join-Path $scratch 'x')
        # The archive holds one folder named after the release. Its contents
        # are what the build wants, so the version stays out of the path.
        $root = Get-ChildItem (Join-Path $scratch 'x') -Directory | Select-Object -First 1
        if (Test-Path $onnxRuntimeFolder) { Remove-Item -Recurse -Force $onnxRuntimeFolder }
        New-Item -ItemType Directory -Force (Split-Path $onnxRuntimeFolder) | Out-Null
        Move-Item $root.FullName $onnxRuntimeFolder
        [IO.File]::WriteAllText((Join-Path $onnxRuntimeFolder 'opendj-source.txt'), $onnxRuntime.Url)
    } finally {
        Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
    }
}

# Only the DLLs are kept: the headers and import libraries are for building
# against CUDA, which nothing here does.
function Install-Cuda {
    if (Test-Path $cudaFolder) { Remove-Item -Recurse -Force $cudaFolder }
    $bin = Join-Path $cudaFolder 'bin'
    New-Item -ItemType Directory -Force $bin | Out-Null

    foreach ($a in $cudaArchives) {
        $scratch = New-ScratchFolder
        try {
            $zip = Join-Path $scratch 'archive.zip'
            Get-Download "$cudaRedist/$($a.Path)" $a.Bytes $a.Sha256 $zip
            Expand-Archive $zip (Join-Path $scratch 'x')
            Get-ChildItem (Join-Path $scratch 'x') -Recurse -Filter *.dll | Copy-Item -Destination $bin
        } finally {
            Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
        }
    }

    [IO.File]::WriteAllText((Join-Path $cudaFolder 'opendj-archives.txt'), $cudaManifest)
}

function Install-StemModelOnnx {
    New-Item -ItemType Directory -Force $modelFolder | Out-Null
    $path = Join-Path $modelFolder $onnxModel.Name
    Get-Download $onnxModel.Url $onnxModel.Bytes $null "$path.part"
    Move-Item -Force "$path.part" $path
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

$items = @(@('git', 'git'), @('msvc', 'Visual Studio 2022 Build Tools (C++ workload)'), @('ffmpeg', 'ffmpeg (RTMP broadcast)'))

if (-not $SkipStems) {
    if ($ForceMissing -contains 'openvino' -or -not (Test-OpenVino)) { $missing['openvino'] = 'OpenVINO' }
    if ($ForceMissing -contains 'stemmodel' -or -not (Test-StemModel)) { $missing['stemmodel'] = 'stem model' }
    $items += , @('openvino', 'OpenVINO runtime (stem separation)')
    $items += , @('stemmodel', 'htdemucs model (stem separation)')

    if (Test-NvidiaGpu) {
        if ($ForceMissing -contains 'onnxruntime' -or -not (Test-OnnxRuntime)) { $missing['onnxruntime'] = 'ONNX Runtime' }
        if ($ForceMissing -contains 'cuda' -or -not (Test-Cuda)) { $missing['cuda'] = 'CUDA libraries' }
        if ($ForceMissing -contains 'stemmodelonnx' -or -not (Test-StemModelOnnx)) { $missing['stemmodelonnx'] = 'ONNX stem model' }
        $items += , @('onnxruntime', 'ONNX Runtime with CUDA (stems on the NVIDIA GPU)')
        $items += , @('cuda', 'CUDA 13 and cuDNN 9 libraries (stems on the NVIDIA GPU)')
        $items += , @('stemmodelonnx', 'htdemucs ONNX model (stems on the NVIDIA GPU)')
    }
}

Write-Host 'Requirements:' -ForegroundColor Cyan
foreach ($item in $items) {
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
if ($missing.Contains('openvino')) {
    $commands += , (@('install', '--id', 'Intel.OpenVINOToolkit.2026.3.0') + $common)
}
if ($missing.Contains('msvc')) {
    $commands += , (@('install', '--id', 'Microsoft.VisualStudio.2022.BuildTools') + $common +
                    @('--override', $vsArgs))
}

if ($DryRun) {
    foreach ($c in $commands) { Write-Host "winget $($c -join ' ')" }
    if ($missing.Contains('stemmodel')) {
        foreach ($f in $modelFiles) { Write-Host "download $modelBase/$($f.Remote) -> $(Join-Path $modelFolder $f.Name)" }
    }
    if ($missing.Contains('onnxruntime')) { Write-Host "download $($onnxRuntime.Url) -> $onnxRuntimeFolder" }
    if ($missing.Contains('cuda')) {
        foreach ($a in $cudaArchives) { Write-Host "download $cudaRedist/$($a.Path) -> $cudaFolder\bin" }
    }
    if ($missing.Contains('stemmodelonnx')) { Write-Host "download $($onnxModel.Url) -> $(Join-Path $modelFolder $onnxModel.Name)" }
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
    $sizes = 'The Build Tools are several GB, the stem model 105 MB'
    if ($missing.Contains('cuda')) { $sizes += ', the CUDA libraries 2.2 GB' }
    $answer = Read-Host "Install the missing requirements? $sizes. [Y/n]"
    if ($answer -and $answer -notmatch '^(y|yes)$') { throw 'Cancelled.' }
}

# The Visual Studio installer needs administrator rights. Ask once, here, rather
# than letting it fail halfway through with an error about the workload.
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
$needsAdmin = $missing.Contains('git') -or $missing.Contains('msvc') -or $missing.Contains('ffmpeg')
if ($needsAdmin -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Administrator rights are needed; accept the UAC prompt.' -ForegroundColor Yellow
    $shell = (Get-Process -Id $PID).Path
    $relaunch = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Yes')
    # One parameter with a comma list: a parameter given twice is an error.
    if ($ForceMissing.Count -gt 0) { $relaunch += @('-ForceMissing', ($ForceMissing -join ',')) }
    if ($SkipStems) { $relaunch += '-SkipStems' }
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

if ($missing.Contains('stemmodel')) { Install-StemModel }
if ($missing.Contains('onnxruntime')) { Install-OnnxRuntime }
if ($missing.Contains('cuda')) { Install-Cuda }
if ($missing.Contains('stemmodelonnx')) { Install-StemModelOnnx }

$stillMissing = @()
if (-not (Test-Git)) { $stillMissing += 'git' }
if (-not (Test-Ffmpeg)) { $stillMissing += 'ffmpeg' }
if (-not $SkipStems) {
    if (-not (Test-OpenVino)) { $stillMissing += 'OpenVINO' }
    if (-not (Test-StemModel)) { $stillMissing += 'the stem model' }
    if (Test-NvidiaGpu) {
        if (-not (Test-OnnxRuntime)) { $stillMissing += 'ONNX Runtime' }
        if (-not (Test-Cuda)) { $stillMissing += 'the CUDA libraries' }
        if (-not (Test-StemModelOnnx)) { $stillMissing += 'the ONNX stem model' }
    }
}
if (-not (Get-VsInstall)) { $stillMissing += 'Visual Studio Build Tools (C++ workload)' }

if ($stillMissing.Count -gt 0) {
    throw "Still missing after install: $($stillMissing -join ', '). A reboot may be needed; run this again afterwards."
}

Write-Host 'Requirements installed. Build with: pwsh scripts/build.ps1' -ForegroundColor Green
