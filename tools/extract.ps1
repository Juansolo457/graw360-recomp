# Extract your own GRAW disc image into port/assets (the whole game partition, about 4.8 GB:
# default.xex, data/, video/, Sound_Bin/, Language/, the Yeti multiplayer executable and
# Yeti.big). Never committed. The ISO is only read.
#   pwsh -NoProfile -File tools/extract.ps1 -Iso 'D:\path\to\graw.iso'
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Iso)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $root 'port\assets'
New-Item -ItemType Directory -Force $assets | Out-Null
python -I (Join-Path $PSScriptRoot 'xdvdfs.py') $Iso extract $assets
if ($LASTEXITCODE) { throw 'Extraction failed.' }
# The campaign and Instant Action read the same disc: their assets folders are junctions to it.
foreach ($project in 'port-mp', 'port-yeti') {
    $link = Join-Path $root "$project\assets"
    $item = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
    if ($item -and $item.LinkType -in 'Junction', 'SymbolicLink' -and
        [IO.Path]::GetFullPath($item.LinkTarget, $item.Parent.FullName) -ne [IO.Path]::GetFullPath($assets)) {
        $item.Delete()   # a link to another copy of the disc: point it at this one
        $item = $null
    } elseif ($item -and !$item.LinkType) {
        throw "$link is a folder of its own, not a link to port\assets: move it away and run this again."
    }
    if (!$item) { New-Item -ItemType Junction -Path $link -Target $assets | Out-Null }
}
python -I (Join-Path $PSScriptRoot 'xexinfo.py') (Join-Path $assets 'default.xex')
"default.xex SHA-256 " + (Get-FileHash (Join-Path $assets 'default.xex') -Algorithm SHA256).Hash.ToLowerInvariant()
