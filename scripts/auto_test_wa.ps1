# Auto-launch WA (OpenGL CPU) and collect wkJellyWorm debug evidence.
param(
  [int]$Seconds = 18,
  [string]$WaDir = "C:\Program Files (x86)\by Decepticon\Worms Armageddon",
  [string]$LogPath = "C:\Users\Admin\Projects\worms-modding-lab\debug-51d698.log",
  [string]$OutDir = "C:\Users\Admin\Projects\worms-modding-lab\sources\wkJellyWorm\scripts\last_run"
)

$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Write-Result($obj) {
  $json = $obj | ConvertTo-Json -Depth 6
  Set-Content -Path (Join-Path $OutDir "result.json") -Value $json -Encoding UTF8
  Write-Host $json
}

# Ensure OpenGL CPU + skip intro
$reg = "HKCU:\Software\Team17SoftwareLtd\WormsArmageddon\Options"
if (Test-Path $reg) {
  Set-ItemProperty -Path $reg -Name "Renderer" -Value 5 -Type DWord
  Set-ItemProperty -Path $reg -Name "SkipIntro" -Value 1 -Type DWord
  Set-ItemProperty -Path $reg -Name "LoadWormKitModules" -Value 1 -Type DWord
}

Get-Process WA -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# Do not delete log here if caller already cleared it; truncate only if requested via env
if ($env:CLEAR_DEBUG_LOG -eq "1" -and (Test-Path $LogPath)) {
  Remove-Item $LogPath -Force -ErrorAction SilentlyContinue
}

$errorLog = Join-Path $WaDir "ERRORLOG.TXT"
$crashBefore = if (Test-Path $errorLog) { (Get-Item $errorLog).LastWriteTimeUtc } else { [datetime]::MinValue }
$errLenBefore = if (Test-Path $errorLog) { (Get-Item $errorLog).Length } else { 0 }

$exe = Join-Path $WaDir "WA.exe"
$proc = Start-Process -FilePath $exe -WorkingDirectory $WaDir -PassThru
Write-Host "STARTED pid=$($proc.Id)"

$deadline = (Get-Date).AddSeconds($Seconds)
$exitedEarly = $false
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 500
  if ($proc.HasExited) {
    $exitedEarly = $true
    Write-Host "WA exited early code=$($proc.ExitCode)"
    break
  }
}

# Bring WA foreground and capture its window (fallback: primary screen)
$shotPath = Join-Path $OutDir "screenshot.png"
try {
  Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
}
"@
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type -AssemblyName System.Drawing
  Start-Sleep -Milliseconds 400
  if (-not $proc.HasExited) {
    [Win]::ShowWindow($proc.MainWindowHandle, 9) | Out-Null
    [Win]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 300
  }
  $rect = New-Object Win+RECT
  $useWindow = (-not $proc.HasExited) -and $proc.MainWindowHandle -ne [IntPtr]::Zero -and [Win]::GetWindowRect($proc.MainWindowHandle, [ref]$rect)
  if ($useWindow -and ($rect.Right - $rect.Left) -gt 64 -and ($rect.Bottom - $rect.Top) -gt 64) {
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $w,$h))
    $bmp.Save($shotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "SHOT_WINDOW $w x $h"
  } else {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bmp.Save($shotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "SHOT_DESKTOP fallback"
  }
} catch {
  Write-Host "SHOT_FAIL $($_.Exception.Message)"
}

# Sample screenshot luminance (very dark => likely black screen)
$avgLuma = -1
$darkPct = -1
if (Test-Path $shotPath) {
  try {
    Add-Type -AssemblyName System.Drawing
    $img = [System.Drawing.Bitmap]::FromFile($shotPath)
    $stepX = [Math]::Max(1, [int]($img.Width / 80))
    $stepY = [Math]::Max(1, [int]($img.Height / 60))
    $sum = 0.0; $n = 0; $dark = 0
    for ($y = 0; $y -lt $img.Height; $y += $stepY) {
      for ($x = 0; $x -lt $img.Width; $x += $stepX) {
        $c = $img.GetPixel($x, $y)
        $l = 0.2126 * $c.R + 0.7152 * $c.G + 0.0722 * $c.B
        $sum += $l; $n++
        if ($l -lt 12) { $dark++ }
      }
    }
    $img.Dispose()
    if ($n -gt 0) {
      $avgLuma = [Math]::Round($sum / $n, 2)
      $darkPct = [Math]::Round(100.0 * $dark / $n, 1)
    }
  } catch {
    Write-Host "LUMA_FAIL $($_.Exception.Message)"
  }
}

Get-Process WA -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

$crashed = $false
$avSnippet = $null
if (Test-Path $errorLog) {
  $head = Get-Content $errorLog -TotalCount 20 -ErrorAction SilentlyContinue
  $joined = ($head -join "`n")
  if ($joined -match "Access Violation" -and $joined -match "Error occurred at") {
    # Only count if ERRORLOG was rewritten during this run
    $info = Get-Item $errorLog
    if ($info.LastWriteTimeUtc -gt $crashBefore) {
      $crashed = $true
      $avSnippet = ($head | Select-String -Pattern "Access Violation|0xc0000005|Write to location|EIP:|Error occurred" | ForEach-Object { $_.Line }) -join " | "
    }
  }
}

$messages = @()
$hypCounts = @{}
if (Test-Path $LogPath) {
  Get-Content $LogPath -ErrorAction SilentlyContinue | ForEach-Object {
    try {
      $o = $_ | ConvertFrom-Json
      $messages += $o.message
      $hid = [string]$o.hypothesisId
      if (-not $hypCounts.ContainsKey($hid)) { $hypCounts[$hid] = 0 }
      $hypCounts[$hid]++
    } catch {}
  }
}

function Count-Msg([string]$name) {
  @($messages | Where-Object { $_ -eq $name }).Count
}

$skipNative = Count-Msg "skip_native_mark_ready"
$passthrough = Count-Msg "native_passthrough"
$deferGlew = Count-Msg "defer_glew_until_skip_native"
$presents = Count-Msg "present"
$result = [ordered]@{
  ok                = (-not $crashed) -and (-not $exitedEarly) -and ($avgLuma -gt 20) -and ((Count-Msg "after_orig") -gt 0) -and ($passthrough -gt 0) -and ($skipNative -eq 0) -and ($presents -gt 0)
  crashed           = $crashed
  crashHint         = $avSnippet
  waExitedEarly     = [bool]$exitedEarly
  logExists         = [bool](Test-Path $LogPath)
  logBytes          = if (Test-Path $LogPath) { (Get-Item $LogPath).Length } else { 0 }
  avgLuma           = $avgLuma
  darkPct           = $darkPct
  likelyBlackScreen = ($avgLuma -ge 0 -and $avgLuma -lt 18 -and $darkPct -gt 85)
  counts = [ordered]@{
    before_orig             = Count-Msg "before_orig"
    after_orig              = Count-Msg "after_orig"
    glew_ok                 = Count-Msg "glew_ok"
    defer_glew              = $deferGlew
    shaders_and_palette_ready = Count-Msg "shaders_and_palette_ready"
    skip_native_mark_ready  = $skipNative
    native_passthrough      = $passthrough
    present                 = $presents
    gl_draw                 = Count-Msg "gl_draw"
    cleared                 = Count-Msg "cleared"
    pure_native             = Count-Msg "pure_native"
    disabled                = Count-Msg "disabled"
  }
  hypothesisHits = $hypCounts
  screenshot     = $shotPath
  seconds        = $Seconds
}

Write-Result $result
if ($result.crashed) { exit 2 }
if ($result.likelyBlackScreen) { exit 3 }
if (-not $result.logExists) { exit 4 }
exit 0
