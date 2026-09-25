$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('biomes-release-test-' + [guid]::NewGuid().ToString('N'))
$previousRepo = $env:GH_REPO
$global:releaseCalls = [Collections.Generic.List[string]]::new()
$global:feedBuild = 4
$global:failUpload = $false
# These mocks prohibit external publication/network calls during this test.
function global:Invoke-RestMethod {
    param($Uri,$Headers,$TimeoutSec)
    if ($global:feedBuild -eq -1) {
        $failure = [Exception]::new('Channel not created yet')
        $failure | Add-Member -NotePropertyName Response -NotePropertyValue ([pscustomobject]@{StatusCode=404})
        throw $failure
    }
    return @{build=$global:feedBuild}
}
function global:gh {
    $command = $args -join ' '
    $global:releaseCalls.Add($command)
    $global:LASTEXITCODE = 0
    if ($command -eq 'release view v1.0.0-beta.5 --json isDraft') { $global:LASTEXITCODE=1 }
    if ($global:feedBuild -eq -1 -and $command -eq 'release view beta') { $global:LASTEXITCODE=1 }
    if ($global:failUpload -and $command.StartsWith('release upload v')) { $global:LASTEXITCODE=1 }
}
function Require([bool]$value,[string]$message) { if (-not $value) { throw $message } }
try {
    New-Item -ItemType Directory -Path "$fixture/scripts","$fixture/dist" | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../scripts/publish_release.ps1') -Destination "$fixture/scripts/publish_release.ps1"
    foreach ($name in @('biomesSetup-v1.0.0-beta.5.exe','Biomes-1.0.0-beta.5-windows-x64.zip')) {
        $file = Join-Path "$fixture/dist" $name
        [IO.File]::WriteAllText($file,'isolated release fixture')
        $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $name" | Set-Content -LiteralPath "$file.sha256"
    }
    $env:GH_REPO = 'AbdelGhafourRebbouh/biomes'
    $parameters = @{Version='1.0.0-beta.5';BuildNumber=5;Commit=('a'*40)}
    & "$fixture/scripts/publish_release.ps1" @parameters
    $feed = Get-Content -LiteralPath "$fixture/dist/update.json" -Raw | ConvertFrom-Json
    Require ($feed.build -eq 5 -and $feed.version -eq '1.0.0-beta.5') 'Feed version mismatch'
    Require ($feed.url -like '*/v1.0.0-beta.5/biomesSetup-v1.0.0-beta.5.exe') 'Feed must reference a versioned installer'
    Require ($global:releaseCalls[$global:releaseCalls.Count-1] -like 'release upload beta *update.json*') 'Feed must be published last'
    Require ($global:releaseCalls.IndexOf('release edit v1.0.0-beta.5 --draft=false') -gt 0) 'Version must publish before channel'
    $global:releaseCalls.Clear(); $global:feedBuild=-1
    & "$fixture/scripts/publish_release.ps1" @parameters
    Require ([bool]($global:releaseCalls | Where-Object { $_ -like 'release create beta *' })) 'First publication must create missing channel'
    $global:releaseCalls.Clear(); $global:feedBuild=6
    $failed=$false; try { & "$fixture/scripts/publish_release.ps1" @parameters } catch { $failed=$true }
    Require ($failed -and $global:releaseCalls.Count -eq 0) 'An older build must not publish'
    $global:feedBuild=4; $global:failUpload=$true; $global:releaseCalls.Clear()
    $failed=$false; try { & "$fixture/scripts/publish_release.ps1" @parameters } catch { $failed=$true }
    Require ($failed -and -not ($global:releaseCalls | Where-Object { $_ -like 'release upload beta*' })) 'Failed version upload must preserve channel'
    $global:failUpload=$false; $global:releaseCalls.Clear()
    [IO.File]::AppendAllText("$fixture/dist/biomesSetup-v1.0.0-beta.5.exe",'tampered')
    $failed=$false; try { & "$fixture/scripts/publish_release.ps1" @parameters } catch { $failed=$true }
    Require ($failed -and $global:releaseCalls.Count -eq 0) 'Checksum failure must prevent publication'
    Write-Output 'PASS: release ordering, fixed channel URL, version rollback rejection and failed upload/checksum protection (mocked; no publication).'
} finally {
    $env:GH_REPO=$previousRepo
    Remove-Item Function:\gh,Function:\Invoke-RestMethod
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^biomes-release-test-[0-9a-f]{32}$') { throw 'Unsafe fixture cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
