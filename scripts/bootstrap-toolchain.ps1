# Download git + cmake + ninja + llvm-mingw into .toolchain/ at the project
# root. Self-contained sandbox: nothing under .toolchain/ is git-tracked,
# nothing reaches HKCU PATH / HKLM / registry — luban-test-induced damage
# to ~/.local/share/luban/ leaves this env intact.
#
# Usage:
#     scripts\bootstrap-toolchain.ps1            # 4 tools, ~600 MiB extracted
#     scripts\bootstrap-toolchain.ps1 -Force     # re-download even if present
#
# After this completes, activate per shell:
#     . scripts\activate-toolchain.ps1

#Requires -Version 5
[CmdletBinding()]
param(
    [switch]$Force
)
$ErrorActionPreference = 'Stop'

$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$toolchain   = Join-Path $projectRoot '.toolchain'
New-Item -ItemType Directory -Force $toolchain | Out-Null

# Win10 1803+ ships curl.exe + tar.exe; both are used directly so there's
# no PowerShell-version-specific zip handling.
$curlExe = (Get-Command curl.exe -ErrorAction SilentlyContinue)?.Source
if (-not $curlExe) { throw "curl.exe not found (Win10 1803+ ships it built-in)" }
$tarExe  = (Get-Command tar.exe -ErrorAction SilentlyContinue)?.Source
if (-not $tarExe)  { throw "tar.exe not found (Win10 1803+ ships it built-in)" }

function Get-LatestRelease($repo, [string]$pattern) {
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" `
        -Headers @{ 'User-Agent' = 'luban-bootstrap' }
    $a = $rel.assets | Where-Object { $_.name -like $pattern } | Select-Object -First 1
    if (-not $a) { throw "no asset matching '$pattern' in latest release of $repo" }
    return @{ Url = $a.browser_download_url; Name = $a.name; Size = $a.size }
}

function Fetch-And-Extract {
    param(
        [string]$Label,        # display name
        [string]$Repo,         # github owner/repo
        [string]$Pattern,      # asset glob
        [string]$DestDir,      # final dir name under .toolchain/
        [string]$ExtractedGlob, # pattern of dir produced by extraction
        [string]$VerifyExe     # exe to call --version on after extraction
    )
    $finalDir = Join-Path $toolchain $DestDir
    if ((Test-Path $finalDir) -and -not $Force) {
        $ver = & (Join-Path $finalDir $VerifyExe) --version 2>$null | Select-Object -First 1
        Write-Host "  ✓ $Label already present: $ver"
        return
    }
    if ($Force -and (Test-Path $finalDir)) {
        Write-Host "  → $Label removing existing (--Force)..."
        Remove-Item $finalDir -Recurse -Force
    }

    $info = Get-LatestRelease $Repo $Pattern
    Write-Host "  → $Label: $($info.Name) ($([int]($info.Size/1MB)) MiB)"
    $tmp = Join-Path $toolchain "_$DestDir.tmp"
    & $curlExe -fsSL --progress-bar -o $tmp $info.Url
    if ($LASTEXITCODE -ne 0) { throw "curl failed for $($info.Url)" }

    if ($info.Name -like '*.7z.exe') {
        # PortableGit self-extractor.
        & $tmp -y "-o`"$finalDir`"" -gm2 | Out-Null
        Remove-Item $tmp -Force
    } else {
        # Plain .zip — let tar.exe handle (faster + lower-memory than
        # Expand-Archive, and works with files curl just released).
        Push-Location $toolchain
        try { & $tarExe -xf $tmp } finally { Pop-Location }
        Remove-Item $tmp -Force

        # Most release zips wrap in a versioned dir (cmake-X.Y.Z-windows-x86_64
        # or llvm-mingw-DATE-msvcrt-x86_64). Rename to the stable DestDir.
        $extracted = Get-ChildItem $toolchain -Directory -Filter $ExtractedGlob `
            | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($extracted -and $extracted.Name -ne $DestDir) {
            Move-Item $extracted.FullName $finalDir -Force
        } elseif (-not $extracted -and -not (Test-Path $finalDir)) {
            # ninja-win.zip extracts straight to .toolchain/ninja.exe (no wrap dir).
            New-Item -ItemType Directory -Force $finalDir | Out-Null
            Get-ChildItem $toolchain -File -Filter '*.exe' | Where-Object {
                $_.Name -in @('ninja.exe')
            } | ForEach-Object { Move-Item $_.FullName $finalDir -Force }
        }
    }

    $exe = Join-Path $finalDir $VerifyExe
    if (-not (Test-Path $exe)) { throw "$Label extraction produced no $exe" }
    $ver = & $exe --version 2>$null | Select-Object -First 1
    Write-Host "    ✓ $ver"
}

Write-Host "→ Bootstrapping .toolchain/ at $toolchain"
Write-Host ""

# git first (we may want to use it to commit-bump-version-and-rebuild later).
Fetch-And-Extract -Label 'git'        -Repo 'git-for-windows/git' `
    -Pattern 'PortableGit-*-64-bit.7z.exe' `
    -DestDir 'git' -ExtractedGlob 'git' -VerifyExe 'cmd\git.exe'

Fetch-And-Extract -Label 'cmake'      -Repo 'Kitware/CMake' `
    -Pattern 'cmake-*-windows-x86_64.zip' `
    -DestDir 'cmake' -ExtractedGlob 'cmake-*' -VerifyExe 'bin\cmake.exe'

Fetch-And-Extract -Label 'ninja'      -Repo 'ninja-build/ninja' `
    -Pattern 'ninja-win.zip' `
    -DestDir 'ninja' -ExtractedGlob '__no_wrap_dir__' -VerifyExe 'ninja.exe'

Fetch-And-Extract -Label 'llvm-mingw' -Repo 'mstorsjo/llvm-mingw' `
    -Pattern 'llvm-mingw-*-msvcrt-x86_64.zip' `
    -DestDir 'llvm-mingw' -ExtractedGlob 'llvm-mingw-*' -VerifyExe 'bin\clang.exe'

Write-Host ""
Write-Host "✓ .toolchain bootstrapped"
$total = (Get-ChildItem $toolchain -Recurse | Measure-Object -Property Length -Sum).Sum / 1GB
Write-Host ("  total: {0:N2} GiB" -f $total)
Write-Host ""
Write-Host "Activate in current shell:"
Write-Host "    . scripts\activate-toolchain.ps1"
