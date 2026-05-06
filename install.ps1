# luban one-shot installer for Windows.
#
# Usage:    irm https://github.com/Coh1e/luban/raw/main/install.ps1 | iex
#
# Drops luban.exe + luban-shim.exe into ~/.local/bin (XDG-style, shared with
# uv/pipx/claude-code), verifies SHA256, and offers to run `luban env --user`
# (HKCU PATH registration) and bootstrap the foundation toolchain via
# `luban bp src add Coh1e/luban-bps` + `luban bp apply main/cpp-base`.
#
# Idempotent: re-running on an already-installed machine detects existing
# binaries (matching SHA = no-op; stale = update), checks PATH membership
# with native-separator normalization (no duplicate entries), and only
# bootstraps the toolchain when it is not already registered.
#
# Override the install dir with $env:LUBAN_INSTALL_DIR or pre-create the
# target dir; the installer never elevates and never writes outside it.
# Set $env:LUBAN_FORCE_REINSTALL=1 to force redownload even when SHAs match.
#
# Flavor: each release ships TWO Windows binaries — MSVC (smaller, ~3 MB,
# default) and LLVM-MinGW (larger, ~6 MB, traditional luban toolchain).
# Both are static-linked. Pick MinGW with $env:LUBAN_FLAVOR='mingw'.
#
# Slow VN / CN network? Set $env:LUBAN_GITHUB_MIRROR_PREFIX before running:
#     $env:LUBAN_GITHUB_MIRROR_PREFIX = 'https://ghfast.top'
#     irm https://ghfast.top/https://github.com/Coh1e/luban/raw/main/install.ps1 | iex
# luban itself reads the same env var for its own downloads (bp apply etc.).

#Requires -Version 5
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---- target dir -------------------------------------------------------------
# Always materialize the path with native separators (Join-Path on Windows
# emits '\') so downstream PATH comparisons line up.
$installDir = if ($env:LUBAN_INSTALL_DIR) { $env:LUBAN_INSTALL_DIR }
              else { Join-Path $env:USERPROFILE '.local\bin' }
$installDir = [System.IO.Path]::GetFullPath($installDir)

if (Test-Path $installDir) {
    Write-Host "→ Install dir: $installDir (exists)"
} else {
    Write-Host "→ Install dir: $installDir (creating)"
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
}

$forceReinstall = [bool]$env:LUBAN_FORCE_REINSTALL

# ---- flavor selection ------------------------------------------------------
$flavor = if ($env:LUBAN_FLAVOR) { $env:LUBAN_FLAVOR.ToLowerInvariant() } else { 'msvc' }
if ($flavor -ne 'msvc' -and $flavor -ne 'mingw') {
    throw "LUBAN_FLAVOR must be 'msvc' or 'mingw' (got '$flavor')"
}
# Map flavor to release-asset names. The on-disk name is always luban.exe /
# luban-shim.exe regardless of flavor — downstream tooling probes for
# `luban.exe` literally; suffix only lives in the release artifact name.
$assetLuban = "luban-$flavor.exe"
$assetShim  = "luban-shim-$flavor.exe"
Write-Host "→ flavor: $flavor"

# ---- mirror prefix (slow VN / CN networks) ---------------------------------
# Set LUBAN_GITHUB_MIRROR_PREFIX to a reverse-proxy URL prefix to bounce all
# github.com / api.github.com / *.githubusercontent.com requests through it.
# Examples that work today: https://ghfast.top, https://gh-proxy.com.
# Format: <prefix>/<full-original-url>. Empty / unset → direct.
$mirror = $env:LUBAN_GITHUB_MIRROR_PREFIX
if ($mirror) { $mirror = $mirror.TrimEnd('/') }

function Mirror-Url($u) {
    if (-not $mirror) { return $u }
    # api.github.com is NOT proxied by ghfast.top / gh-proxy.com (they
    # 403 it). The release JSON is small enough that a direct hit works
    # even on slow links; only the big asset downloads need the mirror.
    $hosts = @(
        'https://github.com/',
        'https://raw.githubusercontent.com/',
        'https://objects.githubusercontent.com/',
        'https://codeload.github.com/'
    )
    foreach ($h in $hosts) { if ($u.StartsWith($h)) { return "$mirror/$u" } }
    return $u
}

# Windows-shipped curl.exe (since 1803) speaks HTTP/2 and is empirically
# 80x faster than Invoke-WebRequest's HTTP/1.1 single-stream against
# GitHub's release CDN (measured 4 MB/s vs 48 KB/s on the same 3 MB
# binary, same instant). Fall back to Invoke-WebRequest only if curl is
# missing — extremely unlikely on Win10 1803+ but defensive doesn't hurt.
$curlExe = (Get-Command curl.exe -ErrorAction SilentlyContinue)?.Source

# Unified progress UI — matches the bar luban itself draws during
# `bp apply` (luban::progress::Bar in src/progress.{hpp,cpp}). Same column
# layout, same glyphs (↓ fetch / ✓ fetch), same `@ rate` tail, same
# format_bytes, same 1024-base units. Goal: install.ps1 download and
# `luban bp apply` download FEEL like one tool, not two.
$ESC = [char]27   # bare backtick-e doesn't survive on Windows PowerShell 5.x
$ARROW_DOWN = [char]0x2193  # ↓
$CHECK      = [char]0x2713  # ✓
$BLK_FULL   = [char]0x2593  # ▓ filled bar cell
$BLK_LIGHT  = [char]0x2591  # ░ empty bar cell

function Format-LubanBytes([int64]$n) {
    if ($n -lt 1024)        { return "$n B" }
    if ($n -lt 1048576)     { return '{0:N1} KiB' -f ($n / 1024.0) }
    if ($n -lt 1073741824)  { return '{0:N1} MiB' -f ($n / 1048576.0) }
    return '{0:N1} GiB' -f ($n / 1073741824.0)
}

function Get-ContentLength([string]$url) {
    try {
        $r = Invoke-WebRequest -Uri $url -Method Head -UseBasicParsing -TimeoutSec 30
        $cl = $r.Headers['Content-Length']
        if ($cl) { return [int64]($cl | Select-Object -First 1) }
    } catch {}
    return -1
}

function Write-LubanBar([string]$Action, [int64]$Done, [int64]$Total, [double]$Rate) {
    $verb = ('{0} {1}' -f $ARROW_DOWN, $Action).PadRight(11)
    $rateStr = (Format-LubanBytes ([int64]$Rate)) + '/s'
    if ($Total -gt 0) {
        $pct = [int](100 * $Done / $Total)
        $W = 12
        $filled = [Math]::Min($W, [int]($W * $Done / $Total))
        $bar = ([string]$BLK_FULL * $filled) + ([string]$BLK_LIGHT * ($W - $filled))
        $line = '  {0}[{1}] {2,3}%  {3}/{4}  @ {5}' -f $verb, $bar, $pct,
            (Format-LubanBytes $Done), (Format-LubanBytes $Total), $rateStr
    } else {
        $line = '  {0}{1}  @ {2}' -f $verb, (Format-LubanBytes $Done), $rateStr
    }
    [Console]::Error.Write("`r$ESC[2K$line")
}

function Write-LubanDoneFetch([int64]$Bytes, [double]$Seconds, [double]$Rate) {
    $verb = ('{0} fetch' -f $CHECK).PadRight(11)
    $rateStr = (Format-LubanBytes ([int64]$Rate)) + '/s'
    $secStr = if ($Seconds -lt 1) { '{0:N0}ms' -f ($Seconds * 1000) }
              elseif ($Seconds -lt 60) { '{0:N1}s' -f $Seconds }
              else { '{0}m {1}s' -f [int]($Seconds / 60), [int]($Seconds % 60) }
    $line = "  $verb$(Format-LubanBytes $Bytes) in $secStr @ $rateStr"
    [Console]::Error.WriteLine("`r$ESC[2K$line")
}

# VN/CN networks regularly get TCP RST mid-download against GitHub's
# release CDN (curl exit 56). Retry with exponential backoff + curl's
# `-C -` so the next attempt resumes from the partial .part file rather
# than restarting from byte 0. The retry loop is in PowerShell (not
# curl's --retry) so old curl versions without --retry-all-errors still
# get the resume behavior. We do NOT delete $dest between attempts —
# resume is the whole point.
function Download-File($url, $dest) {
    $maxAttempts = 6
    $lastError = $null
    for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
        if ($attempt -gt 1) {
            $sleep = [Math]::Min(30, [int][Math]::Pow(2, $attempt - 1))  # 2,4,8,16,30,30
            Write-Host "  retry $attempt/$maxAttempts in ${sleep}s (last: $lastError)..."
            Start-Sleep -Seconds $sleep
        }
        if ($curlExe) {
            # Pre-fetch Content-Length so the bar can render percentage from the
            # first frame. HEAD failure → bar falls back to "running count + rate"
            # without pct, gracefully.
            $totalBytes = Get-ContentLength $url
            $startBytes = if (Test-Path $dest) { (Get-Item $dest).Length } else { 0 }
            $t0 = Get-Date

            # curl runs silent in the background — we render the bar ourselves
            # in PowerShell by polling the .part file size every 100ms. -C - lets
            # a retry resume from the bytes the last attempt left on disk; the
            # `done` shown to the bar is `(file size now) - startBytes` so the
            # rate reflects THIS attempt's throughput, not cumulative.
            $stderrTmp = [System.IO.Path]::GetTempFileName()
            $proc = Start-Process -FilePath $curlExe -NoNewWindow -PassThru `
                -RedirectStandardError $stderrTmp `
                -ArgumentList @(
                    '-fsSL',
                    '--connect-timeout', '30', '--max-time', '900',
                    '-C', '-',
                    '-A', 'luban-installer',
                    '-o', $dest, $url
                )
            while (-not $proc.HasExited) {
                Start-Sleep -Milliseconds 100
                $sizeNow = if (Test-Path $dest) { (Get-Item $dest).Length } else { 0 }
                $dt = ((Get-Date) - $t0).TotalSeconds
                if ($dt -lt 0.001) { $dt = 0.001 }
                $thisAttemptBytes = $sizeNow - $startBytes
                $rate = $thisAttemptBytes / $dt
                Write-LubanBar 'fetch' $sizeNow $totalBytes $rate
            }
            if ($proc.ExitCode -eq 0) {
                $finalSize = (Get-Item $dest).Length
                $dt = ((Get-Date) - $t0).TotalSeconds
                if ($dt -lt 0.001) { $dt = 0.001 }
                $rate = ($finalSize - $startBytes) / $dt
                Write-LubanDoneFetch $finalSize $dt $rate
                Remove-Item $stderrTmp -ErrorAction SilentlyContinue
                return
            }
            # curl failed — clear the live bar, surface curl's stderr text,
            # then loop into retry.
            [Console]::Error.Write("`r$ESC[2K")
            $curlMsg = if (Test-Path $stderrTmp) {
                (Get-Content $stderrTmp -Raw -ErrorAction SilentlyContinue).Trim()
            } else { '' }
            Remove-Item $stderrTmp -ErrorAction SilentlyContinue
            $lastError = "curl exit $($proc.ExitCode)" + $(if ($curlMsg) { ": $curlMsg" })
        } else {
            try {
                Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing -TimeoutSec 900
                return
            } catch {
                $lastError = $_.Exception.Message
            }
        }
    }
    $hint = if (-not $mirror) {
        "`n  hint: set `$env:LUBAN_GITHUB_MIRROR_PREFIX (e.g. 'https://ghfast.top') and retry"
    } else { '' }
    throw "downloading $url failed after $maxAttempts attempts ($lastError)$hint"
}

# ---- discover latest release -----------------------------------------------
Write-Host "→ Querying github.com/Coh1e/luban for the latest release..."
if ($mirror) { Write-Host "  via mirror: $mirror" }
$release = Invoke-RestMethod `
    -Uri (Mirror-Url 'https://api.github.com/repos/Coh1e/luban/releases/latest') `
    -Headers @{ 'User-Agent' = 'luban-installer' }
$tag = $release.tag_name
Write-Host "  found $tag"

function Get-AssetUrl($name) {
    $a = $release.assets | Where-Object { $_.name -eq $name }
    if (-not $a) { throw "release $tag has no asset '$name'" }
    return (Mirror-Url $a.browser_download_url)
}

# ---- SHA256SUMS -------------------------------------------------------------
$sumsTmp = Join-Path $env:TEMP "luban-SHA256SUMS-$([Guid]::NewGuid()).txt"
Download-File (Get-AssetUrl 'SHA256SUMS') $sumsTmp
$sums = @{}
foreach ($line in Get-Content $sumsTmp) {
    $parts = $line -split '\s\s', 2
    if ($parts.Length -eq 2) { $sums[$parts[1].Trim()] = $parts[0].Trim() }
}
Remove-Item $sumsTmp -Force

function Test-Already-Installed($localName, $assetName) {
    $target = Join-Path $installDir $localName
    if (-not (Test-Path $target)) { return $false }
    $expected = $sums[$assetName]
    if (-not $expected) { return $false }
    $actual = (Get-FileHash -Path $target -Algorithm SHA256).Hash.ToLower()
    return ($actual -eq $expected)
}

# Download $assetName from the release and install it as $localName in
# $installDir. The flavor suffix is in the release-asset name only;
# on-disk we always write `luban.exe` / `luban-shim.exe` so PATH probes
# resolve uniformly.
function Install-Asset($localName, $assetName) {
    if ((-not $forceReinstall) -and (Test-Already-Installed $localName $assetName)) {
        Write-Host "  $localName already installed (SHA256 matches $assetName) — skipping"
        return
    }
    $expected = $sums[$assetName]
    if (-not $expected) { throw "SHA256SUMS does not list $assetName" }
    $tmp = Join-Path $env:TEMP "luban-$assetName-$([Guid]::NewGuid()).part"
    Write-Host "→ Downloading $assetName..."
    Download-File (Get-AssetUrl $assetName) $tmp
    $actual = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $expected) {
        Remove-Item $tmp -Force
        throw "SHA256 mismatch for $assetName`n  expected $expected`n  got      $actual"
    }
    $target = Join-Path $installDir $localName
    Move-Item -Path $tmp -Destination $target -Force
    Write-Host "  installed → $target"
}

Install-Asset 'luban.exe'      $assetLuban
Install-Asset 'luban-shim.exe' $assetShim

# ---- HKCU PATH integration (best-effort, prompted) -------------------------
# Read HKCU PATH directly (not $env:Path which is process-merged user+system),
# normalize each entry to backslash + lowercase, and check membership against
# the install dir. No duplicates if already present in any slash form.
function Test-OnUserPath($dir) {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $userPath) { return $false }
    $needle = $dir.TrimEnd('\','/').Replace('/', '\').ToLowerInvariant()
    foreach ($p in ($userPath -split ';')) {
        if (-not $p) { continue }
        $hay = $p.TrimEnd('\','/').Replace('/', '\').ToLowerInvariant()
        if ($hay -eq $needle) { return $true }
    }
    return $false
}

# Helper: invoke luban.exe with the parent console's TTY inherited so its
# own progress meters (download + extract) animate live. `& $exe ...`
# routes stderr through PowerShell's redirection layer which buffers it
# until the child exits — same root cause as the curl progress problem
# above. Start-Process -NoNewWindow keeps the inherited handles raw.
function Invoke-LubanLive {
    param([string[]]$Args)
    $proc = Start-Process -FilePath $lubanExe -ArgumentList $Args `
        -NoNewWindow -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "luban.exe exited $($proc.ExitCode): $($Args -join ' ')"
    }
}

if (Test-OnUserPath $installDir) {
    Write-Host ""
    Write-Host "$installDir is already on your HKCU PATH — no change needed."
} else {
    Write-Host ""
    Write-Host "$installDir is not on your HKCU PATH yet."
    $ans = Read-Host "Add it (and register VCPKG_ROOT / EM_CONFIG when applicable)? [Y/n]"
    if ($ans -eq '' -or $ans -match '^[Yy]') {
        Invoke-LubanLive @('env', '--user')
    } else {
        Write-Host "Run \`luban env --user\` later to register on HKCU PATH."
    }
}

# ---- bootstrap (idempotent) ------------------------------------------------
# luban.exe embeds zero blueprints (议题 AG); the foundation + extras live
# in Coh1e/luban-bps.
#
# Two-phase install:
#   1. main/foundation    — git + ssh + gcm + lfs. ALWAYS pre-applied;
#                           it's the universal prereq of practically every
#                           other bp (including cpp-toolchain). No prompt.
#   2. main/cpp-toolchain — Clang + cmake + ninja + vcpkg. PROMPTED, since
#                           someone installing luban for dotfile / CLI use
#                           may not want a 270-binary C++ stack on first run.
$lubanExe = Join-Path $installDir 'luban.exe'

function Test-BpSourceRegistered($name) {
    try {
        $out = & $lubanExe bp src ls 2>$null
        if ($LASTEXITCODE -ne 0) { return $false }
        return ($out | Select-String -SimpleMatch -Pattern $name -Quiet)
    } catch { return $false }
}

function Test-BpApplied($bp) {
    try {
        $out = & $lubanExe bp ls 2>$null
        if ($LASTEXITCODE -ne 0) { return $false }
        return ($out | Select-String -SimpleMatch -Pattern $bp -Quiet)
    } catch { return $false }
}

$mainRegistered = Test-BpSourceRegistered 'main'

# Phase 1: ensure main is registered + foundation is applied. No prompt.
Write-Host ""
if (-not $mainRegistered) {
    Write-Host "→ Registering Coh1e/luban-bps as `main` (one-time, no prompt)..."
    Invoke-LubanLive @('bp', 'src', 'add', 'Coh1e/luban-bps', '--name', 'main', '--yes')
    $mainRegistered = $true
}

if (Test-BpApplied 'foundation') {
    Write-Host "→ main/foundation already applied — skipping."
} else {
    Write-Host "→ Applying main/foundation (mingit + lfs + gcm + openssh)..."
    Invoke-LubanLive @('bp', 'apply', 'main/foundation')
}

# Phase 2: prompt for cpp-toolchain.
Write-Host ""
if (Test-BpApplied 'cpp-toolchain') {
    Write-Host "main/cpp-toolchain already applied — bootstrap done."
} else {
    $ans = Read-Host "Apply main/cpp-toolchain now? (Clang + cmake + ninja + vcpkg, ~600 MB) [Y/n]"
    if ($ans -eq '' -or $ans -match '^[Yy]') {
        Invoke-LubanLive @('bp', 'apply', 'main/cpp-toolchain')
    } else {
        Write-Host "Later, run:  luban bp apply main/cpp-toolchain"
    }
}

Write-Host ""
Write-Host "✓ luban $tag at $installDir"
Write-Host "  open a new shell for PATH changes to take effect."
