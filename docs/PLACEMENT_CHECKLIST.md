# Biome placement QA checklist

Run against `build/Release/Biomes.exe` before sharing.

## A. Basic Win32
- [ ] App not running → launches and snaps to correct zone
- [ ] App already open → reuses that window (no duplicate)
- [ ] Launch → Close biome → biome apps restore to pre-biome size, then minimize; desktop stays clean
- [ ] Unrelated app (Notepad) minimized on launch → stays minimized on close (not restored)
- [ ] Dashboard is never snapped/minimized incorrectly

## B. Multi-window same exe (Chrome x2 accounts)
- [ ] Two Chrome windows open → biome with **one** Chrome zone snaps only one; the other stays
- [ ] Two Chrome zones → two different HWNDs (never the same window twice)
- [ ] Close the snapped Chrome mid-biome → Close biome → Reopen → does **not** move the remaining Chrome; launches/places a **new** Chrome instead
- [ ] Close biome with both Chromes still open → both restore; reopen reuses the same sticky windows
- [ ] Same pattern with Edge / dual VS Code if installed

## C. App closed / missing
- [ ] Bad/uninstalled path → that zone fails; others still place; STATUS explains
- [ ] `ApplicationFrameHost.exe` slot → skipped with clear note
- [ ] Slow-start app → waits and snaps a **new** HWND only

## D. Electron / special hosts
- [ ] VS Code, Slack, Discord: place when already open
- [ ] Place when not open (launch)
- [ ] Helper processes are not snapped instead of the main window

## D2. Microsoft Store apps (Spotify / Affinity)
- [ ] During **create**: have the Store app open, snap it — JSON should contain non-empty `aumid` (or path under WindowsApps)
- [ ] Affinity closed → biome launch activates via package AUMID (`Canva.Affinity_...!Canva.Affinity`), no WindowsApps exe launch
- [ ] Spotify not running → launches via AUMID (no error dialog)
- [ ] Already open → reuses HWND and snaps to zone
- [ ] Failed Store launch → zone fails gracefully; other zones still place

## D3. Obsidian / Notion (Electron)
- [ ] Notion places without freezing (can click, edit, close normally after biome open)
- [ ] Obsidian URI opens correct vault (not version string)
- [ ] Reopen biome reuses minimized Electron windows (no long re-launch hang)

## E. Partial failure / resilience
- [ ] One zone fails → biome still opens if others placed (`Biome opened: N placed`)
- [ ] No modal Windows “cannot access device/path/file” dialog during launch
- [ ] STATUS toast stays longer when failures are reported

## F. Layout / monitors
- [ ] Primary monitor zones land on primary
- [ ] Secondary zones land on secondary
- [ ] Laptop-only (unplug secondary) → no crash; primary zones still work

## G. Session / hotkey
- [ ] Launch from card and hotkey behave the same
- [ ] Toggle open → close → open is stable
- [ ] Rapid double-hotkey does not corrupt layout

## H. Create-flow integrity
- [ ] Overlay assign stores path + exeName + titleHint + aumid (Store) + launchUri (Obsidian)
- [ ] Save → relaunch uses the binding
- [ ] After a Chrome update (path change), exe-name match still works

## Implementation notes (expected behavior)
- Overlay and snap both use monitor **work area** (`rcWork`) — recreate biomes saved before this fix if zones look offset.
- Store/UWP apps (e.g. Spotify): snap while open during create — AUMID is captured; never launched via `WindowsApps` exe path.
- Obsidian: open the target vault before second Enter — `launchUri` is saved; Biomes never bare-launches `Obsidian.exe`.
- Partial biome success: failed zones log to STATUS; other zones still place; no blocking Store error dialogs.
- Close: biome apps restore pre-biome placement then minimize; clean-slate windows stay minimized.
- Sticky `boxId → HWND` survives biome **close** so reopen reuses the same windows.
- If the sticky HWND was **closed by the user**, the next open **launches new** instead of stealing a sibling `chrome.exe`.
- Weak exe-only match with multiple windows → launch new (never steal).
- STATUS toast lists per-zone notes (`placed existing`, `launched new (avoided sibling window)`, etc.).
