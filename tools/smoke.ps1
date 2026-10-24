# Boot check: start graw.exe (its own profile and cache, debug logging), stop it after N
# seconds and print the end of the log. Its window DOES appear on the user's screen (--headless
# does not hide it) and they can play in it: say so before running. Surviving proves nothing about rendering.
# -Captures N saves N screenshots 400 ms apart just before stopping; -CaptureEvery N saves one
# every N seconds during the run (private/captures/<project>-<time>/ either way).
# -Autopilot <script> plays a controller script (port/src/autopilot.h; scripts in tools/autopilot/).
# --headless (passed unless -Autopilot is given) makes the runtime answer the game's dialogs by
# itself (the save-name keyboard returns its default text at once, message boxes their default
# button): fine for boot checks, wrong for scripted play, where the controller answers them.
#   pwsh -NoProfile -File tools/smoke.ps1 [-Seconds 40] [-Captures 2] [-Extra '--gpu_plugin xenos']
#   pwsh -NoProfile -File tools/smoke.ps1 -Project port-yeti -Seconds 120 -CaptureEvery 5 -Autopilot tools/autopilot/campaign-newgame.txt
[CmdletBinding()]
param(
    [ValidateRange(5,1800)][int]$Seconds = 40,
    [ValidateRange(0,20)][int]$Captures = 0,
    [ValidateRange(0,600)][int]$CaptureEvery = 0,
    [string]$Autopilot = '',
    [string]$Tag = '',       # names the captures folder (<program>-<tag>-<time>)
    [string]$Extra = '',
    # port = the front end (graw.exe, what a player starts); port-mp = Instant Action/multiplayer (grawmp.exe), port-yeti = the campaign (grawyeti.exe), alone
    [ValidateSet('port', 'port-mp', 'port-yeti', 'port-yeti-nx')][string]$Project = 'port',
    [switch]$FreshProfile,   # start from an empty test profile (the same path through the menus every run)
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$port = Join-Path $root $Project | Resolve-Path
$exeName = @{ 'port' = 'graw'; 'port-mp' = 'grawmp'; 'port-yeti' = 'grawyeti'; 'port-yeti-nx' = 'grawyeti' }[$Project]
$executable = Join-Path $port "out\build\win-amd64-$($Configuration.ToLowerInvariant())\$exeName.exe"
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Force (Join-Path $port 'logs') | Out-Null
$log = Join-Path $port "logs\smoke-$stamp.log"
$captureDir = Join-Path $root ("private\captures\$exeName-" + $(if ($Tag) { "$Tag-" } else { "" }) + $stamp)
if ($FreshProfile) { Remove-Item -Recurse -Force (Join-Path $port 'user-smoke') -ErrorAction SilentlyContinue }
$info = [Diagnostics.ProcessStartInfo]::new($executable)
$info.WorkingDirectory = $port
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$extraArguments = @($Extra -split '\s+' | Where-Object { $_ })
if ($Autopilot) { $extraArguments += @('--graw_autopilot', (Resolve-Path (Join-Path $root $Autopilot)).Path) }
$headless = if ($Autopilot) { @() } else { @('--headless') }
foreach ($argument in $headless + @('--no-fullscreen', '--resolution', '720p',
    '--game_data_root', "$port\assets", '--user_data_root', "$port\user-smoke",
    '--cache_root', "$port\cache-smoke", '--log_file', $log, '--log_level', 'debug', '--log_flush_interval', '1') + $extraArguments) {
    $info.ArgumentList.Add($argument)
}
$capture = { & (Join-Path $PSScriptRoot 'capture_window.ps1') -ProcessId $process.Id -Out $captureDir | Out-Null }
$process = [Diagnostics.Process]::Start($info)
$start = Get-Date
$deadline = [Math]::Max(1, $Seconds - $Captures)
$next = $CaptureEvery
while (!$process.HasExited) {
    $elapsed = ((Get-Date) - $start).TotalSeconds
    if ($elapsed -ge $deadline) { break }
    if ($CaptureEvery -gt 0 -and $elapsed -ge $next) {
        New-Item -ItemType Directory -Force $captureDir | Out-Null
        try { & $capture } catch { }
        $next += $CaptureEvery
    }
    Start-Sleep -Milliseconds 250
}
$stopped = !$process.HasExited
if ($stopped) {
    New-Item -ItemType Directory -Force $captureDir | Out-Null
    for ($i = 0; $i -lt $Captures; $i++) {
        try { & $capture } catch { }
        Start-Sleep -Milliseconds 400
    }
    $process.Kill()
    $process.WaitForExit()
}
"stoppedByHarness=$stopped exitCode=$($process.ExitCode) extra='$Extra' log=$log captures=$captureDir"
if (Test-Path $log) { Get-Content $log -Tail 40 }
