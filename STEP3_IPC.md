# Step 3 native layout IPC

The trusted local dashboard can send these objects through the existing WebView2
message bridge. Frontend files are unchanged. Optional `requestId` values must be
strings of at most 128 bytes; responses echo valid IDs and include `success`.
Errors include an `error` string.

## Snapshot

```json
{"action":"SNAPSHOT_LAYOUT","requestId":"snapshot-1"}
```

Returns `SNAPSHOT_LAYOUT_RESULT` with `topologyHash` and `boxes`. Captures visible,
non-minimized application windows and excludes Biomes windows. Each window uses
its largest intersection with a monitor work area, with bounds clipped to that
area. Offscreen windows are skipped. The response includes executable paths,
process names, titles, AUMIDs, monitor identities, and relative bounds.

A snapshot does not save to disk. Pass the returned `boxes` to the existing
`SAVE_BIOME` action with `name` and optional `id`, `hotkey`, and `coverImagePath`.
The existing save flow retains other topology variants and writes through
`AppPaths::BiomesFile()` under `%LOCALAPPDATA%\biomes\config`.

## Restore

```json
{"action":"RESTORE_LAYOUT","requestId":"restore-1","id":"saved-biome-id"}
```

Returns `RESTORE_LAYOUT_RESULT` with `success` and `status`. Reapplies the saved
workspace through the existing activation/session engine, even if already active.
Unlike `ACTIVATE_BIOME`, it does not toggle an active workspace closed. Missing
monitors are skipped. Success reports activation setup; deferred per-app launch
and placement results continue through the existing launch-progress channel.

## Overlay

```json
{"action":"TOGGLE_GRID_OVERLAY","requestId":"grid-1","enabled":true,"id":"saved-biome-id"}
```

Returns `TOGGLE_GRID_OVERLAY_RESULT` with `success` and `visible`. A saved `id`
opens connected zones directly in click-through snapping mode. Without an `id`,
it opens the drawing flow. Optional `rows` and `columns` default to 8 and 14 and
must be integers from 1 to 128. Omit `enabled` to toggle visibility. Explicitly
enabling an already-open overlay leaves its current layout in place.

Enter advances drawing to snapping and then emits the existing
`GRID_LAYOUT_READY` event with boxes for saving. Escape cancels from either mode.
`enabled:false` closes the overlay and restores the dashboard without saving.
Overlay drops use the shared window engine to preserve pre-snap placement and
verify deferred placement. The guide can highlight a target during dragging.

## Persistence and verification

The collection writer emits version 3, validates relative bounds, locks writers,
flushes the staging file, and replaces the destination. Loading accepts legacy
versions 1?3 (missing version defaults to legacy behavior); malformed collections
and unsupported versions fail without replacing the caller's collection. Empty
saved topology variants remain authoritative. Monitor enrichment never resolves
an unavailable saved identity using a reused index.

Build: `cmake --build build-step3-review --config Release --parallel 4`

Tests: `ctest --test-dir build-step3-review -C Release --output-on-failure`

The regression suite uses isolated layout files. Actual drag/drop, mixed-DPI
rendering, and WebView2 message round-trips still require interactive validation.
