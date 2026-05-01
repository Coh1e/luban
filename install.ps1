# luban one-shot installer for Windows.
#
# Usage:    irm https://luban.coh1e.com/install.ps1 | iex
#
# Drops luban.exe + luban-shim.exe into ~/.local/bin (XDG-style, shared with
# uv/pipx/claude-code), verifies SHA256, and offers to run `luban env --user`
# (HKCU PATH registration) and `luban setup` (toolchain install) for you.
#
# Override the install dir with $env:LUBAN_INSTALL_DIR or pre-create the
# target dir; the installer never elevates and never writes outside it.

#Requires -Version 5
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---- target dir -------------------------------------------------------------
$installDir = if ($env:LUBAN_INSTALL_DIR) { $env:LUBAN_INSTALL_DIR }
              else { Join-Path $env:USERPROFILE '.local\bin' }

if (-not (Test-Path $installDir)) {
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
}

# ---- discover latest release -----------------------------------------------
Write-Host "→ Querying github.com/Coh1e/luban for the latest release..."
$release = Invoke-RestMethod `
    -Uri 'https://api.github.com/repos/Coh1e/luban/releases/latest' `
    -Headers @{ 'User-Agent' = 'luban-installer' }
$tag = $release.tag_name
Write-Host "  found $tag"

function Get-AssetUrl($name) {
    $a = $release.assets | Where-Object { $_.name -eq $name }
    if (-not $a) { throw "release $tag has no asset '$name'" }
    return $a.browser_download_url
}

# ---- download with SHA verification ----------------------------------------
$sumsTmp = Join-Path $env:TEMP "luban-SHA256SUMS-$([Guid]::NewGuid()).txt"
Invoke-WebRequest -Uri (Get-AssetUrl 'SHA256SUMS') -OutFile $sumsTmp -UseBasicParsing
$sums = @{}
foreach ($line in Get-Content $sumsTmp) {
    $parts = $line -split '\s\s', 2
    if ($parts.Length -eq 2) { $sums[$parts[1].Trim()] = $parts[0].Trim() }
}
Remove-Item $sumsTmp -Force

function Install-Asset($name) {
    $expected = $sums[$name]
    if (-not $expected) { throw "SHA256SUMS does not list $name" }
    $tmp = Join-Path $env:TEMP "luban-$name-$([Guid]::NewGuid()).part"
    Write-Host "→ Downloading $name..."
    Invoke-WebRequest -Uri (Get-AssetUrl $name) -OutFile $tmp -UseBasicParsing
    $actual = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $expected) {
        Remove-Item $tmp -Force
        throw "SHA256 mismatch for $name`n  expected $expected`n  got      $actual"
    }
    $target = Join-Path $installDir $name
    Move-Item -Path $tmp -Destination $target -Force
    Write-Host "  installed → $target"
}

Install-Asset 'luban.exe'
Install-Asset 'luban-shim.exe'

# ---- HKCU PATH integration (best-effort, prompted) -------------------------
$inPath = ($env:Path -split ';') -contains $installDir
if (-not $inPath) {
    Write-Host ""
    Write-Host "$installDir is not on your HKCU PATH yet."
    $ans = Read-Host "Add it? [Y/n]"
    if ($ans -eq '' -or $ans -match '^[Yy]') {
        & (Join-Path $installDir 'luban.exe') env --user
    } else {
        Write-Host "Run \`luban env --user\` later to register on HKCU PATH."
    }
}

# ---- toolchain bootstrap (prompted) ----------------------------------------
Write-Host ""
$ans = Read-Host "Run \`luban setup\` now to install the default toolchain? [Y/n]"
if ($ans -eq '' -or $ans -match '^[Yy]') {
    & (Join-Path $installDir 'luban.exe') setup
} else {
    Write-Host "Run \`luban setup\` later when you want the toolchain."
}

Write-Host ""
Write-Host "✓ luban $tag installed at $installDir"
Write-Host "  open a new shell for PATH changes to take effect."
