<#
    Capture the OpenDJ window into images/screenshot.jpg, the picture the README
    and the wiki show, so it can be refreshed alongside every commit.

    Usage:  pwsh scripts/screenshot.ps1 [-Config RelWithDebInfo] [-Output images/screenshot.jpg]
                                        [-Width 1280] [-Height 960] [-Quality 90]

    Build first: this launches whatever is in build/, it does not compile. The
    window is sized to the default 1280x960 so every capture is comparable, and
    it is rendered with PrintWindow, so another window lying on top of OpenDJ
    does not end up in the picture. Settings are put back afterwards, because
    the resize would otherwise be remembered as your own window size.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [string]$Output = '',
    [int]$Width = 1280,
    [int]$Height = 960,
    [ValidateRange(1, 100)]
    [int]$Quality = 90,
    [int]$SettleSeconds = 3
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repoRoot "build\OpenDJ_artefacts\$Config\OpenDJ.exe"

if (-not $Output) { $Output = Join-Path $repoRoot 'images\screenshot.jpg' }
if (-not (Test-Path $exe)) { throw "No build at $exe. Run scripts/build.ps1 first." }
if (Get-Process OpenDJ -ErrorAction SilentlyContinue) { throw 'OpenDJ is already running. Close it first.' }

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class OpenDjCapture
{
    public struct Rect { public int Left, Top, Right, Bottom; }
    public struct Point { public int X, Y; }

    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
}
'@

# Per-monitor aware, so the window and client sizes come back in real pixels.
[OpenDjCapture]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null

$settings = Join-Path $env:APPDATA 'OpenDJ\settings.json'
$settingsBackup = if (Test-Path $settings) { Get-Content -Raw -LiteralPath $settings } else { $null }
$process = $null

try {
    $process = Start-Process $exe -PassThru
    $deadline = (Get-Date).AddSeconds(20)

    do { Start-Sleep -Milliseconds 300; $process.Refresh() }
    while ($process.MainWindowHandle -eq 0 -and -not $process.HasExited -and (Get-Date) -lt $deadline)

    if ($process.MainWindowHandle -eq 0) { throw 'OpenDJ did not open a window within 20 seconds.' }
    $window = $process.MainWindowHandle

    # Size the client area to the requested logical size at this monitor's scale.
    $scale = [OpenDjCapture]::GetDpiForWindow($window) / 96.0
    $outer = New-Object OpenDjCapture+Rect
    $inner = New-Object OpenDjCapture+Rect
    [OpenDjCapture]::GetWindowRect($window, [ref]$outer) | Out-Null
    [OpenDjCapture]::GetClientRect($window, [ref]$inner) | Out-Null

    $frameWidth = ($outer.Right - $outer.Left) - $inner.Right
    $frameHeight = ($outer.Bottom - $outer.Top) - $inner.Bottom
    $SWP_NOMOVE_NOZORDER = 0x0002 -bor 0x0004

    [OpenDjCapture]::SetWindowPos($window, [IntPtr]::Zero, 0, 0,
        [int][math]::Round($Width * $scale) + $frameWidth,
        [int][math]::Round($Height * $scale) + $frameHeight, $SWP_NOMOVE_NOZORDER) | Out-Null

    Start-Sleep -Seconds $SettleSeconds

    [OpenDjCapture]::GetWindowRect($window, [ref]$outer) | Out-Null
    [OpenDjCapture]::GetClientRect($window, [ref]$inner) | Out-Null
    $origin = New-Object OpenDjCapture+Point
    [OpenDjCapture]::ClientToScreen($window, [ref]$origin) | Out-Null

    $whole = New-Object System.Drawing.Bitmap ($outer.Right - $outer.Left), ($outer.Bottom - $outer.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($whole)
    $dc = $graphics.GetHdc()
    $rendered = [OpenDjCapture]::PrintWindow($window, $dc, 2)   # PW_RENDERFULLCONTENT
    $graphics.ReleaseHdc($dc)
    $graphics.Dispose()

    if (-not $rendered) { throw 'PrintWindow could not render the OpenDJ window.' }

    # Keep the client area only: no title bar, no border.
    $client = New-Object System.Drawing.Rectangle ($origin.X - $outer.Left), ($origin.Y - $outer.Top), $inner.Right, $inner.Bottom
    $picture = $whole.Clone($client, $whole.PixelFormat)
    $whole.Dispose()

    $jpeg = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
    $encoderParams = New-Object System.Drawing.Imaging.EncoderParameters 1
    $encoderParams.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality, [long]$Quality)

    New-Item -ItemType Directory -Force (Split-Path -Parent $Output) | Out-Null
    $picture.Save($Output, $jpeg, $encoderParams)

    Write-Host "Saved $($picture.Width)x$($picture.Height) to $Output ($([math]::Round((Get-Item $Output).Length / 1KB)) KB)"
    $picture.Dispose()
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process $process -Force; $process.WaitForExit(5000) | Out-Null }

    if ($null -ne $settingsBackup) { Set-Content -LiteralPath $settings -Value $settingsBackup -NoNewline }
}
