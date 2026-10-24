# Build port/ (the ReXGlue GRAW project) inside a VS 2022 x64 developer environment.
# Toolchain: .tools/ (rexglue-sdk nightly, llvm, cmake, ninja), a junction to the N3 project's
# pinned copy. Codegen (translating default.xex to C++) runs as part of the build whenever
# graw_manifest.toml or the executable changed; the first run takes a few minutes.
#   pwsh -NoProfile -File tools/build.ps1 [-Configuration RelWithDebInfo] [-Jobs 14]
#   pwsh -NoProfile -File tools/build.ps1 -Project port-mp     # Instant Action/multiplayer (grawmp.exe); -Project port-yeti = the campaign
#   pwsh -NoProfile -File tools/build.ps1 -Project port-yeti-nx   # the campaign on fear-nx's SDK and Vulkan renderer (#2)
#     (builds that SDK from source: third_party/rexglue-sdk-nx; the first build takes ~15 minutes)
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1,64)][int]$Jobs = 14,
    # port = the front end (default.xex -> graw.exe); port-mp = Instant Action/multiplayer (GR3Xenon_Release.exe -> grawmp.exe); port-yeti = the campaign (Yeti_Final_Xenon.exe -> grawyeti.exe)
    [ValidateSet('port', 'port-mp', 'port-yeti', 'port-yeti-nx')][string]$Project = 'port'
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
    'port-mp' = @('GR3Xenon_Release.exe', '4d8a0abb93f3867df53e229a54f55435a57b403f65781d5737237ff8c36c023a', 'grawmp', 'gr3xenon_release')
    'port-yeti' = @('Yeti_Final_Xenon.exe', '32735d0908bd45e965d427de6d1be2f995cb9ef44f6633b5ee7df5f34dbaadfb', 'grawyeti', 'yeti_final_xenon')
    'port-yeti-nx' = @('Yeti_Final_Xenon.exe', '32735d0908bd45e965d427de6d1be2f995cb9ef44f6633b5ee7df5f34dbaadfb', 'grawyeti', 'yeti_final_xenon')
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
# x86-64-v3 (AVX2, FMA, BMI; every PC CPU since ~2013 and the Steam Deck): the translated code's
# std::fma for every PowerPC multiply-add is then one instruction instead of a C runtime call
# (#11). src/cpu_check.cpp stops older CPUs with a message. Changing these flags rebuilds all.
$archFlags = @('-DCMAKE_C_FLAGS=-march=x86-64-v3', '-DCMAKE_CXX_FLAGS=-march=x86-64-v3')
if ($Project -like '*-nx') {
    # fear-nx's patched SDK from source, Vulkan on, the Xenos plugin linked in (as fear360's
    # tools/build-nx.ps1). fear-nx's clock patch divides an unsigned __int128 (__udivti3), which
    # MSVC's runtime lacks: link compiler-rt's builtins from the LLVM in .tools by path.
    $sdk = Join-Path $root 'third_party/rexglue-sdk-nx'
    if (!(Test-Path "$sdk/src/graphics/plugin_main.cpp")) { throw "Patched SDK missing at $sdk (copy ../fear360-recomp/third_party/rexglue-sdk-nx without its out folder; not a junction: the SDK writes its libraries into its own out folder, which would overwrite fear360's)." }
    $builtins = Get-ChildItem "$tools/llvm/lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib" | Select-Object -First 1
    if (!$builtins) { throw 'clang_rt.builtins-x86_64.lib not found in the LLVM toolchain.' }
    $builtinsPath = ($builtins.FullName -replace '\\', '/')
    $prefix = "-DREXSDK_DIR=$($sdk -replace '\\', '/')"
    $archFlags += @("-DCMAKE_EXE_LINKER_FLAGS=$builtinsPath", "-DCMAKE_SHARED_LINKER_FLAGS=$builtinsPath",
        '-DREXGLUE_USE_D3D12=ON', '-DREXGLUE_USE_VULKAN=ON', '-DREXGLUE_GPU_XENOS_STATIC=ON',
        '-DREXGLUE_BUILD_TESTS=OFF')
}
Push-Location $port
try {
    & $cmake --preset $preset $prefix @archFlags
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    # Codegen first (a no-op when nothing changed), then reconfigure once if the generated
    # sources did not exist yet (the recomp target only appears once they do), then build.
    $firstTime = !(Test-Path (Join-Path $port "generated/$outDir/sources.cmake"))
    & $cmake --build --preset $preset --parallel $Jobs --target "$($name)_codegen"
    if ($LASTEXITCODE) { throw 'Code generation failed.' }
    if ($firstTime) {
        & $cmake --preset $preset $prefix @archFlags
        if ($LASTEXITCODE) { throw 'CMake re-configure failed.' }
    }
    & $cmake --build --preset $preset --parallel $Jobs
    if ($LASTEXITCODE) { throw 'Build failed.' }
    Write-Host "Built $Project/out/build/$preset/$name.exe"
} finally { Pop-Location }
