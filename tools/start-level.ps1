param([double]$Scale = 0.0)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$replay = Join-Path $here "replay_clicks.py"
$clicks = Join-Path $here "title-clicks-640x480.jsonl"

$p = Get-Process xops096 -ErrorAction SilentlyContinue
if (-not $p) { throw "xops096 is not running - launch it and wait at the title" }

# XOPS can change its window handle during the transition; wait for a real one.
for ($i = 0; $i -lt 20 -and $p.MainWindowHandle -eq 0; $i++) {
    Start-Sleep -Seconds 1
    $p.Refresh()
}
if ($p.MainWindowHandle -eq 0) { throw "xops096 has no main window" }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ClientRect {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$r = New-Object ClientRect+RECT
[void][ClientRect]::GetClientRect($p.MainWindowHandle, [ref]$r)
if ($r.Right -le 0 -or $r.Bottom -le 0) { throw "could not read the client rect" }

# The recorded clicks are 640x480. If the software frame is stretched to the
# client (the Present dest override), scale X and Y independently.
if ($Scale -gt 0) {
    $sx = $Scale
    $sy = $Scale
} else {
    $sx = $r.Right / 640.0
    $sy = $r.Bottom / 480.0
}

$hwnd = "0x{0:X}" -f $p.MainWindowHandle.ToInt64()
Write-Host ("client={0}x{1}  click scale={2:0.####},{3:0.####}" -f $r.Right, $r.Bottom, $sx, $sy)
python $replay --hwnd $hwnd --file $clicks --scale-x $sx --scale-y $sy
