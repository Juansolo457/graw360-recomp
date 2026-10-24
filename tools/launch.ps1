# Launch the GRAW recomp (port/) for playing: fullscreen at the display's native size, the
# guest told it is on a 1280x720 display (as on a real 360; also avoids ReXGlue's 4:3 squeeze at
# 200% Windows scaling), rendering at a multiple of 720p with 16x anisotropic filtering.
#   pwsh -NoProfile -File tools/launch.ps1                 # 3x = 3840x2160 internal, fullscreen
#   pwsh -NoProfile -File tools/launch.ps1 -Scale 1 -Windowed -Extra '--log_level debug'
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 1 -Trace   # every kernel call (file opens, ...) in the log
# This opens a window on the user's screen. Never kill a running graw.exe; ask the user to quit.
[CmdletBinding()]
param(
    [ValidateRange(1,3)][int]$Scale = 3,
    [ValidateSet(0,1,2,3,4,5)][int]$Anisotropic = 5,   # SDK enum: 5 = 16x
    [switch]$Windowed,
    [switch]$Trace,       # --log_level trace --log_noisy: every kernel call with its arguments (large log)
    [string]$Extra = '',
    # port = the front end (graw.exe, what a player starts); port-sp = the campaign (grawsp.exe) alone
    [ValidateSet('port', 'port-sp')][string]$Project = 'port',
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$port = Join-Path $PSScriptRoot "..\$Project" | Resolve-Path
$exeName = if ($Project -eq 'port-sp') { 'grawsp' } else { 'graw' }
$running = Get-Process -Name graw, grawsp -ErrorAction SilentlyContinue
if ($running) { throw "GRAW is already running (pid $($running.Id -join ', ')). Quit it first." }
$executable = Join-Path $port "out\build\win-amd64-$($Configuration.ToLowerInvariant())\$exeName.exe"
New-Item -ItemType Directory -Force (Join-Path $port 'logs') | Out-Null
$log = Join-Path $port ('logs\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$fullscreen = if ($Windowed) { '--no-fullscreen' } else { '--fullscreen' }
$arguments = @($fullscreen,
    '--resolution', '720p', '--resolution_scale', "$Scale", '--anisotropic_override', "$Anisotropic",
    '--game_data_root', "$port\assets", '--user_data_root', "$port\user",
    '--cache_root', "$port\cache", '--log_file', $log, '--log_flush_interval', '1') +
    $(if ($Trace) { @('--log_level', 'trace', '--log_noisy') } else { @('--log_level', 'debug') }) + @($Extra -split '\s+' | Where-Object { $_ })
$process = Start-Process -FilePath $executable -WorkingDirectory $port -ArgumentList $arguments -PassThru
"pid $($process.Id), log $log"
