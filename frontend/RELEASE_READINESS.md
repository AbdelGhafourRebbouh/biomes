# Frontend release readiness

The frontend is integrated with the native backend and copied by the default CMake build.

## Connected

- Native grid creation and saved workspace loading; no demo cards or simulated saves.
- Save dialog, cover picker, zone reassignment, correlated save acknowledgement and stable IDs for retries.
- Shortcut validation, conflict checks and temporary hotkey suspension while recording.
- Activation, closing, active-session status, confirmed deletion and disconnected-monitor layout repair.
- Loading, empty and error states; native window controls and allowed external community links.

## Still requires release testing

- Full create/save/restart/activate/close/delete workflow in WebView2.
- Original WINDOWPLACEMENT restoration, multi-monitor repair and delayed application launch behavior.
- Display scaling, narrow windows, keyboard navigation and reduced motion.
- Cover persistence, invalid images, save errors and shortcut conflicts with other applications.
- First-install completion tracking and contextual overlay guidance remain future work; a replayable guide is available.
- Newsletter delivery, Ko-fi and website links remain inactive until their destinations/services are provided.
- Review privacy wording against any future optional online services before release.

Successful compilation and scripted frontend checks do not replace testing the live desktop workflows.
