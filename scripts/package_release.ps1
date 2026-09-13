[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build-stability',
    [switch]$SkipBuild,
    [string]$BootstrapperPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $root $BuildDirectory
}
if (-not $SkipBuild) {
    & cmake -S $root -B $BuildDirectory -DBIOMES_BUILD_TESTS=ON
    if ($LASTEXITCODE) { throw 'Release configure failed.' }
    & cmake --build $BuildDirectory --config Release --parallel 4
    if ($LASTEXITCODE) { throw 'Release build failed.' }
    & ctest --test-dir $BuildDirectory -C Release --output-on-failure --no-tests=error
    if ($LASTEXITCODE) { throw 'Regression tests failed.' }
}
$binary = Join-Path $BuildDirectory 'Release/Biomes.exe'
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($binary).ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+(-[a-zA-Z0-9.-]+)?$') { throw 'Invalid executable version metadata.' }
# Read the PE machine and subsystem rather than guessing the build architecture.
$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($binary))
try {
    $reader.BaseStream.Position = 0x3c
    $peOffset = $reader.ReadInt32()
    $reader.BaseStream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x4550) { throw 'Invalid PE executable.' }
    $machine = $reader.ReadUInt16()
    $architecture = switch ($machine) { 0x8664 { 'x64' } 0x14c { 'x86' } 0xaa64 { 'arm64' } default { throw 'Unsupported PE architecture.' } }
    $reader.BaseStream.Position = $peOffset + 24 + 68
    if ($reader.ReadUInt16() -ne 2) { throw 'Release must use the Windows GUI subsystem.' }
} finally { $reader.Dispose() }
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$stage = Join-Path $dist ('stage-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Copy-Item -LiteralPath $binary -Destination $stage
    Copy-Item -LiteralPath (Join-Path $BuildDirectory 'Release/WebView2Loader.dll') -Destination $stage
    # Only shipped source assets: never copy the entire build output or user profile.
    $files = @('index.html', 'app.js', 'save-dialog.js', 'newsletter.js', 'styles.css',
        'launch-panel.html', 'launch-panel.css', 'launch-panel.js', 'onboarding.css', 'onboarding.js')
    foreach ($file in $files) { Copy-Item -LiteralPath (Join-Path $BuildDirectory "Release/$file") -Destination $stage }
    foreach ($directory in @('assets', 'logo', 'images', 'cardsimages')) {
        # Enumerate the shipped source inventory, but take the tested build copies.
        # Old build folders can contain obsolete assets or pre-isolation user files.
        $sourceDirectory = Join-Path $root "frontend/$directory"
        foreach ($asset in Get-ChildItem -LiteralPath $sourceDirectory -File -Recurse) {
            $relative = $asset.FullName.Substring($sourceDirectory.Length + 1)
            $destination = Join-Path $stage "$directory/$relative"
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $BuildDirectory "Release/$directory/$relative") -Destination $destination
        }
    }
    foreach ($file in @('LICENSE', 'README.md', 'RELEASE.md')) {
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination $stage
    }
    $prerequisites = Join-Path $stage 'prerequisites'
    New-Item -ItemType Directory -Path $prerequisites | Out-Null
    $bootstrapper = Join-Path $prerequisites 'MicrosoftEdgeWebview2Setup.exe'
    if ($BootstrapperPath) {
        Copy-Item -LiteralPath $BootstrapperPath -Destination $bootstrapper
    } else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' -OutFile $bootstrapper
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $bootstrapper
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw 'Bootstrapper must have a valid Microsoft Corporation Authenticode signature.'
    }
    $hashes = Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
    }
    $hashes | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ASCII
    $archive = Join-Path $dist "Biomes-$version-windows-$architecture.zip"
    $temporaryArchive = Join-Path $dist ('package-' + [Guid]::NewGuid().ToString('N') + '.zip')
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $temporaryArchive -CompressionLevel Optimal
    Move-Item -LiteralPath $temporaryArchive -Destination $archive -Force
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$archiveHash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -Encoding ASCII
    Write-Output "Created $archive"
} finally {
    # Validate the exact generated directory before recursive removal.
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $resolvedDist = [IO.Path]::GetFullPath($dist).TrimEnd('\') + '\'
    if (-not $resolvedStage.StartsWith($resolvedDist, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedStage) -notmatch '^stage-[0-9a-f]{32}$') {
        throw 'Refusing unsafe staging cleanup.'
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
