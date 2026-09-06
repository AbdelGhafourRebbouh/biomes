# biomes frontend

This is the WebView2 dashboard shipped with the native application. The root-level legacy dashboard has been removed.

Build and run from the repository root:

```powershell
cmake -B build -S .
cmake --build build --config Release
.\build\Release\Biomes.exe
```

The default build copies the frontend and its assets beside the executable, even when only frontend files change. Close an older running instance before testing.

WebView2 messages connect workspace creation, persistence, activation, closing, deletion and layout repair. Opening the HTML in a browser previews the interface but does not manage desktop workspaces. Existing workspace configuration files are unchanged.
