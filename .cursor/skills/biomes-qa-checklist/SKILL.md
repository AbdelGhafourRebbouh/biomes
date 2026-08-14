---
name: biomes-qa-checklist
description: Run the Biomes V1 placement QA checklist against build/Release/Biomes.exe. Use before releases or after window-engine changes.
---

# Biomes QA Checklist

Run manual tests from `docs/PLACEMENT_CHECKLIST.md` against `build/Release/Biomes.exe`.

## Setup
1. Build: `cmake --build build --config Release`
2. Run: `.\build\Release\Biomes.exe`
3. Watch `build/Release/config/biomes_runtime.log` for `[SCALER]`, `[CLEAN]`, `[SESSION]`, `[PLACE]`, `[MONITOR]`

## Priority order
1. **Section A** — basic open/close/reopen
2. **Section F / F2** — topology, dock/undock, stable monitor identity
3. **Section G** — hotkey toggle stability
4. **Section D** — VS Code, Cursor, Spotify, Discord

## Topology QA (multi-monitor)
- Create biome on 2 monitors → save → unplug secondary → card shows "Designed for 2 screens · opens this screen only"
- Launch on 1 monitor → only primary-screen zones open (secondary apps stay closed / not remapped)
- Replug → launch → zones follow physical panels (check `[MONITOR] matched via stableId`)
- Change display while app open → toast "Display setup changed"
- **Fix layout** → repair overlay opens with remapped zones (optional)

## Expected V1 close behavior
- Biome apps restore to **pre-biome size**, then minimize
- Non-biome apps minimized on open **stay minimized** (no flood of old windows)

## JSON v3
- `stableMonitorId`, `topologyHash` on each zone; `layoutVariants` keyed by topology hash
- v2 biomes without stable IDs still work via `monitorDevice` fallback — re-save once for best topology tracking

## Report format
For each failed item: app name, monitor setup, expected vs actual, relevant log lines.
