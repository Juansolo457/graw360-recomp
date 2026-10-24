# Launch the GRAW recomp (port/, the front end; it hands over to the campaign and Instant Action)
# for playing. The guest is told it is on a 1280x720 display (as on a real 360; this also avoids
# ReXGlue's 4:3 squeeze at 200% Windows scaling) and renders at a multiple of that.
# Defaults: the guest display at 240 Hz (#11); 2x = 2560x1440 internal and 16x anisotropic filtering (#12). No FXAA by
# default (it blurs the image; -Aa fxaa|fxaa_extreme turns it on). The renderer can't force MSAA:
# the game picks its own (the campaign already renders with 4x MSAA). For smoother edges render
# above the window's size and let it downscale (supersampling): -Scale 3 = 3840x2160.
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed                 # 1440p internal, windowed
#   pwsh -NoProfile -File tools/launch.ps1                           # fullscreen
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 3          # 4K internal: supersampled edges
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 1 -Aa none   # as the 360 draws it
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -Trace          # every kernel call (file opens, ...) in the log
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -PerfLog        # per-frame CSV next to the log; fps summary every 10 s in the log (#11)
#   pwsh -NoProfile -File tools/launch.ps1 -Windowed -RefreshRate 60 -PerfLog    # the 360's 60 Hz cap, for comparison (#11)
# This opens a window on the user's screen. Never kill a running graw.exe; ask the user to quit.
[CmdletBinding()]
param(
    [ValidateRange(1,3)][int]$Scale = 2,                 # internal resolution: 1 = 720p, 2 = 1440p, 3 = 4K
    [ValidateSet(-1,0,1,2,3,4,5)][int]$Anisotropic = 5,  # SDK enum: 5 = 16x, -1 = the game's own
    [ValidateSet('none', 'fxaa', 'fxaa_extreme')][string]$Aa = 'none',   # FXAA blurs the image (play-test 2026-10-23)
    [switch]$Windowed,
    [switch]$Trace,       # --log_level trace --log_noisy: every kernel call with its arguments (large log)
    [switch]$PerfLog,     # --graw_frame_log: one CSV row per presented frame, logs\perf-<time>.csv (src/frame_timing.cpp)
    # The refresh rate the guest's display reports (24-240 Hz; 0 = the SDK's 60). GRAW presents
    # once per vblank, so at 60 it's capped at 60 fps; at 240 the campaign averaged 83 fps (up to
    # 136) with game speed unchanged (#11, play-test 2026-10-23).
    [ValidateRange(0,240)][double]$RefreshRate = 240,
    # The campaign on fear-nx's native Vulkan renderer (port-yeti-nx, #2): the front end hands
    # Campaign to that build instead of port-yeti. Built with tools/build.ps1 -Project port-yeti-nx.
    [switch]$Nx,
    # Run under RenderDoc (private/tools/renderdoc, or -RenderDocPath) with child-process hooking,
    # so the campaign started by the front end is captured too. Press F12 or Print Screen in the
    # game window to capture a frame; captures go to <project>/logs/renderdoc/ (#2 comparison).
    [switch]$RenderDoc,
    [string]$RenderDocPath = '',
    [string]$Extra = '',
    # port = the front end (graw.exe, what a player starts); port-mp = Instant Action/multiplayer (grawmp.exe), port-yeti = the campaign (grawyeti.exe), alone; port-yeti-nx = the campaign on the native renderer, alone
    [ValidateSet('port', 'port-mp', 'port-yeti', 'port-yeti-nx')][string]$Project = 'port',
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$port = Join-Path $PSScriptRoot "..\$Project" | Resolve-Path
$exeName = @{ 'port' = 'graw'; 'port-mp' = 'grawmp'; 'port-yeti' = 'grawyeti'; 'port-yeti-nx' = 'grawyeti' }[$Project]
$buildDir = "out\build\win-amd64-$($Configuration.ToLowerInvariant())"
# The native renderer's own options (other programs ignore options they don't know).
$nxArguments = @()
if ($Nx -or $Project -eq 'port-yeti-nx') {
    $campaign = Join-Path $PSScriptRoot "..\port-yeti-nx\$buildDir\grawyeti.exe"
    if (!(Test-Path $campaign)) { throw "port-yeti-nx isn't built: tools/build.ps1 -Project port-yeti-nx" }
    $nxArguments = @('--graw_campaign_exe', (Resolve-Path $campaign).Path,
        '--nx_renderer', 'native', '--nx_native_resolution_scale', "$Scale")
}
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
    $(if ($PerfLog) { @('--graw_frame_log', $perfCsv) } else { @() }) +
    $(if ($RefreshRate -gt 0) { @('--video_mode_refresh_rate', "$RefreshRate") } else { @() }) + $nxArguments +
    $(if ($Trace) { @('--log_level', 'trace', '--log_noisy') } else { @('--log_level', 'debug') }) + @($Extra -split '\s+' | Where-Object { $_ })
if ($RenderDoc) {
    $rdoc = if ($RenderDocPath) { $RenderDocPath } else {
        Get-ChildItem (Join-Path $PSScriptRoot '../private/tools/renderdoc') -Recurse -Filter renderdoccmd.exe -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName }
    if (!$rdoc -or !(Test-Path $rdoc)) { throw 'renderdoccmd.exe not found: put RenderDoc in private/tools/renderdoc or pass -RenderDocPath.' }
    $captureDir = Join-Path $port 'logs/renderdoc'
    New-Item -ItemType Directory -Force $captureDir | Out-Null
    $rdocArguments = @('capture', '--opt-hook-children', '-d', $port, '-c', (Join-Path $captureDir "graw-$stamp"), $executable) + $arguments
    $process = Start-Process -FilePath $rdoc -WorkingDirectory $port -ArgumentList $rdocArguments -PassThru
    "RenderDoc: captures go to $captureDir (F12 or Print Screen in the game window)"
} else {
    $process = Start-Process -FilePath $executable -WorkingDirectory $port -ArgumentList $arguments -PassThru
}
"pid $($process.Id), log $log"
