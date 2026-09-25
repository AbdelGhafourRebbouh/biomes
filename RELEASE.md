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

`.github/workflows/release.yml` runs on pushes to `main`, or manually through Actions. It builds on Windows, runs all native regressions, packages the installer, and publishes only after those steps pass. `GITHUB_TOKEN` uses repository contents-write permission; no personal access token belongs in the app or repository.

Versions are `1.0.0-beta.<workflow run number>`. The run number is embedded in the executable and installer resources, and the updater refuses same-version or older builds. Keep the workflow identity/run counter continuous. Before changing the base version or reaching build 65535, update the version policy and feed parser together.

Each build has a separate versioned prerelease. A mutable `beta` prerelease holds `biomesSetup.exe`, its checksum, and `update.json`. Do not enable immutable assets for this rolling channel. The feed is uploaded last and points to the versioned installer, not the replaceable alias. A rerun cannot overwrite an already published version; start a new workflow-dispatch run if a published run needs to be superseded.

Permanent website/README URL (live after the first successful workflow):

https://github.com/AbdelGhafourRebbouh/biomes/releases/download/beta/biomesSetup.exe

The website download button needs this URL once; subsequent successful releases refresh its target automatically. The website source is managed separately from this repository. GitHub's `/releases/latest/download` shortcut does not select beta prereleases.

biomes checks the feed 20 seconds after startup and every six hours while running. A tray notification and menu offer Update and restart. Downloads use HTTPS, bounded sizes/timeouts, a pinned repository/versioned URL, and SHA-256 verification before execution. These checks protect transfer integrity; they are not a substitute for publisher code signing. The GitHub repository and Actions publishing permissions remain part of the update trust boundary.

Installers are staged under `%LOCALAPPDATA%\biomes\backups\updates\`. After the user confirms, the installer waits for the initiating process to finish normal session shutdown, installs into the current executable directory, and restarts biomes. A download/checksum/installer-launch failure leaves biomes open. If installation itself fails after shutdown, reopen biomes or run the downloaded installer manually. No forced termination of workspace apps is used.

Users of the original beta must download and install the first updater-enabled version manually. Running copies without an updater cannot discover the new feed. Upgrades preserve the existing installer AppId and isolated user data.

### First publication checklist

1. Commit and push the updater/workflow changes to `main`.
2. Check the Actions run, including native regressions and installer compilation. Local tests do not verify the hosted runner environment.
3. Confirm both the versioned release and `beta` assets exist; download the permanent URL and verify its checksum.
4. Install the updater-enabled build on a separate test account/PC with a saved biome.
5. Publish a second build, confirm the tray notification, choose Update and restart, and verify version progression and retained data.
6. Point the website button at the permanent URL and announce the one-time manual upgrade to existing beta users.

Do not announce unattended update delivery before this end-to-end hosted-release/upgrade check succeeds. No workflow run, commit, push, or public release is performed by local packaging scripts alone.
