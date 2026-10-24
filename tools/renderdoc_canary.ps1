# Run Xenia Canary on the user's GRAW disc image under RenderDoc, as the reference side of the
# recomp-vs-Canary frame comparison (#2, docs/CROSSCOM.md). Canary is NOT part of this project:
# it lives in the ignored private/xenia-canary/<build>/ with the user's config, and its outputs
# (captures, logs) stay private. Press F12 a few times in the Canary window: RenderDoc captures
# (Canary also saves its own screenshot; Windows 11 may swallow Print Screen).
#   pwsh -NoProfile -File tools/renderdoc_canary.ps1                    # 263e91b, depth clamp off
#   pwsh -NoProfile -File tools/renderdoc_canary.ps1 -Build 9e03494
# This opens a window on the user's screen. Never kill a running Xenia; ask the user to quit.
[CmdletBinding()]
param(
    [string]$Build = '263e91b',
    [string]$Config = 'graw-depth-clamp-off.toml',
    [string]$Iso = '',
    [string]$RenderDocPath = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "private/xenia-canary/$Build"
$exe = Join-Path $dir 'xenia_canary.exe'
if (!(Test-Path $exe)) { throw "Xenia Canary $Build not found at $dir" }
if (!(Test-Path (Join-Path $dir $Config))) { throw "Config $Config not found in $dir" }
if (!$Iso) {
    $Iso = Get-ChildItem 'D:\x360-recomps\GRAW 1\*.iso' -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (!$Iso -or !(Test-Path -LiteralPath $Iso)) { throw 'GRAW disc image not found; pass -Iso' }
$rdoc = if ($RenderDocPath) { $RenderDocPath } else {
    Get-ChildItem (Join-Path $root 'private/tools/renderdoc') -Recurse -Filter renderdoccmd.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName }
if (!$rdoc -or !(Test-Path $rdoc)) { throw 'renderdoccmd.exe not found in private/tools/renderdoc; pass -RenderDocPath' }
if (Get-Process xenia_canary -ErrorAction SilentlyContinue) { throw 'Xenia Canary is already running. Quit it first.' }
$captureDir = Join-Path $dir 'renderdoc'
New-Item -ItemType Directory -Force $captureDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$arguments = @('capture', '-d', $dir, '-c', (Join-Path $captureDir "canary-$Build-$stamp"), $exe,
    "`"$Iso`"", "--config=$Config", '--launch_module=Yeti_Final_Xenon.exe')
$process = Start-Process -FilePath $rdoc -WorkingDirectory $dir -ArgumentList $arguments -PassThru
"pid $($process.Id); RenderDoc captures go to $captureDir (press F12 a few times in the Canary window)"
