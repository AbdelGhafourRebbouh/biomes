#pragma once
#include <string>
#include <windows.h>

// Independent transparent, no-activate desktop surface. Only owner-thread calls.
class LaunchPanel {
public:
    static void Initialize(HWND dashboard);
    static void Update(const std::string& json);
    static void SetTheme(const std::string& theme);
    static void Shutdown();
#ifdef BIOMES_PANEL_TESTING
    static void CaptureTestPreview(const std::wstring& path);
#endif
};
