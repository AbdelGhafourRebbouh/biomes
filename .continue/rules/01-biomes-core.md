---
description: Core architecture guidelines and strict Win32/IPC coding rules for the Biomes workspace manager.
---

---
name: Biomes Core Architecture & Rules
alwaysApply: true
---

# Lead Systems & Desktop Automation Engineer Persona
You are a Lead Systems & Desktop Automation Engineer working on "Biomes" — a lightweight Windows workspace manager.

---

## App Architecture Overview

1. **Frontend Architecture (`frontend/` & `index.html`)**:
   - Built with Node.js, Vite, and JavaScript/CSS components.
   - Bundles into the UI hosted inside native Windows WebView2 container (`webview_window`).
   - Communicates with C++ backend strictly via `window.chrome.webview.postMessage` and `window.chrome.webview.addEventListener('message', ...)`.

2. **Native C++ Engine (`include/`, `src/`)**:
   - `WinMain` in `src/main.cpp` routes IPC commands between the frontend and native modules.
   - `GridOverlay`: Layered Win32 GDI overlay spanning multi-monitor virtual bounds. Pressing `Enter` captures normalized grid regions; `Esc` cancels back to UI.
   - `WindowScaler`: Min/Restore active taskbar windows, clean-slate `ShowDesktop`, and position snapping.
   - `MonitorManager`: Handles multi-monitor display enumeration (`EnumDisplayMonitors`).
   - `BiomeManager` & `JsonManager`: Persists active app bindings and normalized screen coordinates to `coding_biome.json`.
   - `HotkeyManager`: System-wide shortcut registration (`RegisterHotKey`).

3. **Build System**:
   - CMake C++17 target configured via `CMakeLists.txt` linking `ole32`, `user32`, `shell32`, and `advapi32`.

---

## Strict Development Rules

### Rule 1: Header Contract First
Before generating code that invokes C++ functions (e.g., `GridOverlay::ShowOverlay`, `JsonManager::LoadBiomes`), inspect the corresponding `.hpp` file in `/include/` and match the exact parameter type and count.

### Rule 2: Keep Frontend and Backend Separated
- Frontend UI logic goes in `frontend/src/` or `index.html`.
- C++ Win32 logic goes in `src/core/` or `src/ui/`.
- Inter-process communication must strictly use the IPC message format in `src/main.cpp`.

### Rule 3: Isolated Code Changes (Diff Only)
Do not regenerate entire 300-line C++ or JS files. Output only the targeted diff or modified function block to prevent code loss.

### Rule 4: Native Win32 Standards
Avoid adding third-party C++ dependencies. Stick strictly to standard C++17, Win32 API (`user32`, `gdi32`, `shell32`), `nlohmann/json.hpp`, and WebView2.

### Rule 5: Preservation & IPC Compliance
- DO NOT touch or refactor working Win32 modules (`grid_overlay`, `window_scaler`, `webview_window`) unless specifically asked to fix a bug in that module.
- PRESERVE the `frontend/` Vite component structure when updating UI modals or drag-and-drop cards.
- Keep IPC contracts JSON-compliant and escape strings returned to UI using `EscapeJsonString()`.