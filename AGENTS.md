# Biomes Engine — Complete Technical Architecture & Developer Specification

This document describes the implemented native architecture, including Step 1 (storage isolation) and Step 2 (background lifetime). Source code and tests remain authoritative. Planned packaging behavior is distinguished from implemented behavior.

---

## 1. High-Level Architecture & Tech Stack

Biomes is a lightweight, high-performance Windows desktop application manager built on a **hybrid Win32 + WebView2 architecture**.

~~~text
StartupOptions -> SingleInstance -> AppPaths -> Migration -> NativeSettings
                                      |
                       BackgroundHost (hidden top-level HWND)
                       process loop / global hotkeys / tray
                                      |
                  main.cpp orchestration and trusted-page JSON IPC
                      |                              |
                WebViewWindow                  Native engine
                (dashboard)       WindowScaler / AppLauncher / MonitorManager
                                  JsonManager / GridOverlay / LaunchPanel
~~~

* **Language Standard:** Modern C++17 compiled via MSVC (Visual Studio 2022/2026).
* **Native Win32 Libraries:** `user32`, `gdi32`, `shell32`, `advapi32`, `ole32`, `dwmapi`, `shcore`, `dcomp`.
* **Third-Party Libraries (Zero heavy frameworks):**
  * `nlohmann/json` (header-only JSON serialization).
  * `Microsoft.Web.WebView2` (Native COM interface bindings without WRL dependencies).
* **UI Hosting:** WebView2 renders `frontend/index.html` and separate scripts/styles, copied beside the executable by `BiomesAssets`. The dashboard does not own process lifetime; `BackgroundHost::Run()` does.

---

## 2. Directory, Storage & Path Isolation

### A. Project modules

~~~text
biomes/
|-- CMakeLists.txt                 # Native sources, libraries, asset copying, tests
|-- AGENTS.md
|-- WebView2Loader.dll
|-- resources/                    # Native icon resources
|-- frontend/                     # Frozen HTML/CSS/JS and static assets
|   |-- index.html, app.js, save-dialog.js, newsletter.js, styles.css
|   |-- launch-panel.html, launch-panel.css, launch-panel.js
|   |-- onboarding.css, onboarding.js
|   `-- assets/, logo/, images/, cardsimages/, icons/
|-- include/
|   |-- core/
|   |   |-- app_launcher.hpp          # Executable, packaged-app and URI launches
|   |   |-- app_paths.hpp             # Absolute per-user storage paths
|   |   |-- background_host.hpp       # Hidden lifetime/hotkey owner
|   |   |-- hotkey_manager.hpp        # Shortcut parsing and registration mapping
|   |   |-- json_manager.hpp          # Biome persistence and topology variants
|   |   |-- launch_progress.hpp       # Launch progress state
|   |   |-- legacy_data_migration.hpp # Non-destructive copy-and-switch migration
|   |   |-- monitor_manager.hpp       # Display identity and work areas
|   |   |-- native_settings.hpp       # Validated backend settings
|   |   |-- single_instance.hpp       # Mutex and activation handoff
|   |   |-- startup_options.hpp       # Silent startup flags
|   |   |-- startup_registration.hpp  # Current-user Run registry entry
|   |   `-- window_scaler.hpp         # Discovery, tracking, placement, restoration
|   |-- ui/
|   |   |-- grid_overlay.hpp          # Zone creation and app binding
|   |   |-- launch_panel.hpp          # Launch-progress WebView2 host
|   |   |-- tray_manager.hpp          # Notification icon and context menu
|   |   `-- webview_window.hpp        # Dashboard HWND and IPC
|   `-- external/                    # JSON/WebView2 dependencies
|-- src/
|   |-- main.cpp
|   |-- core/                        # Corresponding compiled core implementations
|   `-- ui/                          # Overlay, launch panel, tray, dashboard
`-- tests/
    |-- stability_tests.cpp
    |-- storage_tests.cpp
    |-- lifecycle_tests.cpp
    `-- launch_panel_preview.cpp
~~~

There is no separate `hotkey_host.hpp`: `BackgroundHost` owns that responsibility. CMake includes all compiled Step 1/2 source modules.

### B. Installation versus user data

The intended installer location is `%LOCALAPPDATA%\Programs\biomes\`. This is a planned packaging destination, not an installer already implemented by Steps 1/2. Development binaries run from CMake build output; asset lookup uses the actual executable directory.

~~~text
%LOCALAPPDATA%\Programs\biomes\    # Planned replaceable installation files
    Biomes.exe
    WebView2Loader.dll
    index.html, scripts, styles, fonts, bundled images, ...

%LOCALAPPDATA%\biomes\             # Implemented stable user-data location
    config\
        biomes.json
        settings.json
        settings.lock
        settings.json.tmp          # Transient settings-save staging file
    logs\
        biomes_runtime.log
        legacy-biomes_runtime.log  # If imported
    webview_data\                  # Both WebView2 hosts, cache and local storage
    images\                        # User-owned image storage directory
    backups\
        migration.lock
        legacy-migration-v1.done
        legacy-stage-{GUID}\       # Staging/recovery artifacts
~~~

Bundled images beside the executable are distinct from `AppPaths::Images()`. Existing inline/base64 covers remain in biome JSON; creating this directory does not automatically relocate external image references.

### C. Centralized paths: `biomes::AppPaths`

- `Initialize()` calls `SHGetKnownFolderPath(FOLDERID_LocalAppData)` and creates the root plus `config`, `logs`, `webview_data`, `images`, and `backups` once.
- Required directories are checked for directory type and reparse points. Initialization errors are surfaced; there is no fallback to the working or installation directory.
- Accessors return absolute `std::filesystem::path` values: `Root`, `Config`, `Logs`, `WebViewData`, `Images`, `Backups`, `BiomesFile`, `SettingsFile`, and `RuntimeLog`.
- `ExecutableDirectory()` uses `GetModuleFileNameW` for shipped assets and the explicit legacy source, not new personal-data writes.
- `JsonManager` receives `BiomesFile()`; main, window tracking, and launch-panel logs use `RuntimeLog()`. Both dashboard and launch-panel WebView2 environments use `WebViewData()`.
- Native settings use `config/settings.json`. Existing frontend preferences/local storage remain in the isolated WebView2 profile; the frozen UI has not been rewritten to use native settings for everything.
- `InitializeForTests()` is compiled only with `BIOMES_STORAGE_TESTING` for isolated temporary roots.

### D. One-time legacy migration

Startup invokes `MigrateLegacyData(AppPaths::ExecutableDirectory())` after path initialization and before normal runtime logging or WebView2 creation.

1. Acquire exclusive `backups/migration.lock`; return when the completion marker exists.
2. Inspect only the explicit legacy executable directory: `config/biomes.json`, `config/settings.json`, `config/biomes_runtime.log`, and `webview_data/`.
3. Preserve existing destination files. Do not merge a populated WebView2 profile with legacy browser data.
4. Reject unsafe linked/reparse sources and inaccessible/locked data. Source read handles exclude writers during staging/promotion.
5. Copy candidates to a unique staging directory and validate configuration structures/supported legacy versions.
6. Promote without replacing existing destination files; persist the completion marker only after success.

This is **copy-and-switch**, not deletion. Original files remain intact; failed staging remains recoverable. Promotion of the whole migration is not atomic: a retry preserves any files already promoted. There is no arbitrary working-directory scan or automatic import from cleanup recovery folders.

---

## 3. Core Domain Models & Data Structures

### `SelectedBox` (Layout Zone Definition)
Represents a single rectangular region on a monitor assigned to an application.
* `int id`: Numeric identifier unique within a layout.
* `int monitorIndex`: Zero-based index of the display where the box resides.
* `int startCol, endCol, startRow, endRow`: Discrete grid coordinates (e.g., columns 0 to 4 in an 8x14 matrix).
* `RECT pixelRect`: Pixel geometry used by the overlay.
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

### Runtime identity and settings models

`WindowIdentity` separates the visible `placementHwnd` from the underlying process/package identity (including ApplicationFrameHost). `OriginalWindowState` retains `WINDOWPLACEMENT`; `BiomeAppSession` retains HWND, PID, pre-biome placement, and whether the window existed before activation. Pending launch work is session-generation-scoped, not persisted layout data.

Native settings default to:

~~~json
{"schemaVersion":1,"launchAtStartup":false,"onboardingCompleted":false}
~~~

Unknown existing keys are retained; IPC updates whitelist the two boolean preferences. Native onboarding state is not yet wired into the frozen onboarding script. The biome collection writer emits version 3 JSON through a temporary file and native replacement; migration validation and normal loading are separate paths, not a universal forward-schema guarantee.

## 4. Application Lifecycles & Session Model

### A. Process startup

1. Parse startup flags and acquire the single-instance guard before touching data. A duplicate activates the existing instance and exits (silent duplicates simply exit).
2. Initialize AppPaths, migrate legacy data, load/create native settings, and reconcile the Run registry entry.
3. Initialize the native environment and hidden BackgroundHost; register global hotkeys against its HWND.
4. Wire dashboard, tray, IPC, display-change, overlay, and shutdown callbacks.
5. Open the dashboard for normal startup. Silent startup leaves it uncreated until requested, unless the tray is unavailable.
6. Enter `BackgroundHost::Run()`; dashboard visibility does not determine process lifetime.

### B. Activation and deferred tracking

~~~text
Dashboard / hotkey
  -> select topology layout; skip disconnected zones
  -> cancel obsolete deferred work
  -> clean-slate pass and pre-biome placement capture
  -> reuse matching windows / launch missing apps with deferred tracking
  -> discover usable windows; request placement; verify settling
  -> report per-app progress and raise placed windows
  -> active session; background engine remains alive
~~~

Clean-slate preparation excludes biomes-owned windows and records original placements before minimizing managed apps. Matching uses executable/package identity, title hints, and exclusions to avoid assigning one HWND to multiple zones.

Missing apps use `LaunchAndTrackApp`, WinEvent discovery, and periodic scans; slow launches do not require waiting synchronously for every app. `LaunchAndSnapApp` remains available but is not the complete asynchronous architecture. Tracking is paused during setup to avoid racing the clean-slate pass. Generation checks and `CancelPendingLaunches()` invalidate old work when closing or switching sessions.

### C. Session close versus dashboard close

- Closing/re-triggering a biome cancels pending work, restores available pre-biome placements, then minimizes session windows. Freshly launched apps are not killed; unrelated clean-slate windows are left alone.
- `RestoreDashboard()` can request lazy dashboard creation.
- Dashboard `WM_CLOSE` / Alt+F4 hides to the tray, not process exit or biome-session close. Ordinary taskbar minimization is a distinct operation.
- **Exit Biomes** disables callbacks, cancels launches, hides overlays, closes the biome session, unregisters hotkeys, shuts down both WebView2 hosts, removes the tray icon, and destroys the background host.
- Restoration depends on the target HWND still existing and accepting placement; do not promise perfect resizing/restoration for every third-party app.

---

## 5. Detailed Subsystem Mechanics

### A. Display & Topology Engine (`MonitorManager`)
Windows display indices (`DISPLAY1`, `DISPLAY2`) change whenever cables are unplugged or graphics drivers restart. `MonitorManager` resolves monitors using **hardware-stable identities**.

* **Work Area Bounds (`rcWork`)**: Always uses `rcWork` rather than full `rcMonitor` bounds. Targets exclude the taskbar; third-party apps can still reject the requested bounds.
* **Stable Monitor ID Construction**:
  1. Queries Win32 Display Configuration APIs (`QueryDisplayConfig` / `GetDisplayConfigBufferSizes`).
  2. Extracts manufacture ID and product code from the monitor EDID string (e.g., `EDID:10AC-D0E2`).
  3. Disambiguates duplicate identical monitors by appending the GDI device name (`@\\.\DISPLAY1`).
  4. If EDID is unavailable (such as in virtual machines), falls back to `GDI:\\.\DISPLAYn`.
* **Topology Hash**: Concatenates all connected monitor stable IDs alphabetically and computes an FNV-1a hash (e.g., `a1b2c3d4`).
* **Zone Resolution Algorithm (`ResolveMonitorForBox`)**:
  - Uses saved stable identity, device information, and legacy index information. Missing saved displays must not silently resolve to another display through index reuse; explicit repair/remapping is separate from normal activation.
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
* **Deferred placement and fullscreen handling**:
  - Maximized/minimized restoration is distinct from application fullscreen. Geometry/style are hints, not proof; current placement does **not** blindly send F11 or Escape.
  - `ForceSnapToBox()` requests placement; timer-driven verification checks restoration, geometry, and renderer settling. Initial API success is not final placement success.
  - WinEvent notifications and periodic scans handle slow launches, welcome screens, and replacement windows. Generation and identity checks prevent stale work affecting a new session.
  - Forced/constrained sizing is distinct from launch failure. Apps rejecting geometry remain open; arbitrarily small zones are not guaranteed to work.
  - Original placement is captured before mutation; progress is exposed through `launch_progress.hpp` and `LaunchPanel`.

### D. Grid Overlay Engine (`GridOverlay`)
* Operates a top-level `WS_POPUP` window per connected monitor, layered with `WS_EX_LAYERED | WS_EX_TOPMOST`.
* **Two-Stage Enter Key Creation Flow**:
  1. **Drawing Phase**: Mouse click & drag draws transparent glass cards aligned to grid rows and columns (default 8 rows by 14 columns; dimensions and theme are configurable).
  2. **First Enter (Snap Phase)**: Overlay switches to pass-through (`WS_EX_TRANSPARENT`). The user drags open windows from their taskbar over grid zones.
  3. **WinEventHook Tracking**: Captures `EVENT_SYSTEM_MOVESIZESTART` and `EVENT_SYSTEM_MOVESIZEEND`. Dropping a window over a zone automatically binds its path, title, executable name, and AUMID to that zone (`BindWindowToBox`).
  4. **Second Enter (Save Phase)**: Overlay closes, restores dashboard, and emits `GRID_LAYOUT_READY` JSON event to UI.

### E. Background Engine, System Tray & Process Lifetime

#### BackgroundHost and hotkey ownership

`biomes::BackgroundHost` creates an unshown top-level `WS_POPUP` window with `WS_EX_TOOLWINDOW`. It is **not** an `HWND_MESSAGE` window, allowing broadcast display/taskbar notifications.

`HotkeyManager` parses shortcuts and registers/unregisters them with `RegisterHotKey` against the host HWND. `WM_HOTKEY`, tray actions, duplicate activation, and display-change callbacks are queued and drained by the outer message loop after dispatch, avoiding session work inside native/COM callbacks.

The host handles display changes, work-area changes, session-end messages, and explicit exit. Global hotkeys remain active while the dashboard is hidden. This is a per-user desktop background process, not a Windows service or crash-restarting watchdog.

#### Dashboard visibility and shutdown

Dashboard `WM_CLOSE` / Alt+F4 invokes the close-request callback. Main hides the dashboard and its WebView2 controller if the tray is available; otherwise it keeps the dashboard accessible. Dashboard destruction does not post a process-wide quit.

The hidden host's own `WM_CLOSE` is different: it requests process shutdown. WebView2 callbacks guard shutdown state; teardown closes controllers and clears callbacks so late initialization does not resurrect the UI.

#### TrayManager

`TrayManager` uses `Shell_NotifyIconW`, the native biomes icon resource, and notification version 4. Its menu contains:

- **Open Biomes** — lazily create, restore, and focus the dashboard.
- **Launch at Windows Startup** — checked startup preference toggle.
- **Exit Biomes** — orderly process shutdown.

Mouse/keyboard activation opens the dashboard. The registered `TaskbarCreated` message re-adds the icon after Explorer restarts; failure falls back to opening the dashboard.

#### Silent startup and registry integration

`StartupOptions::Parse()` uses `CommandLineToArgvW`. Both `--minimized` and `--autostart` select silent launch. Unknown arguments are rejected. Silent launch does not create/show the dashboard or allocate the optional debug console.

`StartupRegistration` manages only the `biomes` string value under:

~~~text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
    biomes = "<actual executable directory>\Biomes.exe" --autostart
~~~

Enabling writes the quoted absolute command; disabling removes only that value. `IsEnabled()` compares the command with the current executable path, not Windows StartupApproved/policy overrides. The installer startup checkbox remains a packaging task.

#### NativeSettings and IPC

`NativeSettings` uses an exclusive `settings.lock`, validates schema/boolean types, and limits loaded files to 1 MB. Missing settings receive defaults; malformed/unsupported files are not replaced with defaults. Saves stage and flush `settings.json.tmp` before native replacement. Existing unknown keys survive valid updates.

Changing `launchAtStartup` snapshots the Run value, updates registration, then saves settings. A save failure attempts registry rollback. Startup reconciliation reflects the existing Run entry instead of silently recreating a removed entry.

Trusted dashboard IPC supports:

~~~json
{"action":"GET_SETTINGS","requestId":"settings-1"}
{"action":"UPDATE_SETTINGS","requestId":"settings-2","settings":{"launchAtStartup":true}}
~~~

Replies use `SETTINGS_RESULT`, the request ID, `success`, and either `settings` or `error`. IDs are optional bounded strings. Only `launchAtStartup` and `onboardingCompleted` boolean updates are accepted. WebView message sources are checked against the configured local dashboard. Step 2 did not change the frozen HTML/CSS/JS to consume these endpoints.

#### SingleInstance: actual mutex and handoff protocol

`SingleInstance` resolves the current user SID and holds a `CreateMutexW` handle for process lifetime:

~~~text
Mutex:        Local\biomes.Background.<user SID>
Window class: biomes.Background.<user SID>
Activation:   SingleInstance::ActivateMessage = WM_APP + 190
~~~

The guard is per user within the current Windows session. `ERROR_ALREADY_EXISTS` identifies a duplicate before storage/profile initialization. An ordinary duplicate finds the hidden host, grants foreground permission to its PID, and uses bounded discovery retries and `SendMessageTimeoutW` to request activation. The host acknowledges and queues opening; the duplicate exits. Silent duplicates exit without showing the UI.

**Specification correction:** the code does not use the proposed literal `biomes_app_single_instance_mutex` or `WM_COPYDATA`. It uses the user-scoped name and payload-free message above. These are documented as implemented, not changed during this documentation update.

## 6. Build, Verification & Maintenance Boundaries

- `CMakeLists.txt` includes the Step 1/2 sources, native libraries, and asset-copy target. Enable `BIOMES_BUILD_TESTS` for regression executables.
- CTest covers stability, seven storage scenarios (`paths`, `migrate`, `existing`, `invalid`, `locked`, `once`, `wrongtype`), and lifecycle regressions. `BiomesPanelPreview` is a separate visual harness.
- Storage tests use temporary roots. Lifecycle tests use isolated registry/settings fixtures, covering startup parsing, validation, persistence, rollback, hidden-host dispatch, mutex behavior, and child-process activation handoff. They must not change real user profiles or startup registration.
- Automated tests do not replace manual tray recovery, Windows sign-in, actual global hotkey, mixed-DPI, disconnected-monitor, slow-app, and cancellation tests.
- Installer generation, fixed installer AppId, upgrade/uninstall preservation, runtime prerequisites, centralized executable version metadata, and update checking remain separate release tasks; these are not implied complete by Steps 1/2.
- Future packaging must preserve `%LOCALAPPDATA%\biomes\` during upgrades/ordinary uninstall and exclude personal biomes, logs, profiles, and test data from distributable assets.
- Workspace persistence is local; optional external services mean the whole application must not be described as completely network-free.
- Keep frontend files frozen unless UI changes are explicitly authorized. Documentation edits must not alter runtime behavior.

## Developer Rules & Coding Standards

1. **Primary AI Agent:** OpenAI Codex is the primary AI assistant for this repository. Ignore any past `.cursor` or `.continue` configurations.
2. **Token Efficiency:** Always inspect specific target files rather than running full repository or folder scans. Ask for confirmation before editing multiple files at once.
3. **C++17 Standards:** Use clean modern C++ following Win32 API guidelines, explicit memory management, and zero unnecessary third-party dependencies.
4. **Safety & Non-Destruction:** Ensure window management logic always respects original `WINDOWPLACEMENT` states when activating or restoring sessions.
5. **Build Verification:** Verify code modifications against `CMakeLists.txt` before finalizing any edits.
