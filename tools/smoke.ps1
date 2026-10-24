# Boot check: start graw.exe (its own profile and cache, debug logging), stop it after N
# seconds and print the end of the log. Its window DOES appear on the user's screen (--headless
# does not hide it) and they can play in it: say so before running. Surviving proves nothing about rendering.
# -Captures N saves N screenshots 400 ms apart just before stopping (private/captures/).
#   pwsh -NoProfile -File tools/smoke.ps1 [-Seconds 40] [-Captures 2] [-Extra '--gpu_plugin xenos']
[CmdletBinding()]
param(
    [ValidateRange(5,600)][int]$Seconds = 40,
    [ValidateRange(0,20)][int]$Captures = 0,
    [string]$Extra = '',
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$port = Join-Path $PSScriptRoot '..\port' | Resolve-Path
$executable = Join-Path $port "out\build\win-amd64-$($Configuration.ToLowerInvariant())\graw.exe"
New-Item -ItemType Directory -Force (Join-Path $port 'logs') | Out-Null
$log = Join-Path $port ('logs\smoke-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$info = [Diagnostics.ProcessStartInfo]::new($executable)
$info.WorkingDirectory = $port
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$extraArguments = @($Extra -split '\s+' | Where-Object { $_ })
foreach ($argument in @('--headless', '--no-fullscreen', '--resolution', '720p',
    '--game_data_root', "$port\assets", '--user_data_root', "$port\user-smoke",
    '--cache_root', "$port\cache-smoke", '--log_file', $log, '--log_level', 'debug', '--log_flush_interval', '1') + $extraArguments) {
    $info.ArgumentList.Add($argument)
}
$process = [Diagnostics.Process]::Start($info)
$stopped = !$process.WaitForExit([Math]::Max(1, $Seconds - $Captures) * 1000)
if ($stopped) {
    for ($i = 0; $i -lt $Captures; $i++) {
        & (Join-Path $PSScriptRoot 'capture_window.ps1') -ProcessId $process.Id -Out (Join-Path $PSScriptRoot '..\private\captures')
        Start-Sleep -Milliseconds 400
    }
    $process.Kill()
    $process.WaitForExit()
}
"stoppedByHarness=$stopped exitCode=$($process.ExitCode) extra='$Extra' log=$log"
if (Test-Path $log) { Get-Content $log -Tail 40 }
