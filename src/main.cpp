#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <filesystem>

// This links to your header file that we built earlier
#include "../include/ui/webview_window.hpp"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 1. CREATE A DEBUG TERMINAL
    // Windows GUI apps hide text by default. This opens a black terminal window 
    // so you can see messages printed with std::cout.
    AllocConsole();
    freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
    freopen_s((FILE**)stderr, "CONOUT$", "w", stderr);

    std::cout << "=== Biomes Workspace Engine Booting Up ===" << std::endl;

    // 2. SET UP JS -> C++ COMMUNICATION
    // This listens for any message sent from your JavaScript UI (index.html).
    WebViewWindow::SetMessageReceivedCallback([](const std::string& message) {
        std::cout << "\n[C++ Core] Heard message from UI: " << message << std::endl;

        // When C++ hears a message, it immediately sends a reply back down to JavaScript (C++ -> JS)
        std::string reply = "{\"status\":\"success\", \"message\":\"Hello from C++ backend!\"}";
        WebViewWindow::SendMessageToUI(reply);
    });

    // 3. FIND index.html AUTOMATICALLY
    // This finds the folder where app.exe is running, and points directly to index.html next to it.
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::filesystem::path exePath(buffer);
    std::filesystem::path htmlPath = exePath.parent_path() / "index.html";
    std::string startUrl = "file:///" + htmlPath.string();

    std::cout << "[WebView] Loading UI from: " << startUrl << std::endl;

    // 4. INITIALIZE THE WINDOW & WEBVIEW
    // This creates the actual desktop window and injects Microsoft Edge (WebView2) into it.
    if (!WebViewWindow::Initialize(hInstance, nCmdShow, startUrl)) {
        std::cerr << "[Error] Failed to create WebView window!" << std::endl;
        return -1;
    }

    // 5. START THE WINDOW MESSAGE LOOP
    // This keeps the app running, listening for mouse clicks, resizing, and keyboard inputs 
    // until you close the window.
    WebViewWindow::RunMessageLoop();

    return 0;
}