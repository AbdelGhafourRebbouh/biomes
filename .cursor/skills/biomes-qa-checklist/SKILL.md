---
name: biomes-qa-checklist
description: Run the Biomes V1 placement QA checklist against build/Release/Biomes.exe. Use before releases or after window-engine changes.
---

# Biomes QA Checklist

Run manual tests from `docs/PLACEMENT_CHECKLIST.md` against `build/Release/Biomes.exe`.

## Setup
1. Build: `cmake --build build --config Release`
2. Run: `.\build\Release\Biomes.exe`
3. Watch `build/Release/config/biomes_runtime.log` for `[SCALER]`, `[CLEAN]`, `[SESSION]`, `[PLACE]`

## Priority order
1. **Section A** — basic open/close/reopen
2. **Section F** — hotkey toggle stability
3. **Section D** — VS Code, Cursor, Spotify, Discord
4. **Section E** — dual monitor + laptop-only unplug

## Expected V1 close behavior
- Biome apps restore to **pre-biome size**, then minimize
- Non-biome apps minimized on open **stay minimized** (no flood of old windows)

## After coordinate fix
Existing saved biomes may be slightly misaligned — **recreate layouts once** after upgrading.

## Report format
For each failed item: app name, monitor setup, expected vs actual, relevant log lines.
