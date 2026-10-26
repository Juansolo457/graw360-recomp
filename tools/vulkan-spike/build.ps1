# Build the M0 spike (#41) in the SDK source tree's own build folder out/win-amd64-vulkan
# (Vulkan on); the D3D12 build in out/win-amd64 is not touched. Local only.
#   pwsh -NoProfile -File tools/vulkan-spike/build.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$sdk = Join-Path $root 'third_party\rexglue-sdk-bd833a2'
$tools = Join-Path $root '.tools'
$cmake = "$tools\cmake\cmake-3.31.8-windows-x86_64\bin\cmake.exe"
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -version '[17.0,18.0)' -property installationPath
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:PATH = "$tools\llvm\bin;$tools\ninja;$env:PATH"
$build = Join-Path $sdk 'out\win-amd64-vulkan'
Push-Location $sdk
try {
    & $cmake --preset win-amd64 -B $build -DREXGLUE_USE_VULKAN=ON -DREXGLUE_BUILD_TESTS=OFF `
        "-DCMAKE_PROJECT_ReXGlue_INCLUDE=$PSScriptRoot\spike.cmake"
    if ($LASTEXITCODE) { throw 'configure failed' }
    & $cmake --build $build --config RelWithDebInfo --parallel ([Environment]::ProcessorCount) --target graw_vulkan_spike
    if ($LASTEXITCODE) { throw 'build failed' }
    Get-ChildItem -Recurse -Filter 'graw_vulkan_spike.exe' $build | Select-Object -ExpandProperty FullName
} finally { Pop-Location }
