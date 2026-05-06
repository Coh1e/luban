@echo off
setlocal
set "BOOTSTRAP_DEV_SELF=%~f0"
set "BOOTSTRAP_DEV_ROOT=%~dp0"
set "BOOTSTRAP_DEV_ARGS=%*"
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "$lines = Get-Content -LiteralPath $env:BOOTSTRAP_DEV_SELF;" ^
  "$marker = '### POWERSHELL ###';" ^
  "$index = [Array]::IndexOf($lines, $marker);" ^
  "if ($index -lt 0) { throw 'bootstrap-dev.cmd marker not found' }" ^
  "$script = ($lines[($index + 1)..($lines.Length - 1)] -join [Environment]::NewLine);" ^
  "$block = [ScriptBlock]::Create($script);" ^
  "& $block"
exit /b %errorlevel%
### POWERSHELL ###
# bootstrap-dev.cmd
#
# Purpose:
#   Recover a Luban development machine when the published luban.exe release
#   is unusable. The script bootstraps the same default toolchain set Luban
#   would normally install, using the repo's manifests_seed/*.json as the
#   single source of truth for versions, URLs, and SHA256 hashes.
#
# Why this shape:
#   - The entrypoint stays a single .cmd so a clean Windows box can invoke it
#     without first trusting PowerShell execution policy or a preinstalled
#     luban.exe.
#   - The implementation itself lives in PowerShell because archive download,
#     SHA256 verification, JSON parsing, and ZIP extraction are all first-
#     class there; rewriting that logic in batch would be fragile.
#   - Directory layout intentionally matches src/paths.cpp and env semantics
#     match src/env_snapshot.cpp / src/commands/env.cpp. That keeps rescue
#     bootstraps and normal Luban installs in one state space instead of two.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Step([string]$Text) {
    Write-Host ("-> " + $Text)
}

function Write-Ok([string]$Text) {
    Write-Host ("ok " + $Text)
}

function Write-Note([string]$Text) {
    Write-Host ("   " + $Text)
}

function Test-HasProperty($Object, [string]$Name) {
    return $null -ne $Object -and ($Object.PSObject.Properties.Name -contains $Name)
}

function Get-TrimmedEnv([string]$Name) {
    $value = [Environment]::GetEnvironmentVariable($Name, 'Process')
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $null
    }
    return $value.Trim()
}

function Get-HomeDir() {
    $userProfile = Get-TrimmedEnv 'USERPROFILE'
    if ($userProfile) {
        return $userProfile
    }

    $drive = Get-TrimmedEnv 'HOMEDRIVE'
    $path = Get-TrimmedEnv 'HOMEPATH'
    if ($drive -and $path) {
        return ($drive + $path)
    }

    return [Environment]::GetFolderPath('UserProfile')
}

function Expand-Tilde([string]$PathText) {
    if ([string]::IsNullOrWhiteSpace($PathText)) {
        return $PathText
    }

    if ($PathText -eq '~') {
        return Get-HomeDir
    }

    if ($PathText.StartsWith('~/') -or $PathText.StartsWith('~\')) {
        return Join-Path (Get-HomeDir) $PathText.Substring(2)
    }

    return $PathText
}

function Get-LocalAppDataDir() {
    $raw = Get-TrimmedEnv 'LOCALAPPDATA'
    if ($raw) {
        return (Expand-Tilde $raw)
    }

    $fallback = [Environment]::GetFolderPath('LocalApplicationData')
    if (-not [string]::IsNullOrWhiteSpace($fallback)) {
        return $fallback
    }

    return (Join-Path (Get-HomeDir) 'AppData\Local')
}

function Resolve-LubanRoleHome([string]$Role, [string]$XdgVar, [string]$WindowsFallback) {
    $prefix = Get-TrimmedEnv 'LUBAN_PREFIX'
    if ($prefix) {
        return (Join-Path (Expand-Tilde $prefix) $Role)
    }

    $xdg = Get-TrimmedEnv $XdgVar
    if ($xdg) {
        return (Join-Path (Expand-Tilde $xdg) 'luban')
    }

    return (Join-Path (Get-LocalAppDataDir) $WindowsFallback)
}

function Get-LubanPaths() {
    $data = Resolve-LubanRoleHome 'data' 'XDG_DATA_HOME' 'luban'
    $cache = Resolve-LubanRoleHome 'cache' 'XDG_CACHE_HOME' 'luban\Cache'
    $state = Resolve-LubanRoleHome 'state' 'XDG_STATE_HOME' 'luban\State'
    $config = Resolve-LubanRoleHome 'config' 'XDG_CONFIG_HOME' 'luban\Config'

    $paths = [ordered]@{
        data = $data
        cache = $cache
        state = $state
        config = $config
        store = Join-Path $data 'store'
        store_sha256 = Join-Path $data 'store\sha256'
        toolchains = Join-Path $data 'toolchains'
        bin = Join-Path $data 'bin'
        env = Join-Path $data 'env'
        registry = Join-Path $data 'registry'
        buckets = Join-Path $data 'registry\buckets'
        overlay = Join-Path $data 'registry\overlay'
        downloads = Join-Path $cache 'downloads'
        vcpkg_binary = Join-Path $cache 'vcpkg-binary'
        vcpkg_cache = Join-Path $cache 'vcpkg'
        vcpkg_downloads = Join-Path $cache 'vcpkg\downloads'
        vcpkg_archives = Join-Path $cache 'vcpkg\archives'
        vcpkg_registries = Join-Path $cache 'vcpkg\registries'
        logs = Join-Path $state 'logs'
        installed_json = Join-Path $state 'installed.json'
        last_sync = Join-Path $state '.last_sync'
        selection_json = Join-Path $config 'selection.json'
        config_toml = Join-Path $config 'config.toml'
    }

    return $paths
}

function Get-LubanInstallDir() {
    $override = Get-TrimmedEnv 'LUBAN_INSTALL_DIR'
    if ($override) {
        return (Expand-Tilde $override)
    }

    return (Join-Path (Get-HomeDir) '.local\bin')
}

function Ensure-Dir([string]$PathText) {
    New-Item -ItemType Directory -Force -Path $PathText | Out-Null
}

function Ensure-LubanDirs($Paths) {
    foreach ($Name in @(
        'data', 'cache', 'state', 'config', 'store', 'store_sha256',
        'toolchains', 'bin', 'env', 'registry', 'buckets', 'overlay',
        'downloads', 'vcpkg_binary', 'vcpkg_cache', 'vcpkg_downloads',
        'vcpkg_archives', 'vcpkg_registries', 'logs'
    )) {
        Ensure-Dir $Paths[$Name]
    }
}

function Copy-SelectionSeedIfMissing([string]$RepoRoot, $Paths) {
    if (Test-Path -LiteralPath $Paths.selection_json) {
        return
    }

    $seed = Join-Path $RepoRoot 'manifests_seed\selection.json'
    if (Test-Path -LiteralPath $seed) {
        Copy-Item -LiteralPath $seed -Destination $Paths.selection_json -Force
    }
}

function Read-JsonFile([string]$PathText) {
    return (Get-Content -LiteralPath $PathText -Raw | ConvertFrom-Json)
}

function Get-ManifestField($Manifest, $ArchBlock, [string]$FieldName) {
    if ($ArchBlock -and (Test-HasProperty $ArchBlock $FieldName)) {
        return $ArchBlock.$FieldName
    }

    if (Test-HasProperty $Manifest $FieldName) {
        return $Manifest.$FieldName
    }

    return $null
}

function Strip-UrlRenameSuffix([string]$Url) {
    if ([string]::IsNullOrWhiteSpace($Url)) {
        return $Url
    }

    $trimmed = $Url.Trim()
    $hashIndex = $trimmed.IndexOf('#')
    if ($hashIndex -ge 0) {
        return $trimmed.Substring(0, $hashIndex)
    }

    return $trimmed
}

function Normalize-HashSpec([string]$HashText) {
    if ([string]::IsNullOrWhiteSpace($HashText)) {
        throw 'manifest hash is empty'
    }

    $trimmed = $HashText.Trim().ToLowerInvariant()
    if ($trimmed.Contains(':')) {
        return $trimmed
    }

    return ('sha256:' + $trimmed)
}

function Coerce-StringList($Raw) {
    if ($null -eq $Raw) {
        return @()
    }

    if ($Raw -is [string]) {
        return @([string]$Raw)
    }

    $out = New-Object System.Collections.Generic.List[string]
    foreach ($Item in @($Raw)) {
        if ($Item -is [string] -and -not [string]::IsNullOrWhiteSpace($Item)) {
            $out.Add($Item)
        }
    }

    return $out.ToArray()
}

function New-BinEntry([string]$RelativePath, [string]$Alias, [string[]]$PrefixArgs) {
    return [pscustomobject]@{
        relative_path = $RelativePath
        alias = $Alias
        prefix_args = $PrefixArgs
    }
}

function Coerce-OneBinEntry($Raw) {
    if ($Raw -is [string]) {
        $alias = [IO.Path]::GetFileNameWithoutExtension($Raw)
        return (New-BinEntry -RelativePath $Raw -Alias $alias -PrefixArgs @())
    }

    $parts = @($Raw)
    if ($parts.Count -eq 0 -or -not ($parts[0] -is [string])) {
        return $null
    }

    $relativePath = [string]$parts[0]
    $alias = [IO.Path]::GetFileNameWithoutExtension($relativePath)
    if ($parts.Count -gt 1 -and $parts[1] -is [string] -and -not [string]::IsNullOrWhiteSpace($parts[1])) {
        $alias = [string]$parts[1]
    }

    $prefixArgs = @()
    if ($parts.Count -gt 2 -and $parts[2] -is [string] -and -not [string]::IsNullOrWhiteSpace($parts[2])) {
        $prefixArgs = ([string]$parts[2]).Split(@(' ', "`t", "`r", "`n"), [System.StringSplitOptions]::RemoveEmptyEntries)
    }

    return (New-BinEntry -RelativePath $relativePath -Alias $alias -PrefixArgs $prefixArgs)
}

function Coerce-BinEntries($Raw) {
    if ($null -eq $Raw) {
        return @()
    }

    if ($Raw -is [string]) {
        return @((Coerce-OneBinEntry $Raw))
    }

    $items = @($Raw)
    if ($items.Count -eq 0) {
        return @()
    }

    $out = New-Object System.Collections.Generic.List[object]
    $allStrings = $true
    foreach ($item in $items) {
        if ($item -isnot [string]) {
            $allStrings = $false
            break
        }
    }

    # PowerShell collapses a JSON tuple like ["vcpkg.exe","vcpkg"] into a
    # plain object[] of strings, which is ambiguous with "list of string bins".
    # For Luban's rescue bootstrap we bias toward the Scoop shorthand tuple
    # when the array looks like [rel, alias] or [rel, alias, prefix-args]:
    # short (2-3 elems), first element path-like, second element alias-like.
    if ($allStrings -and $items.Count -ge 2 -and $items.Count -le 3) {
        $first = [string]$items[0]
        $second = [string]$items[1]
        $firstLooksLikePath = $first.Contains('\') -or $first.Contains('/') -or $first.Contains('.')
        $secondLooksLikeAlias = -not ($second.Contains('\') -or $second.Contains('/'))
        if ($firstLooksLikePath -and $secondLooksLikeAlias) {
            $entry = Coerce-OneBinEntry $items
            if ($entry) {
                return @($entry)
            }
        }
    }

    if ($items[0] -isnot [string] -and $items[0] -is [System.Collections.IEnumerable]) {
        foreach ($item in $items) {
            $entry = Coerce-OneBinEntry $item
            if ($entry) {
                $out.Add($entry)
            }
        }
    } else {
        foreach ($item in $items) {
            $entry = Coerce-OneBinEntry $item
            if ($entry) {
                $out.Add($entry)
            }
        }
    }

    return $out.ToArray()
}

function Resolve-Manifest([string]$RepoRoot, [string]$Name, [string]$Arch) {
    $manifestPath = Join-Path $RepoRoot ("manifests_seed\" + $Name + '.json')
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw ("manifest missing: " + $manifestPath)
    }

    $manifest = Read-JsonFile $manifestPath

    $scoopKey = switch ($Arch) {
        'x86_64' { '64bit' }
        'x86' { '32bit' }
        'aarch64' { 'arm64' }
        default { throw ("unsupported bootstrap arch: " + $Arch) }
    }

    $archBlock = $null
    if (Test-HasProperty $manifest 'architecture') {
        $archTable = $manifest.architecture
        if (Test-HasProperty $archTable $scoopKey) {
            $archBlock = $archTable.$scoopKey
        }
    }

    $version = if (Test-HasProperty $manifest 'version') { [string]$manifest.version } else { 'unknown' }
    $url = Strip-UrlRenameSuffix ([string](Get-ManifestField $manifest $archBlock 'url'))
    $hash = Normalize-HashSpec ([string](Get-ManifestField $manifest $archBlock 'hash'))
    $extractDir = Get-ManifestField $manifest $archBlock 'extract_dir'
    $bins = Coerce-BinEntries (Get-ManifestField $manifest $archBlock 'bin')
    $envAddPath = Coerce-StringList (Get-ManifestField $manifest $null 'env_add_path')
    $depends = Coerce-StringList (Get-ManifestField $manifest $null 'depends')
    $mirrors = New-Object System.Collections.Generic.List[string]
    foreach ($mirror in (Coerce-StringList (Get-ManifestField $manifest $archBlock 'luban_mirrors'))) {
        $mirrors.Add((Strip-UrlRenameSuffix $mirror))
    }

    return [pscustomobject]@{
        name = $Name
        version = $version
        url = $url
        hash = $hash
        extract_dir = $extractDir
        bins = $bins
        env_add_path = $envAddPath
        depends = $depends
        mirrors = $mirrors.ToArray()
        architecture = $Arch
    }
}

function Get-SelectionEnabledNames([string]$SelectionJsonPath) {
    $names = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $SelectionJsonPath)) {
        return $names.ToArray()
    }

    $doc = Read-JsonFile $SelectionJsonPath
    foreach ($groupName in @('components', 'extras')) {
        if (-not (Test-HasProperty $doc $groupName)) {
            continue
        }

        foreach ($entry in @($doc.$groupName)) {
            if (-not (Test-HasProperty $entry 'name')) {
                continue
            }
            $enabled = $false
            if (Test-HasProperty $entry 'enabled') {
                $enabled = [bool]$entry.enabled
            }
            if ($enabled) {
                $names.Add([string]$entry.name)
            }
        }
    }

    return $names.ToArray()
}

function Expand-ComponentDepends([string]$RepoRoot, [string[]]$Names, [string]$Arch) {
    $ordered = New-Object System.Collections.Generic.List[string]
    $seen = @{}

    function Visit-Component([string]$ComponentName) {
        $key = $ComponentName.ToLowerInvariant()
        if ($seen.ContainsKey($key)) {
            return
        }
        $seen[$key] = $true

        $resolved = Resolve-Manifest $RepoRoot $ComponentName $Arch
        foreach ($dep in @($resolved.depends)) {
            Visit-Component $dep
        }
        $ordered.Add($ComponentName)
    }

    foreach ($name in $Names) {
        Visit-Component $name
    }

    return $ordered.ToArray()
}

function Get-ToolchainDirName($Resolved) {
    $safeVersion = $Resolved.version.Replace('/', '-').Replace(':', '-')
    return ($Resolved.name + '-' + $safeVersion + '-' + $Resolved.architecture)
}

function Join-ManifestPath([string]$Root, [string]$RelativePath) {
    $normalized = $RelativePath.Replace('/', '\').Replace('\\', '\')
    return (Join-Path $Root $normalized)
}

function Get-ArchiveFileName([string]$Url, [string]$FallbackName) {
    try {
        $uri = [Uri]$Url
        $name = [IO.Path]::GetFileName($uri.AbsolutePath)
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            return $name
        }
    } catch {
    }

    return $FallbackName
}

function Get-ExpectedSha256([string]$HashSpec) {
    if ($HashSpec -notmatch '^sha256:(?<hex>[0-9a-f]+)$') {
        throw ("unsupported hash spec: " + $HashSpec)
    }

    return $Matches.hex.ToLowerInvariant()
}

function Get-FileSha256([string]$PathText) {
    return ((Get-FileHash -LiteralPath $PathText -Algorithm SHA256).Hash.ToLowerInvariant())
}

function Download-Archive($Resolved, [string]$ArchivePath) {
    $expected = Get-ExpectedSha256 $Resolved.hash

    if (Test-Path -LiteralPath $ArchivePath) {
        if ((Get-FileSha256 $ArchivePath) -eq $expected) {
            Write-Ok ("reused cached " + [IO.Path]::GetFileName($ArchivePath))
            return
        }

        Remove-Item -LiteralPath $ArchivePath -Force
    }

    $tmp = ($ArchivePath + '.part')
    $candidates = @($Resolved.url) + @($Resolved.mirrors)

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $tmp) {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        }

        Write-Step ("download " + [IO.Path]::GetFileName($ArchivePath) + ' from ' + $candidate)
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $candidate -OutFile $tmp
        } catch {
            Write-Note ("download failed: " + $_.Exception.Message)
            continue
        }

        $actual = Get-FileSha256 $tmp
        if ($actual -ne $expected) {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            throw ("SHA256 mismatch for " + [IO.Path]::GetFileName($ArchivePath))
        }

        Move-Item -LiteralPath $tmp -Destination $ArchivePath -Force
        Write-Ok ("downloaded " + [IO.Path]::GetFileName($ArchivePath))
        return
    }

    throw ("all download candidates failed for " + $Resolved.name)
}

function Remove-Tree([string]$PathText) {
    if (Test-Path -LiteralPath $PathText) {
        Remove-Item -LiteralPath $PathText -Recurse -Force
    }
}

function Resolve-ExtractRoot([string]$StagingPath, $Resolved) {
    if ($Resolved.extract_dir -and -not [string]::IsNullOrWhiteSpace([string]$Resolved.extract_dir)) {
        $candidate = Join-ManifestPath $StagingPath ([string]$Resolved.extract_dir)
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            return $candidate
        }
    }

    return $StagingPath
}

function Promote-Tree([string]$SourcePath, [string]$FinalPath) {
    if (Test-Path -LiteralPath $FinalPath) {
        Remove-Tree $FinalPath
    }

    try {
        Move-Item -LiteralPath $SourcePath -Destination $FinalPath -Force
        return
    } catch {
    }

    Copy-Item -LiteralPath $SourcePath -Destination $FinalPath -Recurse -Force
}

function Test-ToolchainReady($Resolved, [string]$FinalPath) {
    if (-not (Test-Path -LiteralPath $FinalPath -PathType Container)) {
        return $false
    }

    foreach ($entry in @($Resolved.bins)) {
        $binPath = Join-ManifestPath $FinalPath $entry.relative_path
        if (Test-Path -LiteralPath $binPath) {
            return $true
        }
    }

    foreach ($relDir in @($Resolved.env_add_path)) {
        $dirPath = Join-ManifestPath $FinalPath $relDir
        if (Test-Path -LiteralPath $dirPath -PathType Container) {
            return $true
        }
    }

    $children = @(Get-ChildItem -LiteralPath $FinalPath -Force -ErrorAction SilentlyContinue)
    return $children.Count -gt 0
}

function Install-ArchiveToToolchains($Resolved, [string]$ArchivePath, [string]$ToolchainsDir) {
    $toolchainDirName = Get-ToolchainDirName $Resolved
    $stagingPath = Join-Path $ToolchainsDir ('.tmp-' + $toolchainDirName)
    $finalPath = Join-Path $ToolchainsDir $toolchainDirName

    Remove-Tree $stagingPath
    Ensure-Dir $stagingPath

    Write-Step ("extract " + [IO.Path]::GetFileName($ArchivePath))
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $stagingPath -Force

    $extractRoot = Resolve-ExtractRoot $stagingPath $Resolved
    Promote-Tree $extractRoot $finalPath

    if ($extractRoot -ne $stagingPath) {
        Remove-Tree $stagingPath
    }

    Write-Ok ("installed " + $Resolved.name + ' ' + $Resolved.version + ' -> ' + $finalPath)
    return $finalPath
}

function Add-UniquePath([System.Collections.Generic.List[string]]$List,
                        [hashtable]$Seen,
                        [string]$PathText) {
    if ([string]::IsNullOrWhiteSpace($PathText)) {
        return
    }

    $full = [IO.Path]::GetFullPath($PathText)
    $key = $full.ToLowerInvariant()
    if ($Seen.ContainsKey($key)) {
        return
    }

    $Seen[$key] = $true
    $List.Add($full)
}

function Add-ComponentSessionPaths($Resolved, [string]$FinalPath,
                                   [System.Collections.Generic.List[string]]$List,
                                   [hashtable]$Seen) {
    foreach ($entry in @($Resolved.bins)) {
        $exePath = Join-ManifestPath $FinalPath $entry.relative_path
        if (Test-Path -LiteralPath $exePath) {
            Add-UniquePath $List $Seen ([IO.Path]::GetDirectoryName($exePath))
        }
    }

    foreach ($relDir in @($Resolved.env_add_path)) {
        $dirPath = Join-ManifestPath $FinalPath $relDir
        if (Test-Path -LiteralPath $dirPath -PathType Container) {
            Add-UniquePath $List $Seen $dirPath
        }
    }
}

function Refresh-SessionPath([System.Collections.Generic.List[string]]$List, [string]$BinDir) {
    $joined = @($List) + @($BinDir)
    $env:PATH = (($joined -join ';') + ';' + $script:OriginalPath)
}

function Get-RelativePathForRegistry([string]$BasePath, [string]$TargetPath) {
    $baseFull = [IO.Path]::GetFullPath($BasePath)
    $targetFull = [IO.Path]::GetFullPath($TargetPath)

    $baseUriText = $baseFull.TrimEnd('\', '/') + '\'
    $baseUri = [Uri]$baseUriText
    $targetUri = [Uri]$targetFull
    $relative = $baseUri.MakeRelativeUri($targetUri).ToString()
    return ([Uri]::UnescapeDataString($relative).Replace('\', '/'))
}

function Get-ComponentBins($Resolved, [string]$FinalPath) {
    $out = New-Object System.Collections.Generic.List[object]
    $seen = @{}

    foreach ($entry in @($Resolved.bins)) {
        $exePath = Join-ManifestPath $FinalPath $entry.relative_path
        if (-not (Test-Path -LiteralPath $exePath)) {
            Write-Note ("skipping missing bin target: " + $exePath)
            continue
        }

        $aliasKey = $entry.alias.ToLowerInvariant()
        if ($seen.ContainsKey($aliasKey)) {
            continue
        }

        $seen[$aliasKey] = $true
        $out.Add([pscustomobject]@{
            alias = $entry.alias
            rel_path = (Get-RelativePathForRegistry $FinalPath $exePath)
        })
    }

    foreach ($relDir in @($Resolved.env_add_path)) {
        $dirPath = Join-ManifestPath $FinalPath $relDir
        if (-not (Test-Path -LiteralPath $dirPath -PathType Container)) {
            continue
        }

        $entries = Get-ChildItem -LiteralPath $dirPath -File | Sort-Object FullName
        foreach ($file in $entries) {
            $ext = $file.Extension.ToLowerInvariant()
            if ($ext -notin @('.exe', '.cmd', '.bat')) {
                continue
            }

            $alias = $file.BaseName
            $aliasKey = $alias.ToLowerInvariant()
            if ($seen.ContainsKey($aliasKey)) {
                continue
            }

            $seen[$aliasKey] = $true
            $out.Add([pscustomobject]@{
                alias = $alias
                rel_path = (Get-RelativePathForRegistry $FinalPath $file.FullName)
            })
        }
    }

    return $out.ToArray()
}

function Get-NowIsoUtc() {
    return (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
}

function Load-InstalledRecords([string]$InstalledJsonPath) {
    $records = [ordered]@{}
    if (-not (Test-Path -LiteralPath $InstalledJsonPath)) {
        return $records
    }

    $doc = Read-JsonFile $InstalledJsonPath
    if (-not (Test-HasProperty $doc 'components')) {
        return $records
    }

    foreach ($prop in $doc.components.PSObject.Properties) {
        $bins = New-Object System.Collections.Generic.List[object]
        foreach ($pair in @($prop.Value.bins)) {
            $parts = @($pair)
            if ($parts.Count -ne 2) {
                continue
            }

            $bins.Add([pscustomobject]@{
                alias = [string]$parts[0]
                rel_path = [string]$parts[1]
            })
        }

        $records[$prop.Name] = [pscustomobject]@{
            version = [string]$prop.Value.version
            source = [string]$prop.Value.source
            url = [string]$prop.Value.url
            hash = [string]$prop.Value.hash
            toolchain_dir = [string]$prop.Value.toolchain_dir
            bins = $bins.ToArray()
            architecture = [string]$prop.Value.architecture
            installed_at = [string]$prop.Value.installed_at
        }
    }

    return $records
}

function Prune-StaleInstalledRecords($Records, $Paths) {
    $pruned = 0
    foreach ($name in @($Records.Keys)) {
        $record = $Records[$name]
        if ([string]::IsNullOrWhiteSpace($record.toolchain_dir)) {
            $Records.Remove($name)
            ++$pruned
            Write-Note ('removed stale registry record with empty toolchain_dir: ' + $name)
            continue
        }

        $toolchainPath = Join-Path $Paths.toolchains $record.toolchain_dir
        if (-not (Test-Path -LiteralPath $toolchainPath -PathType Container)) {
            $Records.Remove($name)
            ++$pruned
            Write-Note ('removed stale registry record: ' + $name + ' -> ' + $toolchainPath)
        }
    }

    return $pruned
}

function Save-InstalledRecords($Records, [string]$InstalledJsonPath) {
    $components = [ordered]@{}
    foreach ($name in ($Records.Keys | Sort-Object)) {
        $record = $Records[$name]
        $bins = New-Object System.Collections.Generic.List[object]
        foreach ($pair in @($record.bins)) {
            $bins.Add(@($pair.alias, $pair.rel_path))
        }

        $components[$name] = [ordered]@{
            version = $record.version
            source = $record.source
            url = $record.url
            hash = $record.hash
            store_keys = @()
            toolchain_dir = $record.toolchain_dir
            bins = $bins.ToArray()
            architecture = $record.architecture
            installed_at = $record.installed_at
        }
    }

    $doc = [ordered]@{
        schema = 1
        components = $components
    }

    $tmp = ($InstalledJsonPath + '.tmp')
    $json = ($doc | ConvertTo-Json -Depth 8)
    Set-Content -LiteralPath $tmp -Value $json -Encoding UTF8
    Move-Item -LiteralPath $tmp -Destination $InstalledJsonPath -Force
}

function New-InstalledRecord($Resolved, [string]$FinalPath, [string]$ToolchainDirName) {
    return [pscustomobject]@{
        name = $Resolved.name
        version = $Resolved.version
        source = 'seed'
        url = $Resolved.url
        hash = $Resolved.hash
        toolchain_dir = $ToolchainDirName
        bins = (Get-ComponentBins $Resolved $FinalPath)
        architecture = $Resolved.architecture
        installed_at = (Get-NowIsoUtc)
    }
}

function Write-EmscriptenConfig($Paths, [string]$FinalPath, $InstalledRecords) {
    $configPath = Join-Path $Paths.config 'emscripten\config'
    Ensure-Dir ([IO.Path]::GetDirectoryName($configPath))

    $nodePath = 'node'
    if ($InstalledRecords.Contains('node')) {
        $nodeRecord = $InstalledRecords['node']
        $nodeRoot = Join-Path $Paths.toolchains $nodeRecord.toolchain_dir
        $candidate = Join-Path $nodeRoot 'node.exe'
        if (Test-Path -LiteralPath $candidate) {
            $nodePath = $candidate.Replace('\', '/')
        }
    }

    $llvmRoot = (Join-Path $FinalPath 'bin').Replace('\', '/')
    $binaryenRoot = $FinalPath.Replace('\', '/')
    $emscriptenRoot = (Join-Path $FinalPath 'emscripten').Replace('\', '/')
    $content = @(
        '# generated by bootstrap-dev.cmd',
        '# mirrors Luban''s emscripten install special-case',
        "LLVM_ROOT = '$llvmRoot'",
        "BINARYEN_ROOT = '$binaryenRoot'",
        "EMSCRIPTEN_ROOT = '$emscriptenRoot'",
        "NODE_JS = '$nodePath'",
        'COMPILER_ENGINE = NODE_JS',
        'JS_ENGINES = [NODE_JS]'
    ) -join "`r`n"
    Write-TextFileCrlf $configPath ($content + "`r`n")
    Write-Ok ('wrote emscripten config -> ' + $configPath)
}

function Ensure-VcpkgBootstrapped([string]$FinalPath) {
    $vcpkgExe = Join-Path $FinalPath 'vcpkg.exe'
    if (Test-Path -LiteralPath $vcpkgExe) {
        return
    }

    $bootstrap = Join-Path $FinalPath 'bootstrap-vcpkg.bat'
    if (-not (Test-Path -LiteralPath $bootstrap)) {
        throw ('bootstrap-vcpkg.bat missing from ' + $FinalPath)
    }

    Write-Step 'bootstrapping vcpkg.exe'
    $process = Start-Process -FilePath 'cmd.exe' `
                             -ArgumentList '/c', 'bootstrap-vcpkg.bat', '-disableMetrics' `
                             -WorkingDirectory $FinalPath `
                             -NoNewWindow `
                             -Wait `
                             -PassThru
    if ($process.ExitCode -ne 0) {
        throw ('bootstrap-vcpkg.bat exited ' + $process.ExitCode)
    }

    if (-not (Test-Path -LiteralPath $vcpkgExe)) {
        throw 'vcpkg bootstrap finished without producing vcpkg.exe'
    }

    Write-Ok 'vcpkg.exe ready'
}

function Install-ComponentFromSeed([string]$RepoRoot,
                                   [string]$Name,
                                   [string]$Arch,
                                   $Paths,
                                   $InstalledRecords,
                                   [System.Collections.Generic.List[string]]$SessionPathDirs,
                                   [hashtable]$SessionSeen) {
    $resolved = Resolve-Manifest $RepoRoot $Name $Arch
    $toolchainDirName = Get-ToolchainDirName $resolved
    $finalPath = Join-Path $Paths.toolchains $toolchainDirName

    if (Test-ToolchainReady $resolved $finalPath) {
        Write-Ok ($Name + ' ' + $resolved.version + ' already present')
    } else {
        $fallbackArchiveName = ($resolved.name + '-' + $resolved.version + '.zip')
        $archiveName = Get-ArchiveFileName $resolved.url $fallbackArchiveName
        $archivePath = Join-Path $Paths.downloads $archiveName
        Download-Archive $resolved $archivePath
        $finalPath = Install-ArchiveToToolchains $resolved $archivePath $Paths.toolchains
    }

    Add-ComponentSessionPaths $resolved $finalPath $SessionPathDirs $SessionSeen
    Refresh-SessionPath $SessionPathDirs $Paths.bin

    if ($Name -eq 'vcpkg') {
        Ensure-VcpkgBootstrapped $finalPath
    } elseif ($Name -eq 'emscripten') {
        Write-EmscriptenConfig $Paths $finalPath $InstalledRecords
    }

    return (New-InstalledRecord $resolved $finalPath $toolchainDirName)
}

function Set-SessionVcpkgEnv($Paths, [string]$ToolchainDirName) {
    $env:VCPKG_ROOT = Join-Path $Paths.toolchains $ToolchainDirName
    $env:VCPKG_DOWNLOADS = $Paths.vcpkg_downloads
    $env:VCPKG_DEFAULT_BINARY_CACHE = $Paths.vcpkg_archives
    $env:X_VCPKG_REGISTRIES_CACHE = $Paths.vcpkg_registries
}

function Invoke-Checked([string]$FilePath, [string[]]$ArgumentList, [string]$WorkingDirectory) {
    $displayArgs = if ($ArgumentList.Count) { ' ' + ($ArgumentList -join ' ') } else { '' }
    Write-Step ($FilePath + $displayArgs)
    $params = @{
        FilePath = $FilePath
        WorkingDirectory = $WorkingDirectory
        NoNewWindow = $true
        Wait = $true
        PassThru = $true
    }
    if ($ArgumentList.Count -gt 0) {
        $params.ArgumentList = $ArgumentList
    }
    $process = Start-Process @params
    if ($process.ExitCode -ne 0) {
        throw ($FilePath + ' exited ' + $process.ExitCode)
    }
}

function Install-LocalLuban([string]$RepoRoot, [string]$InstallDir) {
    Ensure-Dir $InstallDir

    $releaseDir = Join-Path $RepoRoot 'build\release'
    $builtLuban = Join-Path $releaseDir 'luban.exe'
    $builtShim = Join-Path $releaseDir 'luban-shim.exe'
    if (-not (Test-Path -LiteralPath $builtLuban)) {
        throw ('build output missing: ' + $builtLuban)
    }
    if (-not (Test-Path -LiteralPath $builtShim)) {
        throw ('build output missing: ' + $builtShim)
    }

    Copy-Item -LiteralPath $builtLuban -Destination (Join-Path $InstallDir 'luban.exe') -Force
    Copy-Item -LiteralPath $builtShim -Destination (Join-Path $InstallDir 'luban-shim.exe') -Force
    Write-Ok ('installed local luban binaries -> ' + $InstallDir)
}

function Load-ShimTable([string]$TablePath) {
    $table = [ordered]@{}
    if (-not (Test-Path -LiteralPath $TablePath)) {
        return $table
    }

    try {
        $doc = Read-JsonFile $TablePath
    } catch {
        return $table
    }

    foreach ($prop in $doc.PSObject.Properties) {
        $table[$prop.Name] = [string]$prop.Value
    }

    return $table
}

function Write-ShimTable($AliasToExe, [string]$TablePath) {
    $doc = [ordered]@{}
    foreach ($alias in ($AliasToExe.Keys | Sort-Object)) {
        $doc[$alias] = $AliasToExe[$alias]
    }

    $tmp = ($TablePath + '.tmp')
    $json = ($doc | ConvertTo-Json -Depth 4)
    Set-Content -LiteralPath $tmp -Value $json -Encoding UTF8
    Move-Item -LiteralPath $tmp -Destination $TablePath -Force
}

function Write-TextFileCrlf([string]$PathText, [string]$Content) {
    $normalized = $Content -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText($PathText, $normalized, [Text.UTF8Encoding]::new($false))
}

function Quote-CmdArg([string]$Arg) {
    if ([string]::IsNullOrEmpty($Arg)) {
        return '""'
    }

    if ($Arg.IndexOfAny([char[]]" `t`"&|<>") -lt 0) {
        return $Arg
    }

    return '"' + ($Arg -replace '"', '\"') + '"'
}

function Write-CmdShim([string]$Alias, [string]$ExePath, [string[]]$PrefixArgs, [string]$BinDir) {
    $cmdPath = Join-Path $BinDir ($Alias + '.cmd')
    $prefix = ''
    if ($PrefixArgs) {
        foreach ($arg in $PrefixArgs) {
            $prefix += (' ' + (Quote-CmdArg $arg))
        }
    }

    $content = '@echo off' + "`r`n" + '"' + $ExePath + '"' + $prefix + ' %*' + "`r`n"
    Write-TextFileCrlf $cmdPath $content
}

function Ensure-ShimTemplateInBin([string]$SourceShimExe, [string]$BinDir) {
    $target = Join-Path $BinDir 'luban-shim.exe'
    Copy-Item -LiteralPath $SourceShimExe -Destination $target -Force
    return $target
}

function Write-ExeShim([string]$Alias, [string]$TemplateExe, [string]$BinDir) {
    $dst = Join-Path $BinDir ($Alias + '.exe')
    if (Test-Path -LiteralPath $dst) {
        Remove-Item -LiteralPath $dst -Force
    }

    try {
        New-Item -ItemType HardLink -Path $dst -Target $TemplateExe -Force | Out-Null
    } catch {
        Copy-Item -LiteralPath $TemplateExe -Destination $dst -Force
    }
}

function Remove-ShimFiles([string]$Alias, [string]$BinDir) {
    foreach ($suffix in @('.cmd', '.ps1', '', '.exe')) {
        $path = Join-Path $BinDir ($Alias + $suffix)
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        }
    }
}

function Generate-LubanShims($Records, $Paths, [string]$ShimTemplateSource) {
    $binDir = $Paths.bin
    Ensure-Dir $binDir

    $tablePath = Join-Path $binDir '.shim-table.json'
    $oldTable = Load-ShimTable $tablePath
    $templateInBin = Ensure-ShimTemplateInBin $ShimTemplateSource $binDir
    $aliasMap = [ordered]@{}

    foreach ($componentName in ($Records.Keys | Sort-Object)) {
        $record = $Records[$componentName]
        $root = Join-Path $Paths.toolchains $record.toolchain_dir
        foreach ($bin in @($record.bins)) {
            $exePath = Join-ManifestPath $root $bin.rel_path
            if (-not (Test-Path -LiteralPath $exePath)) {
                Write-Note ('skipping shim for missing target: ' + $exePath)
                continue
            }

            Write-CmdShim $bin.alias $exePath @() $binDir
            Write-ExeShim $bin.alias $templateInBin $binDir
            $aliasMap[$bin.alias] = $exePath
        }
    }

    foreach ($oldAlias in $oldTable.Keys) {
        if (-not $aliasMap.Contains($oldAlias)) {
            Remove-ShimFiles $oldAlias $binDir
        }
    }

    Write-ShimTable $aliasMap $tablePath
    Write-Ok ('wrote shims -> ' + $binDir)
}

function Normalize-PathForCompare([string]$PathText) {
    $full = [IO.Path]::GetFullPath($PathText).Replace('/', '\')
    while ($full.Length -gt 3 -and $full.EndsWith('\')) {
        $full = $full.Substring(0, $full.Length - 1)
    }
    return $full.ToLowerInvariant()
}

function Broadcast-EnvironmentChange() {
    if (-not ('LubanBootstrap.NativeMethods' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace LubanBootstrap {
    public static class NativeMethods {
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr SendMessageTimeout(
            IntPtr hWnd,
            uint Msg,
            IntPtr wParam,
            string lParam,
            uint fuFlags,
            uint uTimeout,
            out IntPtr lpdwResult);
    }
}
'@
    }

    $HWND_BROADCAST = [IntPtr]0xffff
    $WM_SETTINGCHANGE = 0x001A
    $SMTO_ABORTIFHUNG = 0x0002
    $result = [IntPtr]::Zero
    [void][LubanBootstrap.NativeMethods]::SendMessageTimeout(
        $HWND_BROADCAST,
        $WM_SETTINGCHANGE,
        [IntPtr]::Zero,
        'Environment',
        $SMTO_ABORTIFHUNG,
        5000,
        [ref]$result
    )
}

function Get-UserEnvironmentKey() {
    return [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment')
}

function Add-ToUserPath([string]$DirPath) {
    $key = Get-UserEnvironmentKey
    try {
        $currentValue = [string]($key.GetValue('PATH', '', 'DoNotExpandEnvironmentNames'))
        $kind = try { $key.GetValueKind('PATH') } catch { [Microsoft.Win32.RegistryValueKind]::ExpandString }
        $target = Normalize-PathForCompare $DirPath
        $parts = New-Object System.Collections.Generic.List[string]

        if (-not [string]::IsNullOrWhiteSpace($currentValue)) {
            foreach ($part in ($currentValue -split ';')) {
                if ([string]::IsNullOrWhiteSpace($part)) {
                    continue
                }
                $parts.Add($part)
            }
        }

        foreach ($part in @($parts)) {
            if ((Normalize-PathForCompare $part) -eq $target) {
                return $false
            }
        }

        $newParts = @($DirPath) + @($parts)
        if ($kind -ne [Microsoft.Win32.RegistryValueKind]::String -and
            $kind -ne [Microsoft.Win32.RegistryValueKind]::ExpandString) {
            $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
        }
        $key.SetValue('PATH', ($newParts -join ';'), $kind)
        Broadcast-EnvironmentChange
        return $true
    } finally {
        $key.Close()
    }
}

function Set-UserEnvVar([string]$Name, [string]$Value) {
    $key = Get-UserEnvironmentKey
    try {
        $key.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::String)
        Broadcast-EnvironmentChange
    } finally {
        $key.Close()
    }
}

function Register-LubanUserEnvironment($Paths, [string]$InstallDir, [string]$VcpkgRoot) {
    if (Add-ToUserPath $Paths.bin) {
        Write-Ok ('added ' + $Paths.bin + ' to HKCU PATH')
    } else {
        Write-Note ($Paths.bin + ' already on HKCU PATH')
    }

    if (Add-ToUserPath $InstallDir) {
        Write-Ok ('added ' + $InstallDir + ' to HKCU PATH')
    } else {
        Write-Note ($InstallDir + ' already on HKCU PATH')
    }

    Set-UserEnvVar 'VCPKG_ROOT' $VcpkgRoot
    Write-Ok ('set HKCU VCPKG_ROOT = ' + $VcpkgRoot)
}

function Show-Usage() {
    @'
bootstrap-dev.cmd

Bootstraps the Luban development toolchain directly from this source tree.
Use it when the published luban.exe release is broken and you need a local
compiler/cmake/ninja/git/vcpkg stack to rebuild Luban from source.

Behavior:
  - reads manifests_seed/*.json for toolchain URLs and SHA256 hashes
  - uses selection.json for opt-in extras, but always installs the rescue baseline
    (llvm-mingw, cmake, ninja, mingit, vcpkg)
  - installs toolchains into the same XDG/LUBAN_PREFIX layout as Luban
  - builds build\release\luban.exe and build\release\luban-shim.exe
  - copies those binaries into ~/.local/bin (or $env:LUBAN_INSTALL_DIR)
  - writes <data>/bin shims + .shim-table.json itself
  - registers ~/.local/bin + <data>/bin on HKCU PATH and sets VCPKG_ROOT

Environment overrides:
  LUBAN_PREFIX
  XDG_DATA_HOME / XDG_CACHE_HOME / XDG_STATE_HOME / XDG_CONFIG_HOME
  LUBAN_INSTALL_DIR
'@ | Write-Host
}

$helpWanted = $false
if ($env:BOOTSTRAP_DEV_ARGS) {
    $helpWanted = ($env:BOOTSTRAP_DEV_ARGS -match '(^|[ \t])(--help|-h|/\?)([ \t]|$)')
}

if ($helpWanted) {
    Show-Usage
    return
}

$script:OriginalPath = $env:PATH
$repoRoot = [IO.Path]::GetFullPath($env:BOOTSTRAP_DEV_ROOT)
$installDir = Get-LubanInstallDir
$paths = Get-LubanPaths

if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt'))) {
    throw ('bootstrap-dev.cmd must live in the Luban repo root: ' + $repoRoot)
}

Write-Step ('repo root: ' + $repoRoot)
Write-Step ('install dir: ' + $installDir)
Write-Step ('data dir: ' + $paths.data)

Ensure-Dir $installDir
Ensure-LubanDirs $paths
Copy-SelectionSeedIfMissing $repoRoot $paths

$sessionPathDirs = New-Object System.Collections.Generic.List[string]
$sessionSeen = @{}
$installedRecords = Load-InstalledRecords $paths.installed_json
Prune-StaleInstalledRecords $installedRecords $paths | Out-Null

Set-Location $repoRoot

$arch = 'x86_64'
$bootstrapBaseline = @('llvm-mingw', 'cmake', 'ninja', 'mingit', 'vcpkg')
$selectionEnabled = Get-SelectionEnabledNames $paths.selection_json
$requestedComponents = New-Object System.Collections.Generic.List[string]
foreach ($name in $bootstrapBaseline) {
    $requestedComponents.Add($name)
}
foreach ($name in $selectionEnabled) {
    if (-not $requestedComponents.Contains($name)) {
        $requestedComponents.Add($name)
    }
}
$componentOrder = Expand-ComponentDepends $repoRoot $requestedComponents.ToArray() $arch
foreach ($componentName in $componentOrder) {
    $record = Install-ComponentFromSeed $repoRoot $componentName $arch $paths $installedRecords $sessionPathDirs $sessionSeen
    $installedRecords[$record.name] = $record
    if ($record.name -eq 'vcpkg') {
        Set-SessionVcpkgEnv $paths $record.toolchain_dir
    }
}

Save-InstalledRecords $installedRecords $paths.installed_json
Write-Ok ('wrote ' + $paths.installed_json)

Invoke-Checked 'cmake' @('--preset', 'release') $repoRoot
Invoke-Checked 'cmake' @('--build', '--preset', 'release') $repoRoot
Invoke-Checked 'cmake' @('--build', '--preset', 'release', '--target', 'luban-tests') $repoRoot
Invoke-Checked (Join-Path $repoRoot 'build\release\luban-tests.exe') @() $repoRoot
Invoke-Checked (Join-Path $repoRoot 'build\release\luban.exe') @('--version') $repoRoot

Install-LocalLuban $repoRoot $installDir

$localLuban = Join-Path $installDir 'luban.exe'
$localShim = Join-Path $installDir 'luban-shim.exe'
Generate-LubanShims $installedRecords $paths $localShim
$vcpkgRoot = Join-Path $paths.toolchains $installedRecords['vcpkg'].toolchain_dir
Register-LubanUserEnvironment $paths $installDir $vcpkgRoot
Invoke-Checked $localLuban @('doctor') $repoRoot

Write-Host ''
Write-Host 'bootstrap-dev complete'
Write-Note 'Open a new shell so HKCU PATH and VCPKG_ROOT are visible there.'
