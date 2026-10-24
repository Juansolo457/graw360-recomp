# Build port/ (the ReXGlue GRAW project) inside a VS 2022 x64 developer environment.
# Toolchain: .tools/ (rexglue-sdk nightly, llvm, cmake, ninja), a junction to the N3 project's
# pinned copy. Codegen (translating default.xex to C++) runs as part of the build whenever
# graw_manifest.toml or the executable changed; the first run takes a few minutes.
#   pwsh -NoProfile -File tools/build.ps1 [-Configuration RelWithDebInfo] [-Jobs 14]
#   pwsh -NoProfile -File tools/build.ps1 -Project port-sp     # the campaign program (grawsp.exe)
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1,64)][int]$Jobs = 14,
    # port = the front end (default.xex -> graw.exe); port-sp = the campaign (GR3Xenon_Release.exe -> grawsp.exe)
    [ValidateSet('port', 'port-sp')][string]$Project = 'port'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$port = Join-Path $root $Project
$tools = Join-Path $root '.tools'
$cmake = "$tools/cmake/cmake-3.31.8-windows-x86_64/bin/cmake.exe"
if (!(Test-Path $cmake)) { throw "Toolchain missing at $tools (see docs/HANDOFF.md)." }
# Each project: its executable on the disc, that file's SHA-256, codegen target, output folder.
$projects = @{
    'port'    = @('default.xex', '295ac4ded72c95bb86e9d9441828bb5b193f863afb5a20a993275e01144cdb83', 'graw', 'default')
    'port-sp' = @('GR3Xenon_Release.exe', '4d8a0abb93f3867df53e229a54f55435a57b403f65781d5737237ff8c36c023a', 'grawsp', 'gr3xenon_release')
}
$xexName, $expected, $name, $outDir = $projects[$Project]
$xex = Join-Path $port "assets/$xexName"
if (!(Test-Path $xex)) { throw "$Project/assets/$xexName is missing: run tools/extract.ps1 first." }
if ((Get-FileHash $xex -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw "$Project/assets/$xexName is not the supported GRAW executable (USA/Europe, title 555307D5)."
}

# vcvars64.bat breaks on quoted PATH entries; the DevShell module does not.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -version '[17.0,18.0)' -property installationPath
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:PATH = "$tools/llvm/bin;$tools/ninja;$env:PATH"

$preset = 'win-amd64-' + $Configuration.ToLowerInvariant()
$prefix = "-DCMAKE_PREFIX_PATH=$tools/rexglue-sdk/win-amd64"
Push-Location $port
try {
    & $cmake --preset $preset $prefix
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    # Codegen first (a no-op when nothing changed), then reconfigure once if the generated
    # sources did not exist yet (the recomp target only appears once they do), then build.
    $firstTime = !(Test-Path (Join-Path $port "generated/$outDir/sources.cmake"))
    & $cmake --build --preset $preset --parallel $Jobs --target "$($name)_codegen"
    if ($LASTEXITCODE) { throw 'Code generation failed.' }
    if ($firstTime) {
        & $cmake --preset $preset $prefix
        if ($LASTEXITCODE) { throw 'CMake re-configure failed.' }
    }
    & $cmake --build --preset $preset --parallel $Jobs
    if ($LASTEXITCODE) { throw 'Build failed.' }
    Write-Host "Built $Project/out/build/$preset/$name.exe"
} finally { Pop-Location }
