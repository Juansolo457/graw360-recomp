# Screenshot the client area of a running window (read-only; the process is not touched).
#   pwsh -NoProfile -File tools/capture_window.ps1 [-ProcessName fear | -ProcessId <pid>] [-Out private/captures]
[CmdletBinding()]
param(
    [string]$ProcessName = 'fear',
    [int]$ProcessId = 0,
    [string]$Out = (Join-Path $PSScriptRoot '..\private\captures')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class CaptureNative {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
[CaptureNative]::SetProcessDPIAware() | Out-Null
$process = if ($ProcessId) { Get-Process -Id $ProcessId } else { Get-Process -Name $ProcessName }
$process = $process | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if (!$process) { throw "No window for process '$ProcessName'." }
$hwnd = $process.MainWindowHandle
if ([CaptureNative]::IsIconic($hwnd)) { throw 'The window is minimized.' }
$rect = New-Object CaptureNative+RECT
[CaptureNative]::GetClientRect($hwnd, [ref]$rect) | Out-Null
$origin = New-Object CaptureNative+POINT
[CaptureNative]::ClientToScreen($hwnd, [ref]$origin) | Out-Null
$bitmap = [System.Drawing.Bitmap]::new($rect.Right, $rect.Bottom)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
# PW_CLIENTONLY | PW_RENDERFULLCONTENT works even when other windows cover this one.
$dc = $graphics.GetHdc()
$printed = [CaptureNative]::PrintWindow($hwnd, $dc, 3)
$graphics.ReleaseHdc($dc)
if (!$printed) { $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size) }
$graphics.Dispose()
New-Item -ItemType Directory -Force $Out | Out-Null
$file = Join-Path (Resolve-Path $Out) ("$($process.ProcessName)-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.png')
$bitmap.Save($file)
$bitmap.Dispose()
"client $($rect.Right)x$($rect.Bottom) at $($origin.X),$($origin.Y) -> $file"
