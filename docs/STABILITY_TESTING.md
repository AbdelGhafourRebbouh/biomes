# Backend stability verification

The UI remains unchanged. This is a testable backend baseline, not a promise
that every third-party application or exclusive-fullscreen renderer is supported.

## Repeatable tests

From the repository root:

```powershell
cmake -S . -B build-stability -DBIOMES_BUILD_TESTS=ON
cmake --build build-stability --config Release
ctest --test-dir build-stability -C Release --output-on-failure
```

Close running instances before replacing Biomes.exe. Tests compile the production
window scaler into a separate executable, replace only monitor lookup and the
WebView status sink, and create their own synthetic windows. They do not load
saved biomes, launch personal applications, or operate on unrelated windows.
Synthetic pending-worker results exercise tracking without launching external apps.

Coverage includes repeated 1/8/24-window placements; negative-origin and scaled
physical geometry; malformed and unavailable-monitor zones; reentrant cancellation;
minimum-size refusal without minimization; AUMID/path mismatch; ambiguous duplicate
windows; stable replacement windows; fixed-size/disabled-window selection;
stale worker completion; the three-worker cap; and original placement restoration.

Simulated geometry is NOT a live mixed-DPI or cable-disconnection test.

## Verification recorded on 2026-09-07

- MSVC RelWithDebInfo desktop build and matching Biomes.pdb: passed.
- Final native regression suite: 325 assertions per run, 20 consecutive runs passed.
- Additional cases cover a native project-dialog class, independent completion
  while another launch is pending, and the bounded renderer resize/return path.
- No frontend source files changed in this hardening pass.
- The original running Release executable was locked during linking. The verified
  desktop executable is build-stability/RelWithDebInfo/Biomes.exe. Close other
  biomes instances before launching it from the repository root.
- No historical Biomes dump was found in the standard local CrashDumps directory.
  Physical monitor tests and a dump-backed root-cause diagnosis remain unverified.

## Physical desktop acceptance matrix (manual)

For each scenario, record the app versions, monitor resolution/scaling, launch
route (cold versus already running), biome ID and time. Keep logs local.

| Scenario | Expected result |
| --- | --- |
| Repeated small and large cold-start biomes | Other apps progress while a slow app launches; no freeze or crash |
| Affinity welcome/project picker | Selecting a project yields a tracked workspace; no automated clicking |
| Notion maximized -> small zone -> close -> reopen | Content relayouts and controls remain usable |
| Duplicate browser or document windows | Existing unrelated windows are not stolen; ambiguity waits rather than guessing |
| Close/switch during launch | Queued work is cancelled and old completions cannot claim the new session |
| Mixed 100/150/200% DPI, monitor left/above primary | Placement uses the selected work area, not taskbar or wrong-origin coordinates |
| Disconnect monitor while waiting for a window | Unavailable zones are skipped/cancelled, not redirected arbitrarily |
| Zone below app minimum | App stays open; bounded retries and an explanatory status, no forced clipping |
| Standard maximized and full-work-area zones | No F11/Escape injection or repeated fullscreen toggling |
| Real application/exclusive fullscreen | Unsupported behavior is reported; no promise of forcibly exiting it |

An OS launch call already executing cannot always be withdrawn: its app may
still open after cancellation, but it must not be snapped into the old biome.
Retries stop after a bounded placement check. A late third-party app may reposition
itself afterwards. Waiting for a workspace currently has a five-minute deadline.
Different-executable bootstrappers and ambiguous duplicate windows may require
app-specific identity adapters. A fixed minimum cannot universally be bypassed.

## Crash diagnosis

Earlier Windows events recorded heap corruption (0xc0000374). Invalidated tracker
references and callback reentrancy were found and hardened, but without the original
dump this is a plausible cause, not a confirmed stack-level diagnosis.

Release builds now retain debugging symbols. Keep Biomes.exe and Biomes.pdb from
the exact failing build together; rebuilding replaces symbols needed for that dump.

For a future crash, enable per-application Windows Error Reporting LocalDumps for
Biomes.exe (administrator action, opt-in). No registry settings are changed by
the app or these tests. See Microsoft's instructions:
https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps

Alternatively attach WinDbg or use Microsoft's ProcDump for the failing process.
Capture the exception and inspect its stack with the matching PDB before changing
more code. Dumps may contain private application data: keep them local and do not
automatically upload them. Record local config/biomes_runtime.log alongside the dump,
especially tracker IDs, launch PIDs, placement mismatch and cancellation times.

The regression suite and user-reported successful desktop tests do not certify
untested hardware, application versions, elevated apps or secure desktops.
