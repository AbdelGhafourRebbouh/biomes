#pragma once
#include <windows.h>
#include <string>

class WebViewWindow {
public:
    // Initializes the Win32 window host and embeds Microsoft WebView2
    static bool Initialize(HINSTANCE hInstance, int nCmdShow, const std::string& startUrl);

    // Message loop handler
    static void RunMessageLoop();
};