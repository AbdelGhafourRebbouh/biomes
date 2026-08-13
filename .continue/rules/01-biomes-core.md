---
description: Core architecture guidelines and strict Win32/IPC coding rules for the Biomes workspace manager.
---

# Biomes Core Architecture & Rules

## App Architecture Overview

1. **UI (`index.html`)**
   - Single-file dashboard hosted in WebView2 (`src/ui/webview_window.cpp`)
   - IPC via `window.chrome.webview.postMessage` / `addEventListener('message')`
   - `frontend/` is deferred — see `frontend/README.md`

2. **Native C++ Engine (`include/`, `src/`)**
   - `main.cpp` — IPC router, biome activate/close, window matching
   - `GridOverlay` — multi-monitor grid overlay on `rcWork`; Enter to snap apps
   - `WindowScaler` — snap, launch, clean slate, session close
   - `AppLauncher` — Store AUMID + Obsidian URI
   - `MonitorManager` — monitor enumeration
   - `JsonManager` — `config/biomes.json` v2 (multi-biome collection)
   - `HotkeyManager` — global hotkeys

3. **Build**
   - CMake C++17; links `ole32`, `user32`, `shell32`, `advapi32`, `gdi32`, `dwmapi`

Full reference: `docs/ARCHITECTURE.md`

## Strict Development Rules

- Read `.hpp` headers before calling C++ APIs
- UI changes: `index.html` (shipped). Native: `src/core/`, `src/ui/`
- IPC shapes are defined in `src/main.cpp` — do not break without updating both sides
- Small diffs; escape JSON strings with `EscapeJsonString()`
- No new third-party C++ deps beyond nlohmann/json and WebView2
- Test with `build/Release/Biomes.exe` after native changes
