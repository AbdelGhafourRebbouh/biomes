[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^1\.0\.0-beta\.\d+$')][string]$Version,
    [Parameter(Mandatory)][ValidateRange(1,65535)][int]$BuildNumber,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$Commit
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Version -ne "1.0.0-beta.$BuildNumber") { throw 'Version/build mismatch' }
if ($env:GH_REPO -ne 'AbdelGhafourRebbouh/biomes') { throw 'Only the official repository can publish the update channel.' }
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
# Prevent reruns of an older workflow from moving the channel backwards.
try {
    $current = Invoke-RestMethod -Uri 'https://github.com/AbdelGhafourRebbouh/biomes/releases/download/beta/update.json' -Headers @{'Cache-Control'='no-cache'} -TimeoutSec 30
    if ([int]$current.build -ge $BuildNumber) { throw 'This build does not advance the beta channel.' }
} catch {
    $response = $_.Exception.PSObject.Properties['Response']
    if (-not $response -or -not $response.Value -or [int]$response.Value.StatusCode -ne 404) { throw }
}
$installer = Join-Path $dist "biomesSetup-v$Version.exe"
$zip = Join-Path $dist "Biomes-$Version-windows-x64.zip"
foreach ($file in @($installer,$zip)) {
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    $expected = ((Get-Content -LiteralPath "$file.sha256" -Raw).Trim() -split '\s+')[0]
    if ($hash -ne $expected) { throw "Checksum mismatch: $file" }
}
$tag = "v$Version"
# Versioned releases are never overwritten. A retry can finish an existing draft.
$existing = & gh release view $tag --json isDraft 2>$null
if ($LASTEXITCODE -ne 0) {
    & gh release create $tag --target $Commit --title "biomes $Version" --draft --prerelease --generate-notes
    if ($LASTEXITCODE) { throw 'Could not create release draft.' }
} elseif (-not ($existing | ConvertFrom-Json).isDraft) {
    throw 'This version is already published. Start a new workflow run for a new version.'
}
& gh release upload $tag $installer "$installer.sha256" $zip "$zip.sha256" --clobber
if ($LASTEXITCODE) { throw 'Versioned upload failed.' }
& gh release edit $tag --draft=false
if ($LASTEXITCODE) { throw 'Could not publish verified version.' }

# Permanent website URL; the feed points to the immutable versioned installer.
$alias = Join-Path $dist 'biomesSetup.exe'
Copy-Item -LiteralPath $installer -Destination $alias -Force
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  biomesSetup.exe" | Set-Content -LiteralPath "$alias.sha256" -Encoding ascii
$feed = Join-Path $dist 'update.json'
@{ schemaVersion=1; channel='beta'; build=$BuildNumber; version=$Version;
   url="https://github.com/AbdelGhafourRebbouh/biomes/releases/download/$tag/biomesSetup-v$Version.exe";
   sha256=$hash; size=(Get-Item -LiteralPath $installer).Length } |
    ConvertTo-Json | Set-Content -LiteralPath $feed -Encoding utf8
& gh release view beta *> $null
if ($LASTEXITCODE -ne 0) {
    & gh release create beta --target $Commit --title 'biomes — current beta' --prerelease --notes 'Latest tested beta. Download biomesSetup.exe. Older versioned releases remain available.'
    if ($LASTEXITCODE) { throw 'Could not create beta channel.' }
}
& gh release upload beta $alias "$alias.sha256" --clobber
if ($LASTEXITCODE) { throw 'Could not refresh permanent installer URL.' }
# Publish the feed last: users never see a version whose installer is absent.
& gh release upload beta $feed --clobber
if ($LASTEXITCODE) { throw 'Could not refresh update feed.' }
Write-Output 'Published: https://github.com/AbdelGhafourRebbouh/biomes/releases/download/beta/biomesSetup.exe'
