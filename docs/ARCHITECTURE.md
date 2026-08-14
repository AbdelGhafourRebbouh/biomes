# Biomes architecture (V1)

Lightweight Windows workspace manager: draw grid zones, assign apps, save biomes, launch/close from the dashboard or a global hotkey.

## Shipped stack

| Layer | Files | Role |
|-------|-------|------|
| UI | `index.html` | Dashboard (WebView2) |
| Host | `src/ui/webview_window.cpp` | Win32 + WebView2 container |
| IPC hub | `src/main.cpp` | JSON `postMessage` router, activate/close, matching |
| Create overlay | `src/ui/grid_overlay.cpp` | Multi-monitor grid, snap apps to zones |
| Window engine | `src/core/window_scaler.cpp` | Snap, launch, clean slate, session close |
| Store / Obsidian | `src/core/app_launcher.cpp` | AUMID launch, Obsidian URI |
| Monitors | `src/core/monitor_manager.cpp` | `rcWork`, EDID stable IDs, topology hash |
| Persistence | `src/core/json_manager.cpp` | `config/biomes.json` v2 |
| Hotkeys | `src/core/hotkey_manager.cpp` | Toggle biomes globally |

Runtime data lives next to the exe (gitignored): `build/Release/config/biomes.json`, `biomes_runtime.log`.

## Session model

**Open biome**

1. `PrepareCleanSlate` — minimize non-biome windows (never Shell MinimizeAll on activate)
2. Per zone: reuse HWND or `LaunchAndSnapApp`
3. `ForceSnapToBox` — absolute screen coords; exit fullscreen for Electron first
4. `RaiseBiomeWindows` — no HWND_TOPMOST (avoids Notion freezes)
5. Dashboard `MinimizeDashboard` (stays on taskbar)

**Close biome**

1. `CloseBiomeSession` — restore pre-biome placement, then minimize biome apps
2. Non-biome windows stay minimized
3. `RestoreDashboard`

Partial success: biome opens if ≥1 zone placed; STATUS lists per-zone notes.

## IPC contract

**UI → native:** `CREATE_NEW_BIOME`, `SAVE_BIOME`, `DELETE_BIOME`, `ACTIVATE_BIOME`, `GET_SAVED_BIOMES`, `CLOSE_BIOME`, `GET_ACTIVE_WINDOWS`

**Native → UI:** `LOADED_BIOMES`, `GRID_LAYOUT_READY`, `STATUS`, `ACTIVE_BIOME_CHANGED`, `ACTIVE_WINDOWS_LIST`, `MONITORS_CHANGED`

Escape user strings with `EscapeJsonString()` before embedding in JSON.

## Create flow

1. `CREATE_NEW_BIOME` → `PrepareForOverlayCreate` + hide dashboard
2. Draw zones on each monitor (`rcWork`)
3. Enter → snap mode → drag windows onto zones (`BindWindowToBox` captures path, title, aumid, launchUri)
4. Enter → `GRID_LAYOUT_READY` → save dialog in `index.html`

## Special apps

- **Store (Spotify, Affinity):** never launch `\WindowsApps\` exe paths; use AUMID via `IApplicationActivationManager`
- **Obsidian:** never bare exe; `obsidian://open?vault=...` from title + `%APPDATA%\obsidian\obsidian.json`
- **Electron (Notion, VS Code):** gentle snap; exit F11 fullscreen before resize

## Not in V1

- `frontend/` Vite/React shell (see `frontend/README.md`)
- Hyprland-style split canvas (Method 2)

## QA

Manual checklist: `docs/PLACEMENT_CHECKLIST.md`
