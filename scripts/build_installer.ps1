[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build-stability',
    [string]$IsccPath,
    [string]$BootstrapperPath,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
if (-not $IsccPath) {
    $command = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($command) { $IsccPath = $command.Source }
    else {
        $candidates = @(
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
            (Join-Path $root 'dist/tools/InnoSetup/ISCC.exe'))
        $IsccPath = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    }
}
if (-not $IsccPath -or -not (Test-Path -LiteralPath $IsccPath -PathType Leaf)) {
    throw 'Install Inno Setup 6 or pass -IsccPath with the full path to ISCC.exe.'
}
& (Join-Path $PSScriptRoot 'package_release.ps1') -BuildDirectory $BuildDirectory -SkipBuild:$SkipBuild -BootstrapperPath $BootstrapperPath
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $BuildDirectory 'Release/Biomes.exe')).ProductVersion
$image = [IO.File]::ReadAllBytes((Join-Path $BuildDirectory 'Release/Biomes.exe'))
$peOffset = [BitConverter]::ToInt32($image, 0x3c)
if ([BitConverter]::ToUInt16($image, $peOffset + 4) -ne 0x8664) { throw 'Installer requires an x64 executable.' }
$dist = Join-Path $root 'dist'
# The installer currently targets the x64 release. Never silently package another architecture.
$archive = Join-Path $dist "Biomes-$version-windows-x64.zip"
if (-not (Test-Path -LiteralPath $archive)) { throw 'An x64 Release package is required.' }
$stage = Join-Path $dist ('installer-stage-' + [Guid]::NewGuid().ToString('N'))
try {
    Expand-Archive -LiteralPath $archive -DestinationPath $stage
    foreach ($line in Get-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt')) {
        $parts = $line -split '  ', 2
        $file = [IO.Path]::GetFullPath((Join-Path $stage $parts[1]))
        if (-not $file.StartsWith($stage + '\', [StringComparison]::OrdinalIgnoreCase) -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $parts[0]) {
            throw 'Release staging checksum failed.'
        }
    }
    & $IsccPath "/DPackageDir=$stage" "/DOutputDir=$dist" (Join-Path $root 'installer/BiomesSetup.iss')
    if ($LASTEXITCODE) { throw "Inno Setup compilation failed: $LASTEXITCODE" }
    $installer = Join-Path $dist "BiomesSetup-v$version.exe"
    if (-not (Test-Path -LiteralPath $installer)) { throw 'Installer output missing.' }
    $hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($installer))" | Set-Content -LiteralPath "$installer.sha256" -Encoding ASCII
    Write-Output "Created $installer"
} finally {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $resolvedDist = [IO.Path]::GetFullPath($dist).TrimEnd('\') + '\'
    if (-not $resolvedStage.StartsWith($resolvedDist, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedStage) -notmatch '^installer-stage-[0-9a-f]{32}$') {
        throw 'Refusing unsafe installer staging cleanup.'
    }
    if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
}
