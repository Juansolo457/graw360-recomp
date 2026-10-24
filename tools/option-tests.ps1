# Renderer option tests: play the same controller script once per option set and keep each run's
# screenshots, then tile them into one contact sheet per run for side-by-side comparison.
# Each run opens a game window on the user's screen (no keyboard input is sent): ask first.
#   pwsh -NoProfile -File tools/option-tests.ps1                       # all sets, ~2.5 min each
#   pwsh -NoProfile -File tools/option-tests.ps1 -Only baseline,readback-some
# Results: private/captures/option-tests-<time>/<set>.jpg plus each run's log in port-yeti/logs/.
[CmdletBinding()]
param(
    [ValidateSet('port', 'port-mp', 'port-yeti')][string]$Project = 'port-yeti',
    [string]$Autopilot = 'tools/autopilot/campaign-newgame.txt',
    [ValidateRange(30,900)][int]$Seconds = 150,
    [ValidateRange(1,60)][int]$CaptureEvery = 5,
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
# Set name -> extra flags. See the GitHub issues each one is for.
$sets = [ordered]@{
    'baseline'          = ''
    'readback-some'     = '--readback_resolve some'                         # #1 #2 #3
    'readback-full'     = '--readback_resolve full'                         # #1 #2 #3
    'invalid-fetch'     = '--gpu_allow_invalid_fetch_constants=true'        # #6 #4
    'depth24-in-shader' = '--depth_float24_convert_in_pixel_shader=true'    # #5
    'rov'               = '--render_target_path_d3d12 rov'                  # #1 #5
}
$out = Join-Path $root ('private\captures\option-tests-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $out | Out-Null
$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
foreach ($name in $sets.Keys) {
    if ($Only.Count -and $Only -notcontains $name) { continue }
    Write-Host "== $name : $($sets[$name])"
    $result = pwsh -NoProfile -File (Join-Path $PSScriptRoot 'smoke.ps1') -Project $Project -Seconds $Seconds `
        -CaptureEvery $CaptureEvery -FreshProfile -Autopilot $Autopilot -Tag $name -Extra $sets[$name] 2>&1
    $line = $result | Where-Object { $_ -match 'stoppedByHarness' } | Select-Object -First 1
    Write-Host $line
    $captures = ($line -replace '.*captures=', '').Trim()
    Add-Content (Join-Path $out 'runs.txt') "$name`t$($sets[$name])`t$line"
    if ($ffmpeg -and (Test-Path $captures)) {
        $frames = @(Get-ChildItem $captures -Filter *.png | Sort-Object Name)
        if ($frames.Count) {
            # Every frame scaled to 480 wide, 6 per row.
            $inputs = @(); $scale = ''; $labels = ''; $layout = @()
            for ($i = 0; $i -lt $frames.Count; $i++) {
                $inputs += @('-i', $frames[$i].FullName)
                $scale += "[$i]scale=480:270[s$i];"
                $labels += "[s$i]"
                $layout += "$(($i % 6) * 480)_$([math]::Floor($i / 6) * 270)"
            }
            $filter = "$scale$labels" + "xstack=inputs=$($frames.Count):layout=$($layout -join '|'):fill=black"
            if ($frames.Count -eq 1) { $filter = '[0]scale=480:270' }
            & $ffmpeg -v error -y @inputs -filter_complex $filter -q:v 4 (Join-Path $out "$name.jpg")
        }
    }
}
"results in $out"
