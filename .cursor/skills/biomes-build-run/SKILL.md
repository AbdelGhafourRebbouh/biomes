---
name: biomes-build-run
description: Build and run Biomes on Windows with CMake and MSVC. Use when building or testing native changes.
---

# Build & Run Biomes

## Requirements
- Windows 10/11, WebView2 Runtime, CMake 3.20+, MSVC

## Build
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Run
```powershell
.\build\Release\Biomes.exe
```

CMake copies `index.html` and `WebView2Loader.dll` into the output folder.
Saved biomes: `build/Release/config/biomes.json` (local, not committed).

## Debug console
`AllocConsole` is enabled in `_DEBUG` builds only.
