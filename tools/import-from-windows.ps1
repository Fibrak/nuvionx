<#
.SYNOPSIS
  Exports your Nuvio for Windows setup (addons, library + Trakt watchlist, watch
  progress, watched history) and imports it into NuvioNX on the Switch.

.EXAMPLE
  # Send straight to the Switch over Wi-Fi (NuvioNX must be open on the Switch).
  # The IP is read from switchip.txt in this folder.
  powershell -ExecutionPolicy Bypass -File import-from-windows.ps1

.EXAMPLE
  # Or give the IP explicitly (overrides switchip.txt)
  powershell -ExecutionPolicy Bypass -File import-from-windows.ps1 -SwitchIp 192.168.1.50

.EXAMPLE
  # Or write nuvio-import.json, then copy it to sdmc:/switch/NuvioNX/ on the SD card
  powershell -ExecutionPolicy Bypass -File import-from-windows.ps1 -OutFile .\nuvio-import.json
#>
param(
    [string]$SwitchIp = "",
    [int]$Port = 8080,
    [string]$OutFile = "",
    [int]$Profile = 0,
    [string]$NuvioDir = "$env:APPDATA\Nuvio"
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $NuvioDir)) { throw "Nuvio for Windows data not found at $NuvioDir" }

# Switch IP: -SwitchIp wins; otherwise read switchip.txt next to this script
# (unless -OutFile was given, which means "write a file for the SD card").
$ipFile = Join-Path $PSScriptRoot 'switchip.txt'
if (-not $SwitchIp -and -not $OutFile -and (Test-Path $ipFile)) {
    $SwitchIp = ((Get-Content $ipFile -Raw) -replace '#.*', '').Trim()
}
if ($SwitchIp) {
    $SwitchIp = $SwitchIp -replace '^https?://', '' -replace '[:/].*$', ''
    if ($SwitchIp -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
        throw "Invalid Switch IP '$SwitchIp' (check $ipFile). Example: 192.168.1.167"
    }
    Write-Host "Switch IP: $SwitchIp"
}

# Java .properties value unescape (\uXXXX, \t, \n, \:, \\ ...)
function ConvertFrom-PropValue([string]$s) {
    return [regex]::Replace($s, '\\(u[0-9a-fA-F]{4}|.)', {
        param($m)
        $x = $m.Groups[1].Value
        if ($x.Length -eq 5) { return [string][char][Convert]::ToInt32($x.Substring(1), 16) }
        switch -CaseSensitive ($x) { 't' { "`t" } 'n' { "`n" } 'r' { "`r" } 'f' { "`f" } default { $x } }
    })
}

function Read-Props([string]$name) {
    $map = @{}
    $file = Join-Path $NuvioDir "$name.properties"
    if (-not (Test-Path $file)) { return $map }
    foreach ($line in [IO.File]::ReadAllLines($file)) {
        if ($line.StartsWith('#') -or $line.StartsWith('!')) { continue }
        $i = $line.IndexOf('=')
        if ($i -lt 0) { continue }
        $map[(ConvertFrom-PropValue $line.Substring(0, $i))] = ConvertFrom-PropValue $line.Substring($i + 1)
    }
    return $map
}

function Get-Json($map, $key) {
    if (-not $map.ContainsKey($key)) { return $null }
    return $map[$key] | ConvertFrom-Json
}

# ---- active profile
if ($Profile -le 0) {
    $prof = Get-Json (Read-Props 'nuvio_profiles') 'profiles'
    $Profile = if ($prof -and $prof.activeProfileIndex) { [int]$prof.activeProfileIndex } else { 1 }
}
Write-Host "Using Nuvio profile $Profile"

# ---- addons (order + enabled state)
$addonProps = Read-Props 'nuvio_addons'
$urls = Get-Json $addonProps "installed_addon_urls_$Profile"
$states = Get-Json $addonProps "addon_enabled_states_$Profile"
$addons = @()
foreach ($u in @($urls)) {
    if (-not $u) { continue }
    $enabled = $true
    if ($states -and $states.PSObject.Properties[$u]) { $enabled = [bool]$states.$u }
    $addons += [ordered]@{ url = $u; enabled = $enabled }
}

# ---- library
$lib = Get-Json (Read-Props 'nuvio_library') "library_$Profile"
$library = @()
foreach ($it in @($lib.items)) {
    if (-not $it.id) { continue }
    $library += [ordered]@{
        id = $it.id; type = $it.type; name = $it.name; poster = $it.poster
        background = $it.banner; logo = $it.logo; description = $it.description
        releaseInfo = $it.releaseInfo; imdbRating = $it.imdbRating
        genres = @($it.genres | Where-Object { $_ })
    }
}

# ---- Trakt watchlist / lists cached by Nuvio for Windows -> merged into the library
# (Trakt sign-in tokens are NOT exported: they only work with NuvioTV's own private
#  Trakt app. Connect Trakt on the Switch with your own Trakt app instead.)
$traktAdded = 0
$traktLib = Get-Json (Read-Props 'nuvio_trakt_library') "trakt_library_$Profile"
if ($traktLib -and $traktLib.entriesByList) {
    $have = @{}
    foreach ($l in $library) { $have[$l.id] = $true }
    foreach ($list in $traktLib.entriesByList.PSObject.Properties) {
        foreach ($it in @($list.Value)) {
            $id = if ($it.imdbId) { $it.imdbId } else { $it.id }
            if (-not $id -or $have.ContainsKey($id)) { continue }
            $have[$id] = $true
            $library += [ordered]@{
                id = $id; type = $it.type; name = $it.name; poster = $it.poster
                background = $it.banner; logo = $it.logo; description = $it.description
                releaseInfo = $it.releaseInfo; imdbRating = $it.imdbRating
                genres = @($it.genres | Where-Object { $_ })
            }
            $traktAdded++
        }
    }
}

# ---- watch progress
$wp = Get-Json (Read-Props 'nuvio_watch_progress') "watch_progress_$Profile"
$progress = @()
foreach ($e in @($wp.entries)) {
    if (-not $e.videoId -or -not $e.parentMetaId) { continue }
    $dur = [double]$e.durationMs / 1000.0
    $pos = [double]$e.lastPositionMs / 1000.0
    if ($e.isCompleted -and $dur -gt 0) { $pos = $dur }
    $progress += [ordered]@{
        videoId = $e.videoId; metaId = $e.parentMetaId
        type = $(if ($e.parentMetaType) { $e.parentMetaType } else { $e.contentType })
        name = $e.title; poster = $e.poster; background = $e.background; logo = $e.logo
        epTitle = $e.episodeTitle
        season = $(if ($e.seasonNumber) { [int]$e.seasonNumber } else { 0 })
        episode = $(if ($e.episodeNumber) { [int]$e.episodeNumber } else { 0 })
        pos = $pos; dur = $dur
        updated = [long]([double]$e.lastUpdatedEpochMs / 1000)
    }
}

# ---- watched history (fully watched movies / episodes)
$w = Get-Json (Read-Props 'nuvio_watched') "watched_$Profile"
$watched = @()
foreach ($it in @($w.items)) {
    if (-not $it.id) { continue }
    $vid = $it.id
    if ($it.season -or $it.episode) { $vid = "$($it.id):$([int]$it.season):$([int]$it.episode)" }
    $watched += [ordered]@{
        videoId = $vid; metaId = $it.id; type = $it.type; name = $it.name
        season = [int]$it.season; episode = [int]$it.episode
        updated = [long]([double]$it.markedAtEpochMs / 1000)
    }
}

$payload = [ordered]@{
    source = 'nuvio-windows'; version = 1; profile = $Profile
    addons = $addons; library = $library; progress = $progress; watched = $watched
}
$json = $payload | ConvertTo-Json -Depth 6 -Compress
$bytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes($json)

Write-Host ("Found {0} addons, {1} library items ({2} from your Trakt watchlist), {3} progress entries, {4} watched items" -f `
    $addons.Count, $library.Count, $traktAdded, $progress.Count, $watched.Count)
if (Test-Path (Join-Path $NuvioDir 'nuvio_trakt_auth.properties')) {
    Write-Host "Note: Trakt sign-in isn't transferable. On the Switch: Account tab > Trakt > Connect Trakt."
}

if ($SwitchIp) {
    $uri = "http://$($SwitchIp):$Port/import"
    Write-Host "Sending to NuvioNX at $uri ..."
    $resp = Invoke-WebRequest -Uri $uri -Method Post -Body $bytes -ContentType 'application/json' -UseBasicParsing -TimeoutSec 30
    Write-Host "Switch replied: $($resp.Content)"
} else {
    if (-not $OutFile) { $OutFile = Join-Path (Get-Location) 'nuvio-import.json' }
    [IO.File]::WriteAllBytes($OutFile, $bytes)
    Write-Host "Wrote $OutFile"
    Write-Host "Copy it to sdmc:/switch/NuvioNX/nuvio-import.json and start NuvioNX."
}
