# Build port/ (the ReXGlue GRAW project) inside a VS 2022 x64 developer environment.
# Toolchain: .tools/ (rexglue-sdk nightly, llvm, cmake, ninja), a junction to the N3 project's
# pinned copy. Codegen (translating default.xex to C++) runs as part of the build whenever
# graw_manifest.toml or the executable changed; the first run takes a few minutes.
#   pwsh -NoProfile -File tools/build.ps1 [-Configuration RelWithDebInfo] [-Jobs 14]
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1,64)][int]$Jobs = 14
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$port = Join-Path $root 'port'
$tools = Join-Path $root '.tools'
$cmake = "$tools/cmake/cmake-3.31.8-windows-x86_64/bin/cmake.exe"
if (!(Test-Path $cmake)) { throw "Toolchain missing at $tools (see docs/HANDOFF.md)." }
$xex = Join-Path $port 'assets/default.xex'
if (!(Test-Path $xex)) { throw 'port/assets/default.xex is missing: run tools/extract.ps1 first.' }
$expected = '295ac4ded72c95bb86e9d9441828bb5b193f863afb5a20a993275e01144cdb83'
if ((Get-FileHash $xex -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'port/assets/default.xex is not the supported GRAW executable (USA/Europe, title 555307D5).'
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
    $firstTime = !(Test-Path (Join-Path $port 'generated/default/sources.cmake'))
    & $cmake --build --preset $preset --parallel $Jobs --target graw_codegen
    if ($LASTEXITCODE) { throw 'Code generation failed.' }
    if ($firstTime) {
        & $cmake --preset $preset $prefix
        if ($LASTEXITCODE) { throw 'CMake re-configure failed.' }
    }
    & $cmake --build --preset $preset --parallel $Jobs
    if ($LASTEXITCODE) { throw 'Build failed.' }
    Write-Host "Built port/out/build/$preset/graw.exe"
} finally { Pop-Location }
