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
# Existing releases/assets are never replaced, including incomplete drafts.
$existing = & gh release view $tag --json isDraft 2>$null
if ($LASTEXITCODE -ne 0) {
    & gh release create $tag --target $Commit --title "biomes $Version" --draft --prerelease --generate-notes
    if ($LASTEXITCODE) { throw 'Could not create release draft.' }
} else {
    throw 'This version already exists. Inspect any incomplete draft and publish a new version tag.'
}
& gh release upload $tag $installer "$installer.sha256" $zip "$zip.sha256"
if ($LASTEXITCODE) { throw 'Versioned upload failed.' }
& gh release edit $tag --draft=false
if ($LASTEXITCODE) { throw 'Could not publish verified version.' }

# Only discovery metadata is mutable. Never replace an installer or its counter.
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
$feed = Join-Path $dist 'update.json'
@{ schemaVersion=1; channel='beta'; build=$BuildNumber; version=$Version;
   url="https://github.com/AbdelGhafourRebbouh/biomes/releases/download/$tag/biomesSetup-v$Version.exe";
   sha256=$hash; size=(Get-Item -LiteralPath $installer).Length } |
    ConvertTo-Json | Set-Content -LiteralPath $feed -Encoding utf8
& gh release view beta *> $null
if ($LASTEXITCODE -ne 0) {
    & gh release create beta --target $Commit --title 'biomes update feed' --prerelease --notes 'Update discovery metadata. Download installers from the versioned releases.'
    if ($LASTEXITCODE) { throw 'Could not create beta channel.' }
}
# Publish the feed last: users never see a version whose installer is absent.
& gh release upload beta $feed --clobber
if ($LASTEXITCODE) { throw 'Could not refresh update feed.' }
Write-Output "Published: https://github.com/AbdelGhafourRebbouh/biomes/releases/tag/$tag"
