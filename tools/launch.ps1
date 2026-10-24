# Launch the GRAW recomp (port/, the front end; it hands over to the campaign and Instant Action)
# for playing. The guest is told it is on a 1280x720 display (as on a real 360; this also avoids
# ReXGlue's 4:3 squeeze at 200% Windows scaling) and renders at a multiple of that.
# Image quality defaults (#12): 2x = 2560x1440 internal, 16x anisotropic filtering, FXAA at its
# highest setting on the finished frame. The game's own MSAA stays as the game sets it.
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed                 # 1440p internal, windowed
#   pwsh -NoProfile -File tools/launch.ps1                           # fullscreen
#   pwsh -NoProfile -File tools/launch.ps1 -Scale 3 -Aa fxaa          # 4K internal, lighter FXAA
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 1 -Aa none   # as the 360 draws it
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -Trace          # every kernel call (file opens, ...) in the log
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -PerfLog        # per-frame CSV (frame time, fps, draws) next to the log (#11)
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -RefreshRate 240 -PerfLog   # the guest's display at 240 Hz (#11)
# This opens a window on the user's screen. Never kill a running graw.exe; ask the user to quit.
[CmdletBinding()]
param(
    [ValidateRange(1,3)][int]$Scale = 2,                 # internal resolution: 1 = 720p, 2 = 1440p, 3 = 4K
    [ValidateSet(-1,0,1,2,3,4,5)][int]$Anisotropic = 5,  # SDK enum: 5 = 16x, -1 = the game's own
    [ValidateSet('none', 'fxaa', 'fxaa_extreme')][string]$Aa = 'fxaa_extreme',
    [switch]$Windowed,
    [switch]$Trace,       # --log_level trace --log_noisy: every kernel call with its arguments (large log)
    [switch]$PerfLog,     # --perf_log_csv: one CSV row per frame (frame_time_us, fps, draw calls, ...), logs\perf-<time>.csv
    # The refresh rate the guest's display reports (24-240 Hz). A 360 game that waits for vblanks
    # to pace itself runs faster when it's higher: F.E.A.R. went from 30 to 120 fps this way.
    [ValidateRange(0,240)][double]$RefreshRate = 0,
    [string]$Extra = '',
    # port = the front end (graw.exe, what a player starts); port-mp = Instant Action/multiplayer (grawmp.exe), port-yeti = the campaign (grawyeti.exe), alone
    [ValidateSet('port', 'port-mp', 'port-yeti')][string]$Project = 'port',
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$port = Join-Path $PSScriptRoot "..\$Project" | Resolve-Path
$exeName = @{ 'port' = 'graw'; 'port-mp' = 'grawmp'; 'port-yeti' = 'grawyeti' }[$Project]
$running = Get-Process -Name graw, grawmp, grawyeti -ErrorAction SilentlyContinue
if ($running) { throw "GRAW is already running (pid $($running.Id -join ', ')). Quit it first." }
$executable = Join-Path $port "out\build\win-amd64-$($Configuration.ToLowerInvariant())\$exeName.exe"
New-Item -ItemType Directory -Force (Join-Path $port 'logs') | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $port "logs\run-$stamp.log"
$perfCsv = Join-Path $port "logs\perf-$stamp.csv"
$fullscreen = if ($Windowed) { '--no-fullscreen' } else { '--fullscreen' }
$arguments = @($fullscreen,
    '--resolution', '720p', '--resolution_scale', "$Scale", '--anisotropic_override', "$Anisotropic",
    '--swap_post_effect', $Aa,
    '--game_data_root', "$port\assets", '--user_data_root', "$port\user",
    '--cache_root', "$port\cache", '--log_file', $log, '--log_flush_interval', '1') +
    $(if ($PerfLog) { @('--perf_log_csv', $perfCsv) } else { @() }) +
    $(if ($RefreshRate -gt 0) { @('--video_mode_refresh_rate', "$RefreshRate") } else { @() }) +
    $(if ($Trace) { @('--log_level', 'trace', '--log_noisy') } else { @('--log_level', 'debug') }) + @($Extra -split '\s+' | Where-Object { $_ })
$process = Start-Process -FilePath $executable -WorkingDirectory $port -ArgumentList $arguments -PassThru
"pid $($process.Id), log $log"
