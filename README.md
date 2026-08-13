# Biomes

Lightweight Windows desktop workspace manager. Create named layouts ("Biomes"), snap apps into grid zones, then launch or close the whole workspace from a dashboard card or a global hotkey.

> **V1 scope:** Matrix Grid Overlay only (Method 1). The Hyprland-style interactive split canvas (Method 2) is intentionally deferred.

## What works in V1

1. **Create New Biome** — tracked clean slate (no Win+D) + multi-monitor grid overlay  
2. **Draw zones** — drag rectangles on the grid  
3. **Enter** — enter snap mode  
4. **Assign apps** — drag real windows onto zones (hover highlights the target zone)  
5. **Enter again** — return to the dashboard save dialog  
6. **Name / hotkey / cover** — save a dashboard card to `config/biomes.json`  
7. **Launch** — from the card or your hotkey (toggles open/close)  
8. **Close** — restores biome + minimized non-biome windows to previous positions  

Launch keeps already-open biome apps visible, minimizes only other windows, and on close biome apps return to their pre-biome size then minimize (other apps stay minimized). The Biomes dashboard minimizes to the taskbar while a biome is open — click it or press the hotkey again to close.

> **Note:** If zones look misaligned after upgrading, recreate the biome once — overlay and snap now both use the monitor work area (`rcWork`).

## Requirements

- Windows 10/11  
- [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (usually already installed with Edge)  
- CMake 3.20+  
- A C++17 compiler (MSVC recommended)

`WebView2Loader.dll` must sit next to the built `Biomes.exe` (CMake copies it automatically on build).

## Build

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Run:

```bash
.\build\Release\Biomes.exe
```

The build copies `index.html` and `WebView2Loader.dll` into the output folder. Saved Biomes live in:

```text
build/Release/config/biomes.json
```

## Hotkeys

Use modifier + key, for example:

- `CTRL+ALT+C`
- `CTRL+SHIFT+1`

Hotkeys require at least one modifier (`CTRL`, `ALT`, `SHIFT`, or `WIN`) plus a letter/number. The same hotkey **toggles** the Biome open/closed.

## Project layout

```text
src/main.cpp              IPC router, activate/close, hotkey wiring
src/ui/grid_overlay.cpp   Multi-monitor grid overlay (Method 1)
src/ui/webview_window.cpp WebView2 dashboard host
src/core/window_scaler.cpp Snap / launch / restore windows
src/core/app_launcher.cpp Store (AUMID) and Obsidian URI launch
src/core/json_manager.cpp Biome persistence (biomes.json v2)
src/core/monitor_manager.cpp Monitor rcWork enumeration
src/core/hotkey_manager.cpp Global hotkey parse + register
index.html                Dashboard UI (shipped with exe)
docs/ARCHITECTURE.md      Architecture reference
frontend/README.md        Deferred Vite/React UI (not used in V1)
```

See `docs/ARCHITECTURE.md` for the full module map and session model.

## Known limitations (V1)

- **UWP frame hosts** saved as `ApplicationFrameHost.exe` cannot be relaunched reliably — use the real Store app window (e.g. Spotify) so Biomes captures its AUMID.
- **Microsoft Store apps** must be assigned while open during create; Biomes activates them via AUMID, not the `WindowsApps` exe path.
- **Obsidian** requires the target vault to be open when you snap the zone; cold start uses `obsidian://open?vault=...` from the title / Obsidian config (never the version string, never the vault picker).
- **Multi-window apps** (Chrome, VS Code): each zone needs its own open window; Biomes will launch a new process window when one is missing.
- Hotkeys need a modifier + key (`CTRL+ALT+C`), not a bare letter.
- Closing a Biome restores window positions Biomes tracked during that session.

## Placement reliability checklist

Before a public share, verify:

**Basic:** app not running launches into zone; already-open app reuses window; close restores positions; unrelated apps restore; dashboard untouched.

**Multi-window (Chrome x2):** one zone takes one HWND; two zones never share; closing the snapped Chrome then reopening the biome launches a **new** window instead of stealing the other account; both Chromes restore when left open.

**Missing/slow:** bad path fails that zone only; UWP/`ApplicationFrameHost` skipped clearly; slow apps wait for a **new** HWND; partial success opens biome when some zones place.

**Store (Spotify):** assign while open (AUMID saved); launch via AUMID without error dialog; recreate old biomes after upgrading.

**Obsidian:** assign with vault open (`launchUri` saved); cold start opens vault via URI; never vault picker from Biomes.

**Electron:** VS Code / Discord place when open or launched.

**Monitors / hotkey:** primary + secondary zones; unplug fallback; card and hotkey toggle match; no double-hotkey corruption.

**Create flow:** overlay saves path + exe + titleHint + aumid/launchUri; relaunch uses binding after Chrome updates (exe name match).

## Feedback welcome

This is an early public V1. Useful feedback areas:

- Overlay UX while drawing / snapping windows  
- Launch reliability for UWP / Store apps  
- Hotkey conflicts and discoverability  
- Multi-monitor edge cases  

Please open an issue with your Windows version, monitor setup, and what you expected vs what happened.

## License

See [LICENSE](LICENSE).
