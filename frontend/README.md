# Frontend (deferred)

V1 ships **`index.html`** next to `Biomes.exe` (copied by CMake). The WebView2 dashboard loads that file directly.

A Vite/React UI was started here but is **not built or used** in V1. Do not add `npm install` to the release path until Method 2 is planned.

When a separate frontend returns:

1. Build to static assets
2. Copy output beside `Biomes.exe` or load via `WebViewWindow::Initialize`

Until then, edit UI in **`/index.html`** at the repo root.
