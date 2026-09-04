# Biomes Engine — Complete Technical Architecture & Developer Specification

This document provides a complete, line-of-thought technical specification of the **Biomes Engine**. It explains every subsystem, data structure, inter-process communication (IPC) channel, window management algorithm, and session lifecycle in explicit detail. Any developer can read this guide and immediately modify, extend, or maintain the project without opening a single source file.

---

## 1. High-Level Architecture & Tech Stack

Biomes is a lightweight, high-performance Windows desktop application manager built on a **hybrid Win32 + WebView2 architecture**.

```
┌────────────────────────────────────────────────────────────────────────┐
│                        FRONTEND UI DASHBOARD                           │
│     index.html (Single-file HTML5/CSS3/Vanilla JS embedded in WebView2) │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
              IPC Bridge via postMessage / WebMessageReceived
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                      NATIVE C++ ENGINE CORE                            │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │ src/main.cpp — Central IPC Message Router & Application Loop     │  │
│  └──────┬──────────────┬──────────────┬──────────────┬──────────────┘  │
│         │              │              │              │                 │
│  ┌──────▼──────┐┌──────▼──────┐┌──────▼──────┐┌──────▼──────┐          │
│  │WindowScaler ││ AppLauncher ││MonitorManager││ JsonManager  │          │
│  │ (Snap/Close)││ (UWP/Obsid) ││ (EDID/Work)  ││ (Persistence)│          │
│  └─────────────┘└─────────────┘└─────────────┘└─────────────┘          │
│         │              │              │              │                 │
│  ┌──────▼──────┐┌──────▼──────┐┌──────▼──────┐                         │
│  │GridOverlay  ││ HotkeyHost   ││ TrayManager  │                         │
│  │ (Creation)  ││ (Win32 Reg)  ││ (System Tray)│                         │
│  └─────────────┘└─────────────┘└─────────────┘                         │
└────────────────────────────────────────────────────────────────────────┘
```

* **Language Standard:** Modern C++17 compiled via MSVC (Visual Studio 2022/2026).
* **Native Win32 Libraries:** `user32`, `gdi32`, `shell32`, `advapi32`, `ole32`, `dwmapi`, `shcore`.
* **Third-Party Libraries (Zero heavy frameworks):**
  * `nlohmann/json` (header-only JSON serialization).
  * `Microsoft.Web.WebView2` (Native COM interface bindings without WRL dependencies).
* **UI Hosting:** Embedded Microsoft Edge WebView2 rendering local `index.html`.

---

## 2. Directory & Module Responsibilities

```
biomes/
├── CMakeLists.txt                # Build configuration & post-build asset copying
├── index.html                    # Frontend dashboard (UI, CSS, JS)
├── WebView2Loader.dll            # Shipped runtime library for WebView2 initialization
├── include/
│   ├── core/
│   │   ├── app_launcher.hpp     # Store (AUMID), Obsidian (URI), and path resolution
│   │   ├── hotkey_host.hpp      # Hidden message window owning RegisterHotKey
│   │   ├── hotkey_manager.hpp   # Shortcut parsing ("CTRL+ALT+C") and ID mapping
│   │   ├── json_manager.hpp     # JSON persistence & multi-topology variant selection
│   │   ├── monitor_manager.hpp  # Multi-monitor enumeration, EDID IDs, and rcWork
│   │   └── window_scaler.hpp    # Window enumeration, snapping, and session close/restore
│   └── ui/
│       ├── grid_overlay.hpp     # Canvas overlay window & drag-and-drop snapping
│       ├── tray_manager.hpp     # Windows notification tray icon & startup registry
│       └── webview_window.hpp   # Win32 window container hosting WebView2
└── src/                          # Implementations corresponding to include/ headers
```

---

## 3. Core Domain Models & Data Structures

### `SelectedBox` (Layout Zone Definition)
Represents a single rectangular region on a monitor assigned to an application.
* `int id`: Numeric identifier unique within a layout.
* `int monitorIndex`: Zero-based index of the display where the box resides.
* `int startCol, endCol, startRow, endRow`: Discrete grid coordinates (e.g., columns 0 to 4 in an 8x14 matrix).
* `float relX, relY, relWidth, relHeight`: Normalized fractional coordinates `[0.0, 1.0]` relative to the monitor's work area (`rcWork`). This allows layouts to scale responsively across different resolutions.
* `std::string assignedApp`: Absolute file path (e.g., `C:\Program Files\Google\Chrome\Application\chrome.exe`) or identifier.
* `std::string exeName`: File basename (e.g., `chrome.exe`). Used for matching running windows across updates or path shifts.
* `std::string titleHint`: Window title captured at assignment time (e.g., `Project - Obsidian`).
* `std::string monitorDevice`: GDI device name (e.g., `\\.\DISPLAY1`).
* `std::string stableMonitorId`: Hardware-stable monitor identity (EDID-derived or `GDI:DISPLAY1`).
* `std::string topologyHash`: Hash of the display topology active when saved.
* `std::string aumid`: Windows Store / UWP Application User Model ID (e.g., `Canva.Affinity_31v2y1p8n2w38!Canva.Affinity`).
* `std::string launchUri`: Protocol URI used for special apps (e.g., `obsidian://open?vault=MyVault`).

### `BiomeProfile` (Workspace Definition)
Represents a saved user workspace.
* `std::string id`: Unique profile ID (e.g., `biome-1711200000000`).
* `std::string name`: Human-readable title (e.g., `Coding & Research`).
* `std::string hotkey`: String combo (e.g., `CTRL+ALT+C`).
* `std::string coverImagePath`: Base64 string or local path for the dashboard card thumbnail.
* `std::string topologyHash`: Topology hash when created.
* `std::vector<SelectedBox> layout`: Primary box configuration.
* `std::unordered_map<std::string, std::vector<SelectedBox>> layoutVariants`: Per-topology layout overrides (e.g., 2-monitor variant vs. 1-monitor laptop variant).

### `WindowInfo` (Active Window Snapshot)
* `HWND hwnd`: Native Win32 window handle.
* `DWORD processId`: Process ID owning the window.
* `std::string title`: Text from `GetWindowTextA`.
* `RECT rect`: Absolute screen coordinates from `GetWindowRect`.
* `std::string processName`: Process executable name (`chrome.exe`).
* `std::string processPath`: Full disk path to process image.
* `std::string aumid`: Package AUMID if UWP.

---

## 4. Application Lifecycles & Session Model

Biomes uses a **non-destructive session tracking model**. Activating a Biome never destroys non-biome windows, and closing a Biome restores all windows cleanly.

```
   [ Dashboard / Hotkey Trigger ]
                 │
                 ▼
   ┌───────────────────────────┐
   │ PrepareCleanSlate()       │ ──> Minimizes non-biome windows
   └─────────────┬─────────────┘     Caches original WINDOWPLACEMENT
                 │
                 ▼
   ┌───────────────────────────┐
   │ Process Zones Sequentially│
   └─────────────┬─────────────┘
                 │
      ┌──────────┴──────────┐
      ▼                     ▼
[Existing Window]    [Launch Missing App]
   Reuse HWND        AUMID / URI / Executable
      │                     │
      └──────────┬──────────┘
                 │
                 ▼
   ┌───────────────────────────┐
   │ ForceSnapToBox()          │ ──> Exits F11 / Fullscreen
   └─────────────┬─────────────┘     Applies rcWork-relative SetWindowPos
                 │
                 ▼
   ┌───────────────────────────┐
   │ RaiseBiomeWindows()       │ ──> Brings placed HWNDs to top
   └─────────────┬─────────────┘     Minimizes Dashboard
                 │
                 ▼
          [ ACTIVE SESSION ]
                 │
    (User closes Biome or triggers Hotkey)
                 │
                 ▼
   ┌───────────────────────────┐
   │ CloseBiomeSession()       │ ──> Restores pre-biome placement
   └─────────────┬─────────────┘     Minimizes Biome windows
                 │                   Restores Dashboard
                 ▼
         [ DESKTOP RESTORED ]
```

### A. Activation Lifecycle (`ACTIVATE_BIOME`)
1. **Clean Slate Initialization (`WindowScaler::PrepareCleanSlate`)**:
   - Enumerates active top-level windows.
   - Ignores dashboard HWND and Biomes process windows.
   - Minimizes managed open windows and records their original state (`WINDOWPLACEMENT`) in `s_cleanSlateMinimized` and `s_originalPositions`.
2. **Zone Placement & HWND Resolution**:
   - Reads boxes for the selected Biome. Filters out zones targeting disconnected monitors.
   - For each zone, attempts to match an existing open window by **AUMID**, **Executable Name**, or **Title Hint**.
   - If an open window is matched, reuses that handle.
   - If no open window exists, calls `WindowScaler::LaunchAndSnapApp` to launch a new process and wait for its main window.
3. **Geometry Snapping (`WindowScaler::ForceSnapToBox`)**:
   - Checks if target window is in F11/exclusive fullscreen (e.g., Electron/Obsidian) and exits fullscreen first.
   - Calculates absolute screen coordinates by combining `rcWork` of the target display with normalized box bounds (`relX`, `relY`, `relWidth`, `relHeight`).
   - Calls `SetWindowPos` with `HWND_TOP`.
4. **Session Elevation & Focus (`WindowScaler::RaiseBiomeWindows`)**:
   - Ensures all snapped HWNDs sit above background windows **without** setting `HWND_TOPMOST` (which prevents focus stealing/freezing bugs).
   - Minimizes the dashboard window to the taskbar.

### B. Deactivation Lifecycle (`CLOSE_BIOME` or Re-triggering Active Biome)
1. **Restore Session Windows (`WindowScaler::CloseBiomeSession`)**:
   - Iterates through all windows managed by the active Biome session.
   - Restores their pre-biome placement (`WINDOWPLACEMENT`) so they return to their exact original size/location.
   - Minimizes the Biome windows.
2. **Dashboard Restoration**:
   - Restores and focuses the WebView2 dashboard window (`WebViewWindow::RestoreDashboard`).
   - Unrelated non-biome windows that were minimized during Clean Slate stay minimized, leaving the desktop clean.

---

## 5. Detailed Subsystem Mechanics

### A. Display & Topology Engine (`MonitorManager`)
Windows display indices (`DISPLAY1`, `DISPLAY2`) change whenever cables are unplugged or graphics drivers restart. `MonitorManager` resolves monitors using **hardware-stable identities**.

* **Work Area Bounds (`rcWork`)**: Always uses `rcWork` rather than full `rcMonitor` bounds. This ensures snapped windows never overlap the taskbar or docked desktop bars.
* **Stable Monitor ID Construction**:
  1. Queries Win32 Display Configuration APIs (`QueryDisplayConfig` / `GetDisplayConfigBufferSizes`).
  2. Extracts manufacture ID and product code from the monitor EDID string (e.g., `EDID:10AC-D0E2`).
  3. Disambiguates duplicate identical monitors by appending the GDI device name (`@\\.\DISPLAY1`).
  4. If EDID is unavailable (such as in virtual machines), falls back to `GDI:\\.\DISPLAYn`.
* **Topology Hash**: Concatenates all connected monitor stable IDs alphabetically and computes an FNV-1a hash (e.g., `a1b2c3d4`).
* **Zone Resolution Algorithm (`ResolveMonitorForBox`)**:
  - Matches saved boxes in strict order: `stableMonitorId` $\rightarrow$ `monitorDevice` $\rightarrow$ `monitorIndex`.
  - **Single Monitor Protection**: If a multi-monitor Biome is launched on a single-monitor laptop, secondary display zones are **skipped** rather than squeezed onto the laptop screen.

### B. Application Launcher (`AppLauncher`)
Standard `CreateProcessA` calls fail or display permission errors when targeting Microsoft Store apps or complex Electron hosts. `AppLauncher` resolves application launch routes dynamically:

1. **Microsoft Store / UWP Apps**:
   - Detected if file path contains `\WindowsApps\`.
   - Never executes `.exe` files directly inside `WindowsApps` (causes Access Denied errors).
   - Resolves or guesses the AUMID (`PackageFamilyName!AppId`).
   - Instantiates COM interface `IApplicationActivationManager` (`CLSID_ApplicationActivationManager`) and calls `ActivateApplication()` with `AO_NOERRORUI`.
2. **Obsidian**:
   - Never launches bare `Obsidian.exe` (which only opens the vault selector).
   - Parses the target vault from the saved title hint or `%APPDATA%\obsidian\obsidian.json`.
   - Constructs an `obsidian://open?vault=VaultName` URI.
   - Launches via `%LocalAppData%\Programs\Obsidian\Obsidian.exe "obsidian://open?vault=..."`.
3. **App Path Resolver (`ResolveAppPath`)**:
   - If given a bare executable name (`chrome.exe`), searches Windows Registry: `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\chrome.exe`.
   - Falls back to `SearchPathA`. This eliminates hardcoded user directory paths (`C:\Users\John\...`).

### C. Snapping & Window Engine (`WindowScaler`)
* **Filtering (`IsMainApplicationWindow`)**:
  - Filters out tooltips, hidden helper windows, cloaked DWM windows (`DWMWA_CLOAKED`), zero-size windows, and windows with parents (`GW_OWNER`).
  - Requires visible dimensions $\ge 200 \times 200$ pixels (unless iconic/minimized).
* **Electron Fullscreen Exit**:
  - Electron apps (Notion, VS Code, Slack, Obsidian) can ignore `SetWindowPos` if stuck in borderless or F11 fullscreen.
  - Detects coverage over monitor work area. If fullscreen, restores window via `WM_SYSCOMMAND` + `SC_RESTORE`, sends `VK_F11` key events, and forces frame recalculation (`SWP_FRAMECHANGED`).

### D. Grid Overlay Engine (`GridOverlay`)
* Operates a top-level `WS_POPUP` window per connected monitor, layered with `WS_EX_LAYERED | WS_EX_TOPMOST`.
* **Two-Stage Enter Key Creation Flow**:
  1. **Drawing Phase**: Mouse click & drag draws transparent glass cards aligned to grid rows and columns ($8 \times 14$).
  2. **First Enter (Snap Phase)**: Overlay switches to pass-through (`WS_EX_TRANSPARENT`). The user drags open windows from their taskbar over grid zones.
  3. **WinEventHook Tracking**: Captures `EVENT_SYSTEM_MOVESIZESTART` and `EVENT_SYSTEM_MOVESIZEEND`. Dropping a window over a zone automatically binds its path, title, executable name, and AUMID to that zone (`BindWindowToBox`).
  4. **Second Enter (Save Phase)**: Overlay closes, restores dashboard, and emits `GRID_LAYOUT_READY` JSON event to UI.

### E. Global Hotkey & System Tray System
* **`HotkeyHost`**: Hidden message-only window (`HWND_MESSAGE`). Handles `WM_HOTKEY` and `WM_DISPLAYCHANGE`.
* **`HotkeyManager`**:
  - Parses string shortcuts (e.g., `CTRL+ALT+C` $\rightarrow$ `MOD_CONTROL | MOD_ALT`,

## Developer Rules & Coding Standards

1. **Primary AI Agent:** OpenAI Codex is the primary AI assistant for this repository. Ignore any past `.cursor` or `.continue` configurations.
2. **Token Efficiency:** Always inspect specific target files rather than running full repository or folder scans. Ask for confirmation before editing multiple files at once.
3. **C++17 Standards:** Use clean modern C++ following Win32 API guidelines, explicit memory management, and zero unnecessary third-party dependencies.
4. **Safety & Non-Destruction:** Ensure window management logic always respects original `WINDOWPLACEMENT` states when activating or restoring sessions.
5. **Build Verification:** Verify code modifications against `CMakeLists.txt` before finalizing any edits.
