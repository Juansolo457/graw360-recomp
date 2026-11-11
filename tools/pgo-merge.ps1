# Merge a Windows PGO training run's counts into the profile the next build uses: the .profraw
# files a tools/build.ps1 -Pgo generate build wrote (pgo-raw/ beside the program, port/src/pgo.cpp)
# into pgo/<project>/default.profdata, which tools/build.ps1 then uses by default. The Windows
# counterpart of tools/pgo-merge.sh, with the llvm-profdata of the LLVM in .tools (the compiler that
# builds with the profile).
#   pwsh -NoProfile -File tools/pgo-merge.ps1 -Project port-yeti port-yeti/out/build/win-amd64-relwithdebinfo-pgogen/pgo-raw
# pgo/ stays out of git: the counts come from runs of the game's code.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('port', 'port-mp', 'port-yeti')][string]$Project,
    # Folders holding .profraw files, or the files.
    [Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Inputs
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$profdata = Join-Path $root '.tools/llvm/bin/llvm-profdata.exe'
if (!(Test-Path $profdata)) { throw "llvm-profdata is missing at $profdata (tools/setup.ps1)." }
$files = foreach ($i in $Inputs) {
    if (Test-Path $i -PathType Container) { Get-ChildItem $i -Filter '*.profraw' | ForEach-Object FullName }
    elseif (Test-Path $i -PathType Leaf) { (Resolve-Path $i).Path }
    else { throw "$i doesn't exist." }
}
if (!$files) { throw "No .profraw files in $($Inputs -join ', ')." }

$outDir = Join-Path $root "pgo/$Project"
New-Item -ItemType Directory -Force $outDir | Out-Null
$work = Join-Path ([IO.Path]::GetTempPath()) ("graw-pgo-" + [guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
try {
    # Each session's total (the last snapshot of a file an older training build appended to),
    # merged, then without the functions the training never reached (tools/pgo_prepare.py has why).
    & python -I (Join-Path $PSScriptRoot 'pgo_prepare.py') last-snapshot "$work/raw" @files
    if ($LASTEXITCODE) { throw 'pgo_prepare.py last-snapshot failed.' }
    & $profdata merge --text -o "$work/all.proftext" (Get-ChildItem "$work/raw" -Filter '*.profraw' | ForEach-Object FullName)
    if ($LASTEXITCODE) { throw 'llvm-profdata merge failed.' }
    & python -I (Join-Path $PSScriptRoot 'pgo_prepare.py') drop-unreached "$work/all.proftext" "$work/reached.proftext"
    if ($LASTEXITCODE) { throw 'pgo_prepare.py drop-unreached failed.' }
    & $profdata merge -o (Join-Path $outDir 'default.profdata') "$work/reached.proftext"
    if ($LASTEXITCODE) { throw 'llvm-profdata merge failed.' }
    Write-Host "llvm-profdata: $profdata"
    Write-Host "Merged $(@($files).Count) run(s) into pgo/$Project/default.profdata"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
