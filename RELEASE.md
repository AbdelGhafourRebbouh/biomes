# biomes 1.0.0-beta for Windows

Extract the whole ZIP to a dedicated folder, such as
`%LOCALAPPDATA%\Programs\biomes`, then run `Biomes.exe`. Keep the loader DLL and
all bundled assets beside the executable. No administrator elevation is needed
for Biomes. Closing its dashboard leaves the background engine in the system
tray; use **Exit biomes** before replacing application files.

If Microsoft WebView2 Evergreen Runtime is missing, run
`prerequisites\MicrosoftEdgeWebview2Setup.exe` as your normal user, then launch
Biomes again. The signed Microsoft bootstrapper requires an internet connection
to download the runtime. It is not executed automatically by packaging.
Evergreen is a separately installed, shared Microsoft runtime with its own update
and storage locations. See Microsoft's [deployment documentation](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution).

Biomes stores personal settings, layouts, logs, images, and its WebView2 profile
under `%LOCALAPPDATA%\biomes\`. Upgrading or removing this extracted application
folder does not remove that data. Do not delete the personal-data directory to
upgrade. This ZIP does not install startup entries; use the app's startup setting.

The Release application uses the static MSVC runtime and needs no separate
Visual C++ Redistributable. This beta ZIP is not a signed installer.
`SHA256SUMS.txt` lists packaged file checksums; the adjacent `.zip.sha256` verifies
the downloaded archive.

## Building the distribution

From a Visual Studio developer PowerShell with CMake and the Windows SDK:

```powershell
.\scripts\package_release.ps1
```

The script configures `build-stability` with regression tests enabled, builds
Release, runs CTest, verifies the Microsoft bootstrapper signature, and creates
`dist\Biomes-1.0.0-beta-windows-<architecture>.zip`. `-SkipBuild` is for a Release
build already verified with CTest. `-BootstrapperPath` accepts a previously
downloaded Microsoft-signed bootstrapper and still checks its signature.

Only the release executable, loader, explicitly listed frontend assets, documents,
and bootstrapper are packaged. Debug symbols, tests, build configuration, and
personal data are excluded. The project license is included as `LICENSE`.

## Windows installer

Run `scripts/build_installer.ps1` with Inno Setup 6 installed (or pass
`-IsccPath`). It builds and tests Release, refreshes the allowlisted ZIP, checks
its payload, and produces `dist/biomesSetup-v1.0.0-beta.exe` plus its SHA-256 file.

Setup defaults to `%LOCALAPPDATA%\Programs\biomes`, creates a Start Menu shortcut,
and offers an optional Desktop shortcut. A missing WebView2 Runtime is installed
with the Microsoft bootstrapper before app files are copied; internet is needed.
The per-user Windows Installed Apps entry uninstalls binaries and shortcuts while
preserving `%LOCALAPPDATA%\biomes`. The installer is currently unsigned.

## Automated beta releases and in-app updates

Releases run only on pushed tags matching `v1.0.0-beta.N`. Pushing to `main`, including README changes, does not publish anything. The workflow builds and tests on Windows before packaging and publication, using repository contents-write permission.

Choose an unused N larger than every previously published build and the current `beta/update.json` build (1..65535). N comes from the tag, not the Actions run number. Existing updaters validate `1.0.0-beta.N` exactly; moving to `1.0.1-beta` or another base requires an updater migration first.

After committing and pushing the desired changes, publish deliberately (replace N with your chosen number):

```powershell
git tag v1.0.0-beta.N
git push origin v1.0.0-beta.N
```

Never move or reuse a release tag. Versioned assets are uploaded without `--clobber`; an existing release, including a draft, is rejected. If a run fails after creating a draft, inspect it and use a new higher tag rather than replacing assets. If publication succeeds but feed upload fails, repair the metadata separately or publish a higher version; rerunning will not overwrite the release.

Only `beta/update.json` remains mutable for compatibility with installed updaters. It is published last and points to a versioned installer. Historical rolling installers and their checksums are left untouched, preserving their remaining counters. The former `beta/biomesSetup.exe` link no longer delivers new builds. Do not delete old assets or releases if you want to retain their counts. Counts already lost through replacement cannot be recovered by this change. A badge summing all asset types includes mutable metadata downloads too; use installer-only statistics for durable installer totals.

Share this download page on the website, README, and social media:

https://github.com/AbdelGhafourRebbouh/biomes/releases

Users select the installer from the newest versioned beta. The private website is managed separately and needs its link changed once. GitHub's latest-release shortcut does not select prereleases.

biomes checks the feed 20 seconds after startup and every six hours while running. Users choose Update and restart. Downloads use HTTPS and SHA-256 verification and are staged under `%LOCALAPPDATA%\biomes\backups\updates\`. Normal shutdown, installer AppId, and isolated user data are unchanged. Original beta users without an updater still need one manual upgrade. The installer remains unsigned.

### Verification before announcing an update

1. Push the new tag and check every Actions step succeeds.
2. Confirm the versioned installer and checksum exist and the feed points to them.
3. Verify previous installers retain their asset IDs and download counts.
4. On a test account/PC, upgrade from an earlier updater-enabled release and confirm restart and saved layouts/settings.
5. Update old website/social download links to the releases page.

Local regression tests mock publication. They do not publish or verify a real hosted upgrade. No commit, tag, push, or release is created by editing these files.