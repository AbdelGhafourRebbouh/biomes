# Biomes 1.0.0-beta for Windows

Extract the whole ZIP to a dedicated folder, such as
`%LOCALAPPDATA%\Programs\biomes`, then run `Biomes.exe`. Keep the loader DLL and
all bundled assets beside the executable. No administrator elevation is needed
for Biomes. Closing its dashboard leaves the background engine in the system
tray; use **Exit Biomes** before replacing application files.

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
its payload, and produces `dist/BiomesSetup-v1.0.0-beta.exe` plus its SHA-256 file.

Setup defaults to `%LOCALAPPDATA%\Programs\Biomes`, creates a Start Menu shortcut,
and offers an optional Desktop shortcut. A missing WebView2 Runtime is installed
with the Microsoft bootstrapper before app files are copied; internet is needed.
The per-user Windows Installed Apps entry uninstalls binaries and shortcuts while
preserving `%LOCALAPPDATA%\biomes`. The installer is currently unsigned.
