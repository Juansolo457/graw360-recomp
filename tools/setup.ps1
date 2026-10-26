# Set up everything tools/build.ps1 needs that isn't in the repository (docs/SETUP.md):
#   1. the toolchain in .tools/: the ReXGlue SDK nightly 0.10.0.24-dev.gbd833a2 (prebuilt), LLVM
#      20.1.8, CMake 3.31.8 and Ninja 1.13.1, each downloaded from its official release and
#      checked against its SHA-256;
#   2. third_party/rexglue-sdk-bd833a2: ReXGlue's source at that nightly's commit with this
#      project's patches (tools/patches), checked against the exact source tree the release was
#      built from;
#   3. from that source, the patched code generator (rexgluerd.exe: vpkd3d128, docs/TACMAP.md)
#      and the campaign's renderer plugin (rexgpu-xenosrd.dll: resolves into freed memory, vblank
#      pacing, gpu_skip_draws for the native renderer). tools/build.ps1 uses both.
#   pwsh -NoProfile -File tools/setup.ps1            # all of it (skips what is there already)
#   pwsh -NoProfile -File tools/setup.ps1 -Force     # replace an existing SDK source checkout
[CmdletBinding()]
param(
    [switch]$Force,
    [ValidateRange(1,64)][int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2)
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is many times slower with it
$root = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root '.tools'
$downloads = Join-Path $tools 'downloads'
$sdk = Join-Path $root 'third_party\rexglue-sdk-bd833a2'
$sdkCommit = 'bd833a2ab69d305f99ddf57418fc38ffec8af93d'
# In order; the last two are pgr4-native's shader translator changes the native renderer
# compiles against (docs/NATIVE_RENDERER.md).
$sdkPatches = @('rexglue-bd833a2-vpkd3d128-alias', 'rexglue-bd833a2-freed-page-resolve',
                'rexglue-bd833a2-vblank-pacing', 'rexglue-bd833a2-gpu-skip-draws',
                'pgr4-native/0021-gpu-sample-computed-LOD-pixel-shader-fetches-with-sa',
                'pgr4-native/0022-gpu-add-a-pixel-shader-modification-for-textures-kno')
$expectedTree = 'e24a63ed00f23999c12618526d20442ceb86a3d4'   # after the last patch

function Invoke-Git {
    & git.exe @args
    if ($LASTEXITCODE) { throw "git $($args -join ' ') failed ($LASTEXITCODE)." }
}

# Prerequisites.
foreach ($cmd in 'git', 'python') {
    if (!(Get-Command $cmd -ErrorAction SilentlyContinue)) { throw "$cmd is not installed or not on PATH (docs/SETUP.md)." }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) {
    & $vswhere -latest -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$vs) { throw "Visual Studio 2022 with the 'Desktop development with C++' workload is not installed (docs/SETUP.md)." }

# 1. Toolchain. Each entry: what proves it is installed, download URL, SHA-256, how to unpack.
$toolchain = @(
    @{ Name = 'ReXGlue SDK 0.10.0.24-dev.gbd833a2'; Check = 'rexglue-sdk/win-amd64/bin/rexglue.exe'
       Url = 'https://github.com/rexglue/rexglue-sdk/releases/download/nightly-20261002-bd833a2a/rexglue-sdk-0.10.0.24-dev.gbd833a2-win-amd64.zip'
       Sha = '1e19d7ec9be0d9d7e1a0088a34a33b068d4dea520181e81318528ef3b37627e7'; Into = 'rexglue-sdk' },
    @{ Name = 'LLVM 20.1.8'; Check = 'llvm/bin/clang++.exe'
       Url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/LLVM-20.1.8-win64.exe'
       Sha = '3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b'; Into = 'llvm' },
    @{ Name = 'CMake 3.31.8'; Check = 'cmake/cmake-3.31.8-windows-x86_64/bin/cmake.exe'
       Url = 'https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip'
       Sha = '81aa9964dbabd71fe02e7ec50472fd3ad56138c49944515ece9001efbff8d719'; Into = 'cmake' },
    @{ Name = 'Ninja 1.13.1'; Check = 'ninja/ninja.exe'
       Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.1/ninja-win.zip'
       Sha = '26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf'; Into = 'ninja' }
)
foreach ($t in $toolchain) {
    if (Test-Path (Join-Path $tools $t.Check)) { Write-Host "$($t.Name): installed"; continue }
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $file = Join-Path $downloads (Split-Path $t.Url -Leaf)
    if (!(Test-Path $file) -or (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $t.Sha) {
        Write-Host "$($t.Name): downloading $($t.Url)"
        Invoke-WebRequest -Uri $t.Url -OutFile $file
    }
    if ((Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $t.Sha) {
        Remove-Item $file
        throw "$($t.Name): the download's SHA-256 doesn't match; try again."
    }
    $dest = Join-Path $tools $t.Into
    Write-Host "$($t.Name): unpacking into $dest"
    if ($file.EndsWith('.exe')) {
        # The LLVM installer (NSIS): silent, into .tools\llvm.
        $p = Start-Process -FilePath $file -ArgumentList '/S', "/D=$dest" -Wait -PassThru
        if ($p.ExitCode) { throw "$($t.Name): the installer failed ($($p.ExitCode))." }
    } else {
        Expand-Archive -Path $file -DestinationPath $dest -Force
    }
    if (!(Test-Path (Join-Path $tools $t.Check))) { throw "$($t.Name): $($t.Check) is missing after unpacking." }
}

# 2. The SDK source with this project's patches.
if ((Test-Path $sdk) -and $Force) { Remove-Item -Recurse -Force $sdk }
if (Test-Path $sdk) {
    # A finished checkout has the graw360 branch at the expected tree. Anything else is a setup
    # that stopped partway (or a tree made by hand): replacing it is -Force's job, since it may
    # hold someone's own changes.
    $done = & git -C $sdk rev-parse --verify --quiet 'graw360^{tree}' 2>$null
    if ($done -ne $expectedTree) {
        throw "SDK source: $sdk is incomplete or wasn't made by tools/setup.ps1. Run it with -Force to replace it."
    }
    Write-Host "SDK source: $sdk exists already (-Force replaces it)"
} else {
    Write-Host 'SDK source: cloning ReXGlue'
    New-Item -ItemType Directory -Force (Split-Path $sdk) | Out-Null
    # Some SDK files sit deep enough to pass Windows' 260-character path limit.
    Invoke-Git -c core.longpaths=true clone --no-checkout https://github.com/rexglue/rexglue-sdk.git $sdk
    Invoke-Git -C $sdk config core.longpaths true
    Invoke-Git -C $sdk checkout --quiet --detach $sdkCommit
    Write-Host "SDK source: applying $($sdkPatches.Count) patches"
    foreach ($name in $sdkPatches) {
        # The patches are LF; a checkout may have given them CRLF.
        $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "patches/$name.patch")) -replace "`r`n", "`n"
        $tmp = New-TemporaryFile
        [IO.File]::WriteAllText($tmp, $text)
        try { Invoke-Git -C $sdk apply $tmp.FullName } finally { Remove-Item $tmp }
    }
    Invoke-Git -C $sdk add -A
    $tree = & git -C $sdk write-tree
    if ($tree -ne $expectedTree) {
        throw "The patched SDK tree is $tree, not ${expectedTree}: tools/patches changed without " +
              'tools/setup.ps1, or a patch applied differently.'
    }
    Invoke-Git -C $sdk -c user.name=graw360-recomp -c user.email=setup@localhost commit --quiet -m 'graw360-recomp patches'
    Invoke-Git -C $sdk switch --quiet -c graw360
}
# Every run, so one that stopped while fetching them picks up where it left off.
Write-Host 'SDK source: fetching its submodules'
Invoke-Git -C $sdk submodule update --init --recursive

# Without Windows symlink support git checks out a symbolic link as a small file holding the
# target's path: libmspack's cabextract/mspack/*.c and *.h are such links, and the compiler
# then reads "../../libmspack/mspack/lzxd.c" as C. Replace each one whose target is a file with
# a copy of it (directory links are macOS-only parts the Windows build doesn't use).
$repos = @('.') + @(& git -C $sdk submodule foreach --quiet --recursive 'echo $displaypath')
foreach ($repo in $repos) {
    $dir = Join-Path $sdk $repo
    foreach ($entry in (& git -C $dir ls-files -s | Where-Object { $_ -like '120000 *' })) {
        $file = Join-Path $dir ($entry -split "`t", 2)[1]
        $item = Get-Item -LiteralPath $file -ErrorAction SilentlyContinue
        if (!$item -or $item.LinkType -or $item.PSIsContainer -or $item.Length -gt 1024) { continue }
        $target = Join-Path (Split-Path $file) (Get-Content -LiteralPath $file -Raw).Trim()
        if (Test-Path -LiteralPath $target -PathType Leaf) { Copy-Item -LiteralPath $target $file -Force }
    }
}

# 3. Build the patched code generator and the campaign's renderer plugin.
$out = Join-Path $sdk 'out\win-amd64\RelWithDebInfo'
$built = @('rexgluerd.exe', 'rexgpu-xenosrd.dll') | Where-Object { !(Test-Path (Join-Path $out $_)) }
if (!$built -and !$Force) {
    Write-Host "SDK tools: built ($out)"
} else {
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    $env:PATH = "$tools\llvm\bin;$tools\ninja;$env:PATH"
    $cmake = "$tools\cmake\cmake-3.31.8-windows-x86_64\bin\cmake.exe"
    Push-Location $sdk
    try {
        Write-Host 'SDK tools: building the code generator and the renderer plugin'
        & $cmake --preset win-amd64 -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_USE_VULKAN=OFF
        if ($LASTEXITCODE) { throw 'SDK configure failed.' }
        & $cmake --build --preset win-amd64-relwithdebinfo --parallel $Jobs --target rexglue rexgpu-xenos
        if ($LASTEXITCODE) { throw 'SDK build failed.' }
    } finally { Pop-Location }
    foreach ($f in 'rexgluerd.exe', 'rexgpu-xenosrd.dll') {
        if (!(Test-Path (Join-Path $out $f))) { throw "SDK build: $f is missing in $out." }
    }
}
Write-Host 'Setup done. Next: tools/extract.ps1 with your disc image, then tools/build.ps1 (docs/SETUP.md).'
