# Biome placement QA checklist

Run against `build/Release/Biomes.exe` before sharing.

## A. Basic Win32
- [ ] App not running → launches and snaps to correct zone
- [ ] App already open → reuses that window (no duplicate)
- [ ] Launch → Close biome → window returns to previous size/position
- [ ] Unrelated app (Notepad) minimized on launch → restored on close
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
- [ ] VS Code, Slack, Discord, Spotify: place when already open
- [ ] Place when not open (launch)
- [ ] Helper processes are not snapped instead of the main window

## E. Layout / monitors
- [ ] Primary monitor zones land on primary
- [ ] Secondary zones land on secondary
- [ ] Laptop-only (unplug secondary) → no crash; primary zones still work

## F. Session / hotkey
- [ ] Launch from card and hotkey behave the same
- [ ] Toggle open → close → open is stable
- [ ] Rapid double-hotkey does not corrupt layout

## G. Create-flow integrity
- [ ] Overlay assign stores path + exeName + titleHint
- [ ] Save → relaunch uses the binding
- [ ] After a Chrome update (path change), exe-name match still works

## Implementation notes (expected behavior)
- Sticky `boxId → HWND` survives biome **close** so reopen reuses the same windows.
- If the sticky HWND was **closed by the user**, the next open **launches new** instead of stealing a sibling `chrome.exe`.
- Weak exe-only match with multiple windows → launch new (never steal).
- STATUS toast lists per-zone notes (`placed existing`, `launched new (avoided sibling window)`, etc.).
