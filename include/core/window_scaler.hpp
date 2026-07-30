#pragma once
#include <windows.h>
#include <string>
#include <vector>
using namespace std;

/// Represents metadata for a single captured desktop window
struct WindowInfo {
    HWND hwnd;          // Unique handle (ID) identifying the window in Windows OS
    std::string title;  // Title bar text of the window
    RECT rect;          // Screen position and dimensions
};

class WindowScaler {
public:
    // Moves and resizes an open window handle
    static void SetPosition(HWND hwnd, int x, int y, int width, int height);

    // Scans all open top-level application windows on Windows OS
    static vector<WindowInfo> GetActiveWindows();

    // Minimizes all active windows to expose the desktop background
    static void ShowDesktop();
};